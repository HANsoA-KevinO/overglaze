# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "source/python"))
from generate_brand_icon import make_icon, read_shapes, rasterize


class BrandIconTests(unittest.TestCase):
    def test_small_icon_is_transparent_at_corners_and_has_both_layers(self):
        pixels = rasterize(read_shapes(ROOT / "assets/branding/overglaze.svg"), 16)
        colours = [tuple(pixels[i:i + 4]) for i in range(0, len(pixels), 4)]
        self.assertEqual(colours[0][3], 0)
        self.assertIn((0x8C, 0xD8, 0xB6, 255), colours)  # foreground BGRA
        self.assertIn((0x47, 0x67, 0x51, 255), colours)  # rear layer BGRA
        self.assertIn((0x18, 0x14, 0x10, 255), colours)

    def test_ico_directory_and_dib_frames_are_consistent_and_repeatable(self):
        source = ROOT / "assets/branding/overglaze.svg"
        content = make_icon(source, sizes=(16, 32))
        self.assertEqual(content, make_icon(source, sizes=(16, 32)))
        self.assertEqual(struct.unpack_from("<HHH", content), (0, 1, 2))
        previous_end = 6 + 2 * 16
        for index, size in enumerate((16, 32)):
            width, height, _, _, planes, bits, length, offset = struct.unpack_from("<BBBBHHII", content, 6 + index * 16)
            self.assertEqual((width, height, planes, bits), (size, size, 1, 32))
            self.assertEqual(offset, previous_end)
            self.assertEqual(struct.unpack_from("<IiiHH", content, offset), (40, size, size * 2, 1, 32))
            self.assertEqual(length, 40 + size * size * 4 + ((size + 31) // 32) * 4 * size)
            previous_end = offset + length
        self.assertEqual(previous_end, len(content))

    def test_unsupported_svg_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "bad.svg"
            source.write_text('<svg viewBox="0 0 64 64"><path d="M0 0"/></svg>', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Unsupported SVG geometry"):
                make_icon(source, sizes=(16,))


if __name__ == "__main__":
    unittest.main()
