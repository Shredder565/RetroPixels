#!/usr/bin/env python3
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
"""Writes a test DOL for PixelU-Wii's video backends (L5b): a GameCube program, no Nintendo
code, that sets up the VI for 640x480 progressive with an XFB at 0x00400000, links the CPU
and GP FIFOs, and every frame clears the EFB to a changing colour and copies it to the XFB
through the GX FIFO (BP register writes in the write-gather pipe). Optionally (--triangle)
it also draws a vertex-coloured triangle first. Its placement is off (it shows as a small
piece in the bottom-right corner, the same with every backend including the software
renderer); it is there to run the vertex loader and the generated shaders, not to look right.

    python3 docs/l5b-test-dol.py out.dol [--triangle]

The screen (and every frame dump) is then green 0xC0 / blue 0x40 with a red level that
changes each frame, so "not black" and "the same in every backend" are easy to check.
"""
import struct
import sys

TEXT_ADDR = 0x80003100
XFB = 0x00400000
FIFO_BASE = 0x00200000
FIFO_SIZE = 0x10000

code = []
labels = {}
fixups = []


def emit(word):
    code.append(word & 0xFFFFFFFF)


def here():
    return len(code)


def lis(rd, imm):
    emit((15 << 26) | (rd << 21) | (imm & 0xFFFF))


def addi(rd, ra, imm):
    emit((14 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF))


def li(rd, imm):
    addi(rd, 0, imm)


def ori(ra, rs, imm):
    emit((24 << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF))


def oris(ra, rs, imm):
    emit((25 << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF))


def andi_(ra, rs, imm):
    emit((28 << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF))


def load32(rd, value):
    lis(rd, (value >> 16) & 0xFFFF)
    ori(rd, rd, value & 0xFFFF)


def stw(rs, d, ra):
    emit((36 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF))


def sth(rs, d, ra):
    emit((44 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF))


def stb(rs, d, ra):
    emit((38 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF))


def lhz(rd, d, ra):
    emit((40 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF))


def spr_field(spr):
    return ((spr & 0x1F) << 5) | (spr >> 5)


def mtspr(spr, rs):
    emit((31 << 26) | (rs << 21) | (spr_field(spr) << 11) | (467 << 1))


def mfspr(rd, spr):
    emit((31 << 26) | (rd << 21) | (spr_field(spr) << 11) | (339 << 1))


def rlwinm(ra, rs, sh, mb, me):
    emit((21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1))


def slwi(ra, rs, n):
    rlwinm(ra, rs, n, 0, 31 - n)


def or_(ra, rs, rb):
    emit((31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (444 << 1))


def cmpwi(ra, imm):
    emit((11 << 26) | (ra << 16) | (imm & 0xFFFF))


def isync():
    emit(0x4C00012C)


def sync():
    emit(0x7C0004AC)


def label(name):
    labels[name] = here()


def branch(kind, name):
    fixups.append((here(), kind, name))
    emit(0)


def b(name):
    branch("b", name)


def bne(name):
    branch("bne", name)


def beq(name):
    branch("beq", name)


# Registers: r31 gather pipe, r30 CP base, r29 PI base, r28 VI base, r27 frame counter.
GP = 31


def gp_u8(value, tmp=3):
    li(tmp, value)
    stb(tmp, 0, GP)


def gp_u16(value, tmp=3):
    li(tmp, value & 0x7FFF) if value < 0x8000 else load32(tmp, value)
    sth(tmp, 0, GP)


def gp_u32(value, tmp=3):
    load32(tmp, value)
    stw(tmp, 0, GP)


def bp(reg, value):
    gp_u8(0x61)
    gp_u32((reg << 24) | (value & 0xFFFFFF))


def cp(reg, value):
    gp_u8(0x08)
    gp_u8(reg)
    gp_u32(value)


def xf(address, values):
    gp_u8(0x10)
    gp_u32(((len(values) - 1) << 16) | address)
    for v in values:
        gp_u32(v)


def f32(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def pad_pipe():
    # 32 GX NOPs push whatever is left in the gather pipe out to the FIFO.
    li(3, 0)
    for _ in range(8):
        stw(3, 0, GP)


triangle = "--triangle" in sys.argv

# --- Setup -------------------------------------------------------------------------------
# HID2: write-gather pipe enable (and paired singles / locked cache bits left as they are).
mfspr(3, 920)
oris(3, 3, 0x4000)
mtspr(920, 3)
load32(3, 0x0C008000)
mtspr(921, 3)  # WPAR
isync()

lis(GP, 0xCC00)
ori(GP, GP, 0x8000)
lis(30, 0xCC00)            # CP at 0xCC000000
load32(29, 0xCC003000)     # PI
load32(28, 0xCC002000)     # VI

# CP: FIFO off, then base/end/watermarks/pointers, then read + link enabled.
li(3, 0)
sth(3, 0x02, 30)
def cp_reg32(lo_offset, value):
    li(3, value & 0xFFFF) if (value & 0xFFFF) < 0x8000 else load32(3, value & 0xFFFF)
    sth(3, lo_offset, 30)
    li(3, (value >> 16) & 0xFFFF)
    sth(3, lo_offset + 2, 30)

fifo_end = FIFO_BASE + FIFO_SIZE - 32
cp_reg32(0x20, FIFO_BASE)
cp_reg32(0x24, fifo_end)
cp_reg32(0x28, FIFO_SIZE - 0x4000)
cp_reg32(0x2C, FIFO_SIZE // 2)
cp_reg32(0x30, 0)
cp_reg32(0x34, FIFO_BASE)
cp_reg32(0x38, FIFO_BASE)
# PI FIFO
load32(3, FIFO_BASE)
stw(3, 0x0C, 29)
load32(3, FIFO_BASE + FIFO_SIZE)
stw(3, 0x10, 29)
load32(3, FIFO_BASE)
stw(3, 0x14, 29)
li(3, 0x11)               # GPReadEnable | GPLinkEnable
sth(3, 0x02, 30)
sync()
isync()

# VI: 640x480 progressive, XFB at XFB (both fields), 16-pixel units: width 40, stride 40.
li(3, (40 << 8) | 40)
sth(3, 0x48, 28)
# Vertical blanking: PRB 48 half lines before, PSB 6 after the active lines (per field).
load32(3, (6 << 16) | 48)
stw(3, 0x0C, 28)
stw(3, 0x10, 28)
load32(3, (1 << 28) | (XFB >> 5))
stw(3, 0x1C, 28)
stw(3, 0x24, 28)
li(3, (480 << 4) | 12)     # ACV 480, EQU 12
sth(3, 0x00, 28)
li(3, 0x0005)              # enable, non-interlaced, NTSC
sth(3, 0x02, 28)

# --- Static GX state --------------------------------------------------------------------
bp(0x41, 0x18)                 # blend mode: colour and alpha update
bp(0x40, 0x10)                 # Z: update, no test
bp(0x43, 0x00)                 # RGB8_Z24
bp(0x49, 0)                    # copy source top-left
bp(0x4A, 639 | (479 << 10))    # copy source size - 1
bp(0x4B, XFB >> 5)             # destination
bp(0x4D, (640 * 2) >> 5)       # destination stride in 32-byte units
bp(0x4E, 0x100)                # Y scale 1.0
bp(0x53, (21 << 12) | (22 << 18))  # vertical copy filter 0,0,21,22,21,0,0 (sum 64)
bp(0x54, 21)
bp(0x51, 0xFFFFFF)             # clear Z
bp(0x20, (342 << 0) | (342 << 12))           # scissor TL (offset 342)
bp(0x21, (342 + 479) | ((342 + 639) << 12))  # scissor BR: bottom, right
bp(0x59, 0)                    # scissor offset

if triangle:
    # One colour channel from the vertex colour, no textures, one TEV stage passing the
    # rasterised colour.
    bp(0x00, (1 << 4))                 # genmode: 0 texgens, 1 colour channel, 1 TEV stage
    bp(0x28, (7 << 0) | (7 << 3) | (0 << 6) | (0 << 7))  # TEV order: no tex, colour chan 0
    # Colour env stage 0: a=b=c=0 (ZERO=15), d=RASC (10): out = rasc.
    bp(0xC0, (15 << 12) | (15 << 8) | (15 << 4) | 10)
    # Alpha env stage 0: d = RASA (5), a=b=c=ZERO (7).
    bp(0xC1, (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4))
    bp(0xF3, (7 << 16) | (7 << 19))    # alpha compare: always / always, AND
    # XF: one colour, no texgens, colour 0 from the vertex, lighting off.
    xf(0x1009, [1])                    # numColors
    xf(0x103F, [0])                    # numTexGens
    xf(0x100E, [1, 1])                 # color0/alpha0 control: material from vertex
    # Position matrix 0 = identity (3x4).
    xf(0x0000, [f32(1), f32(0), f32(0), f32(0),
                f32(0), f32(1), f32(0), f32(0),
                f32(0), f32(0), f32(1), f32(0)])
    # Viewport: 640x480 centred at (342+320, 342+240), z range 0..16777215.
    xf(0x101A, [f32(320), f32(-240), f32(16777215), f32(342 + 320), f32(342 + 240),
                f32(16777215)])
    # Orthographic projection mapping x,y in -1..1, z 0..-1.
    xf(0x1020, [f32(1), f32(0), f32(1), f32(0), f32(-1), f32(0), 1])
    # CP: matrix index 0; VCD: position direct, colour 0 direct; VAT group 0:
    # position xyz float, colour RGBA8.
    cp(0x30, 0)                        # matrix index A
    cp(0x40, 0)                        # matrix index B
    cp(0x50, (1 << 9) | (1 << 13))     # VCD lo: position direct (bits 9-10), color0 direct
    cp(0x60, 0)                        # VCD hi
    cp(0x70, (1 << 0) | (4 << 1) | (1 << 13) | (5 << 14))  # VAT A: pos XYZ F32, col0 RGBA8
    cp(0x80, 0)
    cp(0x90, 0)

pad_pipe()

# --- Frame loop -------------------------------------------------------------------------
li(27, 0)
label("frame")
# Wait for the beam to reach line 200, then to leave it: once per frame.
label("wait_in")
lhz(3, 0x2C, 28)
andi_(3, 3, 0x7FF)
cmpwi(3, 200)
bne("wait_in")
label("wait_out")
lhz(3, 0x2C, 28)
andi_(3, 3, 0x7FF)
cmpwi(3, 200)
beq("wait_out")

addi(27, 27, 1)

if triangle:
    # Three vertices: GX_TRIANGLES (0x90) on VAT 0, count 3.
    gp_u8(0x90)
    gp_u16(3)
    for (x, y, colour) in ((-0.8, -0.8, 0xFF0000FF), (0.8, -0.8, 0x00FF00FF), (0.0, 0.8, 0x0000FFFF)):
        gp_u32(f32(x))
        gp_u32(f32(y))
        gp_u32(f32(0.5))
        gp_u32(colour)

# Clear colour: red from the frame counter, green fixed, blue fixed; alpha 0xFF.
rlwinm(4, 27, 2, 24, 31)       # (counter << 2) & 0xFF
lis(5, 0x4F00)                 # BP 0x4F: A<<8 | R
ori(5, 5, 0xFF00)
or_(5, 5, 4)
gp_u8(0x61)
stw(5, 0, GP)
bp(0x50, (0xC0 << 8) | 0x40)   # G, B
# Copy EFB -> XFB, then clear: clamp top/bottom, clear, to XFB.
bp(0x52, 0x3 | (1 << 11) | (1 << 14))
pad_pipe()
b("frame")

# --- Fix branches and write the DOL -------------------------------------------------------
for at, kind, name in fixups:
    rel = (labels[name] - at) * 4
    if kind == "b":
        code[at] = (18 << 26) | (rel & 0x3FFFFFC)
    else:
        bo, bi = (4, 2) if kind == "bne" else (12, 2)
        code[at] = (16 << 26) | (bo << 21) | (bi << 16) | (rel & 0xFFFC)

text = b"".join(struct.pack(">I", w) for w in code)
while len(text) % 32:
    text += b"\0"

header = bytearray(0x100)
struct.pack_into(">I", header, 0x00, 0x100)        # text0 file offset
struct.pack_into(">I", header, 0x48, TEXT_ADDR)    # text0 address
struct.pack_into(">I", header, 0x90, len(text))    # text0 size
struct.pack_into(">I", header, 0xD8, 0x80400000)   # bss (unused, kept clear of the XFB)
struct.pack_into(">I", header, 0xDC, 0)
struct.pack_into(">I", header, 0xE0, TEXT_ADDR)    # entry

out = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "l5b.dol"
with open(out, "wb") as f:
    f.write(bytes(header) + text)
print(f"{out}: {len(text)} bytes of code, {len(code)} instructions")
