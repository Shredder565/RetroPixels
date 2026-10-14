// RetroPixels — Intel 8080 processor core. Original code, written from the
// documented behaviour of the 8080 (Intel's 8080 manuals): all documented
// instructions, the undocumented duplicate opcodes, the 8080's own flag rules
// (half carry on AND/compare, bit 1 of the flags always set) and its timings.
// Used by the Space Invaders arcade board.
//
//   const c = new RP8080({read(a), write(a,v), in(port), out(port,v)})
//   c.step() -> clock cycles used
//   c.interrupt(opcode) -> cycles used, 0 if interrupts are off (the 8080 runs the opcode the board
//                          puts on the bus, usually RST n: 0xC7 | n << 3)
"use strict";

(function (global) {
  const CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128;
  // sign, zero and parity of every byte
  const SZP = new Uint8Array(256);
  for (let i = 0; i < 256; i++) {
    let p = i; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    SZP[i] = (i & SF) | (i ? 0 : ZF) | ((p & 1) ? 0 : PF);
  }
  // cycles for each opcode (taken branches / calls / returns add more below)
  const CYC = new Uint8Array(256);
  (function () {
    for (let op = 0; op < 256; op++) {
      let t = 4;
      if (op < 0x40) {
        switch (op & 7) {
          case 0: t = 4; break;
          case 1: t = 10; break;                           // LXI / DAD
          case 2: t = [7, 7, 7, 7, 16, 16, 13, 13][(op >> 3) & 7]; break; // STAX/LDAX, SHLD/LHLD, STA/LDA
          case 3: t = 5; break;                            // INX / DCX
          case 4: case 5: t = ((op >> 3) & 7) === 6 ? 10 : 5; break; // INR / DCR
          case 6: t = ((op >> 3) & 7) === 6 ? 10 : 7; break;  // MVI
          case 7: t = 4; break;
        }
      } else if (op < 0x80) {
        t = op === 0x76 ? 7 : ((op & 7) === 6 || ((op >> 3) & 7) === 6) ? 7 : 5;
      } else if (op < 0xc0) {
        t = (op & 7) === 6 ? 7 : 4;
      } else {
        switch (op & 7) {
          case 0: t = 5; break;   // Rcc (11 when taken)
          case 1: t = 10; break;  // POP / RET / PCHL / SPHL
          case 2: t = 10; break;  // Jcc
          case 3: t = 10; break;  // JMP, OUT, IN, XTHL, XCHG, DI, EI
          case 4: t = 11; break;  // Ccc (17 when taken)
          case 5: t = 11; break;  // PUSH / CALL
          case 6: t = 7; break;   // immediate arithmetic
          case 7: t = 11; break;  // RST
        }
      }
      CYC[op] = t;
    }
    CYC[0xc9] = CYC[0xd9] = 10; CYC[0xe9] = 5; CYC[0xf9] = 5;
    CYC[0xe3] = 18; CYC[0xeb] = 4; CYC[0xf3] = 4; CYC[0xfb] = 4;
    CYC[0xcd] = CYC[0xdd] = CYC[0xed] = CYC[0xfd] = 17;
  })();

  function RP8080(bus) {
    const rd = (ad) => bus.read(ad & 0xffff) & 0xff;
    const wr = (ad, v) => bus.write(ad & 0xffff, v & 0xff);
    // registers: B C D E H L M(memory) A — index 6 is memory at HL
    let a, f, b, c, d, e, h, l, sp, pc, inte, halted, eiDelay;

    function fetch() { const v = rd(pc); pc = (pc + 1) & 0xffff; return v; }
    function fetch16() { const lo = fetch(); return lo | (fetch() << 8); }
    function push(v) { sp = (sp - 1) & 0xffff; wr(sp, v >> 8); sp = (sp - 1) & 0xffff; wr(sp, v); }
    function pop() { const lo = rd(sp); sp = (sp + 1) & 0xffff; const hi = rd(sp); sp = (sp + 1) & 0xffff; return lo | (hi << 8); }
    function getR(r) {
      switch (r) { case 0: return b; case 1: return c; case 2: return d; case 3: return e;
        case 4: return h; case 5: return l; case 6: return rd((h << 8) | l); default: return a; }
    }
    function setR(r, v) {
      switch (r) { case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break;
        case 4: h = v; break; case 5: l = v; break; case 6: wr((h << 8) | l, v); break; default: a = v; }
    }
    function getRP(p) { return p === 0 ? (b << 8) | c : p === 1 ? (d << 8) | e : p === 2 ? (h << 8) | l : sp; }
    function setRP(p, v) {
      v &= 0xffff;
      if (p === 0) { b = v >> 8; c = v & 0xff; } else if (p === 1) { d = v >> 8; e = v & 0xff; }
      else if (p === 2) { h = v >> 8; l = v & 0xff; } else sp = v;
    }
    function cond(cc) {
      switch (cc) {
        case 0: return !(f & ZF); case 1: return !!(f & ZF);
        case 2: return !(f & CF); case 3: return !!(f & CF);
        case 4: return !(f & PF); case 5: return !!(f & PF);
        case 6: return !(f & SF); default: return !!(f & SF);
      }
    }
    // the eight accumulator operations: ADD ADC SUB SBB ANA XRA ORA CMP
    function alu(op, v) {
      switch (op) {
        case 0: case 1: {
          const cy = op === 1 ? (f & CF) : 0, r = a + v + cy;
          f = SZP[r & 0xff] | (r > 0xff ? CF : 0) | ((a ^ v ^ r) & AF);
          a = r & 0xff; return;
        }
        case 2: case 3: case 7: {
          // the 8080 subtracts by adding the complement: half carry is the carry out of bit 3 of that sum
          const cy = op === 3 ? (f & CF) : 0, r = a - v - cy;
          f = SZP[r & 0xff] | (r < 0 ? CF : 0) | (~(a ^ v ^ r) & AF);
          if (op !== 7) a = r & 0xff;
          return;
        }
        case 4: { const r = a & v; f = SZP[r] | (((a | v) & 8) ? AF : 0); a = r; return; }
        case 5: a ^= v; f = SZP[a]; return;
        default: a |= v; f = SZP[a]; return;
      }
    }
    function inr(v) { v = (v + 1) & 0xff; f = (f & CF) | SZP[v] | ((v & 0xf) === 0 ? AF : 0); return v; }
    function dcr(v) { v = (v - 1) & 0xff; f = (f & CF) | SZP[v] | ((v & 0xf) !== 0xf ? AF : 0); return v; }
    function daa() {
      let add = 0, cy = f & CF;
      const lo = a & 0xf, hi = a >> 4;
      if ((f & AF) || lo > 9) add = 6;
      if (cy || hi > 9 || (hi >= 9 && lo > 9)) { add |= 0x60; cy = CF; }
      const r = a + add;
      f = SZP[r & 0xff] | cy | ((a ^ add ^ r) & AF);
      a = r & 0xff;
    }

    function exec(op) {
      let t = CYC[op];
      if (op >= 0x40 && op < 0x80) {
        if (op === 0x76) { halted = true; return t; }
        setR((op >> 3) & 7, getR(op & 7)); return t;
      }
      if (op >= 0x80 && op < 0xc0) { alu((op >> 3) & 7, getR(op & 7)); return t; }
      const y = (op >> 3) & 7, p = y >> 1;
      if (op < 0x40) {
        switch (op & 7) {
          case 0: return t; // NOP (and its undocumented copies)
          case 1:
            if (op & 8) { // DAD
              const r = ((h << 8) | l) + getRP(p);
              f = (f & ~CF) | (r > 0xffff ? CF : 0); h = (r >> 8) & 0xff; l = r & 0xff;
            } else setRP(p, fetch16());
            return t;
          case 2:
            switch (y) {
              case 0: wr((b << 8) | c, a); break;
              case 1: a = rd((b << 8) | c); break;
              case 2: wr((d << 8) | e, a); break;
              case 3: a = rd((d << 8) | e); break;
              case 4: { const ad = fetch16(); wr(ad, l); wr(ad + 1, h); break; }
              case 5: { const ad = fetch16(); l = rd(ad); h = rd(ad + 1); break; }
              case 6: wr(fetch16(), a); break;
              default: a = rd(fetch16()); break;
            }
            return t;
          case 3: setRP(p, getRP(p) + ((op & 8) ? -1 : 1)); return t;
          case 4: setR(y, inr(getR(y))); return t;
          case 5: setR(y, dcr(getR(y))); return t;
          case 6: setR(y, fetch()); return t;
          default:
            switch (y) {
              case 0: a = ((a << 1) | (a >> 7)) & 0xff; f = (f & ~CF) | (a & 1); break;           // RLC
              case 1: f = (f & ~CF) | (a & 1); a = ((a >> 1) | (a << 7)) & 0xff; break;           // RRC
              case 2: { const cy = f & CF; f = (f & ~CF) | (a >> 7); a = ((a << 1) | cy) & 0xff; break; } // RAL
              case 3: { const cy = f & CF; f = (f & ~CF) | (a & 1); a = (a >> 1) | (cy << 7); break; }    // RAR
              case 4: daa(); break;
              case 5: a ^= 0xff; break;            // CMA
              case 6: f |= CF; break;              // STC
              default: f ^= CF; break;             // CMC
            }
            return t;
        }
      }
      switch (op & 7) {
        case 0: if (cond(y)) { pc = pop(); t += 6; } return t;
        case 1:
          switch (y) {
            case 1: case 3: pc = pop(); break;                  // RET
            case 5: pc = (h << 8) | l; break;                   // PCHL
            case 7: sp = (h << 8) | l; break;                   // SPHL
            case 6: { const v = pop(); a = v >> 8; f = (v & 0xd7) | 2; break; } // POP PSW
            default: setRP(p, pop());
          }
          return t;
        case 2: { const ad = fetch16(); if (cond(y)) pc = ad; return t; }
        case 3:
          switch (y) {
            case 0: case 1: pc = fetch16(); break;              // JMP
            case 2: { const port = fetch(); if (bus.out) bus.out(port, a); break; }
            case 3: { const port = fetch(); a = (bus.in ? bus.in(port) : 0xff) & 0xff; break; }
            case 4: { const v = rd(sp) | (rd(sp + 1) << 8); wr(sp, l); wr(sp + 1, h); h = v >> 8; l = v & 0xff; break; } // XTHL
            case 5: { let x = d; d = h; h = x; x = e; e = l; l = x; break; } // XCHG
            case 6: inte = false; break;                        // DI
            default: inte = true; eiDelay = true; break;        // EI (takes effect after the next instruction)
          }
          return t;
        case 4: { const ad = fetch16(); if (cond(y)) { push(pc); pc = ad; t += 6; } return t; }
        case 5:
          if (op & 8) { const ad = fetch16(); push(pc); pc = ad; return t; } // CALL (and copies)
          push(p === 3 ? (a << 8) | ((f & 0xd7) | 2) : getRP(p));
          return t;
        case 6: alu(y, fetch()); return t;
        default: push(pc); pc = y << 3; return t;               // RST
      }
    }

    function step() {
      eiDelay = false;
      if (halted) return 4;
      return exec(fetch());
    }

    // An interrupting device puts one instruction on the bus (normally RST n).
    function interrupt(op) {
      if (!inte || eiDelay) return false;
      inte = false; halted = false;
      return exec(op & 0xff);
    }

    this.step = step;
    this.interrupt = interrupt;
    this.reset = function () {
      a = b = c = d = e = h = l = 0; f = 2; sp = 0; pc = 0;
      inte = false; halted = false; eiDelay = false;
    };
    this.getPC = () => pc;
    this.setPC = (v) => { pc = v & 0xffff; };
    this.isHalted = () => halted;
    this.interruptsOn = () => inte;
    const KEYS = ["a", "f", "b", "c", "d", "e", "h", "l", "sp", "pc", "inte", "halted", "ei"];
    this.getState = function () {
      return [a, f, b, c, d, e, h, l, sp, pc, inte ? 1 : 0, halted ? 1 : 0, eiDelay ? 1 : 0];
    };
    this.setState = function (s) {
      [a, f, b, c, d, e, h, l, sp, pc] = s;
      inte = !!s[10]; halted = !!s[11]; eiDelay = !!s[12];
    };
    this.regs = function () {
      const s = this.getState(), o = {};
      KEYS.forEach((k, i) => { o[k] = s[i]; });
      return o;
    };
    // used by the test harness (CP/M calls)
    this.setRegs = function (o) {
      const s = this.getState();
      KEYS.forEach((k, i) => { if (o[k] !== undefined) s[i] = o[k]; });
      this.setState(s);
    };
    this.reset();
  }
  global.RP8080 = RP8080;
})(typeof window !== "undefined" ? window : globalThis);
