"""Builds res/icon.ico: a trend line on a dark rounded square, drawn at each size.

Run:  python res/make_icon.py
"""

import math
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent / "icon.ico"
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

BG = (33, 37, 43)        # background
RIM = (86, 182, 194)     # rim and baseline
BLUE = (86, 156, 214)    # trend stroke

# Geometry in a 256 unit box.
TREND = [(46, 160), (90, 106), (130, 142), (174, 70), (212, 104)]
TREND_SMALL = [(46, 158), (100, 100), (150, 138), (212, 72)]   # fewer bends below 32 px
BASE = ((40, 210), (216, 210))
DOTS = [84, 128, 172]


def rounded_box(px, py, cx, cy, hx, hy, r):
    qx = abs(px - cx) - hx + r
    qy = abs(py - cy) - hy + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def segment(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def cover(d, scale):
    """Pixel coverage at signed distance d from an edge."""
    return max(0.0, min(1.0, 0.5 - d * scale))


def over(dst, rgb, a):
    r, g, b, da = dst
    oa = a + da * (1 - a)
    if oa <= 0:
        return (0.0, 0.0, 0.0, 0.0)
    mix = lambda s, d: (s * a + d * da * (1 - a)) / oa
    return (mix(rgb[0], r), mix(rgb[1], g), mix(rgb[2], b), oa)


def render(size):
    scale = size / 256.0
    # Minimum stroke widths in output pixels.
    min_px = lambda units, px: max(units, px / scale)
    rim = min_px(7.0, 1.0)
    stroke = min_px(11.0, 0.9)
    base = min_px(4.0, 0.6)
    dot = min_px(9.0, 1.0)
    trend = TREND_SMALL if size < 32 else TREND
    radius = 40.0
    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            px = (x + 0.5) / scale
            py = (y + 0.5) / scale
            c = (0.0, 0.0, 0.0, 0.0)
            outer = rounded_box(px, py, 128, 128, 126, 126, radius)
            c = over(c, RIM, cover(outer, scale))
            inner = rounded_box(px, py, 128, 128, 126 - rim, 126 - rim, radius - rim)
            c = over(c, BG, cover(inner, scale))
            if size >= 24:
                d = segment(px, py, *BASE) - base
                for dx in DOTS:
                    d = min(d, math.hypot(px - dx, py - BASE[0][1]) - dot)
                c = over(c, RIM, cover(d, scale))
            d = min(segment(px, py, trend[i], trend[i + 1]) for i in range(len(trend) - 1))
            c = over(c, BLUE, cover(d - stroke, scale))
            row.append(c)
        pixels.append(row)
    return pixels


def png_bytes(pixels):
    size = len(pixels)
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes((round(r), round(g), round(b), round(a * 255)))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def dib_bytes(pixels):
    size = len(pixels)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for row in reversed(pixels):                       # bottom-up
        for r, g, b, a in row:
            xor += bytes((round(b), round(g), round(r), round(a * 255)))
    mask_row = ((size + 31) // 32) * 4
    return header + bytes(xor) + bytes(mask_row * size)   # empty AND mask


def main():
    entries = []
    for s in SIZES:
        px = render(s)
        entries.append((s, png_bytes(px) if s >= 64 else dib_bytes(px)))
    out = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for s, data in entries:
        dim = 0 if s >= 256 else s
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data
    OUT.write_bytes(out)
    print(f"wrote {OUT} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
