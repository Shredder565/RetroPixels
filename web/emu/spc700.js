// RetroPixels — Sony SPC700 sound processor (used by the SNES emulator).
// Original code written from the processor's documented instruction set.
//
//   const s = new RPSPC700({ read(a), write(a, v) }); s.step() -> cycles
"use strict";

(function (global) {
  const CYC = new Uint8Array([
    // 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8, // 0
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6, // 1
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4, // 2
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8, // 3
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6, // 4
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3, // 5
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5, // 6
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6, // 7
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5, // 8
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5, // 9
    3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4, // A
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4, // B
    3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9, // C
    2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3, // D
    2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3, // E
    2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3, // F
  ]);
  const N = 0x80, V = 0x40, P = 0x20, B = 0x10, H = 0x08, I = 0x04, Z = 0x02, C = 0x01;

  function RPSPC700(bus) {
    let a = 0, x = 0, y = 0, sp = 0xef, pc = 0, psw = 0, extra = 0, stopped = false;
    const rd = (ad) => bus.read(ad & 0xffff);
    const wr = (ad, v) => bus.write(ad & 0xffff, v & 0xff);
    const dp = (o) => ((psw & P) << 3) | (o & 0xff); // direct page address
    const imm = () => { const v = rd(pc); pc = (pc + 1) & 0xffff; return v; };
    const imm16 = () => { const l = imm(); return l | (imm() << 8); };
    const rd16dp = (o) => rd(dp(o)) | (rd(dp(o + 1)) << 8);
    const push = (v) => { wr(0x100 | sp, v); sp = (sp - 1) & 0xff; };
    const pop = () => { sp = (sp + 1) & 0xff; return rd(0x100 | sp); };
    const nz = (v) => { psw = (psw & ~(N | Z)) | (v & N) | (v & 0xff ? 0 : Z); return v & 0xff; };
    const nz16 = (v) => { psw = (psw & ~(N | Z)) | ((v >> 8) & N) | (v & 0xffff ? 0 : Z); };

    const adc = (p, q) => {
      const r = p + q + (psw & C);
      psw &= ~(N | V | H | Z | C);
      if (r > 0xff) psw |= C;
      if ((p ^ q ^ r) & 0x10) psw |= H;
      if (~(p ^ q) & (p ^ r) & 0x80) psw |= V;
      return nz(r);
    };
    const sbc = (p, q) => adc(p, q ^ 0xff);
    const cmp = (p, q) => { const r = p - q; psw = (psw & ~C) | (r >= 0 ? C : 0); nz(r); };
    const alu = (op, p, q) => {
      switch (op) {
        case 0: return nz(p | q);
        case 1: return nz(p & q);
        case 2: return nz(p ^ q);
        case 3: cmp(p, q); return p;
        case 4: return adc(p, q);
        default: return sbc(p, q);
      }
    };
    const asl = (v) => { psw = (psw & ~C) | (v >> 7); return nz(v << 1); };
    const rol = (v) => { const c = psw & C; psw = (psw & ~C) | (v >> 7); return nz((v << 1) | c); };
    const lsr = (v) => { psw = (psw & ~C) | (v & 1); return nz(v >> 1); };
    const ror = (v) => { const c = psw & C; psw = (psw & ~C) | (v & 1); return nz((v >> 1) | (c << 7)); };
    const branch = (cond) => { const o = imm(); if (cond) { pc = (pc + ((o << 24) >> 24)) & 0xffff; extra += 2; } };
    // memory.bit operand: 13-bit address + bit number
    const mbit = () => { const w = imm16(); return [w & 0x1fff, w >> 13]; };

    // address of an ALU operand by column (4..9 / 14..19 style), returns value
    function aluOperand(col) {
      switch (col) {
        case 0x04: return rd(dp(imm()));
        case 0x05: return rd(imm16());
        case 0x06: return rd(dp(x));
        case 0x07: { const o = (imm() + x) & 0xff; return rd(rd16dp(o)); }
        case 0x08: return imm();
        case 0x14: return rd(dp(imm() + x));
        case 0x15: return rd((imm16() + x) & 0xffff);
        case 0x16: return rd((imm16() + y) & 0xffff);
        default: { const o = imm(); return rd((rd16dp(o) + y) & 0xffff); } // 0x17
      }
    }

    function exec(op) {
      const hi = op >> 4, lo = op & 15;
      // ALU groups: OR AND EOR CMP ADC SBC at 0x00,0x20,0x40,0x60,0x80,0xA0 (+0x10)
      if ((lo >= 4 && lo <= 9) && hi <= 0xb) {
        const opn = hi >> 1, row = (hi & 1) << 4;
        const col = row | lo;
        if (col === 0x09) { const s = rd(dp(imm())); const d = dp(imm()); const r = alu(opn, rd(d), s); if (opn !== 3) wr(d, r); return; }
        if (col === 0x18) { const s = imm(); const d = dp(imm()); const r = alu(opn, rd(d), s); if (opn !== 3) wr(d, r); return; }
        if (col === 0x19) { const s = rd(dp(y)); const d = dp(x); const r = alu(opn, rd(d), s); if (opn !== 3) wr(d, r); return; }
        const v = aluOperand(col);
        const r = alu(opn, a, v);
        if (opn !== 3) a = r;
        return;
      }
      if (lo === 1) { push(pc >> 8); push(pc & 0xff); const vec = 0xffde - hi * 2; pc = rd(vec) | (rd(vec + 1) << 8); return; } // TCALL
      if (lo === 2) { const d = dp(imm()), bit = 1 << (hi >> 1); const v = rd(d); wr(d, hi & 1 ? v & ~bit : v | bit); return; } // SET1 / CLR1
      if (lo === 3) { const d = dp(imm()), bit = 1 << (hi >> 1); const v = rd(d); branch(hi & 1 ? !(v & bit) : !!(v & bit)); return; } // BBS / BBC
      switch (op) {
        case 0x00: return;
        case 0x0a: { const [ad, b] = mbit(); if ((rd(ad) >> b) & 1) psw |= C; return; }
        case 0x2a: { const [ad, b] = mbit(); if (!((rd(ad) >> b) & 1)) psw |= C; return; }
        case 0x4a: { const [ad, b] = mbit(); if (!((rd(ad) >> b) & 1)) psw &= ~C; return; }
        case 0x6a: { const [ad, b] = mbit(); if ((rd(ad) >> b) & 1) psw &= ~C; return; }
        case 0x8a: { const [ad, b] = mbit(); psw ^= (rd(ad) >> b) & 1; return; }
        case 0xaa: { const [ad, b] = mbit(); psw = (psw & ~C) | ((rd(ad) >> b) & 1); return; }
        case 0xca: { const [ad, b] = mbit(); const v = rd(ad); wr(ad, (v & ~(1 << b)) | ((psw & C) << b)); return; }
        case 0xea: { const [ad, b] = mbit(); wr(ad, rd(ad) ^ (1 << b)); return; }
        case 0x0b: { const d = dp(imm()); wr(d, asl(rd(d))); return; }
        case 0x0c: { const d = imm16(); wr(d, asl(rd(d))); return; }
        case 0x1b: { const d = dp(imm() + x); wr(d, asl(rd(d))); return; }
        case 0x1c: a = asl(a); return;
        case 0x2b: { const d = dp(imm()); wr(d, rol(rd(d))); return; }
        case 0x2c: { const d = imm16(); wr(d, rol(rd(d))); return; }
        case 0x3b: { const d = dp(imm() + x); wr(d, rol(rd(d))); return; }
        case 0x3c: a = rol(a); return;
        case 0x4b: { const d = dp(imm()); wr(d, lsr(rd(d))); return; }
        case 0x4c: { const d = imm16(); wr(d, lsr(rd(d))); return; }
        case 0x5b: { const d = dp(imm() + x); wr(d, lsr(rd(d))); return; }
        case 0x5c: a = lsr(a); return;
        case 0x6b: { const d = dp(imm()); wr(d, ror(rd(d))); return; }
        case 0x6c: { const d = imm16(); wr(d, ror(rd(d))); return; }
        case 0x7b: { const d = dp(imm() + x); wr(d, ror(rd(d))); return; }
        case 0x7c: a = ror(a); return;
        case 0x8b: { const d = dp(imm()); wr(d, nz(rd(d) - 1)); return; }
        case 0x8c: { const d = imm16(); wr(d, nz(rd(d) - 1)); return; }
        case 0x9b: { const d = dp(imm() + x); wr(d, nz(rd(d) - 1)); return; }
        case 0x9c: a = nz(a - 1); return;
        case 0xab: { const d = dp(imm()); wr(d, nz(rd(d) + 1)); return; }
        case 0xac: { const d = imm16(); wr(d, nz(rd(d) + 1)); return; }
        case 0xbb: { const d = dp(imm() + x); wr(d, nz(rd(d) + 1)); return; }
        case 0xbc: a = nz(a + 1); return;
        case 0x1d: x = nz(x - 1); return;
        case 0x3d: x = nz(x + 1); return;
        case 0xdc: y = nz(y - 1); return;
        case 0xfc: y = nz(y + 1); return;
        case 0x0d: push(psw); return;
        case 0x2d: push(a); return;
        case 0x4d: push(x); return;
        case 0x6d: push(y); return;
        case 0x8e: psw = pop(); return;
        case 0xae: a = pop(); return;
        case 0xce: x = pop(); return;
        case 0xee: y = pop(); return;
        case 0x0e: case 0x4e: { const d = imm16(); const v = rd(d); nz(a - v); wr(d, op === 0x0e ? v | a : v & ~a); return; }
        case 0x0f: { push(pc >> 8); push(pc & 0xff); push(psw); psw = (psw | B) & ~I; pc = rd(0xffde) | (rd(0xffdf) << 8); return; }
        case 0x10: branch(!(psw & N)); return;
        case 0x30: branch(psw & N); return;
        case 0x50: branch(!(psw & V)); return;
        case 0x70: branch(psw & V); return;
        case 0x90: branch(!(psw & C)); return;
        case 0xb0: branch(psw & C); return;
        case 0xd0: branch(!(psw & Z)); return;
        case 0xf0: branch(psw & Z); return;
        case 0x2f: branch(true); extra -= 2; return;
        case 0x1a: case 0x3a: { const o = imm(); const v = (rd16dp(o) + (op === 0x3a ? 1 : 0xffff)) & 0xffff; wr(dp(o), v & 0xff); wr(dp(o + 1), v >> 8); nz16(v); return; }
        case 0x5a: { const w = rd16dp(imm()), ya = (y << 8) | a, r = ya - w; psw = (psw & ~C) | (r >= 0 ? C : 0); nz16(r & 0xffff); return; }
        case 0x7a: case 0x9a: {
          const w = rd16dp(imm()), ya = (y << 8) | a;
          let r;
          psw &= ~(N | V | H | Z | C);
          if (op === 0x7a) {
            r = ya + w;
            if (r > 0xffff) psw |= C;
            if ((ya ^ w ^ r) & 0x1000) psw |= H;
            if (~(ya ^ w) & (ya ^ r) & 0x8000) psw |= V;
          } else {
            r = ya - w;
            if (r >= 0) psw |= C;
            if (!((ya ^ w ^ r) & 0x1000)) psw |= H;
            if ((ya ^ w) & (ya ^ r) & 0x8000) psw |= V;
          }
          r &= 0xffff; nz16(r); a = r & 0xff; y = r >> 8; return;
        }
        case 0xba: { const o = imm(); const v = rd16dp(o); a = v & 0xff; y = v >> 8; nz16(v); return; }
        case 0xda: { const o = imm(); wr(dp(o), a); wr(dp(o + 1), y); return; }
        case 0x1e: cmp(x, rd(imm16())); return;
        case 0x3e: cmp(x, rd(dp(imm()))); return;
        case 0x5e: cmp(y, rd(imm16())); return;
        case 0x7e: cmp(y, rd(dp(imm()))); return;
        case 0xad: cmp(y, imm()); return;
        case 0xc8: cmp(x, imm()); return;
        case 0x1f: { const t = (imm16() + x) & 0xffff; pc = rd(t) | (rd((t + 1) & 0xffff) << 8); return; }
        case 0x5f: pc = imm16(); return;
        case 0x3f: { const t = imm16(); push(pc >> 8); push(pc & 0xff); pc = t; return; }
        case 0x4f: { const t = 0xff00 | imm(); push(pc >> 8); push(pc & 0xff); pc = t; return; }
        case 0x6f: { const l = pop(); pc = l | (pop() << 8); return; }
        case 0x7f: { psw = pop(); const l = pop(); pc = l | (pop() << 8); return; }
        case 0x20: psw &= ~P; return;
        case 0x40: psw |= P; return;
        case 0x60: psw &= ~C; return;
        case 0x80: psw |= C; return;
        case 0xa0: psw |= I; return;
        case 0xc0: psw &= ~I; return;
        case 0xe0: psw &= ~(V | H); return;
        case 0xed: psw ^= C; return;
        case 0x2e: { const v = rd(dp(imm())); branch(a !== v); return; }
        case 0xde: { const v = rd(dp(imm() + x)); branch(a !== v); return; }
        case 0x6e: { const d = dp(imm()); const v = (rd(d) - 1) & 0xff; wr(d, v); branch(v !== 0); return; }
        case 0xfe: y = (y - 1) & 0xff; branch(y !== 0); return;
        case 0x5d: x = nz(a); return;
        case 0x7d: a = nz(x); return;
        case 0x9d: x = nz(sp); return;
        case 0xbd: sp = x; return;
        case 0xdd: a = nz(y); return;
        case 0xfd: y = nz(a); return;
        case 0x8d: y = nz(imm()); return;
        case 0xcd: x = nz(imm()); return;
        case 0xe8: a = nz(imm()); return;
        case 0x8f: { const v = imm(); const d = dp(imm()); rd(d); wr(d, v); return; }
        case 0xfa: { const v = rd(dp(imm())); wr(dp(imm()), v); return; }
        case 0x9e: { // DIV YA, X
          const ya = (y << 8) | a;
          psw &= ~(V | H);
          if ((x & 15) <= (y & 15)) psw |= H;
          if (y >= x) psw |= V;
          if (y < (x << 1)) { a = Math.floor(ya / x) & 0xff; y = (ya % x) & 0xff; }
          else { const t = ya - (x << 9); a = (255 - Math.floor(t / (256 - x))) & 0xff; y = (x + (t % (256 - x))) & 0xff; }
          nz(a); return;
        }
        case 0xcf: { const r = y * a; a = r & 0xff; y = r >> 8; nz(y); return; }
        case 0x9f: a = nz((a >> 4) | (a << 4)); return;
        case 0xbe: { // DAS
          if (!(psw & C) || a > 0x99) { a = (a - 0x60) & 0xff; psw &= ~C; }
          if (!(psw & H) || (a & 15) > 9) a = (a - 6) & 0xff;
          nz(a); return;
        }
        case 0xdf: { // DAA
          if ((psw & C) || a > 0x99) { a = (a + 0x60) & 0xff; psw |= C; }
          if ((psw & H) || (a & 15) > 9) a = (a + 6) & 0xff;
          nz(a); return;
        }
        case 0xaf: wr(dp(x), a); x = (x + 1) & 0xff; return;
        case 0xbf: a = nz(rd(dp(x))); x = (x + 1) & 0xff; return;
        case 0xc4: wr(dp(imm()), a); return;
        case 0xc5: wr(imm16(), a); return;
        case 0xc6: wr(dp(x), a); return;
        case 0xc7: { const o = (imm() + x) & 0xff; wr(rd16dp(o), a); return; }
        case 0xc9: wr(imm16(), x); return;
        case 0xcb: wr(dp(imm()), y); return;
        case 0xcc: wr(imm16(), y); return;
        case 0xd4: wr(dp(imm() + x), a); return;
        case 0xd5: wr((imm16() + x) & 0xffff, a); return;
        case 0xd6: wr((imm16() + y) & 0xffff, a); return;
        case 0xd7: { const o = imm(); wr((rd16dp(o) + y) & 0xffff, a); return; }
        case 0xd8: wr(dp(imm()), x); return;
        case 0xd9: wr(dp(imm() + y), x); return;
        case 0xdb: wr(dp(imm() + x), y); return;
        case 0xe4: a = nz(rd(dp(imm()))); return;
        case 0xe5: a = nz(rd(imm16())); return;
        case 0xe6: a = nz(rd(dp(x))); return;
        case 0xe7: { const o = (imm() + x) & 0xff; a = nz(rd(rd16dp(o))); return; }
        case 0xe9: x = nz(rd(imm16())); return;
        case 0xeb: y = nz(rd(dp(imm()))); return;
        case 0xec: y = nz(rd(imm16())); return;
        case 0xf4: a = nz(rd(dp(imm() + x))); return;
        case 0xf5: a = nz(rd((imm16() + x) & 0xffff)); return;
        case 0xf6: a = nz(rd((imm16() + y) & 0xffff)); return;
        case 0xf7: { const o = imm(); a = nz(rd((rd16dp(o) + y) & 0xffff)); return; }
        case 0xf8: x = nz(rd(dp(imm()))); return;
        case 0xf9: x = nz(rd(dp(imm() + y))); return;
        case 0xfb: y = nz(rd(dp(imm() + x))); return;
        case 0xef: case 0xff: stopped = true; return;
      }
    }

    this.step = function () {
      if (stopped) return 2;
      const op = imm();
      extra = 0;
      exec(op);
      return CYC[op] + extra;
    };
    this.reset = function () { a = x = y = 0; sp = 0xef; psw = 0x02; stopped = false; pc = 0xffc0; };
    this.getState = () => [a, x, y, sp, pc, psw, stopped ? 1 : 0];
    this.setState = (s) => { [a, x, y, sp, pc, psw] = s; stopped = !!s[6]; };
    this.regs = () => ({ a, x, y, sp, pc, psw });
    this.setPC = (v) => { pc = v & 0xffff; };
    this.getPC = () => pc;
    this.setRegs = (o) => { if ("a" in o) a = o.a; if ("x" in o) x = o.x; if ("y" in o) y = o.y; if ("sp" in o) sp = o.sp; if ("psw" in o) psw = o.psw; if ("pc" in o) pc = o.pc; stopped = false; };
    this.reset();
  }
  global.RPSPC700 = RPSPC700;
})(typeof window !== "undefined" ? window : globalThis);
