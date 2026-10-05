# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""What is actually inside the two files the controller ships into a game.

Two separate properties, one per binary, and they are not the same property:

  overglaze_controller.dll  the host. It may contain the controller, provider and
                          runtime code, and NO research object. This is what the
                          dual-track split exists for, and a linker map is the
                          only check that catches a research library sneaking
                          back in through a transitive dependency.

  dxgi.dll                the root proxy, the one file that sits in the game's
                          own directory. The game's loader resolves ITS imports
                          before the game runs, so it must contain NO Lab library
                          at all -- not even the host. The previous controller
                          dxgi.dll linked the whole host and dragged 25 modules
                          into the loader lock, where Alan Wake 2 hung. That is
                          what these checks prevent from coming back.

Both maps come from a controller-track build: the directory in
%OVERGLAZE_CONTROLLER_BUILD% when set, else <checkout>/data/_build_controller.
Each check skips itself when that build has not run (Build-Controller.ps1).
"""
import json
import os
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("OVERGLAZE_CONTROLLER_BUILD") or (ROOT / "data/_build_controller")) / "controller"
HOST_MAP = BUILD / "overglaze_controller.map"
PROXY_MAP = BUILD / "dxgi.map"
OWNERSHIP = json.loads((ROOT / "source/native/OWNERSHIP.json").read_text(encoding="utf-8"))

# Every library the controller HOST image is allowed to contain. lab_provider and
# lab_live_runtime_client are here on purpose: they are provider/runtime track
# code, shared with the research host and free of research headers. So is
# lab_indirect_observer (runtime: command-signature creation metadata on the
# early path, which 007 First Light needs); the latency markers live inside
# lab_provider.
ALLOWED_LIBRARIES = {
    "lab_controller_host", "lab_core", "lab_provider", "lab_live_runtime_client",
    "lab_indirect_observer",
    "lab_overlay", "lab_ui_preferences", "lab_imgui", "lab_native_resume",
    "lab_capture_library", "lab_preview_export", "lab_pair_preview", "minhook",
    "x64",  # the toolchain's own import/runtime members, e.g. x64:asm.obj
}
# Objects that would mean a research collector, observer or probe came along.
BANNED_OBJECTS = {
    "frame_capture.cpp.obj", "display_pair.cpp.obj", "command_log.cpp.obj",
    "command_probe.cpp.obj", "command_slots.cpp.obj", "queue_probe.cpp.obj",
    "present_probe.cpp.obj", "present_slots.cpp.obj", "access_probe.cpp.obj",
    "exception_watch.cpp.obj", "post_inspect.cpp.obj",
    "standalone_host.cpp.obj", "live_host.cpp.obj", "nr_live_research.cpp.obj",
    "workbench_research.cpp.obj", "streamline_observer.cpp.obj", "rr_inner.cpp.obj",
    "sl_restore.cpp.obj", "shader_log.cpp.obj", "resource_creation.cpp.obj",
    "gpu_timestamps.cpp.obj", "timing_probe.cpp.obj", "native_command.cpp.obj",
    "nvapi_work.cpp.obj", "present_host.cpp.obj",
}
MEMBER = re.compile(r"\b([A-Za-z_0-9]+):([a-z_0-9]+\.(?:cpp|asm|c)\.obj)\b")


def members(path):
    if not path.is_file():
        raise unittest.SkipTest(f"{path} not built; run Build-Controller.ps1")
    return set(MEMBER.findall(path.read_text(encoding="utf-8", errors="replace")))


class ControllerHostClosure(unittest.TestCase):
    """overglaze_controller.dll: the controller may be here, research may not."""

    @classmethod
    def setUpClass(cls):
        cls.members = members(HOST_MAP)

    def test_only_allowed_libraries_are_linked(self):
        libraries = sorted({lib for lib, _ in self.members})
        unexpected = [lib for lib in libraries if lib not in ALLOWED_LIBRARIES]
        self.assertEqual(unexpected, [], f"the controller host links unexpected libraries (all: {libraries})")

    def test_no_research_object_is_linked(self):
        found = sorted(f"{lib}:{obj}" for lib, obj in self.members if obj in BANNED_OBJECTS)
        self.assertEqual(found, [], "a research object reached the controller host")

    def test_the_banned_library_list_is_honoured(self):
        banned = set(OWNERSHIP.get("controller_ban", {}).get("libraries", []))
        found = sorted(lib for lib, _ in self.members if lib in banned)
        self.assertEqual(found, [], "OWNERSHIP controller_ban library in the controller host")

    def test_the_host_and_its_shared_libraries_are_present(self):
        # A map that lost these would pass the bans for the wrong reason.
        libraries = {lib for lib, _ in self.members}
        for required in ("lab_controller_host", "lab_provider", "lab_live_runtime_client", "lab_core"):
            with self.subTest(library=required):
                self.assertIn(required, libraries)


class RootProxyClosure(unittest.TestCase):
    """dxgi.dll: the file in the game's directory carries nothing of ours."""

    @classmethod
    def setUpClass(cls):
        cls.members = members(PROXY_MAP)

    def test_no_lab_library_is_linked(self):
        # Not "no research library" -- no Lab library at all, the host included.
        # Everything Lab needs arrives later, from the payload subdirectory.
        found = sorted({lib for lib, _ in self.members if lib.startswith("lab_")})
        self.assertEqual(found, [], "the root proxy must carry no Lab library into the game's loader")

    def test_it_is_built_from_its_own_two_sources(self):
        objects = sorted({obj for _, obj in self.members})
        self.assertNotIn("controller_host.cpp.obj", objects)
        self.assertNotIn("dxgi_forward.cpp.obj", objects, "the proxy uses dxgi_proxy.cpp, not the host-linked forwarder")

    def test_the_import_table_stays_tiny(self):
        # The real invariant, checked against the built file rather than the map:
        # every module here is resolved under the loader lock before the game
        # runs. d3d12, the shader compiler, WinTrust, OLE32, Shell32 and the MSVC
        # runtime DLLs were all in the previous list and must not return.
        dll = BUILD / "dxgi.dll"
        if not dll.is_file():
            raise unittest.SkipTest(f"{dll} not built")
        imports = imported_modules(dll)
        forbidden = {"d3d12.dll", "d3dcompiler_47.dll", "wintrust.dll", "ole32.dll",
                     "shell32.dll", "dxgi.dll", "advapi32.dll", "imm32.dll", "version.dll", "bcrypt.dll"}
        self.assertEqual(sorted(forbidden & imports), [], f"the root proxy imports too much (all: {sorted(imports)})")
        self.assertEqual(sorted(m for m in imports if m.startswith(("msvcp", "vcruntime", "api-ms-win-crt"))), [],
                         "the root proxy must use the static CRT so no runtime DLL is resolved for it")
        self.assertLessEqual(len(imports), 4, f"the root proxy's import list grew: {sorted(imports)}")


def imported_modules(path):
    """Module names in a PE64 import directory, read straight from the file."""
    import struct

    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    table = []
    for i in range(count):
        base = sections + i * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, base + 8)
        table.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def offset(rva):
        for virtual_address, size, raw_pointer in table:
            if virtual_address <= rva < virtual_address + size:
                return raw_pointer + (rva - virtual_address)
        raise AssertionError(f"RVA {rva:#x} outside every section")

    magic = struct.unpack_from("<H", data, pe + 24)[0]
    directory = pe + 24 + (112 if magic == 0x20B else 96)
    imports = struct.unpack_from("<I", data, directory + 8)[0]
    if not imports:
        return set()
    names, cursor = set(), offset(imports)
    while True:
        entry = data[cursor:cursor + 20]
        if len(entry) < 20 or entry == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", entry, 12)[0]
        if not name_rva:
            break
        start = offset(name_rva)
        names.add(data[start:data.index(b"\0", start)].decode("ascii").lower())
        cursor += 20
    return names


if __name__ == "__main__":
    unittest.main()
