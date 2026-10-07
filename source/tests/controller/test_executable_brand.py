# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Read embedded resources as data. Never start the viewer or load its imports."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("OVERGLAZE_CONTROLLER_BUILD") or ROOT / "data/_build_controller")
EXE = BUILD / "overglaze_viewer.exe"


@unittest.skipUnless(os.name == "nt" and EXE.is_file(), "Windows controller build required")
class ExecutableBrandTests(unittest.TestCase):
    def test_multisize_icon_is_embedded_in_the_actual_executable(self):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD)
        kernel.LoadLibraryExW.restype = ctypes.c_void_p
        kernel.FindResourceW.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
        kernel.FindResourceW.restype = ctypes.c_void_p
        kernel.LoadResource.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
        kernel.LoadResource.restype = ctypes.c_void_p
        kernel.LockResource.argtypes = (ctypes.c_void_p,)
        kernel.LockResource.restype = ctypes.c_void_p
        kernel.SizeofResource.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
        kernel.SizeofResource.restype = wintypes.DWORD
        kernel.FreeLibrary.argtypes = (ctypes.c_void_p,)
        kernel.FreeLibrary.restype = wintypes.BOOL
        # LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE: no DllMain,
        # executable code, graphics device or dependency loading.
        module = kernel.LoadLibraryExW(str(EXE), None, 0x22)
        self.assertTrue(module, ctypes.get_last_error())
        try:
            group = kernel.FindResourceW(module, 101, 14)
            self.assertTrue(group, "The window's icon resource 101 must exist")
            size = kernel.SizeofResource(module, group)
            data = ctypes.string_at(kernel.LockResource(kernel.LoadResource(module, group)), size)
            reserved, kind, count = struct.unpack_from("<HHH", data)
            self.assertEqual((reserved, kind, count), (0, 1, 8))
            dimensions = []
            for i in range(count):
                width, height, _, _, planes, bits, length, resource_id = struct.unpack_from("<BBBBHHIH", data, 6 + 14 * i)
                dimensions.append(width or 256)
                self.assertEqual(width, height)
                self.assertEqual((planes, bits), (1, 32))
                icon = kernel.FindResourceW(module, resource_id, 3)
                self.assertTrue(icon)
                self.assertEqual(kernel.SizeofResource(module, icon), length)
            self.assertEqual(dimensions, [16, 20, 24, 32, 48, 64, 128, 256])
        finally:
            kernel.FreeLibrary(module)

    def test_windows_product_identity_is_not_the_old_lab_or_an_unversioned_binary(self):
        version = ctypes.WinDLL("version", use_last_error=True)
        version.GetFileVersionInfoSizeW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
        version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
        version.GetFileVersionInfoW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p)
        version.GetFileVersionInfoW.restype = wintypes.BOOL
        version.VerQueryValueW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT))
        version.VerQueryValueW.restype = wintypes.BOOL
        unused = wintypes.DWORD()
        size = version.GetFileVersionInfoSizeW(str(EXE), ctypes.byref(unused))
        self.assertGreater(size, 0)
        data = ctypes.create_string_buffer(size)
        self.assertTrue(version.GetFileVersionInfoW(str(EXE), 0, size, data))
        expected = {"ProductName": "Overglaze", "ProductVersion": "0.2.0-preview.3",
                    "OriginalFilename": "overglaze_viewer.exe", "CompanyName": "HANsoA-KevinO"}
        for key, value in expected.items():
            pointer, length = ctypes.c_void_p(), wintypes.UINT()
            self.assertTrue(version.VerQueryValueW(data, "\\StringFileInfo\\040904B0\\" + key,
                                                  ctypes.byref(pointer), ctypes.byref(length)))
            self.assertEqual(ctypes.wstring_at(pointer, length.value).rstrip("\0"), value)


if __name__ == "__main__":
    unittest.main()
