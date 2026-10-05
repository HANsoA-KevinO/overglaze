# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
"""Render the original rect-only SVG into a reproducible Windows ICO.

Python standard library only. This intentionally supports the small SVG subset
used by our brand mark and fails on unrecognised geometry rather than silently
producing an incomplete icon. Every frame is an uncompressed 32-bit DIB, so
output bytes do not depend on graphics drivers, fonts, or an image encoder.
"""
import argparse
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

SIZES = (16, 20, 24, 32, 48, 64, 128, 256)
SAMPLES = 4


def color(value):
    if value == "none":
        return None
    if len(value) != 7 or not value.startswith("#"):
        raise ValueError("Only #RRGGBB SVG colours are supported")
    return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))


def read_shapes(path):
    root = ET.parse(path).getroot()
    if root.attrib.get("viewBox") != "0 0 64 64":
        raise ValueError("Brand SVG must use viewBox='0 0 64 64'")
    shapes = []
    for element in root:
        kind = element.tag.split("}")[-1]
        if kind in ("title", "desc"):
            continue
        if kind != "rect":
            raise ValueError(f"Unsupported SVG geometry: {kind}")
        a = element.attrib
        if set(a) - {"x", "y", "width", "height", "rx", "fill", "stroke", "stroke-width"}:
            raise ValueError("Unsupported rectangle attribute")
        geometry = tuple(float(a.get(k, "0")) for k in ("x", "y", "width", "height", "rx"))
        stroke = float(a.get("stroke-width", "0"))
        if not all(math.isfinite(v) for v in (*geometry, stroke)):
            raise ValueError("Non-finite SVG coordinates")
        x, y, width, height, radius = geometry
        if width <= 0 or height <= 0 or not 0 <= radius <= min(width, height) / 2 or stroke < 0:
            raise ValueError("Invalid rounded rectangle")
        shapes.append((geometry, color(a.get("fill", "none")), color(a.get("stroke", "none")), stroke))
    if not shapes:
        raise ValueError("Empty brand SVG")
    return shapes


def contains(px, py, geometry, expand=0):
    x, y, width, height, radius = geometry
    left, top = x - expand, y - expand
    width, height = width + 2 * expand, height + 2 * expand
    if width <= 0 or height <= 0:
        return False
    radius = max(0, min(radius + expand, width / 2, height / 2))
    if not left <= px <= left + width or not top <= py <= top + height:
        return False
    dx = max(left + radius - px, 0, px - (left + width - radius))
    dy = max(top + radius - py, 0, py - (top + height - radius))
    return dx * dx + dy * dy <= radius * radius


def rasterize(shapes, size):
    pixels = bytearray()
    step = 64 / (size * SAMPLES)
    count = SAMPLES * SAMPLES
    for y in range(size):
        for x in range(size):
            red = green = blue = covered = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    px, py = (x * SAMPLES + sx + .5) * step, (y * SAMPLES + sy + .5) * step
                    value = None
                    for geometry, fill, stroke, width in shapes:
                        if fill is not None and contains(px, py, geometry):
                            value = fill
                        if stroke is not None and contains(px, py, geometry, width / 2) and not contains(px, py, geometry, -width / 2):
                            value = stroke
                    if value is not None:
                        red += value[0]
                        green += value[1]
                        blue += value[2]
                        covered += 1
            if covered:
                pixels.extend((round(blue / covered), round(green / covered), round(red / covered), round(255 * covered / count)))
            else:
                pixels.extend((0, 0, 0, 0))
    return pixels


def dib(pixels, size):
    stride = size * 4
    bitmap = b"".join(pixels[row * stride:(row + 1) * stride] for row in reversed(range(size)))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytearray(mask_stride * size)
    for y in range(size):
        for x in range(size):
            if pixels[(y * size + x) * 4 + 3] == 0:
                mask[(size - 1 - y) * mask_stride + x // 8] |= 0x80 >> (x % 8)
    return struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(bitmap), 0, 0, 0, 0) + bitmap + mask


def make_icon(source, sizes=SIZES):
    shapes = read_shapes(source)
    payloads = [dib(rasterize(shapes, size), size) for size in sizes]
    offset = 6 + 16 * len(sizes)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    for size, payload in zip(sizes, payloads):
        directory.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(payload), offset))
        offset += len(payload)
    return bytes(directory) + b"".join(payloads)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error("Output must not overwrite the SVG source")
    content = make_icon(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != content:
        args.output.write_bytes(content)


if __name__ == "__main__":
    main()
