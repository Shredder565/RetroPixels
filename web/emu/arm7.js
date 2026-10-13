// RetroPixels — ARM7TDMI processor (ARM and Thumb instruction sets), used by the
// Game Boy Advance emulator. Original code written from the ARMv4T architecture's
// documented behaviour.
//
// Bus interface (all addresses are passed through unaligned; the bus aligns):
//   read8/read16/read32(addr), write8/write16/write32(addr, v)  — data accesses
//   fetch16/fetch32(addr)                                        — instruction fetch
//   branched(addr, thumb)  (optional) — pipeline refill after a jump (timing only)
//   swi(n) (optional)  — return true if the call was handled (high-level BIOS)
"use strict";

(function (global) {
  const M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1b, M_SYS = 0x1f;
  const BANK = new Int8Array(32).fill(0);
  BANK[M_FIQ] = 1; BANK[M_IRQ] = 2; BANK[M_SVC] = 3; BANK[M_ABT] = 4; BANK[M_UND] = 5;

  const ror = (v, n) => (n &= 31) ? ((v >>> n) | (v << (32 - n))) : v;

  // 32 x 32 -> 64-bit multiply. Returns low word in out[0], high in out[1] (both int32).
  const MUL64 = new Int32Array(2);
  function mul64(a, b, signed) {
    let neg = false;
    if (signed) {
      if (a < 0) { a = -a; neg = !neg; }
      if (b < 0) { b = -b; neg = !neg; }
    }
    a >>>= 0; b >>>= 0;
    const a0 = a & 0xffff, a1 = a >>> 16, b0 = b & 0xffff, b1 = b >>> 16;
    const p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const mid = p01 + p10 + Math.floor(p00 / 65536); // < 2^34
    let lo = ((mid & 0xffff) * 65536 + (p00 & 0xffff)) >>> 0;
    let hi = (p11 + Math.floor(mid / 65536)) >>> 0;
    if (neg) { lo = (~lo + 1) >>> 0; hi = (~hi + (lo === 0 ? 1 : 0)) >>> 0; }
    MUL64[0] = lo; MUL64[1] = hi;
  }
  function add64(lo, hi, alo, ahi) {
    const l = (lo >>> 0) + (alo >>> 0);
    MUL64[0] = l >>> 0;
    MUL64[1] = ((hi >>> 0) + (ahi >>> 0) + (l > 0xffffffff ? 1 : 0)) >>> 0;
  }
  // internal cycles of a multiply by rs
  function mulCycles(rs, signed) {
    if (signed) {
      if ((rs >> 8) === 0 || (rs >> 8) === -1) return 1;
      if ((rs >> 16) === 0 || (rs >> 16) === -1) return 2;
      if ((rs >> 24) === 0 || (rs >> 24) === -1) return 3;
      return 4;
    }
    rs >>>= 0;
    if (rs < 0x100) return 1;
    if (rs < 0x10000) return 2;
    if (rs < 0x1000000) return 3;
    return 4;
  }

  class ARM7 {
    constructor(bus) {
      this.bus = bus;
      this.r = new Int32Array(16);
      this.bank13 = new Int32Array(6); this.bank14 = new Int32Array(6); this.spsrBank = new Int32Array(6);
      this.fiqR = new Int32Array(5); this.usrR = new Int32Array(5);
      this.n = 0; this.z = 0; this.c = 0; this.v = 0;
      this.i = 1; this.f = 1; this.t = 0; this.mode = M_SVC;
      this.pc = 0;          // address of the next instruction to run
      this.cycles = 0;
      this.halted = false;
      this.irqLine = false;
      this.sc = 0;          // shifter carry out
      this.pipe0 = 0; this.pipe1 = 0; // fetched instructions waiting to run
      if (!ARM7.armTable) buildTables();
    }

    // ---- status registers ----
    getCPSR() { return (this.n << 31) | (this.z << 30) | (this.c << 29) | (this.v << 28) | (this.i << 7) | (this.f << 6) | (this.t << 5) | this.mode; }
    setCPSR(v) {
      this.n = (v >>> 31) & 1; this.z = (v >>> 30) & 1; this.c = (v >>> 29) & 1; this.v = (v >>> 28) & 1;
      this.i = (v >> 7) & 1; this.f = (v >> 6) & 1; this.t = (v >> 5) & 1;
      this.switchMode(v & 0x1f);
    }
    get spsr() { const b = BANK[this.mode]; return b ? this.spsrBank[b] : this.getCPSR(); }
    set spsr(v) { const b = BANK[this.mode]; if (b) this.spsrBank[b] = v; }
    switchMode(m) {
      const old = this.mode;
      if (old === m) return;
      const r = this.r, ob = BANK[old], nb = BANK[m];
      this.bank13[ob] = r[13]; this.bank14[ob] = r[14];
      if (old === M_FIQ) { for (let k = 0; k < 5; k++) { this.fiqR[k] = r[8 + k]; r[8 + k] = this.usrR[k]; } }
      if (m === M_FIQ) { for (let k = 0; k < 5; k++) { this.usrR[k] = r[8 + k]; r[8 + k] = this.fiqR[k]; } }
      r[13] = this.bank13[nb]; r[14] = this.bank14[nb];
      this.mode = m;
    }
    // registers of user mode (for LDM/STM with the S bit)
    getUser(k) {
      if (k >= 8 && k <= 12 && this.mode === M_FIQ) return this.usrR[k - 8];
      if ((k === 13 || k === 14) && BANK[this.mode]) return k === 13 ? this.bank13[0] : this.bank14[0];
      return this.r[k];
    }
    setUser(k, v) {
      if (k >= 8 && k <= 12 && this.mode === M_FIQ) { this.usrR[k - 8] = v; return; }
      if ((k === 13 || k === 14) && BANK[this.mode]) { if (k === 13) this.bank13[0] = v; else this.bank14[0] = v; return; }
      this.r[k] = v;
    }

    // ---- flow ----
    branch(addr) {
      if (this.t) addr &= ~1;
      this.pc = addr >>> 0;
      const bus = this.bus;
      if (this.t) {
        this.r[15] = addr + 4;
        this.pipe0 = bus.fetch16(this.pc & ~1); this.pipe1 = bus.fetch16((this.pc + 2) & ~1);
      } else {
        this.r[15] = addr + 8;
        this.pipe0 = bus.fetch32(this.pc & ~3); this.pipe1 = bus.fetch32((this.pc + 4) & ~3);
      }
      if (bus.branched) bus.branched(this.pc, this.t);
    }
    exception(vector, mode, lr) {
      const cpsr = this.getCPSR();
      this.switchMode(mode);
      this.spsr = cpsr;
      this.r[14] = lr;
      this.t = 0; this.i = 1;
      if (mode === M_FIQ) this.f = 1;
      this.branch(vector);
    }
    undefinedInstr(size) { this.exception(0x04, M_UND, this.pc); }
    swi(n) {
      if (this.bus.swi && this.bus.swi(n)) return;
      this.exception(0x08, M_SVC, this.pc);
    }
    irq() {
      this.halted = false;
      this.exception(0x18, M_IRQ, this.pc + 4);
    }

    reset() {
      this.r.fill(0); this.bank13.fill(0); this.bank14.fill(0); this.spsrBank.fill(0); this.fiqR.fill(0); this.usrR.fill(0);
      this.mode = M_SVC; this.i = 1; this.f = 1; this.t = 0; this.n = this.z = this.c = this.v = 0;
      this.halted = false;
      this.branch(0);
    }

    // Run one instruction (or take an interrupt).
    step() {
      if (this.irqLine && !this.i) { this.irq(); return; }
      if (this.halted) return;
      const a = this.pc;
      if (this.t) {
        const op = this.pipe0;
        this.pipe0 = this.pipe1;
        this.pc = (a + 2) >>> 0;
        this.r[15] = a + 4;
        this.pipe1 = this.bus.fetch16((a + 4) & ~1);
        THUMB[op >>> 6](this, op);
      } else {
        const op = this.pipe0;
        this.pipe0 = this.pipe1;
        this.pc = (a + 4) >>> 0;
        this.r[15] = a + 8;
        this.pipe1 = this.bus.fetch32((a + 8) & ~3);
        if (condPass(this, op >>> 28)) ARM[((op >>> 16) & 0xff0) | ((op >>> 4) & 0xf)](this, op);
      }
    }

    // ---- shifter ----
    shiftImm(v, type, amt) {
      switch (type) {
        case 0: if (amt === 0) { this.sc = this.c; return v; } this.sc = (v >>> (32 - amt)) & 1; return v << amt;
        case 1: if (amt === 0) { this.sc = v >>> 31; return 0; } this.sc = (v >>> (amt - 1)) & 1; return v >>> amt;
        case 2: if (amt === 0) { this.sc = v >>> 31; return v >> 31; } this.sc = (v >> (amt - 1)) & 1; return v >> amt;
        default:
          if (amt === 0) { this.sc = v & 1; return (v >>> 1) | (this.c << 31); }
          this.sc = (v >>> (amt - 1)) & 1; return ror(v, amt);
      }
    }
    shiftReg(v, type, s) {
      if (s === 0) { this.sc = this.c; return v; }
      switch (type) {
        case 0:
          if (s < 32) { this.sc = (v >>> (32 - s)) & 1; return v << s; }
          this.sc = s === 32 ? v & 1 : 0; return 0;
        case 1:
          if (s < 32) { this.sc = (v >>> (s - 1)) & 1; return v >>> s; }
          this.sc = s === 32 ? v >>> 31 : 0; return 0;
        case 2:
          if (s < 32) { this.sc = (v >> (s - 1)) & 1; return v >> s; }
          this.sc = v >>> 31; return v >> 31;
        default:
          s &= 31;
          if (s === 0) { this.sc = v >>> 31; return v; }
          this.sc = (v >>> (s - 1)) & 1; return ror(v, s);
      }
    }

    // ---- arithmetic with flags ----
    add(a, b, cin, setf) {
      const r = (a + b + cin) | 0;
      if (setf) {
        this.n = r >>> 31; this.z = r === 0 ? 1 : 0;
        this.c = (a >>> 0) + (b >>> 0) + cin > 0xffffffff ? 1 : 0;
        this.v = ((~(a ^ b) & (a ^ r)) >>> 31);
      }
      return r;
    }
    sub(a, b, cin, setf) { // a - b - (1 - cin)
      const r = (a - b - (1 - cin)) | 0;
      if (setf) {
        this.n = r >>> 31; this.z = r === 0 ? 1 : 0;
        this.c = (a >>> 0) >= (b >>> 0) + (1 - cin) ? 1 : 0;
        this.v = (((a ^ b) & (a ^ r)) >>> 31);
      }
      return r;
    }
    logic(r, setf) { if (setf) { this.n = r >>> 31; this.z = r === 0 ? 1 : 0; this.c = this.sc; } return r; }

    saveState() {
      return { r: this.r.slice(), b13: this.bank13.slice(), b14: this.bank14.slice(), spsr: this.spsrBank.slice(), fiq: this.fiqR.slice(), usr: this.usrR.slice(),
        cpsr: this.getCPSR(), pc: this.pc, cycles: this.cycles, halted: this.halted, irqLine: this.irqLine, pipe: [this.pipe0, this.pipe1] };
    }
    loadState(o) {
      this.r.set(o.r); this.bank13.set(o.b13); this.bank14.set(o.b14); this.spsrBank.set(o.spsr); this.fiqR.set(o.fiq); this.usrR.set(o.usr);
      const c = o.cpsr;
      this.n = (c >>> 31) & 1; this.z = (c >>> 30) & 1; this.c = (c >>> 29) & 1; this.v = (c >>> 28) & 1;
      this.i = (c >> 7) & 1; this.f = (c >> 6) & 1; this.t = (c >> 5) & 1; this.mode = c & 0x1f;
      this.pc = o.pc; this.cycles = o.cycles; this.halted = o.halted; this.irqLine = o.irqLine;
      [this.pipe0, this.pipe1] = o.pipe;
    }
  }

  function condPass(c, cond) {
    switch (cond) {
      case 0: return c.z === 1;
      case 1: return c.z === 0;
      case 2: return c.c === 1;
      case 3: return c.c === 0;
      case 4: return c.n === 1;
      case 5: return c.n === 0;
      case 6: return c.v === 1;
      case 7: return c.v === 0;
      case 8: return c.c === 1 && c.z === 0;
      case 9: return c.c === 0 || c.z === 1;
      case 10: return c.n === c.v;
      case 11: return c.n !== c.v;
      case 12: return c.z === 0 && c.n === c.v;
      case 13: return c.z === 1 || c.n !== c.v;
      case 14: return true;
      default: return false;
    }
  }

  // =====================================================================
  // ARM instructions
  // =====================================================================
  let ARM, THUMB;

  function armUndefined(c) { c.undefinedInstr(4); }

  function makeDP(opc, S, imm, regShift) {
    const logical = [0, 1, 8, 9, 12, 13, 14, 15].includes(opc);
    const test = opc >= 8 && opc <= 11;
    return function (c, op) {
      const r = c.r, rn = (op >>> 16) & 15, rd = (op >>> 12) & 15;
      let b;
      if (imm) {
        const rot = (op >>> 7) & 0x1e;
        b = ror(op & 0xff, rot);
        c.sc = rot ? b >>> 31 : c.c;
      } else {
        const rm = op & 15, type = (op >>> 5) & 3;
        if (regShift) {
          const s = r[(op >>> 8) & 15] & 0xff;
          c.cycles++;
          b = c.shiftReg(rm === 15 ? r[15] + 4 : r[rm], type, s);
        } else b = c.shiftImm(r[rm], type, (op >>> 7) & 31);
      }
      let a = r[rn];
      if (rn === 15 && regShift) a += 4;
      const setf = S && (rd !== 15 || test || !BANK[c.mode]);
      let res;
      switch (opc) {
        case 0: res = c.logic(a & b, setf); break;
        case 1: res = c.logic(a ^ b, setf); break;
        case 2: res = c.sub(a, b, 1, setf); break;
        case 3: res = c.sub(b, a, 1, setf); break;
        case 4: res = c.add(a, b, 0, setf); break;
        case 5: res = c.add(a, b, c.c, setf); break;
        case 6: res = c.sub(a, b, c.c, setf); break;
        case 7: res = c.sub(b, a, c.c, setf); break;
        case 8: c.logic(a & b, true); break;
        case 9: c.logic(a ^ b, true); break;
        case 10: c.sub(a, b, 1, true); break;
        case 11: c.add(a, b, 0, true); break;
        case 12: res = c.logic(a | b, setf); break;
        case 13: res = c.logic(b, setf); break;
        case 14: res = c.logic(a & ~b, setf); break;
        default: res = c.logic(~b, setf);
      }
      if (test) {
        if (rd === 15 && S && BANK[c.mode]) { c.setCPSR(c.spsr); }
        return;
      }
      if (rd === 15) {
        if (S && BANK[c.mode]) c.setCPSR(c.spsr);
        c.branch(res);
      } else r[rd] = res;
      void logical;
    };
  }

  function armMul(c, op) {
    const r = c.r, rd = (op >>> 16) & 15, rn = (op >>> 12) & 15, rs = (op >>> 8) & 15, rm = op & 15;
    const acc = (op >>> 21) & 1, S = (op >>> 20) & 1;
    let res = Math.imul(r[rm], r[rs]);
    c.cycles += mulCycles(r[rs], true);
    if (acc) { res = (res + (rn === 15 ? r[15] + 4 : r[rn])) | 0; c.cycles++; }
    r[rd] = res;
    if (S) { c.n = res >>> 31; c.z = res === 0 ? 1 : 0; }
    if (rd === 15) c.branch(res);
  }
  function armMull(c, op) {
    const r = c.r, hi = (op >>> 16) & 15, lo = (op >>> 12) & 15, rs = (op >>> 8) & 15, rm = op & 15;
    const signed = (op >>> 22) & 1, acc = (op >>> 21) & 1, S = (op >>> 20) & 1;
    mul64(r[rm], r[rs], signed);
    c.cycles += mulCycles(r[rs], signed) + 1;
    if (acc) { add64(MUL64[0], MUL64[1], r[lo], r[hi]); c.cycles++; }
    r[lo] = MUL64[0]; r[hi] = MUL64[1];
    if (S) { c.n = MUL64[1] >>> 31; c.z = (MUL64[0] | MUL64[1]) === 0 ? 1 : 0; }
  }
  function armSwp(c, op) {
    const r = c.r, rn = (op >>> 16) & 15, rd = (op >>> 12) & 15, rm = op & 15, byte = (op >>> 22) & 1;
    const addr = r[rn] >>> 0, val = rm === 15 ? r[15] + 4 : r[rm];
    let t;
    if (byte) { t = c.bus.read8(addr); c.bus.write8(addr, val & 0xff); }
    else { t = ror(c.bus.read32(addr), (addr & 3) * 8); c.bus.write32(addr, val); }
    c.cycles++;
    if (rd === 15) c.branch(t); else r[rd] = t;
  }
  function armBX(c, op) {
    const v = c.r[op & 15];
    c.t = v & 1;
    c.branch(v);
  }
  function armMRS(c, op) {
    const rd = (op >>> 12) & 15;
    const v = (op >>> 22) & 1 ? c.spsr : c.getCPSR();
    if (rd === 15) c.branch(v); else c.r[rd] = v;
  }
  function armMSR(c, op) {
    let v;
    if (op & 0x02000000) v = ror(op & 0xff, (op >>> 7) & 0x1e);
    else v = c.r[op & 15];
    let mask = 0;
    if (op & 0x80000) mask |= 0xff000000;
    if (op & 0x40000) mask |= 0x00ff0000;
    if (op & 0x20000) mask |= 0x0000ff00;
    if (op & 0x10000) mask |= 0x000000ff;
    if ((op >>> 22) & 1) {
      if (!BANK[c.mode]) return;
      c.spsr = (c.spsr & ~mask) | (v & mask);
    } else {
      if (c.mode === M_USR) mask &= 0xff000000;
      const nv = (c.getCPSR() & ~mask) | (v & mask);
      c.setCPSR(nv | 0x10);
    }
  }
  function armHalf(c, op) {
    const r = c.r, P = (op >>> 24) & 1, U = (op >>> 23) & 1, I = (op >>> 22) & 1, W = (op >>> 21) & 1, L = (op >>> 20) & 1;
    const rn = (op >>> 16) & 15, rd = (op >>> 12) & 15, sh = (op >>> 5) & 3;
    const off = I ? ((op >>> 4) & 0xf0) | (op & 15) : r[op & 15];
    const base = r[rn];
    const addr = (P ? (U ? base + off : base - off) : base) >>> 0;
    const wb = (P ? (U ? base + off : base - off) : (U ? base + off : base - off)) | 0;
    const bus = c.bus;
    if (L) {
      let v;
      if (sh === 1) v = ror(bus.read16(addr), (addr & 1) * 8);
      else if (sh === 2) v = (bus.read8(addr) << 24) >> 24;
      else v = (addr & 1) ? ((bus.read16(addr) >> 8) << 24) >> 24 : (bus.read16(addr) << 16) >> 16;
      c.cycles++;
      if (!P || W) r[rn] = wb;
      if (rd === 15) c.branch(v); else r[rd] = v;
    } else {
      if (sh === 1) bus.write16(addr, (rd === 15 ? r[15] + 4 : r[rd]) & 0xffff);
      else {
        // doubleword forms don't exist on this processor: treat as a load / store of halfwords
        if (sh === 2) { const v = ror(bus.read16(addr), (addr & 1) * 8); c.cycles++; if (!P || W) r[rn] = wb; if (rd === 15) c.branch(v); else r[rd] = v; return; }
        bus.write16(addr, (rd === 15 ? r[15] + 4 : r[rd]) & 0xffff);
      }
      if (!P || W) r[rn] = wb;
    }
  }
  function makeSDT(I, P, U, B, W, L) {
    return function (c, op) {
      const r = c.r, rn = (op >>> 16) & 15, rd = (op >>> 12) & 15;
      let off;
      if (I) off = c.shiftImm(r[op & 15], (op >>> 5) & 3, (op >>> 7) & 31);
      else off = op & 0xfff;
      const base = r[rn];
      const moved = (U ? base + off : base - off) | 0;
      const addr = (P ? moved : base) >>> 0;
      const bus = c.bus;
      if (L) {
        const v = B ? bus.read8(addr) : ror(bus.read32(addr), (addr & 3) * 8);
        c.cycles++;
        if (!P || W) { r[rn] = moved; if (rn === 15) c.branch(moved); }
        if (rd === 15) c.branch(v); else r[rd] = v;
      } else {
        const v = rd === 15 ? r[15] + 4 : r[rd];
        if (B) bus.write8(addr, v & 0xff); else bus.write32(addr, v);
        if (!P || W) { r[rn] = moved; if (rn === 15) c.branch(moved); }
      }
    };
  }
  function armBlock(c, op) {
    const r = c.r, P = (op >>> 24) & 1, U = (op >>> 23) & 1, S = (op >>> 22) & 1, W = (op >>> 21) & 1, L = (op >>> 20) & 1;
    const rn = (op >>> 16) & 15;
    let list = op & 0xffff;
    const bus = c.bus;
    let n = 0; for (let k = list; k; k &= k - 1) n++;
    const base = r[rn];
    let bytes = n * 4, empty = false;
    if (n === 0) { list = 0x8000; bytes = 0x40; empty = true; }
    let addr;
    if (U) addr = P ? base + 4 : base;
    else addr = P ? base - bytes : base - bytes + 4;
    addr >>>= 0;
    const newBase = (U ? base + bytes : base - bytes) | 0;
    const userBank = S && !(L && (list & 0x8000));
    if (L) {
      if (W) r[rn] = newBase;
      for (let k = 0; k < 16; k++) {
        if (!(list & (1 << k))) continue;
        const v = bus.read32(addr);
        addr = (addr + 4) >>> 0;
        if (k === 15) {
          if (S) c.setCPSR(c.spsr);
          c.branch(v);
        } else if (userBank) c.setUser(k, v);
        else r[k] = v;
      }
      c.cycles++;
    } else {
      let first = true;
      for (let k = 0; k < 16; k++) {
        if (!(list & (1 << k))) continue;
        let v = k === 15 ? r[15] + 4 : userBank ? c.getUser(k) : r[k];
        if (k === rn && !first && W && !empty) v = newBase;
        bus.write32(addr, v);
        addr = (addr + 4) >>> 0;
        first = false;
      }
      if (W) r[rn] = newBase;
    }
  }
  function armBranch(c, op) {
    const off = (op << 8) >> 6;
    if (op & 0x01000000) c.r[14] = c.pc;
    c.branch(c.r[15] + off);
  }
  function armSWI(c, op) { c.swi((op >>> 16) & 0xff); }

  // =====================================================================
  // Thumb instructions
  // =====================================================================
  function thShiftImm(c, op) {
    const type = (op >>> 11) & 3, amt = (op >>> 6) & 31, rs = (op >>> 3) & 7, rd = op & 7;
    const v = c.shiftImm(c.r[rs], type, amt);
    c.r[rd] = v; c.n = v >>> 31; c.z = v === 0 ? 1 : 0; c.c = c.sc;
  }
  function thAddSub(c, op) {
    const immF = (op >>> 10) & 1, sub = (op >>> 9) & 1, rn = (op >>> 6) & 7, rs = (op >>> 3) & 7, rd = op & 7;
    const b = immF ? rn : c.r[rn];
    c.r[rd] = sub ? c.sub(c.r[rs], b, 1, true) : c.add(c.r[rs], b, 0, true);
  }
  function thImm(c, op) {
    const o = (op >>> 11) & 3, rd = (op >>> 8) & 7, imm = op & 0xff, r = c.r;
    switch (o) {
      case 0: r[rd] = imm; c.n = 0; c.z = imm === 0 ? 1 : 0; break;
      case 1: c.sub(r[rd], imm, 1, true); break;
      case 2: r[rd] = c.add(r[rd], imm, 0, true); break;
      default: r[rd] = c.sub(r[rd], imm, 1, true);
    }
  }
  function thAlu(c, op) {
    const o = (op >>> 6) & 15, rs = (op >>> 3) & 7, rd = op & 7, r = c.r;
    const a = r[rd], b = r[rs];
    let v;
    switch (o) {
      case 0: v = a & b; break;
      case 1: v = a ^ b; break;
      case 2: v = c.shiftReg(a, 0, b & 0xff); c.c = c.sc; c.cycles++; break;
      case 3: v = c.shiftReg(a, 1, b & 0xff); c.c = c.sc; c.cycles++; break;
      case 4: v = c.shiftReg(a, 2, b & 0xff); c.c = c.sc; c.cycles++; break;
      case 5: r[rd] = c.add(a, b, c.c, true); return;
      case 6: r[rd] = c.sub(a, b, c.c, true); return;
      case 7: v = c.shiftReg(a, 3, b & 0xff); c.c = c.sc; c.cycles++; break;
      case 8: v = a & b; c.n = v >>> 31; c.z = v === 0 ? 1 : 0; return;
      case 9: r[rd] = c.sub(0, b, 1, true); return;
      case 10: c.sub(a, b, 1, true); return;
      case 11: c.add(a, b, 0, true); return;
      case 12: v = a | b; break;
      case 13: v = Math.imul(a, b); c.cycles += mulCycles(a, true); break;
      case 14: v = a & ~b; break;
      default: v = ~b;
    }
    r[rd] = v; c.n = v >>> 31; c.z = v === 0 ? 1 : 0;
  }
  function thHi(c, op) {
    const o = (op >>> 8) & 3, rs = ((op >>> 3) & 15), rd = (op & 7) | ((op >>> 4) & 8), r = c.r;
    const b = r[rs];
    switch (o) {
      case 0: { const v = (r[rd] + b) | 0; if (rd === 15) c.branch(v); else r[rd] = v; break; }
      case 1: c.sub(r[rd], b, 1, true); break;
      case 2: if (rd === 15) c.branch(b); else r[rd] = b; break;
      default: {
        c.t = b & 1;
        c.branch(b);
      }
    }
  }
  function thLdrPc(c, op) {
    const rd = (op >>> 8) & 7, addr = ((c.r[15] & ~2) + (op & 0xff) * 4) >>> 0;
    c.r[rd] = c.bus.read32(addr);
    c.cycles++;
  }
  function thLdrReg(c, op) {
    const o = (op >>> 10) & 3, ro = (op >>> 6) & 7, rb = (op >>> 3) & 7, rd = op & 7, r = c.r;
    const addr = (r[rb] + r[ro]) >>> 0, bus = c.bus;
    switch (o) {
      case 0: bus.write32(addr, r[rd]); break;
      case 1: bus.write8(addr, r[rd] & 0xff); break;
      case 2: r[rd] = ror(bus.read32(addr), (addr & 3) * 8); c.cycles++; break;
      default: r[rd] = bus.read8(addr); c.cycles++;
    }
  }
  function thLdrH(c, op) {
    const o = (op >>> 10) & 3, ro = (op >>> 6) & 7, rb = (op >>> 3) & 7, rd = op & 7, r = c.r;
    const addr = (r[rb] + r[ro]) >>> 0, bus = c.bus;
    switch (o) {
      case 0: bus.write16(addr, r[rd] & 0xffff); break;
      case 1: r[rd] = (bus.read8(addr) << 24) >> 24; c.cycles++; break;
      case 2: r[rd] = ror(bus.read16(addr), (addr & 1) * 8); c.cycles++; break;
      default: r[rd] = (addr & 1) ? ((bus.read16(addr) >> 8) << 24) >> 24 : (bus.read16(addr) << 16) >> 16; c.cycles++;
    }
  }
  function thLdrImm(c, op) {
    const B = (op >>> 12) & 1, L = (op >>> 11) & 1, off = (op >>> 6) & 31, rb = (op >>> 3) & 7, rd = op & 7, r = c.r, bus = c.bus;
    const addr = (r[rb] + (B ? off : off * 4)) >>> 0;
    if (L) { r[rd] = B ? bus.read8(addr) : ror(bus.read32(addr), (addr & 3) * 8); c.cycles++; }
    else if (B) bus.write8(addr, r[rd] & 0xff); else bus.write32(addr, r[rd]);
  }
  function thLdrHImm(c, op) {
    const L = (op >>> 11) & 1, off = ((op >>> 6) & 31) * 2, rb = (op >>> 3) & 7, rd = op & 7, r = c.r;
    const addr = (r[rb] + off) >>> 0;
    if (L) { r[rd] = ror(c.bus.read16(addr), (addr & 1) * 8); c.cycles++; }
    else c.bus.write16(addr, r[rd] & 0xffff);
  }
  function thLdrSp(c, op) {
    const L = (op >>> 11) & 1, rd = (op >>> 8) & 7, r = c.r;
    const addr = (r[13] + (op & 0xff) * 4) >>> 0;
    if (L) { r[rd] = ror(c.bus.read32(addr), (addr & 3) * 8); c.cycles++; }
    else c.bus.write32(addr, r[rd]);
  }
  function thAddr(c, op) {
    const sp = (op >>> 11) & 1, rd = (op >>> 8) & 7;
    c.r[rd] = ((sp ? c.r[13] : c.r[15] & ~2) + (op & 0xff) * 4) | 0;
  }
  function thAddSp(c, op) {
    const v = (op & 0x7f) * 4;
    c.r[13] = (op & 0x80) ? c.r[13] - v : c.r[13] + v;
  }
  function thPushPop(c, op) {
    const L = (op >>> 11) & 1, R = (op >>> 8) & 1, r = c.r, bus = c.bus;
    let list = op & 0xff;
    if (L) {
      if (!list && !R) { // empty list quirk
        const v = bus.read32(r[13]); r[13] += 0x40; c.branch(v); return;
      }
      let a = r[13] >>> 0;
      for (let k = 0; k < 8; k++) if (list & (1 << k)) { r[k] = bus.read32(a); a = (a + 4) >>> 0; }
      if (R) { const v = bus.read32(a); a = (a + 4) >>> 0; r[13] = a; c.cycles++; c.branch(v); return; }
      r[13] = a; c.cycles++;
    } else {
      if (!list && !R) { r[13] -= 0x40; bus.write32(r[13], r[15] + 2); return; }
      let n = R ? 1 : 0; for (let k = list; k; k &= k - 1) n++;
      let a = (r[13] - n * 4) >>> 0;
      r[13] = a;
      for (let k = 0; k < 8; k++) if (list & (1 << k)) { bus.write32(a, r[k]); a = (a + 4) >>> 0; }
      if (R) bus.write32(a, r[14]);
    }
  }
  function thBlock(c, op) {
    const L = (op >>> 11) & 1, rb = (op >>> 8) & 7, list = op & 0xff, r = c.r, bus = c.bus;
    if (!list) {
      if (L) { const v = bus.read32(r[rb]); r[rb] += 0x40; c.branch(v); }
      else { bus.write32(r[rb], r[15] + 2); r[rb] += 0x40; }
      return;
    }
    let n = 0; for (let k = list; k; k &= k - 1) n++;
    let a = r[rb] >>> 0;
    const newBase = (r[rb] + n * 4) | 0;
    if (L) {
      r[rb] = newBase;
      for (let k = 0; k < 8; k++) if (list & (1 << k)) { r[k] = bus.read32(a); a = (a + 4) >>> 0; }
      c.cycles++;
    } else {
      let first = true;
      for (let k = 0; k < 8; k++) if (list & (1 << k)) {
        bus.write32(a, k === rb && !first ? newBase : r[k]);
        a = (a + 4) >>> 0; first = false;
      }
      r[rb] = newBase;
    }
  }
  function thBcond(c, op) {
    const cond = (op >>> 8) & 15;
    if (cond === 15) { c.swi(op & 0xff); return; }
    if (condPass(c, cond)) c.branch(c.r[15] + (((op & 0xff) << 24) >> 23));
  }
  function thB(c, op) { c.branch(c.r[15] + (((op & 0x7ff) << 21) >> 20)); }
  function thBLhi(c, op) { c.r[14] = c.r[15] + (((op & 0x7ff) << 21) >> 9); }
  function thBLlo(c, op) {
    const target = c.r[14] + ((op & 0x7ff) << 1);
    c.r[14] = c.pc | 1;
    c.branch(target);
  }
  function thUndef(c) { c.undefinedInstr(2); }

  function buildTables() {
    ARM = new Array(4096);
    for (let i = 0; i < 4096; i++) {
      const hi = i >> 4, lo = i & 15; // bits 27-20, bits 7-4
      const top3 = hi >> 5;
      let f = armUndefined;
      switch (top3) {
        case 0:
          if (lo === 9) {
            if ((hi & 0xfc) === 0x00) f = armMul;
            else if ((hi & 0xf8) === 0x08) f = armMull;
            else if ((hi & 0xfb) === 0x10) f = armSwp;
            else f = armUndefined;
          } else if ((lo & 9) === 9) f = armHalf;
          else if ((hi & 0xf9) === 0x10) { // TST/TEQ/CMP/CMN without S: PSR transfer / BX
            if (hi === 0x12 && lo === 1) f = armBX;
            else if (lo === 0) f = (hi & 2) ? armMSR : armMRS;
            else f = armUndefined;
          } else f = makeDP((hi >> 1) & 15, hi & 1, 0, lo & 1);
          break;
        case 1:
          if ((hi & 0xf9) === 0x30) f = (hi & 2) ? armMSR : armUndefined;
          else f = makeDP((hi >> 1) & 15, hi & 1, 1, 0);
          break;
        case 2: f = makeSDT(0, (hi >> 4) & 1, (hi >> 3) & 1, (hi >> 2) & 1, (hi >> 1) & 1, hi & 1); break;
        case 3: f = (lo & 1) ? armUndefined : makeSDT(1, (hi >> 4) & 1, (hi >> 3) & 1, (hi >> 2) & 1, (hi >> 1) & 1, hi & 1); break;
        case 4: f = armBlock; break;
        case 5: f = armBranch; break;
        case 6: f = armUndefined; break;
        default: f = (hi & 0x10) ? armSWI : armUndefined;
      }
      ARM[i] = f;
    }
    THUMB = new Array(1024);
    for (let i = 0; i < 1024; i++) {
      const op = i << 6;
      let f = thUndef;
      if ((op & 0xf800) === 0x1800) f = thAddSub;
      else if ((op & 0xe000) === 0x0000) f = thShiftImm;
      else if ((op & 0xe000) === 0x2000) f = thImm;
      else if ((op & 0xfc00) === 0x4000) f = thAlu;
      else if ((op & 0xfc00) === 0x4400) f = thHi;
      else if ((op & 0xf800) === 0x4800) f = thLdrPc;
      else if ((op & 0xf200) === 0x5000) f = thLdrReg;
      else if ((op & 0xf200) === 0x5200) f = thLdrH;
      else if ((op & 0xe000) === 0x6000) f = thLdrImm;
      else if ((op & 0xf000) === 0x8000) f = thLdrHImm;
      else if ((op & 0xf000) === 0x9000) f = thLdrSp;
      else if ((op & 0xf000) === 0xa000) f = thAddr;
      else if ((op & 0xff00) === 0xb000) f = thAddSp;
      else if ((op & 0xf600) === 0xb400) f = thPushPop;
      else if ((op & 0xf000) === 0xc000) f = thBlock;
      else if ((op & 0xf000) === 0xd000) f = thBcond;
      else if ((op & 0xf800) === 0xe000) f = thB;
      else if ((op & 0xf800) === 0xf000) f = thBLhi;
      else if ((op & 0xf800) === 0xf800) f = thBLlo;
      THUMB[i] = f;
    }
    ARM7.armTable = ARM; ARM7.thumbTable = THUMB;
  }

  ARM7.condPass = condPass;
  global.RPARM7 = ARM7;
})(typeof window !== "undefined" ? window : globalThis);
