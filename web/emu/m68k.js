// RetroPixels — Motorola 68000 processor (used by the Sega Genesis emulator).
//
// Written from the 68000's documented behaviour; original code.
// Every bus access costs 4 clock cycles and each instruction adds its internal
// cycles, so instruction timing matches the processor's published timing tables.
// The opcode table is built once: each of the 65,536 opcodes gets a small
// function with its operands already decoded.
"use strict";

(function (global) {
  const MASK = [0, 0xff, 0xffff, 0, 0xffffffff];
  const BITS = [0, 8, 16, 0, 32];

  const sx8 = (v) => (v << 24) >> 24;
  const sx16 = (v) => (v << 16) >> 16;
  const clip = (v, sz) => (sz === 4 ? v >>> 0 : v & MASK[sz]);
  const msb = (v, sz) => (sz === 4 ? v >>> 31 : (v >> (BITS[sz] - 1)) & 1);
  const popcount16 = (v) => { let n = 0; while (v) { n += v & 1; v >>>= 1; } return n; };

  // Which addressing modes an instruction accepts.
  //  mode 0 Dn, 1 An, 2 (An), 3 (An)+, 4 -(An), 5 d16(An), 6 d8(An,Xn),
  //  7.0 abs.W, 7.1 abs.L, 7.2 d16(PC), 7.3 d8(PC,Xn), 7.4 #imm
  function eaOk(mode, reg, cls) {
    if (mode === 7 && reg > 4) return false;
    const isPC = mode === 7 && (reg === 2 || reg === 3), isImm = mode === 7 && reg === 4;
    switch (cls) {
      case "all": return true;
      case "data": return mode !== 1;
      case "mem": return mode >= 2;
      case "alt": return !isPC && !isImm;
      case "dataAlt": return mode !== 1 && !isPC && !isImm;
      case "memAlt": return mode >= 2 && !isPC && !isImm;
      case "ctrl": return mode === 2 || mode === 5 || mode === 6 || (mode === 7 && reg <= 3);
      case "ctrlAlt": return mode === 2 || mode === 5 || mode === 6 || (mode === 7 && reg <= 1);
      case "ctrlPostinc": return mode === 2 || mode === 3 || mode === 5 || mode === 6 || (mode === 7 && reg <= 3);
    }
    return false;
  }

  class M68K {
    // bus: read8(a), read16(a), write8(a, v), write16(a, v), and optionally
    //   ack(level) -> vector number (autovector when it returns -1 / undefined),
    //   reset() for the RESET instruction.
    constructor(bus) {
      this.bus = bus;
      this.d = new Uint32Array(8);
      this.a = new Uint32Array(8);
      this.pc = 0; this.usp = 0; this.ssp = 0;
      this.s = 1; this.t = 0; this.ipl = 7;
      this.x = 0; this.n = 0; this.z = 0; this.v = 0; this.c = 0;
      this.cycles = 0;
      this.stopped = false;
      this.irq = 0;           // interrupt level being requested (0 = none)
      this.nmiSeen = false;   // level 7 is edge-triggered
      this.tasWrites = true;  // the Genesis bus ignores TAS's write
      this.op = 0;            // opcode being executed
      this.opPC = 0;          // where it started
      if (!M68K.table) M68K.table = buildTable();
      this.table = M68K.table;
    }

    // ---- status register ----
    getCCR() { return (this.x << 4) | (this.n << 3) | (this.z << 2) | (this.v << 1) | this.c; }
    setCCR(v) { this.x = (v >> 4) & 1; this.n = (v >> 3) & 1; this.z = (v >> 2) & 1; this.v = (v >> 1) & 1; this.c = v & 1; }
    getSR() { return (this.t << 15) | (this.s << 13) | (this.ipl << 8) | this.getCCR(); }
    setSR(v) {
      const s = (v >> 13) & 1;
      if (s !== this.s) {
        if (this.s) { this.ssp = this.a[7]; this.a[7] = this.usp; }
        else { this.usp = this.a[7]; this.a[7] = this.ssp; }
        this.s = s;
      }
      this.t = (v >> 15) & 1; this.ipl = (v >> 8) & 7;
      this.setCCR(v & 0x1f);
    }

    // ---- bus (each access 4 cycles) ----
    r8(a) { this.cycles += 4; return this.bus.read8(a & 0xffffff); }
    r16(a) { this.cycles += 4; return this.bus.read16(a & 0xfffffe); }
    r32(a) { const hi = this.r16(a); return ((hi << 16) | this.r16(a + 2)) >>> 0; }
    w8(a, v) { this.cycles += 4; this.bus.write8(a & 0xffffff, v & 0xff); }
    w16(a, v) { this.cycles += 4; this.bus.write16(a & 0xfffffe, v & 0xffff); }
    w32(a, v) { this.w16(a, v >>> 16); this.w16(a + 2, v & 0xffff); }
    rm(a, sz) { return sz === 1 ? this.r8(a) : sz === 2 ? this.r16(a) : this.r32(a); }
    wm(a, sz, v) { if (sz === 1) this.w8(a, v); else if (sz === 2) this.w16(a, v); else this.w32(a, v); }
    fetch() { const v = this.r16(this.pc); this.pc = (this.pc + 2) >>> 0; return v; }
    fetch32() { const hi = this.fetch(); return ((hi << 16) | this.fetch()) >>> 0; }
    push16(v) { this.a[7] -= 2; this.w16(this.a[7], v); }
    push32(v) { this.a[7] -= 4; this.w32(this.a[7], v); }
    pop16() { const v = this.r16(this.a[7]); this.a[7] += 2; return v; }
    pop32() { const v = this.r32(this.a[7]); this.a[7] += 4; return v; }

    // ---- effective addresses ----
    index(base) {
      const ext = this.fetch();
      this.cycles += 2;
      const r = (ext >> 12) & 7;
      let idx = ext & 0x8000 ? this.a[r] : this.d[r];
      idx = ext & 0x800 ? idx | 0 : sx16(idx & 0xffff);
      return (base + idx + sx8(ext & 0xff)) >>> 0;
    }
    // Memory address of an operand (performs (An)+ / -(An) and reads extension words).
    addr(mode, reg, sz) {
      switch (mode) {
        case 2: return this.a[reg];
        case 3: { const a = this.a[reg]; this.a[reg] += sz === 1 && reg === 7 ? 2 : sz; return a; }
        case 4: this.cycles += 2; this.a[reg] -= sz === 1 && reg === 7 ? 2 : sz; return this.a[reg];
        case 5: { const base = this.a[reg]; return (base + sx16(this.fetch())) >>> 0; }
        case 6: return this.index(this.a[reg]);
        case 7:
          switch (reg) {
            case 0: return sx16(this.fetch()) >>> 0;
            case 1: return this.fetch32();
            case 2: { const base = this.pc; return (base + sx16(this.fetch())) >>> 0; }
            case 3: return this.index(this.pc);
          }
      }
      return 0;
    }
    read(mode, reg, sz) {
      if (mode === 0) return clip(this.d[reg], sz);
      if (mode === 1) return clip(this.a[reg], sz);
      if (mode === 7 && reg === 4) return sz === 1 ? this.fetch() & 0xff : sz === 2 ? this.fetch() : this.fetch32();
      return this.rm(this.addr(mode, reg, sz), sz);
    }
    setD(r, v, sz) {
      if (sz === 4) this.d[r] = v;
      else if (sz === 2) this.d[r] = (this.d[r] & 0xffff0000) | (v & 0xffff);
      else this.d[r] = (this.d[r] & 0xffffff00) | (v & 0xff);
    }
    // read-modify-write of an operand: fn(old) -> new (undefined = no write)
    rmw(mode, reg, sz, fn) {
      if (mode === 0) { const r = fn(clip(this.d[reg], sz)); if (r !== undefined) this.setD(reg, r, sz); return; }
      const a = this.addr(mode, reg, sz);
      const r = fn(this.rm(a, sz));
      if (r !== undefined) this.wm(a, sz, r);
    }

    // ---- flags ----
    nz(v, sz) { this.n = msb(v, sz); this.z = clip(v, sz) === 0 ? 1 : 0; }
    logic(v, sz) { this.nz(v, sz); this.v = 0; this.c = 0; }
    add(a, b, sz, x) {
      const r = a + b + x, m = clip(r, sz);
      const sa = msb(a, sz), sb = msb(b, sz), sr = msb(m, sz);
      this.c = r > MASK[sz] ? 1 : 0;
      this.v = sa === sb && sr !== sa ? 1 : 0;
      this.n = sr;
      return m;
    }
    sub(a, b, sz, x) { // a - b - x
      const r = a - b - x, m = clip(r, sz);
      const sa = msb(a, sz), sb = msb(b, sz), sr = msb(m, sz);
      this.c = r < 0 ? 1 : 0;
      this.v = sa !== sb && sr !== sa ? 1 : 0;
      this.n = sr;
      return m;
    }
    cond(cc) {
      switch (cc) {
        case 0: return true;
        case 1: return false;
        case 2: return !this.c && !this.z;          // HI
        case 3: return !!(this.c || this.z);        // LS
        case 4: return !this.c;                     // CC
        case 5: return !!this.c;                    // CS
        case 6: return !this.z;                     // NE
        case 7: return !!this.z;                    // EQ
        case 8: return !this.v;                     // VC
        case 9: return !!this.v;                    // VS
        case 10: return !this.n;                    // PL
        case 11: return !!this.n;                   // MI
        case 12: return this.n === this.v;          // GE
        case 13: return this.n !== this.v;          // LT
        case 14: return !this.z && this.n === this.v; // GT
        default: return !!this.z || this.n !== this.v; // LE
      }
    }

    // ---- exceptions ----
    exception(vec, pc) {
      const sr = this.getSR();
      if (!this.s) { this.usp = this.a[7]; this.a[7] = this.ssp; this.s = 1; }
      this.t = 0;
      this.push32(pc === undefined ? this.pc : pc);
      this.push16(sr);
      this.pc = this.r32(vec * 4);
    }
    trap(vec, extra) { this.cycles += extra; this.exception(vec); }
    privilege() { this.cycles += 10; this.exception(8, this.opPC); }
    illegal(vec) { this.cycles += 10; this.exception(vec, this.opPC); }

    setIRQ(level) { this.irq = level; if (level !== 7) this.nmiSeen = false; }
    reset() {
      this.s = 1; this.t = 0; this.ipl = 7; this.stopped = false;
      this.a[7] = this.r32(0); this.ssp = this.a[7];
      this.pc = this.r32(4);
    }

    checkInterrupt() {
      const lv = this.irq;
      if (!lv) return false;
      if (lv === 7) { if (this.nmiSeen) return false; this.nmiSeen = true; }
      else if (lv <= this.ipl) return false;
      this.stopped = false;
      const sr = this.getSR();
      if (!this.s) { this.usp = this.a[7]; this.a[7] = this.ssp; this.s = 1; }
      this.t = 0;
      this.ipl = lv;
      this.cycles += 24;
      let vec = this.bus.ack ? this.bus.ack(lv) : -1;
      if (vec === undefined || vec < 0) vec = 24 + lv;
      this.push32(this.pc);
      this.push16(sr);
      this.pc = this.r32(vec * 4);
      return true;
    }

    // Run one instruction (or an interrupt, or 4 idle cycles when stopped).
    step() {
      const start = this.cycles;
      if (this.checkInterrupt()) return this.cycles - start;
      if (this.stopped) { this.cycles += 4; return 4; }
      this.opPC = this.pc;
      const op = this.op = this.fetch();
      this.table[op](this, op);
      return this.cycles - start;
    }
    run(until) {
      while (this.cycles < until) {
        if (this.stopped && !(this.irq && (this.irq > this.ipl || this.irq === 7))) { this.cycles = until; break; }
        this.step();
      }
    }

    saveState() {
      return { d: this.d.slice(), a: this.a.slice(), pc: this.pc, usp: this.usp, ssp: this.ssp, sr: this.getSR(),
        cycles: this.cycles, stopped: this.stopped, irq: this.irq, nmiSeen: this.nmiSeen };
    }
    loadState(o) {
      this.d.set(o.d); this.a.set(o.a); this.pc = o.pc; this.usp = o.usp; this.ssp = o.ssp;
      this.s = (o.sr >> 13) & 1; this.t = (o.sr >> 15) & 1; this.ipl = (o.sr >> 8) & 7; this.setCCR(o.sr & 0x1f);
      this.cycles = o.cycles; this.stopped = o.stopped; this.irq = o.irq; this.nmiSeen = o.nmiSeen;
    }
  }

  // =====================================================================
  // Instruction table
  // =====================================================================
  function buildTable() {
    const T = new Array(65536);
    const illegal = (c, op) => {
      const top = op >> 12;
      c.illegal(top === 0xa ? 10 : top === 0xf ? 11 : 4);
    };
    for (let op = 0; op < 65536; op++) T[op] = decode(op) || illegal;
    return T;
  }

  const SIZE2 = [1, 2, 4]; // size field 00/01/10
  // internal cycles for long ops on a data register vs memory source
  const longExtra = (mode, reg) => (mode === 0 || mode === 1 || (mode === 7 && reg === 4) ? 4 : 2);

  function decode(op) {
    const hi = op >> 12;
    const rx = (op >> 9) & 7, opm = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
    const szf = (op >> 6) & 3;
    switch (hi) {
      case 0x0: return decode0(op, rx, mode, reg, szf);
      case 0x1: case 0x2: case 0x3: return decodeMove(op, hi, rx, opm, mode, reg);
      case 0x4: return decode4(op, rx, opm, mode, reg, szf);
      case 0x5: return decode5(op, rx, mode, reg, szf);
      case 0x6: return decodeBranch(op);
      case 0x7:
        if (op & 0x100) return null;
        return (c) => { const v = sx8(op & 0xff) >>> 0; c.d[rx] = v; c.logic(v, 4); };
      case 0x8: return decodeOrDiv(op, rx, opm, mode, reg);
      case 0x9: return decodeAddSub(op, rx, opm, mode, reg, false);
      case 0xb: return decodeCmpEor(op, rx, opm, mode, reg);
      case 0xc: return decodeAndMul(op, rx, opm, mode, reg);
      case 0xd: return decodeAddSub(op, rx, opm, mode, reg, true);
      case 0xe: return decodeShift(op, rx, mode, reg, szf);
    }
    return null; // A-line / F-line handled by the illegal handler
  }

  // ---------------- group 0: bit operations, immediates, MOVEP ----------------
  function decode0(op, rx, mode, reg, szf) {
    // MOVEP
    if ((op & 0x0138) === 0x0108) {
      const toMem = (op >> 7) & 1, long = (op >> 6) & 1;
      return (c) => {
        let a = (c.a[reg] + sx16(c.fetch())) >>> 0;
        if (toMem) {
          const v = c.d[rx];
          if (long) { c.w8(a, v >>> 24); c.w8(a + 2, v >>> 16); a += 4; }
          c.w8(a, v >>> 8); c.w8(a + 2, v);
        } else {
          if (long) { const b3 = c.r8(a), b2 = c.r8(a + 2), b1 = c.r8(a + 4), b0 = c.r8(a + 6); c.d[rx] = ((b3 << 24) | (b2 << 16) | (b1 << 8) | b0) >>> 0; }
          else { const b1 = c.r8(a), b0 = c.r8(a + 2); c.setD(rx, (b1 << 8) | b0, 2); }
        }
      };
    }
    // dynamic / static bit ops
    if (op & 0x100 || (op & 0x0f00) === 0x0800) {
      const kind = (op >> 6) & 3; // 0 BTST, 1 BCHG, 2 BCLR, 3 BSET
      const dynamic = !!(op & 0x100);
      if (mode === 1) return null;
      if (kind === 0) { if (!eaOk(mode, reg, dynamic ? "data" : "data") || (!dynamic && mode === 7 && reg === 4)) return null; }
      else if (!eaOk(mode, reg, "dataAlt")) return null;
      return (c) => {
        let bit = dynamic ? c.d[rx] : c.fetch() & 0xff;
        if (mode === 0) {
          bit &= 31;
          const m = (1 << bit) >>> 0, v = c.d[reg];
          c.z = v & m ? 0 : 1;
          if (kind === 0) { c.cycles += 2; return; }
          c.d[reg] = kind === 1 ? v ^ m : kind === 2 ? v & ~m : v | m;
          c.cycles += (kind === 2 ? 4 : 2) + (bit >= 16 ? 2 : 0);
          return;
        }
        bit &= 7;
        const m = 1 << bit;
        if (kind === 0) { const v = c.read(mode, reg, 1); c.z = v & m ? 0 : 1; if (mode === 7 && reg === 4) c.cycles += 2; return; }
        c.rmw(mode, reg, 1, (v) => { c.z = v & m ? 0 : 1; return kind === 1 ? v ^ m : kind === 2 ? v & ~m : v | m; });
      };
    }
    const kind = (op >> 9) & 7; // 0 ORI 1 ANDI 2 SUBI 3 ADDI 5 EORI 6 CMPI
    // to CCR / SR
    if ((op & 0xff) === 0x3c || (op & 0xff) === 0x7c) {
      if (kind !== 0 && kind !== 1 && kind !== 5) return null;
      const toSR = (op & 0xff) === 0x7c;
      return (c) => {
        if (toSR && !c.s) { c.privilege(); return; }
        const imm = c.fetch();
        const cur = toSR ? c.getSR() : c.getCCR();
        const v = kind === 0 ? cur | imm : kind === 1 ? cur & imm : cur ^ imm;
        if (toSR) c.setSR(v & 0xa71f); else c.setCCR(v & 0x1f);
        c.cycles += 12;
      };
    }
    if (szf === 3 || kind === 4 || kind === 7) return null;
    const sz = SIZE2[szf];
    if (!eaOk(mode, reg, "dataAlt")) return null;
    return (c) => {
      const imm = sz === 4 ? c.fetch32() : sz === 2 ? c.fetch() : c.fetch() & 0xff;
      if (kind === 6) { // CMPI
        const v = c.read(mode, reg, sz);
        c.sub(v, imm, sz, 0); c.z = clip(v - imm, sz) === 0 ? 1 : 0;
        if (sz === 4 && mode === 0) c.cycles += 2;
        return;
      }
      if (mode === 0 && sz === 4) c.cycles += 4;
      c.rmw(mode, reg, sz, (v) => {
        let r;
        switch (kind) {
          case 0: r = clip(v | imm, sz); c.logic(r, sz); return r;
          case 1: r = clip(v & imm, sz); c.logic(r, sz); return r;
          case 5: r = clip(v ^ imm, sz); c.logic(r, sz); return r;
          case 2: r = c.sub(v, imm, sz, 0); c.x = c.c; c.z = r === 0 ? 1 : 0; return r;
          case 3: r = c.add(v, imm, sz, 0); c.x = c.c; c.z = r === 0 ? 1 : 0; return r;
        }
      });
    };
  }

  // ---------------- MOVE / MOVEA ----------------
  function decodeMove(op, hi, rx, dmode, smode, sreg) {
    const sz = hi === 1 ? 1 : hi === 3 ? 2 : 4;
    if (!eaOk(smode, sreg, "all") || (sz === 1 && smode === 1)) return null;
    if (dmode === 1) { // MOVEA
      if (sz === 1) return null;
      return (c) => {
        const v = c.read(smode, sreg, sz);
        c.a[rx] = sz === 2 ? sx16(v) : v;
      };
    }
    if (!eaOk(dmode, rx, "dataAlt")) return null;
    return (c) => {
      const v = c.read(smode, sreg, sz);
      if (dmode === 0) { c.setD(rx, v, sz); c.logic(v, sz); return; }
      let a;
      if (dmode === 4) { c.a[rx] -= sz === 1 && rx === 7 ? 2 : sz; a = c.a[rx]; } // no extra time as a MOVE destination
      else a = c.addr(dmode, rx, sz);
      c.logic(v, sz);
      c.wm(a, sz, v);
    };
  }

  // ---------------- group 4: miscellaneous ----------------
  function decode4(op, rx, opm, mode, reg, szf) {
    // LEA / CHK
    if (opm === 7) {
      if (!eaOk(mode, reg, "ctrl")) return null;
      return (c) => {
        const a = c.addr(mode, reg, 4);
        if (mode === 6 || (mode === 7 && reg === 3)) c.cycles += 2;
        c.a[rx] = a;
      };
    }
    if (opm === 6) {
      if (!eaOk(mode, reg, "data")) return null;
      return (c) => {
        const bound = sx16(c.read(mode, reg, 2)), v = sx16(c.d[rx] & 0xffff);
        c.cycles += 6;
        c.z = v === 0 ? 1 : 0; c.v = 0; c.c = 0; c.n = 0;
        if (v < 0) { c.n = 1; c.trap(6, 10); }
        else if (v > bound) c.trap(6, 8);
      };
    }
    switch (op & 0x0f00) {
      case 0x0000: // NEGX / MOVE from SR
        if (szf === 3) {
          if (!eaOk(mode, reg, "dataAlt")) return null;
          return (c) => {
            const sr = c.getSR();
            if (mode === 0) { c.setD(reg, sr, 2); c.cycles += 2; return; }
            c.rmw(mode, reg, 2, () => sr);
          };
        }
        if (!eaOk(mode, reg, "dataAlt")) return null;
        return unary(mode, reg, SIZE2[szf], (c, v, sz) => { const z = c.z; const r = c.sub(0, v, sz, c.x); c.x = c.c; c.z = r === 0 ? z : 0; return r; });
      case 0x0200: // CLR
        if (szf === 3 || !eaOk(mode, reg, "dataAlt")) return null;
        return unary(mode, reg, SIZE2[szf], (c) => { c.n = 0; c.z = 1; c.v = 0; c.c = 0; return 0; });
      case 0x0400: // NEG / MOVE to CCR
        if (szf === 3) {
          if (!eaOk(mode, reg, "data")) return null;
          return (c) => { c.setCCR(c.read(mode, reg, 2) & 0x1f); c.cycles += 8; };
        }
        if (!eaOk(mode, reg, "dataAlt")) return null;
        return unary(mode, reg, SIZE2[szf], (c, v, sz) => { const r = c.sub(0, v, sz, 0); c.x = c.c; c.z = r === 0 ? 1 : 0; return r; });
      case 0x0600: // NOT / MOVE to SR
        if (szf === 3) {
          if (!eaOk(mode, reg, "data")) return null;
          return (c) => {
            if (!c.s) { c.privilege(); return; }
            c.setSR(c.read(mode, reg, 2) & 0xa71f); c.cycles += 8;
          };
        }
        if (!eaOk(mode, reg, "dataAlt")) return null;
        return unary(mode, reg, SIZE2[szf], (c, v, sz) => { const r = clip(~v, sz); c.logic(r, sz); return r; });
      case 0x0800:
        if (szf === 0) { // NBCD
          if (!eaOk(mode, reg, "dataAlt")) return null;
          return (c) => {
            if (mode === 0) c.cycles += 2;
            c.rmw(mode, reg, 1, (v) => bcdSub(c, 0, v));
          };
        }
        if (szf === 1) {
          if (mode === 0) return (c) => { const v = c.d[reg]; const r = ((v >>> 16) | (v << 16)) >>> 0; c.d[reg] = r; c.logic(r, 4); }; // SWAP
          if (!eaOk(mode, reg, "ctrl")) return null;
          return (c) => { // PEA
            const a = c.addr(mode, reg, 4);
            if (mode === 6 || (mode === 7 && reg === 3)) c.cycles += 2;
            c.push32(a);
          };
        }
        if (mode === 0) { // EXT
          return szf === 2
            ? (c) => { const r = sx8(c.d[reg] & 0xff) & 0xffff; c.setD(reg, r, 2); c.logic(r, 2); }
            : (c) => { const r = sx16(c.d[reg] & 0xffff) >>> 0; c.d[reg] = r; c.logic(r, 4); };
        }
        if (!eaOk(mode, reg, "ctrlAlt") && mode !== 4) return null;
        return movemToMem(mode, reg, szf === 3 ? 4 : 2);
      case 0x0a00: // TST / TAS / ILLEGAL
        if (op === 0x4afc) return null;
        if (szf === 3) {
          if (!eaOk(mode, reg, "dataAlt")) return null;
          return (c) => {
            if (mode === 0) { const v = c.d[reg] & 0xff; c.logic(v, 1); c.d[reg] |= 0x80; return; }
            const a = c.addr(mode, reg, 1);
            const v = c.r8(a);
            c.logic(v, 1);
            c.cycles += 2;
            if (c.tasWrites) c.w8(a, v | 0x80); else c.cycles += 4;
          };
        }
        if (!eaOk(mode, reg, "all") || (szf === 0 && mode === 1)) return null;
        return (c) => { const sz = SIZE2[szf]; c.logic(c.read(mode, reg, sz), sz); };
      case 0x0c00: // MOVEM to registers
        if (szf < 2) return null;
        if (!eaOk(mode, reg, "ctrlPostinc")) return null;
        return movemToReg(mode, reg, szf === 3 ? 4 : 2);
      case 0x0e00:
        if ((op & 0xfff0) === 0x4e40) { const v = op & 15; return (c) => c.trap(32 + v, 10); }
        if ((op & 0xfff8) === 0x4e50) return (c) => { // LINK
          const disp = sx16(c.fetch());
          c.push32(c.a[reg]); // (LINK A7 stores A7's value from before the push)
          c.a[reg] = c.a[7];
          c.a[7] += disp;
        };
        if ((op & 0xfff8) === 0x4e58) return (c) => { // UNLK
          c.a[7] = c.a[reg];
          const v = c.pop32();
          c.a[reg] = v;
        };
        if ((op & 0xfff8) === 0x4e60) return (c) => { if (!c.s) return c.privilege(); c.usp = c.a[reg]; };
        if ((op & 0xfff8) === 0x4e68) return (c) => { if (!c.s) return c.privilege(); c.a[reg] = c.usp; };
        switch (op) {
          case 0x4e70: return (c) => { if (!c.s) return c.privilege(); if (c.bus.reset) c.bus.reset(); c.cycles += 128; };
          case 0x4e71: return () => {};
          case 0x4e72: return (c) => {
            if (!c.s) return c.privilege();
            const v = c.fetch(); c.setSR(v & 0xa71f); c.stopped = true;
          };
          case 0x4e73: return (c) => {
            if (!c.s) return c.privilege();
            const sr = c.pop16(), pc = c.pop32();
            c.setSR(sr & 0xa71f); c.pc = pc; c.cycles += 4;
          };
          case 0x4e75: return (c) => { c.pc = c.pop32(); c.cycles += 4; };
          case 0x4e76: return (c) => { if (c.v) c.trap(7, 10); };
          case 0x4e77: return (c) => { const ccr = c.pop16(); c.setCCR(ccr & 0x1f); c.pc = c.pop32(); c.cycles += 4; };
        }
        if ((op & 0xffc0) === 0x4e80) { // JSR
          if (!eaOk(mode, reg, "ctrl")) return null;
          return (c) => {
            const a = c.addr(mode, reg, 4);
            c.cycles += jumpExtra(mode, reg);
            c.push32(c.pc); c.pc = a;
          };
        }
        if ((op & 0xffc0) === 0x4ec0) { // JMP
          if (!eaOk(mode, reg, "ctrl")) return null;
          return (c) => { const a = c.addr(mode, reg, 4); c.cycles += jumpExtra(mode, reg); c.pc = a; };
        }
        return null;
    }
    return null;
  }
  // Extra internal time JMP/JSR take for each addressing mode.
  function jumpExtra(mode, reg) {
    if (mode === 2) return 4;
    if (mode === 5) return 2;
    if (mode === 6) return 4;
    if (mode === 7) return reg === 0 ? 2 : reg === 1 ? 0 : reg === 2 ? 2 : 4;
    return 0;
  }
  // single-operand instructions (CLR, NEG, NEGX, NOT): registers take 2 extra for long
  function unary(mode, reg, sz, fn) {
    return (c) => {
      if (mode === 0) {
        if (sz === 4) c.cycles += 2;
        c.setD(reg, fn(c, clip(c.d[reg], sz), sz), sz);
        return;
      }
      c.rmw(mode, reg, sz, (v) => fn(c, v, sz));
    };
  }

  function movemToMem(mode, reg, sz) {
    return (c) => {
      const mask = c.fetch();
      if (mode === 4) {
        let a = c.a[reg];
        const start = a;
        for (let i = 0; i < 16; i++) {
          if (!(mask & (1 << i))) continue;
          const r = 15 - i;
          a = (a - sz) >>> 0;
          // the address register being decremented is stored with its original value
          const v = r >= 8 ? (r - 8 === reg ? start : c.a[r - 8]) : c.d[r];
          if (sz === 4) { c.w16(a + 2, v & 0xffff); c.w16(a, v >>> 16); }
          else c.w16(a, v);
        }
        c.a[reg] = a;
        return;
      }
      let a = c.addr(mode, reg, sz);
      for (let i = 0; i < 16; i++) {
        if (!(mask & (1 << i))) continue;
        const v = i >= 8 ? c.a[i - 8] : c.d[i];
        c.wm(a, sz, v); a = (a + sz) >>> 0;
      }
    };
  }
  function movemToReg(mode, reg, sz) {
    return (c) => {
      const mask = c.fetch();
      let a = mode === 3 ? c.a[reg] : c.addr(mode, reg, sz);
      for (let i = 0; i < 16; i++) {
        if (!(mask & (1 << i))) continue;
        const v = sz === 4 ? c.r32(a) : sx16(c.r16(a)) >>> 0;
        if (i >= 8) c.a[i - 8] = v; else c.d[i] = v;
        a = (a + sz) >>> 0;
      }
      c.r16(a); // one extra read at the end
      if (mode === 3) c.a[reg] = a;
    };
  }

  // ---------------- group 5: ADDQ / SUBQ / Scc / DBcc ----------------
  function decode5(op, rx, mode, reg, szf) {
    const cc = (op >> 8) & 15;
    if (szf === 3) {
      if (mode === 1) { // DBcc
        return (c) => {
          const base = c.pc, disp = sx16(c.fetch());
          if (c.cond(cc)) { c.cycles += 4; return; }
          const v = (c.d[reg] - 1) & 0xffff;
          c.setD(reg, v, 2);
          if (v !== 0xffff) { c.pc = (base + disp) >>> 0; c.cycles += 2; }
          else c.cycles += 6;
        };
      }
      if (!eaOk(mode, reg, "dataAlt")) return null;
      return (c) => { // Scc
        const t = c.cond(cc);
        if (mode === 0) { c.setD(reg, t ? 0xff : 0, 1); if (t) c.cycles += 2; return; }
        c.rmw(mode, reg, 1, () => (t ? 0xff : 0));
      };
    }
    const q = rx || 8, sub = !!(op & 0x100), sz = SIZE2[szf];
    if (!eaOk(mode, reg, "alt") || (mode === 1 && sz === 1)) return null;
    if (mode === 1) return (c) => { c.a[reg] = sub ? c.a[reg] - q : c.a[reg] + q; c.cycles += 4; };
    return (c) => {
      if (mode === 0 && sz === 4) c.cycles += 4;
      c.rmw(mode, reg, sz, (v) => {
        const r = sub ? c.sub(v, q, sz, 0) : c.add(v, q, sz, 0);
        c.x = c.c; c.z = r === 0 ? 1 : 0; return r;
      });
    };
  }

  // ---------------- group 6: Bcc / BRA / BSR ----------------
  function decodeBranch(op) {
    const cc = (op >> 8) & 15, d8 = op & 0xff;
    return (c) => {
      const base = c.pc;
      let disp;
      if (d8 === 0) disp = sx16(c.fetch()); else disp = sx8(d8);
      if (cc === 1) { // BSR
        c.cycles += d8 === 0 ? 2 : 6;
        c.push32(c.pc);
        c.pc = (base + disp) >>> 0;
        return;
      }
      if (c.cond(cc)) { c.pc = (base + disp) >>> 0; c.cycles += d8 === 0 ? 2 : 6; }
      else c.cycles += 4;
    };
  }

  // ---------------- OR / DIVU / DIVS / SBCD ----------------
  function decodeOrDiv(op, rx, opm, mode, reg) {
    if (opm === 3 || opm === 7) {
      if (!eaOk(mode, reg, "data")) return null;
      const signed = opm === 7;
      return (c) => {
        const src = c.read(mode, reg, 2);
        divide(c, rx, src, signed);
      };
    }
    if (opm === 4 && mode <= 1) return bcdOp(rx, mode, reg, false);
    return logicOp(op, rx, opm, mode, reg, (a, b) => a | b);
  }

  function divide(c, rx, src, signed) {
    const dividend = c.d[rx];
    if (src === 0) { c.cycles += 4; c.v = 0; c.c = 0; c.n = 0; c.z = 0; c.trap(5, 10); return; }
    if (!signed) {
      const q = Math.floor(dividend / src), r = dividend % src;
      c.cycles += divuTime(dividend, src);
      if (q > 0xffff) { c.v = 1; c.n = 1; c.z = 0; c.c = 0; return; }
      c.d[rx] = ((r << 16) | q) >>> 0;
      c.n = (q >> 15) & 1; c.z = q === 0 ? 1 : 0; c.v = 0; c.c = 0;
      return;
    }
    const dv = dividend | 0, sv = sx16(src);
    const q = Math.trunc(dv / sv), r = dv - q * sv;
    c.cycles += divsTime(dv, sv);
    if (q > 32767 || q < -32768) { c.v = 1; c.n = 1; c.z = 0; c.c = 0; return; }
    c.d[rx] = (((r & 0xffff) << 16) | (q & 0xffff)) >>> 0;
    c.n = (q >> 15) & 1; c.z = q === 0 ? 1 : 0; c.v = 0; c.c = 0;
  }
  // Division takes a data-dependent time: the chip works out the quotient one
  // bit at a time, and how long a bit takes depends on that bit and on the
  // partial remainder before it. These return the instruction's time with a
  // register operand, minus the 4-cycle opcode fetch.
  //
  // DIVU: 72 cycles, plus for each quotient bit 15..1: nothing when the partial
  // remainder going in had its top bit set, otherwise 2 for a 1 bit, 4 for a 0 bit.
  // An overflow (quotient wider than 16 bits) is caught after 6 cycles.
  function divuTime(dividend, divisor) {
    dividend >>>= 0;
    if (Math.floor(dividend / 65536) >= divisor) return 6;
    const q = Math.floor(dividend / divisor);
    let t = 72;
    for (let bit = 15; bit >= 1; bit--) {
      const partial = Math.floor(dividend / 2 ** (bit + 1)) % divisor;
      if (partial < 0x8000) t += (q >> bit) & 1 ? 2 : 4;
    }
    return t;
  }
  // DIVS: a base time set by the operands' signs, plus 2 cycles for every 0 among
  // bits 15..1 of the unsigned quotient. Overflow: 12 cycles (14 for a negative dividend).
  function divsTime(dividend, divisor) {
    const negN = dividend < 0 ? 1 : 0, negD = divisor < 0 ? 1 : 0;
    const n = Math.abs(dividend), d = Math.abs(divisor);
    if (Math.floor(n / 65536) >= d) return 12 + 2 * negN;
    let q = (Math.floor(n / d) >> 1) & 0x7fff, ones = 0;
    for (; q; q &= q - 1) ones++;
    return [[116, 118], [122, 120]][negN][negD] + 2 * (15 - ones);
  }

  function logicOp(op, rx, opm, mode, reg, fn) {
    const sz = SIZE2[opm & 3], toMem = opm >= 4;
    if (toMem) {
      if (!eaOk(mode, reg, "memAlt")) return null;
      return (c) => { c.rmw(mode, reg, sz, (v) => { const r = clip(fn(v, c.d[rx]), sz); c.logic(r, sz); return r; }); };
    }
    if (!eaOk(mode, reg, "data")) return null;
    return (c) => {
      const v = c.read(mode, reg, sz);
      const r = clip(fn(c.d[rx], v), sz);
      c.setD(rx, r, sz); c.logic(r, sz);
      if (sz === 4) c.cycles += longExtra(mode, reg);
    };
  }

  // ---------------- ADD / SUB / ADDA / SUBA / ADDX / SUBX ----------------
  function decodeAddSub(op, rx, opm, mode, reg, isAdd) {
    if (opm === 3 || opm === 7) { // ADDA / SUBA
      if (!eaOk(mode, reg, "all")) return null;
      const sz = opm === 3 ? 2 : 4;
      return (c) => {
        let v = c.read(mode, reg, sz);
        if (sz === 2) v = sx16(v);
        c.a[rx] = isAdd ? c.a[rx] + v : c.a[rx] - v;
        c.cycles += sz === 2 ? 4 : longExtra(mode, reg);
      };
    }
    const sz = SIZE2[opm & 3];
    if (opm >= 4 && mode <= 1) { // ADDX / SUBX
      return (c) => {
        let s, d, a = 0;
        if (mode === 0) { s = clip(c.d[reg], sz); d = clip(c.d[rx], sz); if (sz === 4) c.cycles += 4; }
        else {
          c.a[reg] -= sz === 1 && reg === 7 ? 2 : sz; s = c.rm(c.a[reg], sz);
          c.a[rx] -= sz === 1 && rx === 7 ? 2 : sz; a = c.a[rx]; d = c.rm(a, sz);
          c.cycles += 2;
        }
        const z = c.z;
        const r = isAdd ? c.add(d, s, sz, c.x) : c.sub(d, s, sz, c.x);
        c.x = c.c; c.z = r === 0 ? z : 0;
        if (mode === 0) c.setD(rx, r, sz); else c.wm(a, sz, r);
      };
    }
    if (opm >= 4) { // Dn op <ea> -> <ea>
      if (!eaOk(mode, reg, "memAlt")) return null;
      return (c) => {
        c.rmw(mode, reg, sz, (v) => {
          const r = isAdd ? c.add(v, clip(c.d[rx], sz), sz, 0) : c.sub(v, clip(c.d[rx], sz), sz, 0);
          c.x = c.c; c.z = r === 0 ? 1 : 0; return r;
        });
      };
    }
    if (!eaOk(mode, reg, "all") || (sz === 1 && mode === 1)) return null;
    return (c) => {
      const v = c.read(mode, reg, sz), d = clip(c.d[rx], sz);
      const r = isAdd ? c.add(d, v, sz, 0) : c.sub(d, v, sz, 0);
      c.x = c.c; c.z = r === 0 ? 1 : 0;
      c.setD(rx, r, sz);
      if (sz === 4) c.cycles += longExtra(mode, reg);
    };
  }

  // ---------------- CMP / CMPA / CMPM / EOR ----------------
  function decodeCmpEor(op, rx, opm, mode, reg) {
    if (opm === 3 || opm === 7) { // CMPA
      if (!eaOk(mode, reg, "all")) return null;
      const sz = opm === 3 ? 2 : 4;
      return (c) => {
        let v = c.read(mode, reg, sz);
        if (sz === 2) v = sx16(v) >>> 0;
        const r = c.sub(c.a[rx], v, 4, 0); c.z = r === 0 ? 1 : 0;
        c.cycles += 2;
      };
    }
    const sz = SIZE2[opm & 3];
    if (opm < 3) { // CMP
      if (!eaOk(mode, reg, "all") || (sz === 1 && mode === 1)) return null;
      return (c) => {
        const v = c.read(mode, reg, sz);
        const r = c.sub(clip(c.d[rx], sz), v, sz, 0); c.z = r === 0 ? 1 : 0;
        if (sz === 4) c.cycles += 2;
      };
    }
    if (mode === 1) { // CMPM (Ay)+,(Ax)+
      return (c) => {
        const s = c.rm(c.addr(3, reg, sz), sz), d = c.rm(c.addr(3, rx, sz), sz);
        const r = c.sub(d, s, sz, 0); c.z = r === 0 ? 1 : 0;
      };
    }
    if (!eaOk(mode, reg, "dataAlt")) return null; // EOR Dn,<ea>
    if (mode === 0) return (c) => { const r = clip(c.d[reg] ^ c.d[rx], sz); c.setD(reg, r, sz); c.logic(r, sz); if (sz === 4) c.cycles += 4; };
    return (c) => { c.rmw(mode, reg, sz, (v) => { const r = clip(v ^ c.d[rx], sz); c.logic(r, sz); return r; }); };
  }

  // ---------------- AND / MULU / MULS / ABCD / EXG ----------------
  function decodeAndMul(op, rx, opm, mode, reg) {
    if (opm === 3 || opm === 7) {
      if (!eaOk(mode, reg, "data")) return null;
      const signed = opm === 7;
      return (c) => {
        const s = c.read(mode, reg, 2);
        let r, bits;
        if (signed) { r = (sx16(s) * sx16(c.d[rx] & 0xffff)) | 0; bits = popcount16((s ^ (s << 1)) & 0xffff); }
        else { r = (s * (c.d[rx] & 0xffff)) >>> 0; bits = popcount16(s); }
        c.d[rx] = r >>> 0; c.logic(r >>> 0, 4);
        c.cycles += 34 + 2 * bits;
      };
    }
    if (opm === 4 && mode <= 1) return bcdOp(rx, mode, reg, true);
    if (opm === 5 && mode === 0) return (c) => { const t = c.d[rx]; c.d[rx] = c.d[reg]; c.d[reg] = t; c.cycles += 2; };
    if (opm === 5 && mode === 1) return (c) => { const t = c.a[rx]; c.a[rx] = c.a[reg]; c.a[reg] = t; c.cycles += 2; };
    if (opm === 6 && mode === 1) return (c) => { const t = c.d[rx]; c.d[rx] = c.a[reg]; c.a[reg] = t; c.cycles += 2; };
    if (opm === 6 && mode === 0) return null;
    return logicOp(op, rx, opm, mode, reg, (a, b) => a & b);
  }

  // ABCD / SBCD
  function bcdOp(rx, mode, reg, isAdd) {
    return (c) => {
      let s, d, a = 0;
      if (mode === 0) { s = c.d[reg] & 0xff; d = c.d[rx] & 0xff; c.cycles += 2; }
      else {
        c.a[reg] -= reg === 7 ? 2 : 1; s = c.r8(c.a[reg]);
        c.a[rx] -= rx === 7 ? 2 : 1; a = c.a[rx]; d = c.r8(a);
        c.cycles += 2;
      }
      const r = isAdd ? bcdAdd(c, d, s) : bcdSub(c, d, s);
      if (mode === 0) c.setD(rx, r, 1); else c.w8(a, r);
    };
  }
  // Decimal add/subtract, including what the chip does with invalid (non-BCD)
  // digits and the officially undefined N and V flags: the binary result is
  // corrected by 6 in each nibble that produced a binary or decimal carry.
  function bcdAdd(c, d, s) {
    const sum = d + s + c.x;
    const carries = ((s & d) | (~sum & s) | (~sum & d)) & 0x88;      // binary carry out of each nibble
    const decimal = (((sum + 0x66) ^ sum) & 0x110) >> 1;            // nibbles that went past 9
    const both = carries | decimal;
    const res = sum + both - (both >> 2);                          // add 6 per flagged nibble
    c.c = c.x = ((carries | (sum & ~res)) >> 7) & 1;
    c.v = ((~sum & res) >> 7) & 1;
    c.n = (res >> 7) & 1;
    if (res & 0xff) c.z = 0;
    return res & 0xff;
  }
  function bcdSub(c, d, s) {
    const diff = d - s - c.x;
    const borrows = ((~d & s) | (diff & ~d) | (diff & s)) & 0x88;   // binary borrow out of each nibble
    const res = diff - (borrows - (borrows >> 2));                 // subtract 6 per borrowing nibble
    c.c = c.x = ((borrows | (~diff & res)) >> 7) & 1;
    c.v = ((diff & ~res) >> 7) & 1;
    c.n = (res >> 7) & 1;
    if (res & 0xff) c.z = 0;
    return res & 0xff;
  }

  // ---------------- shifts and rotates ----------------
  function decodeShift(op, rx, mode, reg, szf) {
    if (szf === 3) { // memory, one bit, word
      const type = (op >> 9) & 3, left = !!(op & 0x100);
      if ((op & 0x0800) || !eaOk(mode, reg, "memAlt")) return null;
      return (c) => { c.rmw(mode, reg, 2, (v) => shift(c, type, left, v, 1, 2)); };
    }
    const sz = SIZE2[szf], type = (op >> 3) & 3, left = !!(op & 0x100), byReg = !!(op & 0x20);
    return (c) => {
      const n = byReg ? c.d[rx] & 63 : rx || 8;
      c.cycles += (sz === 4 ? 4 : 2) + 2 * n;
      c.setD(reg, shift(c, type, left, clip(c.d[reg], sz), n, sz), sz);
    };
  }
  function shift(c, type, left, v, n, sz) {
    const bits = BITS[sz], top = sz === 4 ? 0x80000000 : 1 << (bits - 1);
    let carry = 0, vf = 0;
    if (n === 0) {
      c.c = type === 2 ? c.x : 0; c.v = 0; c.nz(v, sz);
      return v;
    }
    switch (type) {
      case 0: // ASL / ASR
        if (left) {
          for (let i = 0; i < n; i++) {
            carry = msb(v, sz);
            const nv = clip(v * 2, sz);
            if (msb(nv, sz) !== msb(v, sz)) vf = 1;
            v = nv;
          }
        } else {
          const sign = msb(v, sz);
          for (let i = 0; i < n; i++) { carry = v & 1; v = clip(Math.floor(v / 2) + (sign ? top : 0), sz); }
        }
        c.x = carry; break;
      case 1: // LSL / LSR
        for (let i = 0; i < n; i++) {
          if (left) { carry = msb(v, sz); v = clip(v * 2, sz); }
          else { carry = v & 1; v = Math.floor(v / 2); }
        }
        c.x = carry; break;
      case 2: { // ROXL / ROXR
        let x = c.x;
        for (let i = 0; i < n; i++) {
          if (left) { carry = msb(v, sz); v = clip(v * 2 + x, sz); }
          else { carry = v & 1; v = Math.floor(v / 2) + (x ? top : 0); }
          x = carry;
        }
        c.x = x; carry = x; break;
      }
      case 3: // ROL / ROR
        for (let i = 0; i < n; i++) {
          if (left) { carry = msb(v, sz); v = clip(v * 2 + carry, sz); }
          else { carry = v & 1; v = Math.floor(v / 2) + (carry ? top : 0); }
        }
        break;
    }
    v = clip(v, sz);
    c.c = carry; c.v = vf; c.nz(v, sz);
    return v;
  }

  global.RPM68K = M68K;
})(typeof window !== "undefined" ? window : globalThis);
