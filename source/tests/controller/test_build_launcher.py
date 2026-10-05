# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from msvc_launcher import normalize


class BuildLauncherTests(unittest.TestCase):
    def test_chinese_prefix_and_path_in_system_code_page(self):
        value = "注意: 包含文件:  D:\\src\\研究\\header.hpp\r\n"
        result = normalize(value.encode("cp936")).decode("utf-8")
        self.assertEqual(result, "Note: including file: D:\\src\\研究\\header.hpp\r\n")

    def test_utf8_chinese_prefix(self):
        value = "注意: 包含文件:  D:\\src\\header.hpp\r\n"
        self.assertTrue(normalize(value.encode()).startswith(b"Note: including file: D:"))

    def test_english_diagnostic_preserved(self):
        value = b"test.cpp(1): error C2001: newline in constant\r\n"
        self.assertEqual(normalize(value), value)
