#!/usr/bin/env python3
"""Generates the LiveArea images (8-bit palette PNGs, as the Vita requires)
using only the Python standard library.

usage: make_livearea.py <sce_sys directory>
"""
import math
import os
import struct
import sys
import zlib

# Palette: 0-127 background gradient, 128 white, 129-132 face button colors
PALETTE = []
for i in range(128):
    t = i / 127.0
    PALETTE.append((int(10 + 20 * t), int(14 + 40 * t), int(40 + 110 * t)))
PALETTE.append((240, 240, 245))   # 128 white
PALETTE.append((64, 226, 160))    # 129 triangle (green)
PALETTE.append((255, 102, 102))   # 130 circle (red)
PALETTE.append((124, 178, 232))   # 131 cross (blue)
PALETTE.append((255, 105, 248))   # 132 square (pink)
while len(PALETTE) < 256:
    PALETTE.append((0, 0, 0))


def write_png(path, width, height, pixels):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + bytes(pixels[y * width:(y + 1) * width]) for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0))
    png += chunk(b"PLTE", b"".join(struct.pack("BBB", *c) for c in PALETTE))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def render(width, height, symbol_size):
    pixels = [0] * (width * height)
    for y in range(height):
        for x in range(width):
            t = (x / max(width - 1, 1) * 0.4 + y / max(height - 1, 1) * 0.6)
            pixels[y * width + x] = int(127 * (1.0 - t))

    cx, cy = width / 2.0, height / 2.0
    r = symbol_size
    gap = symbol_size * 1.35
    centers = [(cx, cy - gap, 129), (cx + gap, cy, 130), (cx, cy + gap, 131), (cx - gap, cy, 132)]
    thickness = max(2.0, symbol_size * 0.22)

    def inside(px, py, kind, ox, oy):
        dx, dy = px - ox, py - oy
        if kind == 129:  # triangle outline
            def edge_dist(ax, ay, bx, by):
                ex, ey = bx - ax, by - ay
                return ((px - ax) * ey - (py - ay) * ex) / math.hypot(ex, ey)
            a = (ox, oy - r)
            b = (ox + r * 0.95, oy + r * 0.65)
            c = (ox - r * 0.95, oy + r * 0.65)
            d = [edge_dist(*a, *b), edge_dist(*b, *c), edge_dist(*c, *a)]
            return all(v <= 0 for v in d) and max(d) > -thickness
        if kind == 130:  # circle outline
            dist = math.hypot(dx, dy)
            return r - thickness <= dist <= r
        if kind == 131:  # cross
            return (abs(dx) <= r * 0.8 and abs(dy) <= r * 0.8 and
                    (abs(dx - dy) <= thickness * 0.75 or abs(dx + dy) <= thickness * 0.75))
        if kind == 132:  # square outline
            m = max(abs(dx), abs(dy))
            return r * 0.8 - thickness <= m <= r * 0.8
        return False

    for ox, oy, kind in centers:
        for y in range(int(oy - r - 2), int(oy + r + 3)):
            for x in range(int(ox - r - 2), int(ox + r + 3)):
                if 0 <= x < width and 0 <= y < height and inside(x + 0.5, y + 0.5, kind, ox, oy):
                    pixels[y * width + x] = kind
    return pixels


def main(out_dir):
    os.makedirs(os.path.join(out_dir, "livearea", "contents"), exist_ok=True)
    write_png(os.path.join(out_dir, "icon0.png"), 128, 128, render(128, 128, 18))
    write_png(os.path.join(out_dir, "livearea", "contents", "bg.png"), 840, 500, render(840, 500, 60))
    write_png(os.path.join(out_dir, "livearea", "contents", "startup.png"), 280, 158, render(280, 158, 20))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "sce_sys")
