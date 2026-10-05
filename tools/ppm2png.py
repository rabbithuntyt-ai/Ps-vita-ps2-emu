#!/usr/bin/env python3
"""Converts a binary PPM (P6) to PNG using only the standard library."""
import struct
import sys
import zlib


def main(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    raw = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(dst, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
