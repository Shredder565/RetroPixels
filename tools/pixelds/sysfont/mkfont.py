#!/usr/bin/env python3
"""Renders the glyphs of PixelDS's 3DS system font (the font 3DS games
draw their text with) from DejaVu Sans, whose licence allows this (see
LICENSE-DejaVu.txt), into cores/pixelds/ctr/sysfont.bin. PixelDS turns
that into the 3DS's font format at start-up (cores/pixelds/ctr/font.go).

File: "PXFN", cell width, cell height, baseline, ascent, line feed (u8
each), reserved (u8), glyph count (u16); per glyph: code point (u16),
left bearing (s8), glyph width (u8), advance (u8), reserved (u8); then
each glyph's cell, rows top to bottom, 4-bit coverage, two pixels a
byte (the left one in the low bits).

usage: mkfont.py /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf out.bin
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

SIZE = 20
CELL_W, CELL_H = 24, 26


def codes():
    cs = list(range(0x20, 0x7f)) + list(range(0xa0, 0x180))
    cs += [0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026,
           0x20ac, 0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2605, 0x2606,
           0x2665, 0x266a, 0x25b2, 0x25bc, 0x25c0, 0x25b6, 0x3000]
    cs += list(range(0x391, 0x3aa)) + list(range(0x3b1, 0x3ca))  # Greek
    cs += list(range(0x410, 0x450))  # Cyrillic
    return cs


def main():
    font = ImageFont.truetype(sys.argv[1], SIZE)
    ascent, descent = font.getmetrics()
    baseline = ascent + 1
    line_feed = ascent + descent + 2
    out_glyphs, cells = [], []
    for c in codes():
        ch = chr(c)
        if c == 0x3000:
            ch = " "
        l, t, r, b = font.getbbox(ch, anchor="ls")
        adv = int(round(font.getlength(ch)))
        if c == 0x3000:
            adv = SIZE
        w = max(0, min(CELL_W, r - l))
        img = Image.new("L", (CELL_W, CELL_H), 0)
        ImageDraw.Draw(img).text((-l, baseline), ch, font=font, fill=255, anchor="ls")
        px = img.load()
        cell = bytearray()
        for y in range(CELL_H):
            for x in range(0, CELL_W, 2):
                a, b2 = px[x, y] >> 4, px[x + 1, y] >> 4
                cell.append(a | b2 << 4)
        out_glyphs.append((c, max(-128, min(127, l)), w, max(0, min(255, adv))))
        cells.append(bytes(cell))
    data = b"PXFN" + struct.pack("<6BH", CELL_W, CELL_H, baseline, ascent, line_feed, 0, len(out_glyphs))
    for c, l, w, adv in out_glyphs:
        data += struct.pack("<HbBBB", c, l, w, adv, 0)
    data += b"".join(cells)
    open(sys.argv[2], "wb").write(data)
    print(len(out_glyphs), "glyphs,", len(data), "bytes; ascent", ascent, "descent", descent)


if __name__ == "__main__":
    main()
