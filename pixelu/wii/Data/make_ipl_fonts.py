#!/usr/bin/env python3
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
"""Makes the two GameCube boot ROM fonts PixelU-Wii uses when there is no boot ROM dump:
Sys/GC/font_western.bin (Windows-1252) and Sys/GC/font_japanese.bin (Shift JIS).

GameCube games draw text with the fonts in the console's boot ROM. Without a dump of it,
Core/HW/EXI/EXI_DeviceIPL.cpp puts these files where the ROM keeps its fonts (the Shift JIS one
at 0x1AFF00, the Windows-1252 one at 0x1FCF00), so they must fit the space the games read there:
at most 0x4A24D and 0x2575 bytes.

  python3 pixelu/wii/Data/make_ipl_fonts.py WESTERN.ttf JAPANESE.ttf           write both
  python3 pixelu/wii/Data/make_ipl_fonts.py WESTERN.ttf JAPANESE.ttf --check   compare

The fonts shipped are drawn from Noto Sans Regular (Latin) and Noto Sans JP Regular (Japanese),
both under the SIL Open Font License 1.1 (Sys/GC/font-licenses.txt). The files used are the
TrueType copies in the npm packages @expo-google-fonts/noto-sans 0.4.2
(400Regular/NotoSans_400Regular.ttf) and @expo-google-fonts/noto-sans-jp 0.4.4
(400Regular/NotoSansJP_400Regular.ttf); FONT_SHA256 below names them exactly. Needs Pillow.

The font format (YAGCD, "IPL fonts"; big endian), after Yay0 decompression:

  0x00 u16 encoding (0 = Windows-1252, 2 = Shift JIS)   0x02 u16 first character code
  0x04 u16 last character code                         0x06 u16 code drawn for unknown ones
  0x08 u16 ascent    0x0A u16 descent    0x0C u16 widest character    0x0E u16 line spacing
  0x10 u16 cell width                    0x12 u16 cell height
  0x14 u32 size of one sheet as the games expand it (4 bits per pixel)
  0x18 u16 sheet texture format (0 = I4)
  0x1A u16 cells per sheet row           0x1C u16 cell rows per sheet
  0x1E u16 sheet width                   0x20 u16 sheet height
  0x22 u16 offset of the width table     0x24 u32 offset of the sheets
  0x28 u32 size of all sheets expanded   0x2C u8[4] grey level of each 2-bit pixel value
  width table: one advance width (pixels) per glyph
  sheets: 2 bits per pixel, most significant first, in 8 x 8 pixel tiles, tiles left to right
          and then top to bottom; glyph n is cell n % (columns * rows) of sheet
          n / (columns * rows), cells left to right and then top to bottom.

Glyph order. Windows-1252: one glyph per code from 0x20 to 0xFF (codes Windows-1252 leaves
undefined are blank). Shift JIS (how the games' font library numbers the glyphs):
  - the 524 JIS X 0208 non-kanji characters of rows 1 to 8 (codes 0x8140 to 0x84BE), in code
    order;
  - JIS X 0201 (single byte): 0x20 to 0x7E (with the yen sign at 0x5C and the overline at 0x7E)
    and the half-width katakana 0xA1 to 0xDF;
  - 31 glyphs for kana the single-byte set lacks: wi, we, small wa, small ka, small ke, vu and
    the voiced katakana ga ... po (shown here as their full-width JIS X 0208 forms);
  - the 83 NEC special characters of row 13 (codes 0x8740 to 0x879C), in code order;
  - the 2965 level 1 kanji (JIS X 0208 rows 16 to 47, codes 0x889F to 0x9872), in code order.
  Then the width table is padded to a multiple of four entries.
"""

import hashlib
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
GC_DIR = os.path.join(HERE, "Sys", "GC")

FONT_SHA256 = {
    "western": "fe8c022f48d8dd29f17b744d16f9346f4357e16f7d4f7be58b000ae7c291b614",
    "japanese": "6255ea4692e97ef27330304fb96fd650f9111f868cd585fcf2802d42823930e5",
}

CELL = 24
COLUMNS = 21
ROWS = 21
SHEET = 512
GLYPHS_PER_SHEET = COLUMNS * ROWS
SHEET_EXPANDED = SHEET * SHEET // 2  # as I4
RENDER_SIZE = 20  # pixels per em
BASELINE = 19  # from the top of the cell
ASCENT, DESCENT, LINE_SPACING = 24, 0, 28
MAX_BYTES = {"western": 0x2575, "japanese": 0x4A24D}


# ---------------------------------------------------------------- character tables


def sjis_char(code):
    try:
        return bytes([code >> 8, code & 0xFF]).decode("cp932")
    except UnicodeDecodeError:
        return None


def is_jis_x0208(code):
    try:
        bytes([code >> 8, code & 0xFF]).decode("shift_jis")
        return True
    except UnicodeDecodeError:
        return False


def sjis_codes(first, last):
    """Every two-byte Shift JIS code position from first to last, in order."""
    for lead in range(first >> 8, (last >> 8) + 1):
        for trail in range(0x40, 0xFD):
            code = (lead << 8) | trail
            if trail == 0x7F or code < first or code > last:
                continue
            yield code


def western_table():
    out = []
    for code in range(0x20, 0x100):
        try:
            out.append(bytes([code]).decode("cp1252"))
        except UnicodeDecodeError:
            out.append(None)
    return out


def japanese_table():
    out = []
    # Rows 1 to 8: the JIS X 0208 symbols, digits, Latin, kana, Greek, Cyrillic and box parts.
    # (cp932 gives the full-width forms that the font draws for the few codes where it and
    # strict Shift JIS differ.)
    out += [sjis_char(c) for c in sjis_codes(0x8140, 0x84BE) if is_jis_x0208(c)]
    assert len(out) == 524
    # JIS X 0201.
    out += [chr(c) for c in range(0x20, 0x7E)]
    out[524 + 0x5C - 0x20] = "¥"
    out.append("‾")
    out += [chr(c) for c in range(0xFF61, 0xFFA0)]
    # Kana the single-byte set has no codes for.
    out += list("ヰヱヮヵヶヴ" "ガギグゲゴ" "ザジズゼゾ" "ダヂヅデド" "バパビピブプベペボポ")
    # NEC row 13.
    nec = [sjis_char(c) for c in sjis_codes(0x8740, 0x879C)]
    out += [ch for ch in nec if ch]
    assert len(out) == 524 + 95 + 63 + 31 + 83 == 796
    # Level 1 kanji.
    kanji = [sjis_char(c) for c in sjis_codes(0x889F, 0x9872)]
    assert len(kanji) == 2965 and all(kanji)
    return out + kanji


# ---------------------------------------------------------------- drawing


def draw_text(font, ch):
    im = Image.new("L", (CELL, CELL), 0)
    ImageDraw.Draw(im).text((0, BASELINE), ch, fill=255, font=font, anchor="ls")
    return im


def substitute_overline(font):
    """U+203E (JIS X 0201's 0x7E), drawn as a bar at the top of the cell."""
    im = Image.new("L", (CELL, CELL), 0)
    width = int(round(font.getlength("-")))
    ImageDraw.Draw(im).rectangle((1, 3, max(width, 8), 4), fill=255)
    return im, max(width, 8) + 2


def substitute_nearly_equal(font):
    """U+2252 (nearly equal to): an equals sign with a dot above its left end and one below its
    right end."""
    im = draw_text(font, "＝")
    box = im.getbbox() or (4, 8, 18, 14)
    d = ImageDraw.Draw(im)
    d.ellipse((box[0], box[1] - 4, box[0] + 2, box[1] - 2), fill=255)
    d.ellipse((box[2] - 3, box[3] + 1, box[2] - 1, box[3] + 3), fill=255)
    return im, int(round(font.getlength("＝")))


SUBSTITUTES = {"‾": substitute_overline, "≒": substitute_nearly_equal}


def glyph_cell(font, ch):
    """The glyph drawn in a CELL x CELL greyscale image with its pen at the left edge, and its
    advance width. Control codes are blank; a character the font lacks is drawn by hand when
    there is a substitute for it, else left blank."""
    if ch is None or ord(ch) < 0x20 or 0x7F <= ord(ch) < 0xA0:
        return Image.new("L", (CELL, CELL), 0), 0
    im = draw_text(font, ch)
    advance = int(round(font.getlength(ch)))
    if im.tobytes() == draw_text(font, "\U0010fffd").tobytes():  # the font's "missing" box
        if ch in SUBSTITUTES:
            im, advance = SUBSTITUTES[ch](font)
        else:
            print("warning: U+%04X is not in the font; left blank" % ord(ch))
            im = Image.new("L", (CELL, CELL), 0)
    return im, max(0, min(CELL, advance))


def to_2bit(value):
    return (value * 3 + 127) // 255


def encode_sheet(sheet):
    """A SHEET x SHEET greyscale image as 2-bit pixels in 8 x 8 tiles."""
    px = sheet.load()
    out = bytearray()
    for ty in range(0, SHEET, 8):
        for tx in range(0, SHEET, 8):
            for y in range(ty, ty + 8):
                for x0 in (tx, tx + 4):
                    b = 0
                    for x in range(x0, x0 + 4):
                        b = (b << 2) | to_2bit(px[x, y])
                    out.append(b)
    return bytes(out)


def build_font(kind, font_path):
    font = ImageFont.truetype(font_path, RENDER_SIZE)
    if kind == "western":
        table, encoding, first, last = western_table(), 0, 0x20, 0xFF
    else:
        table, encoding, first, last = japanese_table(), 2, 0x8140, 0x9872
    space_width = int(round(font.getlength(" ")))

    widths = bytearray()
    sheets = []
    for index, ch in enumerate(table):
        if index % GLYPHS_PER_SHEET == 0:
            sheets.append(Image.new("L", (SHEET, SHEET), 0))
        cell, advance = glyph_cell(font, ch)
        if advance == 0:
            # Undefined and control codes advance like a space.
            advance = space_width
        slot = index % GLYPHS_PER_SHEET
        sheets[-1].paste(cell, ((slot % COLUMNS) * CELL, (slot // COLUMNS) * CELL))
        widths.append(advance)
    # The sheets start on a 4-byte boundary.
    while len(widths) % 4:
        widths.append(0)

    width_offset = 0x30
    sheet_offset = width_offset + len(widths)
    header = struct.pack(
        ">HHHHHHHHHHIHHHHHHII4B",
        encoding, first, last, 0x20,
        ASCENT, DESCENT, CELL, LINE_SPACING,
        CELL, CELL,
        SHEET_EXPANDED, 0, COLUMNS, ROWS, SHEET, SHEET,
        width_offset, sheet_offset, SHEET_EXPANDED * len(sheets),
        0x00, 0x55, 0xAA, 0xFF)
    assert len(header) == width_offset
    raw = header + bytes(widths) + b"".join(encode_sheet(s) for s in sheets)
    return raw, sheets


# ---------------------------------------------------------------- Yay0


def yay0_compress(data):
    """Yay0: a 32-bit mask word for every 32 steps (1 = copy one byte from the chunk table,
    0 = a 16-bit back reference from the link table: 4 bits of length - 2, or 0 for a length
    byte + 18 in the chunk table, then 12 bits of distance - 1)."""
    n = len(data)
    head = {}
    prev = [-1] * n
    masks, links, chunks = [], bytearray(), bytearray()
    mask = 0
    bits = 0
    max_len = 255 + 18  # the longest a length byte can say
    max_chain = 128

    def insert(pos):
        if pos + 3 <= n:
            key = data[pos:pos + 3]
            prev[pos] = head.get(key, -1)
            head[key] = pos

    def longest(pos):
        best_len, best_dist = 0, 0
        if pos + 3 > n:
            return 0, 0
        cand = head.get(data[pos:pos + 3], -1)
        chain = 0
        limit = min(max_len, n - pos)
        while cand >= 0 and pos - cand <= 0x1000 and chain < max_chain:
            length = 3
            while length < limit and data[cand + length] == data[pos + length]:
                length += 1
            if length > best_len:
                best_len, best_dist = length, pos - cand
                if length == limit:
                    break
            cand = prev[cand]
            chain += 1
        return best_len, best_dist

    def emit(is_literal):
        nonlocal mask, bits
        mask = (mask << 1) | (1 if is_literal else 0)
        bits += 1
        if bits == 32:
            masks.append(mask)
            mask, bits = 0, 0

    pos = 0
    while pos < n:
        length, dist = longest(pos)
        if length >= 3:
            # Lazy step: a longer match one byte later wins over this one.
            insert(pos)
            next_len, _ = longest(pos + 1) if pos + 1 < n else (0, 0)
            if next_len > length + 1:
                emit(True)
                chunks.append(data[pos])
                pos += 1
                continue
            emit(False)
            if length <= 17:
                links += struct.pack(">H", ((length - 2) << 12) | (dist - 1))
            else:
                links += struct.pack(">H", dist - 1)
                chunks.append(length - 18)
            for p in range(pos + 1, pos + length):
                insert(p)
            pos += length
        else:
            insert(pos)
            emit(True)
            chunks.append(data[pos])
            pos += 1
    if bits:
        masks.append(mask << (32 - bits))

    def pad4(b):
        return b + bytes(-len(b) % 4)

    mask_bytes = b"".join(struct.pack(">I", m) for m in masks)
    link_offset = 16 + len(mask_bytes)
    links = pad4(bytes(links))
    chunk_offset = link_offset + len(links)
    return b"Yay0" + struct.pack(">III", n, link_offset, chunk_offset) + mask_bytes + links + \
        pad4(bytes(chunks))


def yay0_decompress(src):
    size, link, chunk = struct.unpack(">III", src[4:16])
    out = bytearray()
    mp, mask, bits = 16, 0, 0
    while len(out) < size:
        if bits == 0:
            mask = struct.unpack(">I", src[mp:mp + 4])[0]
            mp += 4
            bits = 32
        if mask & 0x80000000:
            out.append(src[chunk])
            chunk += 1
        else:
            v = struct.unpack(">H", src[link:link + 2])[0]
            link += 2
            dist = (v & 0xFFF) + 1
            length = v >> 12
            if length == 0:
                length = src[chunk] + 18
                chunk += 1
            else:
                length += 2
            for _ in range(length):
                out.append(out[-dist])
        mask = (mask << 1) & 0xFFFFFFFF
        bits -= 1
    return bytes(out)


# ---------------------------------------------------------------- main


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv[1:]
    if len(args) != 2:
        print(__doc__)
        return 2
    sources = {"western": args[0], "japanese": args[1]}
    for kind, path in sources.items():
        if sha256(path) != FONT_SHA256[kind]:
            print("warning: %s is not the font the shipped %s font was made from" % (path, kind))

    bad = 0
    for kind, name in (("western", "font_western.bin"), ("japanese", "font_japanese.bin")):
        raw, _ = build_font(kind, sources[kind])
        packed = yay0_compress(raw)
        assert yay0_decompress(packed) == raw
        if len(packed) > MAX_BYTES[kind]:
            print("%s: %d bytes, more than the %d the boot ROM has room for" % (name, len(packed),
                                                                              MAX_BYTES[kind]))
            return 1
        path = os.path.join(GC_DIR, name)
        if check:
            try:
                with open(path, "rb") as f:
                    same = f.read() == packed
            except OSError:
                same = False
            print("%s: %s" % (name, "same" if same else "DIFFERS"))
            bad += 0 if same else 1
        else:
            os.makedirs(GC_DIR, exist_ok=True)
            with open(path, "wb") as f:
                f.write(packed)
            print("%s: %d glyphs, %d bytes (%d unpacked, room for %d)" % (
                name, len(western_table() if kind == "western" else japanese_table()),
                len(packed), len(raw), MAX_BYTES[kind]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
