#!/usr/bin/env python3
"""Checks the frame produced by tools/make_test_elf.py's program."""
import sys


def main(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    pixels = parts[3]

    def px(x, y):
        i = (y * width + x) * 3
        return tuple(pixels[i:i + 3])

    ok = (width, height) == (640, 448)
    if not ok:
        print("unexpected size", width, height)
    if px(5, 5) != (16, 32, 96):
        print("background mismatch", px(5, 5)); ok = False
    r, g, b = px(320, 45)
    if not (r > 200 and g < 60 and b < 60):
        print("top vertex mismatch", (r, g, b)); ok = False
    r, g, b = px(90, 395)
    if not (g > 200 and r < 60 and b < 60):
        print("left vertex mismatch", (r, g, b)); ok = False
    r, g, b = px(550, 395)
    if not (b > 200 and r < 60 and g < 60):
        print("right vertex mismatch", (r, g, b)); ok = False
    print("frame check:", "passed" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
