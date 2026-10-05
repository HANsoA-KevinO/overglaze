# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Read-only, bounded path discovery for lightweight NR adapters.

Never executes a target, loads a DLL, installs files, or enables a probe.
Module presence and public export names are navigation hints, not compatibility.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import time

MAX_ENTRIES = 8192
MAX_DEPTH = 7
MAX_BINARIES = 64
MAX_BINARY_BYTES = 512 << 20
MAX_HASH_BYTES = 1 << 30
MAX_OUTPUT_BYTES = 128 << 10
RUNTIME_NAMES = {'dxgi.dll', 'd3d12.dll', 'd3d12core.dll', 'd3d11.dll', 'vulkan-1.dll',
                 'nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll', 'nvngx_dlssnr.dll',
                 'overglaze_nvngx.dll', 'overglaze.install.json', 'dlsslab_nvngx.dll', 'dlsslab.install.json', 'winmm.dll', 'version.dll'}
SL_EXPORTS = {'slInit', 'slEvaluateFeature', 'slGetNewFrameToken', 'slSetConstants',
              'slSetTag', 'slSetTagForFrame', 'slGetNativeInterface', 'slGetFeatureFunction'}
SAFETY_MARKERS = ('easyanticheat', 'battleye', 'beservice', 'beclient', 'equ8', 'anticheat', 'vgk.sys')


def plain(path):
    path = Path(os.path.abspath(path))
    for p in (path, *path.parents):
        s = p.lstat()
        if stat.S_ISLNK(s.st_mode) or getattr(s, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Reparse/symlink path refused: ' + str(p))
    return path


def pe_summary(data):
    """Minimal raw-file PE mapping. No imports, signature trust or ABI inference."""
    def unpack(fmt, pos):
        if pos < 0 or pos + struct.calcsize(fmt) > len(data):
            raise ValueError('Truncated PE field')
        return struct.unpack_from(fmt, data, pos)
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Not a PE image')
    pe, = unpack('<I', 60)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Missing PE signature')
    machine, count = unpack('<HH', pe + 4)
    opt_size, flags = unpack('<HH', pe + 20)
    opt = pe + 24
    magic, = unpack('<H', opt)
    if magic not in (0x20b, 0x10b) or not 1 <= count <= 96:
        raise ValueError('Unsupported PE header')
    directory = opt + (112 if magic == 0x20b else 96)
    directory_count, = unpack('<I', directory - 4)
    if directory > opt + opt_size or opt + opt_size > len(data):
        raise ValueError('Invalid optional header')
    sections = []
    for i in range(count):
        off = opt + opt_size + i * 40
        virtual, rva, size, raw = unpack('<IIII', off + 8)
        characteristics, = unpack('<I', off + 36)
        if raw + size > len(data):
            raise ValueError('Truncated section')
        sections.append((rva, raw, size, characteristics))
    def mapped(rva, size):
        matches = [raw + rva - base for base, raw, length, _ in sections
                   if base <= rva and rva + size <= base + length]
        if len(matches) != 1:
            raise ValueError('Unmapped/ambiguous raw RVA')
        return matches[0]
    exports, entrypoints = [], []
    if directory_count:
        if directory + 8 > opt + opt_size:
            raise ValueError('Missing export directory')
        rva, size = unpack('<II', directory)
        if rva and size:
            if size < 40:
                raise ValueError('Truncated export directory')
            mapped(rva, size)
            off = mapped(rva, 40)
            functions, = unpack('<I', off + 20)
            names, = unpack('<I', off + 24)
            function_table, = unpack('<I', off + 28)
            table, = unpack('<I', off + 32)
            ordinal_table, = unpack('<I', off + 36)
            if names > 8192 or functions > 8192:
                raise ValueError('Export name budget exceeded')
            if names:
                if not functions:
                    raise ValueError('Named exports without function table')
                address = mapped(table, names * 4)
                ordinals = mapped(ordinal_table, names * 2)
                addresses = mapped(function_table, functions * 4)
                for i in range(names):
                    name_rva, = unpack('<I', address + i * 4)
                    start = mapped(name_rva, 1)
                    end = data.find(b'\0', start, min(start + 257, len(data)))
                    if end < 0:
                        raise ValueError('Unterminated export name')
                    mapped(name_rva, end - start + 1)
                    name = data[start:end].decode('ascii', 'strict')
                    if name in SL_EXPORTS:
                        if name in exports:
                            raise ValueError('Duplicate Streamline export name')
                        ordinal, = unpack('<H', ordinals + i * 2)
                        if ordinal >= functions:
                            raise ValueError('Export ordinal outside function table')
                        target, = unpack('<I', addresses + ordinal * 4)
                        item = dict(name=name, rva=hex(target), kind='absent')
                        if rva <= target < rva + size:
                            forward = mapped(target, 1)
                            end_forward = data.find(b'\0', forward, min(forward + 257, forward + rva + size - target))
                            if end_forward < 0:
                                raise ValueError('Unterminated export forwarder')
                            item.update(kind='forwarder', forwarder=data[forward:end_forward].decode('ascii', 'strict'))
                        elif target:
                            mapped(target, 1)
                            code = next(c for b, _, n, c in sections if b <= target < b + n)
                            item['kind'] = 'executable-section' if code & 0x20000000 else 'non-executable-section'
                        # A code section is navigation, not an ABI, instruction
                        # boundary, runtime implementation or trusted signature.
                        entrypoints.append(item)
                        exports.append(name)
    return dict(machine=hex(machine), x64=machine == 0x8664 and magic == 0x20b,
                is_dll=bool(flags & 0x2000), public_streamline_exports=sorted(exports),
                streamline_entrypoints=sorted(entrypoints, key=lambda item: item['name']),
                export_addresses_grant_hook_permission=False,
                tagging_mode='runtime-call-observation-required' if {'slSetTag', 'slSetTagForFrame'} & set(exports) else None)


def inspect(root, executable=None):
    root = plain(root)
    if root.is_file():
        if executable is not None:
            raise ValueError('Supply a directory plus --exe OR one executable')
        executable, root = root, root.parent
    if not root.is_dir() or root == Path(root.anchor):
        raise ValueError('One specific game installation directory required')
    if executable is not None:
        executable = plain(executable)
        if not executable.is_relative_to(root) or executable.suffix.lower() != '.exe' or not executable.is_file():
            raise ValueError('Executable must be inside the selected installation')
    pending = [(root, 0)]
    binaries, markers, links, errors = [], [], [], []
    entries, hashed = 0, 0
    deadline = time.monotonic() + 15
    complete = True
    while pending:
        folder, depth = pending.pop()
        with os.scandir(folder) as directory:
            for entry in directory:
                entries += 1
                if entries > MAX_ENTRIES or time.monotonic() > deadline:
                    complete = False; pending.clear(); break
                path = Path(entry.path)
                rel = path.relative_to(root).as_posix()
                lower = entry.name.lower()
                s = entry.stat(follow_symlinks=False)
                if any(m in lower for m in SAFETY_MARKERS):
                    markers.append(rel)
                if stat.S_ISLNK(s.st_mode) or getattr(s, 'st_file_attributes', 0) & 0x400:
                    links.append(rel); complete = False; continue
                if stat.S_ISDIR(s.st_mode):
                    if depth < MAX_DEPTH: pending.append((path, depth + 1))
                    else: complete = False
                    continue
                if not stat.S_ISREG(s.st_mode): continue
                if not (lower.endswith('.exe') or lower in RUNTIME_NAMES or
                        lower.startswith('sl.') and lower.endswith('.dll') or lower.endswith('.addon64')):
                    continue
                if len(binaries) >= MAX_BINARIES:
                    complete = False; pending.clear(); break
                row = dict(path=rel, bytes=s.st_size, sha256=None)
                binaries.append(row)
                if not 0 < s.st_size <= MAX_BINARY_BYTES or hashed + s.st_size > MAX_HASH_BYTES:
                    row['error'] = 'binary_byte_budget'; complete = False; continue
                plain(path)
                # Windows DirEntry.stat may omit the file index (st_ino=0).
                # Compare matching path-stat APIs, not that cached enumeration.
                s = path.stat()
                if not 0 < s.st_size <= MAX_BINARY_BYTES or hashed + s.st_size > MAX_HASH_BYTES:
                    row['error'] = 'binary_changed_or_budget'; complete = False; continue
                row['bytes'] = s.st_size
                with path.open('rb') as stream:
                    data = stream.read(s.st_size + 1)
                after = path.stat()
                if len(data) != s.st_size or (s.st_size, s.st_mtime_ns, s.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
                    raise ValueError('Input changed during inspection: ' + rel)
                hashed += len(data); row['sha256'] = hashlib.sha256(data).hexdigest()
                if lower.endswith(('.exe', '.dll', '.addon64')):
                    try: row.update(pe_summary(data))
                    except (ValueError, UnicodeError, struct.error) as e:
                        row['error'] = str(e); errors.append(rel)
    exes = [r for r in binaries if r['path'].lower().endswith('.exe')]
    selected = executable.relative_to(root).as_posix() if executable else (exes[0]['path'] if len(exes) == 1 else None)
    if selected and not any(r['path'] == selected for r in exes):
        selected = None; complete = False
    # Never infer offline/anti-cheat scope from absent filenames, or compatibility
    # from an adjacent DLL. Exact module admission remains the native host's job.
    return dict(schema='dlsslab-game-preflight-v1', root=str(root), selected_executable=selected,
                executable_candidates=[r['path'] for r in exes], files=binaries,
                scan_complete=complete, entries_examined=entries, bytes_read=hashed,
                skipped_links=links, unreadable_pe=errors, safety_markers=markers,
                safety=dict(offline_singleplayer_confirmed=False, no_anticheat_confirmed=False,
                            absence_of_markers_proves_safety=False),
                nr=dict(available=False, status='adapter-and-runtime-verification-required',
                        required=['actual Color/Depth/MV and constants', 'color input/output contract',
                                  'safe insertion and command restoration', 'GPU lifetime and completion',
                                  'exact executable/module admission', 'offline/no-anticheat confirmation']),
                actions=dict(target_executed=False, dll_loaded=False, files_installed=False,
                             probes_enabled=False, game_settings_changed=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--exe', type=Path)
    args = parser.parse_args()
    result = json.dumps(inspect(args.path, args.exe), ensure_ascii=False, indent=2)
    if len(result.encode('utf-8')) > MAX_OUTPUT_BYTES:
        raise ValueError('Preflight output budget exceeded')
    print(result)


if __name__ == '__main__':
    main()
