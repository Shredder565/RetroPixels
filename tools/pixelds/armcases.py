#!/usr/bin/env python3
"""Makes cores/pixelds/arm/testdata/arm946.bin: single-instruction test cases
run on Unicorn's ARM946 (QEMU's model), to check PixelDS's ARM9 interpreter
against. Usage: armcases.py <out file> [count]"""
import random, struct, sys
from unicorn import *
from unicorn.arm_const import *

CODE, DATA, DSIZE = 0x1000, 0x10000, 0x400
REGS = [UC_ARM_REG_R0+i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC]

def rr(a, b): return random.randint(a, b)
def lowreg(): return rr(0, 12)

def arm_case():
    cond = 0xE if random.random() < 0.8 else rr(0, 13)
    k = rr(0, 11)
    if k <= 3:  # data processing
        opc, s = rr(0, 15), rr(0, 1)
        if 8 <= opc <= 11: s = 1
        rd, rn = lowreg(), lowreg()
        if random.random() < 0.05: rn = 15
        f = rr(0, 2)
        if f == 0: op2 = 1 << 25 | rr(0, 15) << 8 | rr(0, 255)
        elif f == 1: op2 = rr(0, 31) << 7 | rr(0, 3) << 5 | lowreg()
        else:
            op2 = lowreg() << 8 | rr(0, 3) << 5 | 1 << 4 | lowreg()
            if rn == 15: rn = 0
        return cond << 28 | opc << 21 | s << 20 | rn << 16 | rd << 12 | op2
    if k == 4:  # multiplies
        t = rr(0, 1)
        rd, rn, rs, rm = random.sample(range(13), 4)
        if t == 0: return cond << 28 | rr(0, 1) << 21 | rr(0, 1) << 20 | rd << 16 | rn << 12 | rs << 8 | 9 << 4 | rm
        return cond << 28 | 1 << 23 | rr(0, 3) << 21 | rr(0, 1) << 20 | rd << 16 | rn << 12 | rs << 8 | 9 << 4 | rm
    if k == 5:  # CLZ, Q arithmetic, DSP multiplies
        rd, rn, rs, rm = random.sample(range(13), 4)
        t = rr(0, 2)
        if t == 0: return cond << 28 | 0x016F0F10 | rd << 12 | rm
        if t == 1: return cond << 28 | 0x01000050 | rr(0, 3) << 21 | rn << 16 | rd << 12 | rm
        return cond << 28 | 0x01000080 | rr(0, 3) << 21 | rd << 16 | rn << 12 | rs << 8 | rr(0, 3) << 5 | rm
    if k in (6, 7):  # LDR / STR with SP as the base
        rd = lowreg()
        p, u, b, w, l = rr(0, 1), rr(0, 1), rr(0, 1), rr(0, 1), rr(0, 1)
        # word accesses stay aligned: Unicorn doesn't rotate unaligned loads
        # or align stores as the ARM946 does
        off = rr(0, 255) & (~3 if not b else ~0)
        if b and random.random() < 0.3:
            rm = lowreg()
            return cond << 28 | 1 << 26 | 1 << 25 | p << 24 | u << 23 | b << 22 | 0 << 21 | l << 20 | 13 << 16 | rd << 12 | rm  # register offset (regs kept small)
        return cond << 28 | 1 << 26 | p << 24 | u << 23 | b << 22 | (w if p else 0) << 21 | l << 20 | 13 << 16 | rd << 12 | off
    if k == 8:  # halfword, signed and doubleword
        rd = rr(0, 10) & ~1
        p, u, w = rr(0, 1), rr(0, 1), rr(0, 1)
        sh, l = rr(1, 3), rr(0, 1)
        off = rr(0, 127) * 2
        if l == 0 and sh != 1 or sh == 3 and l == 0: off = rr(0, 31) * 8
        return cond << 28 | p << 24 | u << 23 | 1 << 22 | (w if p else 0) << 21 | l << 20 | 13 << 16 | rd << 12 | (off >> 4) << 8 | 1 << 7 | sh << 5 | 1 << 4 | (off & 15)
    if k == 9:  # LDM / STM
        lst = rr(1, 0x1fff) | (random.random() < 0.2) << 14
        p, u, w, l = rr(0, 1), rr(0, 1), rr(0, 1), rr(0, 1)
        return cond << 28 | 4 << 25 | p << 24 | u << 23 | w << 21 | l << 20 | 13 << 16 | lst
    if k == 10:  # MRS / MSR flags
        if rr(0, 1): return cond << 28 | 0x010F0000 | lowreg() << 12
        return cond << 28 | 0x0328F000 | rr(0, 15) << 8 | rr(0, 255)
    return cond << 28 | 0x0A000000 | rr(0, 0xffffff)  # B / BL (only the PC checked)

def thumb_case():
    k = rr(0, 9)
    lo = lambda: rr(0, 7)
    if k == 0: return rr(0, 2) << 11 | rr(0, 31) << 6 | lo() << 3 | lo()
    if k == 1: return 3 << 11 | rr(0, 3) << 9 | lo() << 6 | lo() << 3 | lo()
    if k == 2: return 1 << 13 | rr(0, 3) << 11 | lo() << 8 | rr(0, 255)
    if k == 3: return 0x4000 | rr(0, 15) << 6 | lo() << 3 | lo()
    if k == 4:  # hi register ADD/CMP/MOV, not to the PC or SP
        op = rr(0, 2); rd = rr(0, 12); rs = rr(0, 12)
        return 0x4400 | op << 8 | (rd >> 3) << 7 | (rs >> 3) << 6 | (rs & 7) << 3 | (rd & 7)
    if k == 5: return 0x9000 | rr(0, 1) << 11 | lo() << 8 | rr(0, 255)  # SP relative
    if k == 6: return 0xA000 | rr(0, 1) << 11 | lo() << 8 | rr(0, 255)
    if k == 7: return 0xB000 | rr(0, 1) << 7 | rr(0, 127)
    if k == 8: return 0xB400 | rr(0, 1) << 11 | rr(1, 255)  # PUSH/POP without the PC
    return 0xD000 | rr(0, 13) << 8 | rr(0, 255)

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

def run(thumb):
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mu.ctl_set_cpu_model(UC_CPU_ARM_946)
    mu.mem_map(0, 0x20000)
    regs = [random.choice([0, 1, 0xffffffff, 0x80000000, 0x7fffffff, rr(0, 0xffffffff), rr(0, 0xffff), rr(0, 40)]) for _ in range(13)]
    sp = DATA + 0x200
    flags = rr(0, 15) << 28
    seed = rr(0, 0xffffffff)
    mem = lcg(seed)
    mu.mem_write(DATA, mem)
    if thumb:
        op = thumb_case(); code = struct.pack('<H', op) + b'\x00\xbf'
    else:
        op = arm_case(); code = struct.pack('<I', op)
    mu.mem_write(CODE, code)
    for i in range(13): mu.reg_write(REGS[i], regs[i])
    mu.reg_write(UC_ARM_REG_SP, sp)
    mu.reg_write(UC_ARM_REG_LR, 0x1234)
    cpsr = flags | 0x13 | 0xC0 | (0x20 if thumb else 0)
    mu.reg_write(UC_ARM_REG_CPSR, cpsr)
    try:
        mu.emu_start(CODE | thumb, CODE + 0x100, count=1)
    except UcError:
        return None
    out = [mu.reg_read(r) for r in REGS[:15]]
    pc = mu.reg_read(UC_ARM_REG_PC)
    out_cpsr = mu.reg_read(UC_ARM_REG_CPSR)
    return struct.pack('<B I 16I I I', thumb, op, *(regs + [sp, 0x1234, 0]), cpsr, seed) + \
        struct.pack('<15I I I I', *out, pc, out_cpsr, fnv(bytes(mu.mem_read(DATA, DSIZE))))

out, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 20000
random.seed(946)
with open(out, 'wb') as f:
    made = 0
    while made < n:
        c = run(made % 3 == 2)
        if c: f.write(c); made += 1
