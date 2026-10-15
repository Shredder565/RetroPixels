# Generates "fuzz" Master System test ROMs: a program that, driven by a
# pseudo-random generator mixed with everything it reads back (pads, VDP
# data/status, V/H counters, ROM and cartridge RAM), keeps changing VDP
# registers, VRAM, CRAM, PSG, the I/O control port and the mapper, with
# raster effects from line interrupts and Pause (NMI) counted. Every bank holds
# the same code, so any bank switching is safe.
import os, random, subprocess, sys

#
# usage: python3 fuzzrom.py <pyz80> <output dir> <work dir>
# (pyz80: https://github.com/simonowen/pyz80). sms_test.go embeds the
# assembled program of fz_sega256k and fz_cm128k.
PYZ80, OUT, TMP = sys.argv[1:4]

def source(seed, cm, tmsbias):
    if cm:
        amap = """
a_map:  call rnd
        and 3
        ld e,a
        call rnd
        ld d,a
        ld a,e
        or a
        jr nz,am1
        ld a,d
        ld (0x0000),a
        ret
am1:    dec e
        jr nz,am2
        ld a,d
        ld (0x4000),a
        ret
am2:    dec e
        jr nz,am3
        ld a,d
        ld (0x8000),a
        ret
am3:    call rnd
        and 0x1f
        or 0xa0
        ld h,a
        call rnd
        ld l,a
        ld (hl),d
        ld a,(hl)
        call mix
        ret
"""
    else:
        amap = """
a_map:  call rnd
        and 3
        ld e,a
        call rnd
        ld d,a
        ld a,e
        or a
        jr nz,am1
        ld a,d
        and 0x1f
        ld (0xfffc),a
        ret
am1:    ld hl,0xfffc
        add a,l
        ld l,a
        ld (hl),d
        ret
"""
    r0bias = "and 3\n        jr z,r0raw" if not tmsbias else "and 3\n        jr nz,r0raw"
    return f"""
seed:   equ 0xc000
frames: equ 0xc002
paused: equ 0xc003
last:   equ 0xc004

        org 0
        di
        im 1
        ld sp,0xdff0
        jp main
        ds 0x38-$
irq:    push af
        push hl
        in a,(0xbf)
        bit 7,a
        jr z,lineirq
        ld hl,frames
        inc (hl)
        jr irqdone
lineirq: call rnd
        out (0xbf),a
        ld a,0x88
        out (0xbf),a
irqdone: pop hl
        pop af
        ei
        reti
        ds 0x66-$
nmi:    push af
        ld a,(paused)
        inc a
        ld (paused),a
        pop af
        retn

rnd:    push hl
        ld hl,(seed)
        ld a,h
        rra
        ld a,l
        rra
        xor h
        ld h,a
        ld a,l
        rra
        ld a,h
        rra
        xor l
        ld l,a
        xor h
        ld h,a
        ld (seed),hl
        pop hl
        ret

mix:    push hl
        ld hl,(seed)
        xor l
        ld l,a
        or h
        jr nz,mixok
        inc l
mixok:  ld (seed),hl
        pop hl
        ret

main:   ld hl,{seed & 0xffff | 1}
        ld (seed),hl
        xor a
        ld (frames),a
        ld (paused),a
        ld (last),a
        ; registers
        ld hl,regs
        ld b,11
        ld c,0x80
setreg: ld a,(hl)
        out (0xbf),a
        ld a,c
        out (0xbf),a
        inc hl
        inc c
        djnz setreg
        ; VRAM: random
        xor a
        out (0xbf),a
        ld a,0x40
        out (0xbf),a
        ld de,0x4000
vfill:  call rnd
        out (0xbe),a
        dec de
        ld a,d
        or e
        jr nz,vfill
        ; CRAM: random
        xor a
        out (0xbf),a
        ld a,0xc0
        out (0xbf),a
        ld b,32
cfill:  call rnd
        out (0xbe),a
        djnz cfill

loop:   call rnd
        and 3
        jr z,busy
        ei
        halt
        di
        jr check
busy:   ei
        ld b,0
bl:     in a,(0x7e)
        djnz bl
        di
        call mix
check:  ld a,(frames)
        ld hl,last
        cp (hl)
        jr z,loop
        ld (hl),a
        call actions
        jr loop

actions: in a,(0xdc)
        call mix
        in a,(0xdd)
        call mix
        ld a,(paused)
        call mix
        call rnd
        and 7
        inc a
        ld b,a
actloop: push bc
        call rnd
        and 15
        add a,a
        ld e,a
        ld d,0
        ld hl,acttab
        add hl,de
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
        call jphl
        pop bc
        djnz actloop
        ret
jphl:   jp (hl)

acttab: dw a_reg, a_reg, a_reg, a_reg, a_vram, a_vram, a_vram, a_cram
        dw a_cram, a_vread, a_psg, a_psg, a_hv, a_map, a_cart, a_reg

a_reg:  call rnd
        and 15
        ld c,a
        call rnd
        ld b,a
        ld a,c
        or a
        jr nz,notr0
        call rnd
        {r0bias}
        ld a,b
        xor 4
        ld b,a
r0raw:  jr regout
notr0:  cp 1
        jr nz,regout
        ld a,b
        or 0x20
        ld b,a
        call rnd
        and 7
        jr z,regout
        ld a,b
        or 0x40
        ld b,a
regout: ld a,b
        out (0xbf),a
        ld a,c
        or 0x80
        out (0xbf),a
        ret

a_vram: call rnd
        out (0xbf),a
        call rnd
        and 0x3f
        or 0x40
        out (0xbf),a
        call rnd
        and 63
        inc a
        ld b,a
vl:     call rnd
        out (0xbe),a
        djnz vl
        ret

a_cram: call rnd
        and 31
        out (0xbf),a
        ld a,0xc0
        out (0xbf),a
        call rnd
        and 7
        inc a
        ld b,a
cl:     call rnd
        out (0xbe),a
        djnz cl
        ret

a_vread: call rnd
        out (0xbf),a
        call rnd
        and 0x3f
        out (0xbf),a
        ld b,4
rl:     in a,(0xbe)
        call mix
        djnz rl
        in a,(0xbf)
        call mix
        ret

a_psg:  call rnd
        out (0x7f),a
        call rnd
        out (0x7f),a
        ret

a_hv:   call rnd
        out (0x3f),a
        in a,(0x7e)
        call mix
        in a,(0x7f)
        call mix
        in a,(0xdd)
        call mix
        ret

a_cart: call rnd
        and 0x7f
        add a,0x40
        ld h,a
        call rnd
        ld l,a
        ld a,(hl)
        call mix
        call rnd
        and 0x3f
        or 0x80
        ld h,a
        call rnd
        ld l,a
        call rnd
        ld (hl),a
        ret
{amap}
regs:   db 0x36,0xe0,0xff,0xff,0xff,0xff,0xfb,0x00,0x00,0x00,0x20
        ds 0x2000-$
"""

def build(name, seed, banks, cm=False, tmsbias=False, header=0, trim=None):
    src = os.path.join(TMP, name + ".asm")
    obj = os.path.join(TMP, name + ".bin")
    open(src, "w").write(source(seed, cm, tmsbias))
    subprocess.run([PYZ80, "--obj=" + obj, "-o", "/dev/null", src], check=True, stdout=subprocess.DEVNULL)
    code = open(obj, "rb").read()
    assert len(code) <= 0x2000, len(code)
    rng = random.Random(seed)
    rom = bytearray()
    for k in range(banks):
        b = bytearray(0x4000)
        b[:len(code)] = code
        for i in range(0x2000, 0x3fe0):
            b[i] = rng.randrange(256)
        b[0x3fe0] = k
        if cm:
            b[0x3fe6:0x3fea] = bytes([0x34, 0x12, 0xcc, 0xed])
        b[0x3ff0:0x3ff8] = b"TMR SEGA"
        rom += b
    if trim:
        rom = rom[:trim]
    rom = bytes(header) + bytes(rom) if header else bytes(rom)
    open(os.path.join(OUT, name + ".sms"), "wb").write(rom)
    print(name, len(rom))

build("fz_sega256k", 0x1357, 16)
build("fz_sega48k", 0x2468, 3)
build("fz_tms64k", 0x9abc, 4, tmsbias=True)
build("fz_cm128k", 0x4321, 8, cm=True)
build("fz_cm_tms64k", 0x7777, 4, cm=True, tmsbias=True)
build("fz_hdr32k", 0x5555, 2, header=512)
build("fz_odd", 0x6789, 3, trim=0x9000)
