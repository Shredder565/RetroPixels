#!/usr/bin/env python3
"""Draw RetroPixels' own crosshair pictures for the Model 3 engine.

Usage (from the repo root):
  python3 tools/supermodel-crosshairs.py      writes supermodel/Assets/p1crosshair.bmp and p2crosshair.bmp

The engine loads these with CrosshairStyle=bmp (OSD/SDL/Crosshair.cpp):
32-bit BMPs whose fourth byte is the alpha. Each is a ring with four ticks
pointing at the centre and a dot in the middle, white with a coloured
outline (player 1 orange, player 2 cyan), anti-aliased by supersampling.
"""
import math
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "supermodel", "Assets")
SIZE = 128
SAMPLES = 4  # per axis, per pixel


def coverage(x, y, grow):
    """1 where the shape (widened by grow, in pixels) covers point x, y."""
    c = SIZE / 2.0
    dx, dy = x - c, y - c
    r = math.hypot(dx, dy)
    ring_r, ring_w = SIZE * 0.36, 3.0
    if abs(r - ring_r) <= ring_w / 2 + grow:
        return True
    if r <= 3.0 + grow:  # the centre dot
        return True
    # ticks: along each axis, from inside the ring towards the centre
    tick_w, inner, outer = 3.0, SIZE * 0.12, SIZE * 0.46
    for a, b in ((abs(dx), abs(dy)), (abs(dy), abs(dx))):
        if a <= tick_w / 2 + grow and inner - grow <= b <= outer + grow:
            return True
    return False


def draw(colour):
    rows = []
    for py in range(SIZE):
        row = bytearray()
        for px in range(SIZE):
            fill = edge = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    x = px + (sx + 0.5) / SAMPLES
                    y = py + (sy + 0.5) / SAMPLES
                    if coverage(x, y, 0):
                        fill += 1
                    elif coverage(x, y, 1.5):
                        edge += 1
            n = SAMPLES * SAMPLES
            a = (fill + edge) / n
            if a == 0:
                row += b"\0\0\0\0"
                continue
            w = fill / (fill + edge)  # white inside, colour at the edge
            r, g, b = (round(255 * w + c * (1 - w)) for c in colour)
            row += bytes((b, g, r, round(255 * a)))
        rows.append(bytes(row))
    pixels = b"".join(reversed(rows))  # BMP rows go bottom up
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, SIZE, SIZE, 1, 32, 0, len(pixels), 2835, 2835, 0, 0)
    return header + info + pixels


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, colour in (("p1crosshair.bmp", (255, 120, 0)), ("p2crosshair.bmp", (0, 200, 255))):
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(draw(colour))
        print("wrote", os.path.join("supermodel", "Assets", name))


if __name__ == "__main__":
    main()
