# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'python'))
import lab_game_preflight as p


def image(machine=0x8664, dll=False, exports=False):
    b = bytearray(1024); b[:2] = b'MZ'; struct.pack_into('<I', b, 60, 128)
    b[128:132] = b'PE\0\0'; struct.pack_into('<HH', b, 132, machine, 1)
    struct.pack_into('<HH', b, 148, 240, 0x2000 if dll else 0)
    struct.pack_into('<H', b, 152, 0x20b); struct.pack_into('<I', b, 260, 16)
    struct.pack_into('<IIII', b, 400, 512, 0x1000, 512, 512)
    struct.pack_into('<I', b, 428, 0x60000020)
    if exports:
        struct.pack_into('<II', b, 264, 0x1000, 100)
        struct.pack_into('<I', b, 532, 1); struct.pack_into('<I', b, 540, 0x1048)
        struct.pack_into('<I', b, 536, 1); struct.pack_into('<I', b, 544, 0x1040)
        struct.pack_into('<I', b, 548, 0x1044); struct.pack_into('<H', b, 580, 0)
        struct.pack_into('<I', b, 584, 0x1100)
        struct.pack_into('<I', b, 576, 0x1060)
        name = b'slEvaluateFeature\0'; b[608:608 + len(name)] = name
    return bytes(b)


class PreflightTests(unittest.TestCase):
    def test_exports_are_hints_not_runtime(self):
        r = p.pe_summary(image(exports=True, dll=True))
        self.assertTrue(r['x64']); self.assertTrue(r['is_dll'])
        self.assertEqual(r['public_streamline_exports'], ['slEvaluateFeature'])
        self.assertEqual(r['streamline_entrypoints'], [dict(name='slEvaluateFeature', rva='0x1100', kind='executable-section')])
        self.assertFalse(r['export_addresses_grant_hook_permission'])

    def test_export_ordinal_and_missing_function_table_refused(self):
        b = bytearray(image(exports=True)); struct.pack_into('<H', b, 580, 1)
        with self.assertRaises(ValueError): p.pe_summary(b)
        b = bytearray(image(exports=True)); struct.pack_into('<I', b, 532, 0)
        with self.assertRaises(ValueError): p.pe_summary(b)

    def test_forwarder_not_treated_as_code(self):
        b = bytearray(image(exports=True)); struct.pack_into('<I', b, 584, 0x1050)
        b[592:605] = b'other.fn\0xxxx'
        r = p.pe_summary(b)['streamline_entrypoints'][0]
        self.assertEqual(r['kind'], 'forwarder'); self.assertEqual(r['forwarder'], 'other.fn')

    def test_forwarder_must_terminate_inside_export_directory(self):
        b = bytearray(image(exports=True)); struct.pack_into('<I', b, 584, 0x1050)
        b[592:612] = b'x' * 20
        # Retain the export name outside the forwarder range.
        struct.pack_into('<I', b, 576, 0x1080); b[640:658] = b'slEvaluateFeature\0'
        with self.assertRaises(ValueError): p.pe_summary(b)

    def test_missing_and_non_code_targets_are_not_admitted(self):
        b = bytearray(image(exports=True)); struct.pack_into('<I', b, 584, 0)
        self.assertEqual(p.pe_summary(b)['streamline_entrypoints'][0]['kind'], 'absent')
        b = bytearray(image(exports=True)); struct.pack_into('<I', b, 428, 0x40000040)
        self.assertEqual(p.pe_summary(b)['streamline_entrypoints'][0]['kind'], 'non-executable-section')

    def test_both_tag_names_require_observed_calls(self):
        # The mere existence of either ABI must not select a runtime mode.
        for name in (b'slSetTag\0', b'slSetTagForFrame\0'):
            b = bytearray(image(exports=True)); b[608:608+len(name)] = name
            r = p.pe_summary(b)
            self.assertEqual(r['tagging_mode'], 'runtime-call-observation-required')
            self.assertFalse(r['export_addresses_grant_hook_permission'])

    def test_both_tag_exports_preserved_without_guessing_runtime(self):
        b = bytearray(image(exports=True))
        struct.pack_into('<IIII', b, 532, 2, 2, 0x1050, 0x1040)
        struct.pack_into('<I', b, 548, 0x1048)
        struct.pack_into('<II', b, 576, 0x1080, 0x10b0)
        struct.pack_into('<HH', b, 584, 0, 1)
        struct.pack_into('<II', b, 592, 0x1100, 0x1110)
        b[640:649] = b'slSetTag\0'; b[688:705] = b'slSetTagForFrame\0'
        r = p.pe_summary(b)
        self.assertEqual(r['public_streamline_exports'], ['slSetTag', 'slSetTagForFrame'])
        self.assertEqual([e['rva'] for e in r['streamline_entrypoints']], ['0x1100', '0x1110'])
        self.assertEqual(r['tagging_mode'], 'runtime-call-observation-required')
        struct.pack_into('<I', b, 580, 0x1080)
        with self.assertRaisesRegex(ValueError, 'Duplicate'): p.pe_summary(b)

    def test_truncated_pe(self):
        for data in [b'', image()[:100], image()[:420], image()[:-1]]:
            with self.assertRaises(ValueError): p.pe_summary(data)

    def test_raw_rva_cannot_use_virtual_padding(self):
        b = bytearray(image(exports=True)); struct.pack_into('<I', b, 576, 0x1400)
        with self.assertRaises(ValueError): p.pe_summary(b)

    def test_wrong_architecture_not_admitted(self):
        self.assertFalse(p.pe_summary(image(0xaa64))['x64'])

    def test_readonly_ambiguous_selection_and_unsafe_markers(self):
        # The checkout's data directory when it has one, else the system temporary
        # directory; either way a fresh directory with no reparse point on the way.
        data = p.Path(__file__).resolve().parents[3] / 'data'
        with tempfile.TemporaryDirectory(dir=data if data.is_dir() else None) as tmp:
            root = Path(tmp); (root/'子目录').mkdir()
            for name in ['game.exe', 'launcher.exe', 'sl.interposer.dll']:
                (root/name).write_bytes(image(exports=name.endswith('.dll')))
            (root/'子目录'/'EasyAntiCheat').mkdir()
            before = {str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in root.rglob('*') if x.is_file()}
            result = p.inspect(root)
            self.assertIsNone(result['selected_executable']); self.assertTrue(result['safety_markers'])
            self.assertFalse(result['nr']['available']); self.assertFalse(any(result['actions'].values()))
            self.assertEqual(result['nr']['status'], 'adapter-and-runtime-verification-required')
            self.assertEqual(before, {str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in root.rglob('*') if x.is_file()})
            self.assertEqual(p.inspect(root, root/'game.exe')['selected_executable'], 'game.exe')
            self.assertEqual(p.inspect(root/'game.exe')['selected_executable'], 'game.exe')

    def test_absent_markers_do_not_confirm_safety(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = p.inspect(tmp)
            self.assertFalse(result['safety']['no_anticheat_confirmed'])
            self.assertTrue(result['scan_complete'])

    def test_bounds_are_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root/'game.exe').write_bytes(image())
            with patch.object(p, 'MAX_BINARY_BYTES', 1):
                r = p.inspect(root); self.assertFalse(r['scan_complete']); self.assertIsNone(r['files'][0]['sha256'])
            with patch.object(p, 'MAX_ENTRIES', 0):
                self.assertFalse(p.inspect(root)['scan_complete'])

    def test_explicit_executable_outside_root_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root/'game').mkdir(); (root/'elsewhere.exe').write_bytes(image())
            with self.assertRaises(ValueError): p.inspect(root/'game', root/'elsewhere.exe')

    def test_reparse_path_rejected_before_resolve(self):
        with patch.object(Path, 'lstat', return_value=SimpleNamespace(st_mode=0, st_file_attributes=0x400)):
            with self.assertRaises(ValueError): p.plain(Path.cwd())

    def test_excess_export_names_rejected(self):
        b = bytearray(image(exports=True)); struct.pack_into('<I', b, 536, 8193)
        with self.assertRaises(ValueError): p.pe_summary(b)


if __name__ == '__main__': unittest.main()
