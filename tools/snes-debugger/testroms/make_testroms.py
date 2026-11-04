#!/usr/bin/env python3
"""Makes the small SNES test programs the RetroPixels SNES Debugger is tested
with (written for RetroPixels, no game code). Each shows one Mode 1 background
on BG1, loaded in a different way, so the debugger's analysis and swap can be
checked from end to end:

  bgtest-direct.sfc  DMA from ROM, addresses as LDA #immediate before STA $43x2
  bgtest-table.sfc   DMA from ROM, addresses read from a pointer table
  bgtest-wram.sfc    graphics copied ROM -> WRAM by DMA ($2180), then WRAM -> VRAM

usage: make_testroms.py <out dir>
"""
import os
import sys

ROM_SIZE = 0x10000  # 64 KB LoROM: banks $00-$01


class Asm:
    def __init__(self, org):
        self.org = org
        self.b = bytearray()
        self.m16 = False
        self.x16 = False
        self.labels = {}
        self.fix = []

    def pc(self):
        return self.org + len(self.b)

    def emit(self, *bs):
        self.b.extend(bs)

    def rep(self, v):
        self.emit(0xC2, v)
        if v & 0x20: self.m16 = True
        if v & 0x10: self.x16 = True

    def sep(self, v):
        self.emit(0xE2, v)
        if v & 0x20: self.m16 = False
        if v & 0x10: self.x16 = False

    def lda_i(self, v):
        self.emit(0xA9, v & 0xFF, *(((v >> 8) & 0xFF,) if self.m16 else ()))

    def ldx_i(self, v):
        self.emit(0xA2, v & 0xFF, *(((v >> 8) & 0xFF,) if self.x16 else ()))

    def cpx_i(self, v):
        self.emit(0xE0, v & 0xFF, *(((v >> 8) & 0xFF,) if self.x16 else ()))

    def sta(self, a):
        self.emit(0x8D, a & 0xFF, a >> 8)

    def stz(self, a):
        self.emit(0x9C, a & 0xFF, a >> 8)

    def lda_absx(self, a):
        self.emit(0xBD, a & 0xFF, a >> 8)

    def lda_longx(self, a):
        self.emit(0xBF, a & 0xFF, (a >> 8) & 0xFF, a >> 16)

    def sta_longx(self, a):
        self.emit(0x9F, a & 0xFF, (a >> 8) & 0xFF, a >> 16)

    def lda_long(self, a):
        self.emit(0xAF, a & 0xFF, (a >> 8) & 0xFF, a >> 16)

    def label(self, n):
        self.labels[n] = self.pc()

    def bne(self, n):
        self.emit(0xD0, 0)
        self.fix.append((len(self.b) - 1, n))

    def bra(self, n):
        self.emit(0x80, 0)
        self.fix.append((len(self.b) - 1, n))

    def done(self):
        for at, n in self.fix:
            rel = self.labels[n] - (self.org + at + 1)
            assert -128 <= rel < 128
            self.b[at] = rel & 0xFF
        return bytes(self.b)


def lorom_off(addr):
    return ((addr >> 16) & 0x7F) * 0x8000 + (addr & 0x7FFF)


def picture():
    """A 256x256 picture made of a few repeating 8x8 tiles, two palettes."""
    pal0 = [(0, 0, 0), (31, 0, 0), (31, 16, 0), (31, 31, 0), (0, 31, 0), (0, 31, 31), (0, 0, 31), (16, 0, 31),
            (31, 31, 31), (20, 20, 20), (10, 10, 10), (31, 20, 20), (20, 31, 20), (20, 20, 31), (5, 5, 5), (25, 10, 5)]
    pal1 = [(0, 0, 0)] + [(i * 2, 31 - i * 2, 16) for i in range(15)]
    tiles, tmap = [], []
    keys = {}
    for ty in range(32):
        for tx in range(32):
            kind = ((tx // 4) + (ty // 4)) % 6
            pal = 1 if kind >= 4 else 0
            px = []
            for y in range(8):
                for x in range(8):
                    if kind == 0: c = 1 + ((x + y) % 7)
                    elif kind == 1: c = 8 if (x // 2 + y // 2) % 2 else 9
                    elif kind == 2: c = 1 + (y % 15)
                    elif kind == 3: c = 0 if (x - 3.5) ** 2 + (y - 3.5) ** 2 < 9 else 10 + (x % 5)
                    elif kind == 4: c = 1 + ((x * y) % 15)
                    else: c = 1 + ((tx + x) % 15)
                    px.append(c)
            k = (tuple(px))
            if k not in keys:
                keys[k] = len(tiles)
                tiles.append(px)
            tmap.append(keys[k] | (pal << 10))
    return tiles, tmap, [pal0, pal1]


def enc4bpp(px):
    out = bytearray(32)
    for y in range(8):
        for x in range(8):
            c = px[y * 8 + x]
            bit = 7 - x
            for p in range(4):
                if c >> p & 1:
                    out[(p >> 1) * 16 + y * 2 + (p & 1)] |= 1 << bit
    return bytes(out)


def make(kind):
    rom = bytearray([0xFF]) * ROM_SIZE
    tiles, tmap, pals = picture()
    tile_bytes = b"".join(enc4bpp(t) for t in tiles)
    map_bytes = b"".join(bytes([e & 0xFF, e >> 8]) for e in tmap)
    pal_bytes = b"".join(bytes([(r | g << 5 | b << 10) & 0xFF, (r | g << 5 | b << 10) >> 8]) for p in pals for (r, g, b) in p)
    TILES, MAP, PAL, TABLE = 0x018000, 0x01C000, 0x00F000, 0x00E800
    for addr, data in ((TILES, tile_bytes), (MAP, map_bytes), (PAL, pal_bytes)):
        o = lorom_off(addr)
        rom[o:o + len(data)] = data
    # pointer table: tiles (addr24, size16), tilemap (addr24, size16)
    t = bytes([TILES & 0xFF, TILES >> 8 & 0xFF, TILES >> 16, len(tile_bytes) & 0xFF, len(tile_bytes) >> 8,
               MAP & 0xFF, MAP >> 8 & 0xFF, MAP >> 16, len(map_bytes) & 0xFF, len(map_bytes) >> 8])
    if kind == "table":
        rom[lorom_off(TABLE):lorom_off(TABLE) + len(t)] = t

    a = Asm(0x8000)
    a.emit(0x78, 0x18, 0xFB)  # SEI, CLC, XCE: native mode
    a.sep(0x30)
    a.lda_i(0x80); a.sta(0x2100)  # forced blank
    a.lda_i(0x01); a.sta(0x2105)  # mode 1
    a.lda_i(0x10); a.sta(0x2107)  # BG1 tilemap at word $1000, 32x32
    a.stz(0x210B)                 # BG1 tiles at word $0000
    a.stz(0x210D); a.stz(0x210D); a.stz(0x210E); a.stz(0x210E)

    def dma_vram(vaddr, src, size, from_table=None):
        a.sep(0x20)
        a.lda_i(0x80); a.sta(0x2115)
        a.rep(0x20)
        a.lda_i(vaddr); a.sta(0x2116)
        a.sep(0x20)
        a.lda_i(0x01); a.sta(0x4300)
        a.lda_i(0x18); a.sta(0x4301)
        if from_table is None:
            a.rep(0x20)
            a.lda_i(src & 0xFFFF); a.sta(0x4302)
            a.sep(0x20)
            a.lda_i(src >> 16); a.sta(0x4304)
            a.rep(0x20)
            a.lda_i(size); a.sta(0x4305)
        else:
            a.rep(0x20)
            a.lda_absx(from_table); a.sta(0x4302)
            a.sep(0x20)
            a.lda_absx(from_table + 2); a.sta(0x4304)
            a.rep(0x20)
            a.lda_absx(from_table + 3); a.sta(0x4305)
        a.sep(0x20)
        a.lda_i(0x01); a.sta(0x420B)

    if kind == "direct":
        dma_vram(0x0000, TILES, len(tile_bytes))
        dma_vram(0x1000, MAP, len(map_bytes))
    elif kind == "table":
        a.sep(0x10); a.ldx_i(0)
        dma_vram(0x0000, 0, 0, from_table=TABLE & 0xFFFF)
        a.ldx_i(5)
        dma_vram(0x1000, 0, 0, from_table=TABLE & 0xFFFF)
    else:  # wram: ROM -> $7F:0000 through $2180, then WRAM -> VRAM
        for src, size, wram in ((TILES, len(tile_bytes), 0x7F0000), (MAP, len(map_bytes), 0x7F4000)):
            a.rep(0x20)
            a.lda_i(wram & 0xFFFF); a.sta(0x2181)
            a.sep(0x20)
            a.lda_i(1); a.sta(0x2183)
            a.stz(0x4300)
            a.lda_i(0x80); a.sta(0x4301)
            a.rep(0x20)
            a.lda_i(src & 0xFFFF); a.sta(0x4302)
            a.sep(0x20)
            a.lda_i(src >> 16); a.sta(0x4304)
            a.rep(0x20)
            a.lda_i(size); a.sta(0x4305)
            a.sep(0x20)
            a.lda_i(1); a.sta(0x420B)
        dma_vram(0x0000, 0x7F0000, len(tile_bytes))
        dma_vram(0x1000, 0x7F4000, len(map_bytes))
    # palette: 32 colours to CGRAM 0
    a.sep(0x20)
    a.stz(0x2121)
    a.stz(0x4300)
    a.lda_i(0x22); a.sta(0x4301)
    a.rep(0x20)
    a.lda_i(PAL & 0xFFFF); a.sta(0x4302)
    a.sep(0x20)
    a.lda_i(PAL >> 16); a.sta(0x4304)
    a.rep(0x20)
    a.lda_i(len(pal_bytes)); a.sta(0x4305)
    a.sep(0x20)
    a.lda_i(1); a.sta(0x420B)
    a.lda_i(0x01); a.sta(0x212C)  # BG1 on the main screen
    a.lda_i(0x0F); a.sta(0x2100)  # screen on, full brightness
    a.label("forever")
    a.bra("forever")
    code = a.done()
    rom[0:len(code)] = code

    # header (LoROM)
    h = 0x7FC0
    title = ("RP BGTEST " + kind.upper()).ljust(21)[:21].encode()
    rom[h:h + 21] = title
    rom[h + 0x15] = 0x20
    rom[h + 0x16] = 0x00
    rom[h + 0x17] = 0x06  # 64 KB
    rom[h + 0x18] = 0x00
    rom[h + 0x19] = 0x01
    rom[h + 0x1A] = 0x00
    rom[h + 0x1B] = 0x00
    for v in range(0x7FE0, 0x8000, 2):
        rom[v] = 0x00; rom[v + 1] = 0x80
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80
    # checksum
    rom[h + 0x1C:h + 0x20] = bytes([0xFF, 0xFF, 0, 0])
    s = sum(rom) & 0xFFFF
    rom[h + 0x1C] = (s ^ 0xFFFF) & 0xFF; rom[h + 0x1D] = (s ^ 0xFFFF) >> 8
    rom[h + 0x1E] = s & 0xFF; rom[h + 0x1F] = s >> 8
    return bytes(rom)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    for k in ("direct", "table", "wram"):
        with open(os.path.join(out, f"bgtest-{k}.sfc"), "wb") as f:
            f.write(make(k))
    print("ok")
