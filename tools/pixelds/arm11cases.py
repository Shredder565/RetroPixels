#!/usr/bin/env python3
"""Makes cores/pixelds/arm/testdata/arm11.bin: single-instruction test cases
for the ARM11's ARMv6 media instructions and VFPv2, run on Unicorn's ARM11
MPCore model (QEMU's), to check PixelDS's interpreter against.
Usage: arm11cases.py <out file> [count]

Each case: op, r0-r15, CPSR, S0-S31, FPSCR, memory seed; then the results:
r0-r14, PC, CPSR, S0-S31, FPSCR, a hash of the data memory."""
import random, struct, sys
from unicorn import *
from unicorn.arm_const import *

CODE, DATA, DSIZE = 0x1000, 0x10000, 0x400
REGS = [UC_ARM_REG_R0+i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC]
DREGS = [UC_ARM_REG_D0+i for i in range(16)]

def rr(a, b): return random.randint(a, b)
def r(): return rr(0, 12)

def media():
    cond = 0xE
    rd, rn, rm, rs = r(), r(), r(), r()
    k = rr(0, 10)
    if k <= 2:  # parallel add / sub
        op1 = random.choice([1, 2, 3, 5, 6, 7])
        op2 = random.choice([0, 1, 2, 3, 4, 7])
        return cond << 28 | 0x06000010 | op1 << 20 | rn << 16 | rd << 12 | 0xf00 | op2 << 5 | rm
    if k == 3:  # PKHBT / PKHTB, SEL
        if rr(0, 2) == 0: return cond << 28 | 0x06800fb0 | rn << 16 | rd << 12 | rm
        return cond << 28 | 0x06800010 | rn << 16 | rd << 12 | rr(0, 31) << 7 | rr(0, 1) << 6 | rm
    if k == 4:  # SSAT / USAT
        return cond << 28 | 0x06a00010 | rr(0, 1) << 22 | rr(0, 31) << 16 | rd << 12 | rr(0, 31) << 7 | rr(0, 1) << 6 | rm
    if k == 5:  # SSAT16 / USAT16, REV, REV16, REVSH
        t = rr(0, 4)
        if t == 0: return cond << 28 | 0x06a00f30 | rr(0, 15) << 16 | rd << 12 | rm
        if t == 1: return cond << 28 | 0x06e00f30 | rr(0, 15) << 16 | rd << 12 | rm
        return cond << 28 | [0x06bf0f30, 0x06bf0fb0, 0x06ff0fb0][t-2] | rd << 12 | rm
    if k == 6:  # extend (and add)
        op1 = random.choice([0, 2, 3, 4, 6, 7])
        n = 15 if rr(0, 1) else rn
        return cond << 28 | 0x06800070 | op1 << 20 | n << 16 | rd << 12 | rr(0, 3) << 10 | rm
    if k == 7:  # SMLAD / SMUAD / SMLSD / SMUSD
        ra = 15 if rr(0, 1) else rs
        return cond << 28 | 0x07000010 | rd << 16 | ra << 12 | rm << 8 | rr(0, 3) << 5 | rn
    if k == 8:  # SMLALD / SMLSLD
        lo, hi = random.sample(range(13), 2)
        return cond << 28 | 0x07400010 | hi << 16 | lo << 12 | rm << 8 | rr(0, 3) << 5 | rn
    if k == 9:  # SMMUL / SMMLA / SMMLS
        ra = 15 if rr(0, 2) == 0 else rs
        op2 = random.choice([0, 1, 6, 7])
        if op2 >= 6 and ra == 15: ra = rs
        return cond << 28 | 0x07500010 | rd << 16 | ra << 12 | rm << 8 | op2 << 5 | rn
    ra = 15 if rr(0, 1) else rs  # USAD8 / USADA8
    return cond << 28 | 0x07800010 | rd << 16 | ra << 12 | rm << 8 | rn

def vfp():
    cond = 0xE
    dbl = rr(0, 1)
    cp = 11 if dbl else 10
    fd, fn, fm = rr(0, 15), rr(0, 15), rr(0, 15)
    D, N, M = (0, 0, 0) if dbl else (rr(0, 1), rr(0, 1), rr(0, 1))
    k = rr(0, 9)
    if k <= 3:  # arithmetic
        opc = rr(0, 8)
        p, q, rbit, s = opc >> 3 & 1, opc >> 2 & 1, opc >> 1 & 1, opc & 1
        return cond << 28 | 0x0e000000 | p << 23 | D << 22 | q << 21 | rbit << 20 | fn << 16 | fd << 12 | cp << 8 | N << 7 | s << 6 | M << 5 | fm
    if k <= 6:  # extension: copy, abs, neg, sqrt, compares, conversions
        ext = random.choice([0, 1, 2, 3, 8, 9, 10, 11, 15, 16, 17, 24, 25, 26, 27])
        en, eN = ext >> 1, ext & 1
        if ext in (16, 17) and dbl: M = rr(0, 1)  # the integer source is a single register
        if ext in (24, 25, 26, 27) and dbl: D = rr(0, 1)  # the integer result too
        if ext == 15 and dbl: D = rr(0, 1)  # FCVTSD writes a single
        if ext == 15 and not dbl: D = 0  # FCVTDS writes a double
        if ext in (16, 17) and dbl: pass
        if dbl and ext in (16, 17): fm_ = fm
        return cond << 28 | 0x0eb00040 | D << 22 | en << 16 | fd << 12 | cp << 8 | eN << 7 | M << 5 | fm
    if k == 7:  # register transfers
        t = rr(0, 3)
        rd = r()
        L = rr(0, 1)
        if t == 0: return cond << 28 | 0x0e000a10 | L << 20 | fn << 16 | rd << 12 | N << 7  # FMSR / FMRS
        if t == 1: return cond << 28 | 0x0e000b10 | rr(0, 1) << 21 | L << 20 | fn << 16 | rd << 12  # FMDLR..FMRDH
        if t == 2:  # FMRX FPSCR / FMSTAT / FMXR FPSCR
            if L: return cond << 28 | 0x0ef10a10 | (15 if rr(0, 1) else rd) << 12
            return cond << 28 | 0x0ee10a10 | rd << 12
        rd2 = r()
        while rd2 == rd: rd2 = r()
        if dbl: return cond << 28 | 0x0c400b10 | L << 20 | rd2 << 16 | rd << 12 | fm
        sm = rr(0, 30)
        return cond << 28 | 0x0c400a10 | L << 20 | rd2 << 16 | rd << 12 | (sm & 1) << 5 | sm >> 1
    # loads and stores with SP as the base
    L = rr(0, 1)
    if rr(0, 1):  # single
        return cond << 28 | 0x0d000000 | rr(0, 1) << 23 | D << 22 | L << 20 | 13 << 16 | fd << 12 | cp << 8 | rr(0, 60)
    # multiple: IA (with or without writeback) or DB with writeback
    first = rr(0, 15)
    n = rr(1, 16 - first) if dbl else rr(1, 8)
    words = n * 2 if dbl else n
    if not dbl:
        sfirst = rr(0, 32 - n)
        fd_, D = sfirst >> 1, sfirst & 1
    else:
        fd_ = first
    if rr(0, 1): pu = (0, 1, rr(0, 1))
    else: pu = (1, 0, 1)
    p, u, w = pu
    return cond << 28 | 0x0c000000 | p << 24 | u << 23 | D << 22 | w << 21 | L << 20 | 13 << 16 | fd_ << 12 | cp << 8 | words

def lcg(seed):
    out = bytearray(DSIZE)
    for i in range(DSIZE):
        seed = (seed * 1664525 + 1013904223) & 0xffffffff
        out[i] = seed >> 24
    return bytes(out)

def fnv(b):
    h = 0x811c9dc5
    for x in b: h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h

def rand_single():
    return random.choice([
        0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x00000001, 0x80400000,
        0x7fc00000, 0x7f800001, 0xffc12345, 0x4b000000, 0x4f000000, 0xcf000000, 0x5f800000,
        struct.unpack('<I', struct.pack('<f', random.uniform(-1000, 1000)))[0],
        struct.unpack('<I', struct.pack('<f', random.uniform(-2, 2)))[0],
        rr(0, 0xffffffff), rr(0, 0x7fffff)])

def rand_double():
    return random.choice([
        0, 1 << 63, 0x3ff0000000000000, 0xbff0000000000000, 0x7ff0000000000000, 0x7ff8000000000000,
        0x7ff0000000000001, 0x41e0000000000000, 0x41f0000000000000, 1,
        struct.unpack('<Q', struct.pack('<d', random.uniform(-1e6, 1e6)))[0],
        struct.unpack('<Q', struct.pack('<d', random.uniform(-3, 3)))[0],
        rr(0, 0xffffffffffffffff)])

def run():
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mu.ctl_set_cpu_model(UC_CPU_ARM_11MPCORE)
    mu.mem_map(0, 0x20000)
    regs = [random.choice([0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0x8000, 0x7fff, 0x80008000, rr(0, 0xffffffff), rr(0, 0xffff), rr(0, 40)]) for _ in range(13)]
    sp = DATA + 0x200
    cpsr = rr(0, 15) << 28 | rr(0, 1) << 27 | rr(0, 15) << 16 | 0x13 | 0xC0
    seed = rr(0, 0xffffffff)
    mu.mem_write(DATA, lcg(seed))
    sregs = []
    for i in range(16):
        if rr(0, 1):
            d = rand_double()
            sregs += [d & 0xffffffff, d >> 32]
        else:
            sregs += [rand_single(), rand_single()]
    fpscr = random.choice([0, 0, 0x03000000, 0x00400000, 0x00800000, 0x00c00000, 0x01000000, 0x02000000]) | rr(0, 15) << 28
    op = media() if rr(0, 2) == 0 else vfp()
    mu.mem_write(CODE, struct.pack('<I', op))
    for i in range(13): mu.reg_write(REGS[i], regs[i])
    mu.reg_write(UC_ARM_REG_SP, sp)
    mu.reg_write(UC_ARM_REG_LR, 0x1234)
    mu.reg_write(UC_ARM_REG_CPSR, cpsr)
    mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    for i in range(16): mu.reg_write(DREGS[i], sregs[2*i] | sregs[2*i+1] << 32)
    mu.reg_write(UC_ARM_REG_FPSCR, fpscr)
    try:
        mu.emu_start(CODE, CODE + 0x100, count=1)
    except UcError:
        return None
    pc = mu.reg_read(UC_ARM_REG_PC)
    if pc != CODE + 4:
        return None  # it took the undefined vector
    out = [mu.reg_read(x) for x in REGS[:15]]
    out_s = []
    for i in range(16):
        d = mu.reg_read(DREGS[i])
        out_s += [d & 0xffffffff, d >> 32]
    return struct.pack('<I 16I I 32I I I', op, *(regs + [sp, 0x1234, 0]), cpsr, *sregs, fpscr, seed) + \
        struct.pack('<15I I I 32I I I', *out, pc, mu.reg_read(UC_ARM_REG_CPSR), *out_s,
                    mu.reg_read(UC_ARM_REG_FPSCR), fnv(bytes(mu.mem_read(DATA, DSIZE))))

out, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 20000
random.seed(1176)
with open(out, 'wb') as f:
    made = tries = 0
    while made < n:
        tries += 1
        c = run()
        if c: f.write(c); made += 1
print(made, "cases from", tries, "tries")
