// RetroPixels — Konami 053248 ("Konami-2") processor, used by The Simpsons
// arcade board. Original code, written from the documented behaviour of the chip.
//
// The 053248 is Konami's own member of the Motorola 6809 family: the same
// registers (A, B / D, X, Y, U, S, PC, DP, CC with E F H I N Z V C) and the same
// three interrupts (NMI, FIRQ, IRQ with the 6809's vectors at FFF6-FFFF), but a
// different opcode map and extra instructions:
//   - every memory operand uses one "index" postbyte, which also covers direct
//     page and extended addresses, so each instruction only has an immediate
//     form and an indexed form;
//   - block instructions: MOVE / BMOVE (copy from Y to X, U bytes), BSET / BSETW
//     (fill U bytes / words at X with A / D), DECB,JNZ and DECX,JNZ loops;
//   - 16-bit maths: LMUL (X:Y = X * Y), DIVX (X = X / B, B = remainder), shifts
//     of D by a count, 16-bit memory shifts, CLR/NEG/INC/DEC/TST of D and of
//     memory words, ABSA / ABSB / ABSD;
//   - SETLINES, which puts a byte on the chip's spare output lines (boards use
//     them to switch ROM banks).
//
// Postbyte of an indexed operand:
//   0x07 extended address (16 bits follow)      0x0f the same, indirect
//   0xc4 direct page (8 bits follow)             0xcc the same, indirect
//   0rrr i ttt  register r (2 X, 3 Y, 5 U, 6 S, 7 PC), i = indirect, ttt:
//       0 ,R+   1 ,R++   2 ,-R   3 ,--R   4 n8,R   5 n16,R   6 ,R
//   1rrr i ttt  with ttt 0 A,R   1 B,R   7 D,R   (signed accumulator offsets)
// TFR/EXG register numbers: 0 A, 1 B, 2 X, 3 Y, 4 S, 5 U (others read 0xff). Unlike the
// 6809, TFR copies the register in the postbyte's LOW nibble into the one in its high nibble.
//
// Interface:
//   const cpu = new RPKonami2({ read(a), write(a, v), setLines(v) });
//   cpu.reset();  cpu.step() -> clock cycles used;  cpu.irq / cpu.firq = true/false
//   (level-sensitive lines);  cpu.nmi() (edge);  cpu.getState() / cpu.setState(o).
//   An optional bus.ack(kind) is called when an interrupt is taken ("irq", "firq", "nmi").
"use strict";

(function (global) {
  const CF = 0x01, VF = 0x02, ZF = 0x04, NF = 0x08, IF = 0x10, HF = 0x20, FF = 0x40, EF = 0x80;
  const s8 = (v) => (v << 24) >> 24;
  const s16 = (v) => (v << 16) >> 16;

  // base cycles of each opcode (indexed forms add their addressing time); rough 6809-like timing
  const CYC = new Uint8Array(256).fill(2);
  (function () {
    const set = (ops, n) => { for (const o of ops) CYC[o] = n; };
    for (let o = 0x08; o <= 0x0b; o++) CYC[o] = 4;          // LEA
    set([0x0c, 0x0d, 0x0e, 0x0f], 5);                        // PSH/PUL (+1 per byte)
    for (let o = 0x10; o <= 0x3b; o++) CYC[o] = (o & 2) ? 4 : 2; // 8-bit ops: imm 2, idx 4
    set([0x38], 2); set([0x39], 4);
    set([0x3c, 0x3d], 3); set([0x3e], 8); set([0x3f], 6);
    for (let o = 0x40; o <= 0x53; o++) CYC[o] = (o & 1) ? 5 : 3; // 16-bit imm 3, idx 5
    set([0x54, 0x56], 4); set([0x55, 0x57], 6);
    for (let o = 0x58; o <= 0x5c; o++) CYC[o] = 5;
    for (let o = 0x60; o <= 0x67; o++) { CYC[o] = 3; CYC[o + 0x10] = 3; CYC[o + 8] = 5; CYC[o + 0x18] = 5; }
    for (let o = 0x80; o <= 0xa7; o++) CYC[o] = 2;
    set([0x82, 0x85, 0x88, 0x8b, 0x8e, 0x92, 0x95, 0x98, 0x9b, 0x9e, 0xa2], 6);
    set([0xa3, 0xa4, 0xa5, 0xa6, 0xa7], 8);
    set([0x8f], 5); set([0x9f], 6);
    set([0xa8], 3); set([0xa9], 7); set([0xaa], 7); set([0xab], 9); set([0xac, 0xad], 3);
    set([0xb0], 3); set([0xb3], 11); set([0xb4], 18); set([0xb5], 20); set([0xb6], 2); set([0xb7], 2);
    for (let o = 0xb8; o <= 0xc1; o++) CYC[o] = (o & 1) ? 6 : 4;
    set([0xc2, 0xc4, 0xc6, 0xc8, 0xca], 3); set([0xc3, 0xc5, 0xc7, 0xc9, 0xcb], 7);
    set([0xcc, 0xcd, 0xce], 3); set([0xcf, 0xd0], 2);
  })();

  class RPKonami2 {
    constructor(bus) {
      this.bus = bus;
      this.a = 0; this.b = 0; this.x = 0; this.y = 0; this.u = 0; this.s = 0; this.pc = 0; this.dp = 0; this.cc = 0;
      this.irq = false; this.firq = false;
      this.nmiPending = false; this.nmiArmed = false;
      this.cycles = 0;   // extra cycles of the instruction being run
      this.lines = 0;    // last SETLINES value
    }

    // ---- bus ----
    rd(a) { return this.bus.read(a & 0xffff) & 0xff; }
    wr(a, v) { this.bus.write(a & 0xffff, v & 0xff); }
    rd16(a) { return (this.rd(a) << 8) | this.rd(a + 1); }
    wr16(a, v) { this.wr(a, v >> 8); this.wr(a + 1, v); }
    imm8() { const v = this.rd(this.pc); this.pc = (this.pc + 1) & 0xffff; return v; }
    imm16() { const v = this.rd16(this.pc); this.pc = (this.pc + 2) & 0xffff; return v; }
    get d() { return (this.a << 8) | this.b; }
    set d(v) { this.a = (v >> 8) & 0xff; this.b = v & 0xff; }

    push8S(v) { this.s = (this.s - 1) & 0xffff; this.wr(this.s, v); }
    push16S(v) { this.push8S(v); this.push8S(v >> 8); }
    pull8S() { const v = this.rd(this.s); this.s = (this.s + 1) & 0xffff; return v; }
    pull16S() { const h = this.pull8S(); return (h << 8) | this.pull8S(); }

    reset() {
      this.dp = 0; this.cc = IF | FF;
      this.nmiPending = false; this.nmiArmed = false;
      this.pc = this.rd16(0xfffe);
      this.lines = 0;
    }
    nmi() { this.nmiPending = true; }

    // ---- index postbyte -> effective address ----
    ireg(r) { return r === 2 ? this.x : r === 3 ? this.y : r === 5 ? this.u : r === 6 ? this.s : this.pc; }
    setIreg(r, v) {
      v &= 0xffff;
      if (r === 2) this.x = v; else if (r === 3) this.y = v; else if (r === 5) this.u = v; else if (r === 6) this.s = v;
      else this.pc = v;
    }
    ea() {
      const pb = this.imm8();
      let addr;
      if (pb === 0x07 || pb === 0x0f) { addr = this.imm16(); this.cycles += 2; }
      else if (pb === 0xc4 || pb === 0xcc) { addr = (this.dp << 8) | this.imm8(); this.cycles += 1; }
      else {
        const r = (pb >> 4) & 7, t = pb & 7;
        if (pb & 0x80) {
          const off = t === 0 ? s8(this.a) : t === 1 ? s8(this.b) : t === 7 ? s16(this.d) : 0;
          addr = this.ireg(r) + off; this.cycles += t === 7 ? 4 : 1;
        } else switch (t) {
          case 0: addr = this.ireg(r); this.setIreg(r, addr + 1); this.cycles += 2; break;
          case 1: addr = this.ireg(r); this.setIreg(r, addr + 2); this.cycles += 3; break;
          case 2: addr = (this.ireg(r) - 1) & 0xffff; this.setIreg(r, addr); this.cycles += 2; break;
          case 3: addr = (this.ireg(r) - 2) & 0xffff; this.setIreg(r, addr); this.cycles += 3; break;
          case 4: { const o = s8(this.imm8()); addr = this.ireg(r) + o; this.cycles += 1; break; }
          case 5: { const o = s16(this.imm16()); addr = this.ireg(r) + o; this.cycles += 4; break; }
          default: addr = this.ireg(r); break;
        }
      }
      addr &= 0xffff;
      if (pb & 0x08) { addr = this.rd16(addr); this.cycles += 3; }
      return addr;
    }

    // ---- flags / maths ----
    nz8(r) { this.cc = (this.cc & ~(NF | ZF)) | (r & 0x80 ? NF : 0) | ((r & 0xff) ? 0 : ZF); }
    nz16(r) { this.cc = (this.cc & ~(NF | ZF)) | (r & 0x8000 ? NF : 0) | ((r & 0xffff) ? 0 : ZF); }
    add8(a, b, c) {
      const r = a + b + c;
      this.cc = (this.cc & ~(HF | NF | ZF | VF | CF)) | ((a ^ b ^ r) & 0x10 ? HF : 0) | (r & 0x80 ? NF : 0) |
        ((r & 0xff) ? 0 : ZF) | ((a ^ r) & (b ^ r) & 0x80 ? VF : 0) | (r & 0x100 ? CF : 0);
      return r & 0xff;
    }
    sub8(a, b, c) {
      const r = a - b - c;
      this.cc = (this.cc & ~(NF | ZF | VF | CF)) | (r & 0x80 ? NF : 0) | ((r & 0xff) ? 0 : ZF) |
        ((a ^ b) & (a ^ r) & 0x80 ? VF : 0) | (r & 0x100 ? CF : 0);
      return r & 0xff;
    }
    add16(a, b) {
      const r = a + b;
      this.cc = (this.cc & ~(NF | ZF | VF | CF)) | (r & 0x8000 ? NF : 0) | ((r & 0xffff) ? 0 : ZF) |
        ((a ^ r) & (b ^ r) & 0x8000 ? VF : 0) | (r & 0x10000 ? CF : 0);
      return r & 0xffff;
    }
    sub16(a, b) {
      const r = a - b;
      this.cc = (this.cc & ~(NF | ZF | VF | CF)) | (r & 0x8000 ? NF : 0) | ((r & 0xffff) ? 0 : ZF) |
        ((a ^ b) & (a ^ r) & 0x8000 ? VF : 0) | (r & 0x10000 ? CF : 0);
      return r & 0xffff;
    }
    logic8(r) { this.nz8(r); this.cc &= ~VF; return r & 0xff; }
    logic16(r) { this.nz16(r); this.cc &= ~VF; return r & 0xffff; }
    // one-operand ops; w = 8 or 16 bits
    unary(kind, v, w) {
      const top = w === 8 ? 0x80 : 0x8000, mask = w === 8 ? 0xff : 0xffff;
      const c = this.cc & CF;
      let r, cc = this.cc & ~(NF | ZF | VF);
      switch (kind) {
        case "clr": r = 0; cc &= ~CF; break;
        case "com": r = ~v & mask; cc |= CF; break;
        case "neg": r = (-v) & mask; if (v === top) cc |= VF; cc = v ? cc | CF : cc & ~CF; break;
        case "inc": r = (v + 1) & mask; if (r === top) cc |= VF; break;
        case "dec": r = (v - 1) & mask; if (v === top) cc |= VF; break;
        case "tst": r = v; break;
        case "lsr": r = v >>> 1; cc = (cc & ~CF) | (v & 1); break;
        case "ror": r = (v >>> 1) | (c ? top : 0); cc = (cc & ~CF) | (v & 1); break;
        case "asr": r = (v >>> 1) | (v & top); cc = (cc & ~CF) | (v & 1); break;
        case "asl": r = (v << 1) & mask; cc = (cc & ~CF) | (v & top ? CF : 0); if (((v << 1) ^ v) & top) cc |= VF; break;
        case "rol": r = ((v << 1) | c) & mask; cc = (cc & ~CF) | (v & top ? CF : 0); if (((v << 1) ^ v) & top) cc |= VF; break;
        case "abs": if (v & top) { r = (-v) & mask; if (v === top) cc |= VF; } else r = v; break;
      }
      cc |= (r & top ? NF : 0) | (r ? 0 : ZF);
      this.cc = cc;
      return r;
    }
    rmw8(kind) { const a = this.ea(); this.wr(a, this.unary(kind, this.rd(a), 8)); }
    rmw16(kind) { const a = this.ea(); const r = this.unary(kind, this.rd16(a), 16); if (kind !== "tst") this.wr16(a, r); }
    shiftD(kind, n) {
      let v = this.d;
      for (let i = 0; i < n; i++) v = this.unary(kind, v, 16);
      if (!n) this.nz16(v);
      this.d = v;
      this.cycles += n;
    }

    // 8-bit register operand helpers: imm or indexed byte
    op8(ix) { return ix ? this.rd(this.ea()) : this.imm8(); }
    op16(ix) { return ix ? this.rd16(this.ea()) : this.imm16(); }

    cond(c) {
      const cc = this.cc, n = (cc >> 3) & 1, z = (cc >> 2) & 1, v = (cc >> 1) & 1, cy = cc & 1;
      switch (c) {
        case 0: return true;                 // BRA
        case 1: return !(cy | z);            // BHI
        case 2: return !cy;                  // BCC
        case 3: return !z;                   // BNE
        case 4: return !v;                   // BVC
        case 5: return !n;                   // BPL
        case 6: return n === v;              // BGE
        default: return !z && n === v;       // BGT
      }
    }

    regGet(r) {
      switch (r & 7) { case 0: return this.a; case 1: return this.b; case 2: return this.x; case 3: return this.y;
        case 4: return this.s; case 5: return this.u; default: return 0xff; }
    }
    regSet(r, v) {
      switch (r & 7) { case 0: this.a = v & 0xff; break; case 1: this.b = v & 0xff; break; case 2: this.x = v & 0xffff; break;
        case 3: this.y = v & 0xffff; break; case 4: this.s = v & 0xffff; break; case 5: this.u = v & 0xffff; break; }
    }

    // push/pull with the 6809's postbyte bits (CC A B DP X Y U/S PC)
    pushRegs(useU, mask) {
      let sp = useU ? this.u : this.s;
      const p8 = (v) => { sp = (sp - 1) & 0xffff; this.wr(sp, v); this.cycles++; };
      const p16 = (v) => { p8(v); p8(v >> 8); };
      if (mask & 0x80) p16(this.pc);
      if (mask & 0x40) p16(useU ? this.s : this.u);
      if (mask & 0x20) p16(this.y);
      if (mask & 0x10) p16(this.x);
      if (mask & 0x08) p8(this.dp);
      if (mask & 0x04) p8(this.b);
      if (mask & 0x02) p8(this.a);
      if (mask & 0x01) p8(this.cc);
      if (useU) this.u = sp; else this.s = sp;
    }
    pullRegs(useU, mask) {
      let sp = useU ? this.u : this.s;
      const q8 = () => { const v = this.rd(sp); sp = (sp + 1) & 0xffff; this.cycles++; return v; };
      const q16 = () => { const h = q8(); return (h << 8) | q8(); };
      if (mask & 0x01) this.cc = q8();
      if (mask & 0x02) this.a = q8();
      if (mask & 0x04) this.b = q8();
      if (mask & 0x08) this.dp = q8();
      if (mask & 0x10) this.x = q16();
      if (mask & 0x20) this.y = q16();
      if (mask & 0x40) { const v = q16(); if (useU) this.s = v; else this.u = v; }
      if (mask & 0x80) this.pc = q16();
      if (useU) this.u = sp; else this.s = sp;
    }

    interrupt(vec, full, mask) {
      if (full) { this.cc |= EF; this.pushRegs(false, 0xff); }
      else { this.cc &= ~EF; this.pushRegs(false, 0x81); }
      this.cc |= mask;
      this.pc = this.rd16(vec);
    }

    // Run one instruction (or take an interrupt). Returns the clock cycles used.
    step() {
      this.cycles = 0;
      if (this.nmiPending) {
        this.nmiPending = false;
        this.interrupt(0xfffc, true, IF | FF);
        if (this.bus.ack) this.bus.ack("nmi");
        return this.cycles + 7;
      }
      if (this.firq && !(this.cc & FF)) {
        this.interrupt(0xfff6, false, IF | FF);
        if (this.bus.ack) this.bus.ack("firq");
        return this.cycles + 7;
      }
      if (this.irq && !(this.cc & IF)) {
        this.interrupt(0xfff8, true, IF);
        if (this.bus.ack) this.bus.ack("irq");
        return this.cycles + 7;
      }
      const op = this.imm8();
      this.exec(op);
      return CYC[op] + this.cycles;
    }

    exec(op) {
      // 0x10-0x37: 8-bit A/B operations, in groups of four: A imm, B imm, A idx, B idx
      if (op >= 0x10 && op <= 0x37) {
        const ix = op & 2, isB = op & 1, g = (op - 0x10) >> 2;
        const v = this.op8(ix), r = isB ? this.b : this.a;
        let out = r, keep = true;
        switch (g) {
          case 0: out = v; this.logic8(v); break;                         // LD
          case 1: out = this.add8(r, v, 0); break;                        // ADD
          case 2: out = this.add8(r, v, this.cc & CF); break;             // ADC
          case 3: out = this.sub8(r, v, 0); break;                        // SUB
          case 4: out = this.sub8(r, v, this.cc & CF); break;             // SBC
          case 5: out = this.logic8(r & v); break;                        // AND
          case 6: this.logic8(r & v); keep = false; break;                // BIT
          case 7: out = this.logic8(r ^ v); break;                        // EOR
          case 8: out = this.logic8(r | v); break;                        // OR
          case 9: this.sub8(r, v, 0); keep = false; break;                // CMP
        }
        if (keep) { if (isB) this.b = out; else this.a = out; }
        return;
      }
      // 0x40-0x53: 16-bit loads and compares, imm / idx pairs
      if (op >= 0x40 && op <= 0x53) {
        const v = this.op16(op & 1), g = (op - 0x40) >> 1;
        switch (g) {
          case 0: this.d = this.logic16(v); break;
          case 1: this.x = this.logic16(v); break;
          case 2: this.y = this.logic16(v); break;
          case 3: this.u = this.logic16(v); break;
          case 4: this.s = this.logic16(v); this.nmiArmed = true; break;
          case 5: this.sub16(this.d, v); break;
          case 6: this.sub16(this.x, v); break;
          case 7: this.sub16(this.y, v); break;
          case 8: this.sub16(this.u, v); break;
          case 9: this.sub16(this.s, v); break;
        }
        return;
      }
      // 0x60-0x7f: short and long branches
      if (op >= 0x60 && op <= 0x7f) {
        const long = op & 8, c = op & 7, neg = op & 0x10;
        const off = long ? s16(this.imm16()) : s8(this.imm8());
        let t = this.cond(c);
        if (neg) t = !t;
        if (t) { this.pc = (this.pc + off) & 0xffff; if (long) this.cycles++; }
        return;
      }
      switch (op) {
        case 0x08: this.x = this.ea(); this.cc = (this.cc & ~ZF) | (this.x ? 0 : ZF); break;   // LEAX
        case 0x09: this.y = this.ea(); this.cc = (this.cc & ~ZF) | (this.y ? 0 : ZF); break;   // LEAY
        case 0x0a: this.u = this.ea(); break;                                                // LEAU
        case 0x0b: this.s = this.ea(); this.nmiArmed = true; break;                          // LEAS
        case 0x0c: this.pushRegs(false, this.imm8()); break;  // PSHS
        case 0x0d: this.pushRegs(true, this.imm8()); break;   // PSHU
        case 0x0e: this.pullRegs(false, this.imm8()); break;  // PULS
        case 0x0f: this.pullRegs(true, this.imm8()); break;   // PULU
        case 0x38: case 0x39: { const v = this.op8(op & 1); this.lines = v; if (this.bus.setLines) this.bus.setLines(v); break; } // SETLINES
        case 0x3a: this.logic8(this.a); this.wr(this.ea(), this.a); break;  // STA
        case 0x3b: this.logic8(this.b); this.wr(this.ea(), this.b); break;  // STB
        case 0x3c: this.cc &= this.imm8(); break;   // ANDCC
        case 0x3d: this.cc |= this.imm8(); break;   // ORCC
        case 0x3e: { const p = this.imm8(), r1 = p >> 4, r2 = p & 15, v1 = this.regGet(r1), v2 = this.regGet(r2);
          this.regSet(r1, v2); this.regSet(r2, v1); break; }   // EXG
        case 0x3f: { const p = this.imm8(); this.regSet(p >> 4, this.regGet(p & 15)); break; } // TFR: low nibble is the source, high the destination
        case 0x54: case 0x55: this.d = this.add16(this.d, this.op16(op & 1)); break;  // ADDD
        case 0x56: case 0x57: this.d = this.sub16(this.d, this.op16(op & 1)); break;  // SUBD
        case 0x58: { const a = this.ea(); this.logic16(this.d); this.wr16(a, this.d); break; }  // STD
        case 0x59: { const a = this.ea(); this.logic16(this.x); this.wr16(a, this.x); break; }  // STX
        case 0x5a: { const a = this.ea(); this.logic16(this.y); this.wr16(a, this.y); break; }  // STY
        case 0x5b: { const a = this.ea(); this.logic16(this.u); this.wr16(a, this.u); break; }  // STU
        case 0x5c: { const a = this.ea(); this.logic16(this.s); this.wr16(a, this.s); break; }  // STS

        // 8-bit one-operand ops on A, B and memory
        case 0x80: this.a = this.unary("clr", this.a, 8); break;
        case 0x81: this.b = this.unary("clr", this.b, 8); break;
        case 0x82: this.rmw8("clr"); break;
        case 0x83: this.a = this.unary("com", this.a, 8); break;
        case 0x84: this.b = this.unary("com", this.b, 8); break;
        case 0x85: this.rmw8("com"); break;
        case 0x86: this.a = this.unary("neg", this.a, 8); break;
        case 0x87: this.b = this.unary("neg", this.b, 8); break;
        case 0x88: this.rmw8("neg"); break;
        case 0x89: this.a = this.unary("inc", this.a, 8); break;
        case 0x8a: this.b = this.unary("inc", this.b, 8); break;
        case 0x8b: this.rmw8("inc"); break;
        case 0x8c: this.a = this.unary("dec", this.a, 8); break;
        case 0x8d: this.b = this.unary("dec", this.b, 8); break;
        case 0x8e: this.rmw8("dec"); break;
        case 0x8f: this.pc = this.pull16S(); break;   // RTS
        case 0x90: this.unary("tst", this.a, 8); break;
        case 0x91: this.unary("tst", this.b, 8); break;
        case 0x92: this.unary("tst", this.rd(this.ea()), 8); break;
        case 0x93: this.a = this.unary("lsr", this.a, 8); break;
        case 0x94: this.b = this.unary("lsr", this.b, 8); break;
        case 0x95: this.rmw8("lsr"); break;
        case 0x96: this.a = this.unary("ror", this.a, 8); break;
        case 0x97: this.b = this.unary("ror", this.b, 8); break;
        case 0x98: this.rmw8("ror"); break;
        case 0x99: this.a = this.unary("asr", this.a, 8); break;
        case 0x9a: this.b = this.unary("asr", this.b, 8); break;
        case 0x9b: this.rmw8("asr"); break;
        case 0x9c: this.a = this.unary("asl", this.a, 8); break;
        case 0x9d: this.b = this.unary("asl", this.b, 8); break;
        case 0x9e: this.rmw8("asl"); break;
        case 0x9f: // RTI
          this.cc = this.pull8S();
          if (this.cc & EF) { this.pullRegs(false, 0xfe); } else this.pc = this.pull16S();
          break;
        case 0xa0: this.a = this.unary("rol", this.a, 8); break;
        case 0xa1: this.b = this.unary("rol", this.b, 8); break;
        case 0xa2: this.rmw8("rol"); break;
        case 0xa3: this.rmw16("lsr"); break;   // LSRW
        case 0xa4: this.rmw16("ror"); break;   // RORW
        case 0xa5: this.rmw16("asr"); break;   // ASRW
        case 0xa6: this.rmw16("asl"); break;   // ASLW
        case 0xa7: this.rmw16("rol"); break;   // ROLW
        case 0xa8: this.pc = this.ea(); break; // JMP
        case 0xa9: { const a = this.ea(); this.push16S(this.pc); this.pc = a; break; }  // JSR
        case 0xaa: { const o = s8(this.imm8()); this.push16S(this.pc); this.pc = (this.pc + o) & 0xffff; break; }  // BSR
        case 0xab: { const o = s16(this.imm16()); this.push16S(this.pc); this.pc = (this.pc + o) & 0xffff; break; } // LBSR
        case 0xac: { const o = s8(this.imm8()); this.b = this.unary("dec", this.b, 8); if (this.b) this.pc = (this.pc + o) & 0xffff; break; } // DECB,JNZ
        case 0xad: { const o = s8(this.imm8()); this.x = (this.x - 1) & 0xffff; this.nz16(this.x); if (this.x) this.pc = (this.pc + o) & 0xffff; break; } // DECX,JNZ
        case 0xae: break; // NOP
        case 0xb0: this.x = (this.x + this.b) & 0xffff; break; // ABX
        case 0xb1: { // DAA
          const a = this.a, c = this.cc & CF;
          let adj = 0, cy = c;
          if ((this.cc & HF) || (a & 0x0f) > 9) adj |= 0x06;
          if (c || (a >> 4) > 9 || ((a >> 4) > 8 && (a & 0x0f) > 9)) { adj |= 0x60; cy = 1; }
          const r = a + adj;
          this.a = r & 0xff; this.nz8(this.a);
          this.cc = (this.cc & ~(CF | VF)) | (cy || (r & 0x100) ? CF : 0);
          break;
        }
        case 0xb2: this.a = (this.b & 0x80) ? 0xff : 0; this.nz16(this.d); break; // SEX
        case 0xb3: { const r = this.a * this.b; this.d = r; this.cc = (this.cc & ~(ZF | CF)) | (r ? 0 : ZF) | (r & 0x80 ? CF : 0); break; } // MUL
        case 0xb4: { // LMUL: X:Y = X * Y
          const r = this.x * this.y; // fits in 32 bits exactly
          this.x = Math.floor(r / 65536) & 0xffff; this.y = r & 0xffff;
          this.cc = (this.cc & ~(ZF | CF)) | (r ? 0 : ZF) | (r & 0x8000 ? CF : 0);
          break;
        }
        case 0xb5: { // DIVX: X = X / B, B = X % B
          const d = this.b;
          if (d) { const q = Math.floor(this.x / d), r = this.x % d; this.x = q & 0xffff; this.b = r; }
          else { this.x = 0; this.b = 0; }
          this.cc = (this.cc & ~(ZF | CF)) | (this.x ? 0 : ZF) | (this.x & 0x80 ? CF : 0);
          break;
        }
        case 0xb6: // BMOVE: copy U bytes from Y to X
          while (this.u) { this.wr(this.x, this.rd(this.y)); this.x = (this.x + 1) & 0xffff; this.y = (this.y + 1) & 0xffff; this.u = (this.u - 1) & 0xffff; this.cycles += 2; }
          break;
        case 0xb7: // MOVE: one byte of the same
          this.wr(this.x, this.rd(this.y)); this.x = (this.x + 1) & 0xffff; this.y = (this.y + 1) & 0xffff; this.u = (this.u - 1) & 0xffff;
          break;
        case 0xb8: case 0xb9: this.shiftD("lsr", this.op8(op & 1)); break; // LSRD
        case 0xba: case 0xbb: this.shiftD("ror", this.op8(op & 1)); break; // RORD
        case 0xbc: case 0xbd: this.shiftD("asr", this.op8(op & 1)); break; // ASRD
        case 0xbe: case 0xbf: this.shiftD("asl", this.op8(op & 1)); break; // ASLD
        case 0xc0: case 0xc1: this.shiftD("rol", this.op8(op & 1)); break; // ROLD
        case 0xc2: this.d = this.unary("clr", this.d, 16); break;  // CLRD
        case 0xc3: this.rmw16("clr"); break;                       // CLRW
        case 0xc4: this.d = this.unary("neg", this.d, 16); break;  // NEGD
        case 0xc5: this.rmw16("neg"); break;                       // NEGW
        case 0xc6: this.d = this.unary("inc", this.d, 16); break;  // INCD
        case 0xc7: this.rmw16("inc"); break;                       // INCW
        case 0xc8: this.d = this.unary("dec", this.d, 16); break;  // DECD
        case 0xc9: this.rmw16("dec"); break;                       // DECW
        case 0xca: this.unary("tst", this.d, 16); break;           // TSTD
        case 0xcb: this.rmw16("tst"); break;                       // TSTW
        case 0xcc: this.a = this.unary("abs", this.a, 8); break;   // ABSA
        case 0xcd: this.b = this.unary("abs", this.b, 8); break;   // ABSB
        case 0xce: this.d = this.unary("abs", this.d, 16); break;  // ABSD
        case 0xcf: // BSET: fill U bytes at X with A
          while (this.u) { this.wr(this.x, this.a); this.x = (this.x + 1) & 0xffff; this.u = (this.u - 1) & 0xffff; this.cycles += 2; }
          break;
        case 0xd0: // BSETW: fill U words at X with D
          while (this.u) { this.wr16(this.x, this.d); this.x = (this.x + 2) & 0xffff; this.u = (this.u - 1) & 0xffff; this.cycles += 3; }
          break;
        default: break; // undefined opcodes do nothing
      }
    }

    getState() {
      return [this.a, this.b, this.x, this.y, this.u, this.s, this.pc, this.dp, this.cc,
        this.irq ? 1 : 0, this.firq ? 1 : 0, this.nmiPending ? 1 : 0, this.nmiArmed ? 1 : 0, this.lines];
    }
    setState(s) {
      [this.a, this.b, this.x, this.y, this.u, this.s, this.pc, this.dp, this.cc] = s;
      this.irq = !!s[9]; this.firq = !!s[10]; this.nmiPending = !!s[11]; this.nmiArmed = !!s[12]; this.lines = s[13] | 0;
    }
  }

  global.RPKonami2 = RPKonami2;
})(typeof window !== "undefined" ? window : globalThis);
