// RetroPixels — WDC 65C816 processor (the SNES main CPU). Original code written
// from the processor's documented behaviour (native and emulation modes).
//
//   const c = new RP65816({ read(addr24), write(addr24, v), idle() });
//   c.step() runs one instruction (or an interrupt); timing is counted by the bus.
"use strict";

(function (global) {
  function RP65816(bus) {
    // registers
    let a = 0, x = 0, y = 0, s = 0x1ff, d = 0, db = 0, pb = 0, pc = 0;
    let fn = 0, fv = 0, fm = 1, fx = 1, fd = 0, fi = 1, fz = 0, fc = 0, e = 1;
    let waiting = false, stopped = false;
    const self = this;

    const rd = (ad) => bus.read(ad & 0xffffff);
    const wr = (ad, v) => bus.write(ad & 0xffffff, v & 0xff);
    const io = () => bus.idle();
    const fetch = () => { const v = rd((pb << 16) | pc); pc = (pc + 1) & 0xffff; return v; };
    const fetch16 = () => { const l = fetch(); return l | (fetch() << 8); };
    const fetch24 = () => { const l = fetch16(); return l | (fetch() << 16); };

    const getP = () => (fn << 7) | (fv << 6) | (fm << 5) | (fx << 4) | (fd << 3) | (fi << 2) | (fz << 1) | fc;
    function setP(v) {
      fn = (v >> 7) & 1; fv = (v >> 6) & 1; fd = (v >> 3) & 1; fi = (v >> 2) & 1; fz = (v >> 1) & 1; fc = v & 1;
      if (e) { fm = 1; fx = 1; } else { fm = (v >> 5) & 1; fx = (v >> 4) & 1; }
      if (fx) { x &= 0xff; y &= 0xff; }
    }
    // stack
    function push(v) { wr(s, v); s = e ? 0x100 | ((s - 1) & 0xff) : (s - 1) & 0xffff; }
    function pull() { s = e ? 0x100 | ((s + 1) & 0xff) : (s + 1) & 0xffff; return rd(s); }
    // "new" instructions use the full 16-bit stack pointer even in emulation mode
    function pushN(v) { wr(s, v); s = (s - 1) & 0xffff; }
    function pullN() { s = (s + 1) & 0xffff; return rd(s); }
    function fixS() { if (e) s = 0x100 | (s & 0xff); }

    const nz8 = (v) => { fn = (v >> 7) & 1; fz = (v & 0xff) === 0 ? 1 : 0; };
    const nz16 = (v) => { fn = (v >> 15) & 1; fz = (v & 0xffff) === 0 ? 1 : 0; };
    const nzM = (v) => (fm ? nz8(v) : nz16(v));
    const nzX = (v) => (fx ? nz8(v) : nz16(v));

    // ---- addressing (return 24-bit effective address) ----
    function dpBase(off) { if (d & 0xff) io(); return (d + off) & 0xffff; }
    // direct page with index, emulation-mode page wrap when DL = 0
    function dpIdx(off, idx) {
      if (d & 0xff) io();
      io();
      if (e && !(d & 0xff)) return (d & 0xff00) | ((off + idx) & 0xff);
      return (d + off + idx) & 0xffff;
    }
    // 16-bit read of a direct-page pointer (wraps in page in emulation mode with DL = 0)
    function dpPtr(ad) {
      const lo = rd(ad);
      const hiAd = e && !(d & 0xff) ? (ad & 0xff00) | ((ad + 1) & 0xff) : (ad + 1) & 0xffff;
      return lo | (rd(hiAd) << 8);
    }
    function dpPtrLong(ad) { return rd(ad) | (rd((ad + 1) & 0xffff) << 8) | (rd((ad + 2) & 0xffff) << 16); }

    // memory operand readers / writers for M or X width, at a 24-bit address (data wraps in 24 bits)
    function rdW(ad, w8) { const l = rd(ad); if (w8) return l; return l | (rd((ad + 1) & 0xffffff) << 8); }
    function wrW(ad, v, w8) { wr(ad, v); if (!w8) wr((ad + 1) & 0xffffff, v >> 8); }
    // bank-0 (direct page / stack) variants wrap at 16 bits
    function rdW0(ad, w8) { const l = rd(ad); if (w8) return l; return l | (rd((ad + 1) & 0xffff) << 8); }
    function wrW0(ad, v, w8) { wr(ad, v); if (!w8) wr((ad + 1) & 0xffff, v >> 8); }

    // Address modes. `kind`: 0 read, 1 write, 2 read-modify-write (affects the extra index cycle).
    // Returns {ad, bank0} encoded as ad | (bank0 ? 0x1000000 : 0)
    const B0 = 0x1000000;
    function addr(mode, kind, wide) {
      switch (mode) {
        case 0: { return B0 | dpBase(fetch()); }                             // dp
        case 1: { return B0 | dpIdx(fetch(), x); }                           // dp,X
        case 2: { return B0 | dpIdx(fetch(), y); }                           // dp,Y
        case 3: { return (db << 16) | fetch16(); }                           // abs
        case 4: { const b = fetch16(), base = (db << 16) | b, ad = (base + x) & 0xffffff; if (kind || !fx || ((b & 0xff00) !== ((b + x) & 0xff00))) io(); return ad; } // abs,X
        case 5: { const b = fetch16(), base = (db << 16) | b, ad = (base + y) & 0xffffff; if (kind || !fx || ((b & 0xff00) !== ((b + y) & 0xff00))) io(); return ad; } // abs,Y
        case 6: { return fetch24(); }                                        // long
        case 7: { return (fetch24() + x) & 0xffffff; }                       // long,X
        case 8: { const p = dpBase(fetch()); return (db << 16) | dpPtr(p); } // (dp)
        case 9: { const p = dpIdx(fetch(), x); return (db << 16) | dpPtr(p); } // (dp,X)
        case 10: { // (dp),Y
          const p = dpBase(fetch()), b = dpPtr(p), ad = (((db << 16) | b) + y) & 0xffffff;
          if (kind || !fx || ((b & 0xff00) !== ((b + y) & 0xff00))) io();
          return ad;
        }
        case 11: { const p = dpBase(fetch()); return dpPtrLong(p); }          // [dp]
        case 12: { const p = dpBase(fetch()); return (dpPtrLong(p) + y) & 0xffffff; } // [dp],Y
        case 13: { const o = fetch(); io(); return B0 | ((s + o) & 0xffff); } // sr,S
        default: { // (sr,S),Y
          const o = fetch(); io();
          const p = (s + o) & 0xffff;
          const b = rd(p) | (rd((p + 1) & 0xffff) << 8);
          io();
          return (((db << 16) | b) + y) & 0xffffff;
        }
      }
      void wide;
    }
    const rdOp = (ea, w8) => (ea & B0 ? rdW0(ea & 0xffffff, w8) : rdW(ea, w8));
    const wrOp = (ea, v, w8) => (ea & B0 ? wrW0(ea & 0xffffff, v, w8) : wrW(ea, v, w8));

    // ALU
    function adc(v) {
      const w8 = fm;
      let r;
      const acc = w8 ? a & 0xff : a;
      if (!fd) {
        r = acc + v + fc;
        if (w8) { fv = (~(acc ^ v) & (acc ^ r) & 0x80) ? 1 : 0; fc = r > 0xff ? 1 : 0; r &= 0xff; }
        else { fv = (~(acc ^ v) & (acc ^ r) & 0x8000) ? 1 : 0; fc = r > 0xffff ? 1 : 0; r &= 0xffff; }
      } else {
        // decimal: digit by digit
        let c = fc, res = 0;
        const digits = w8 ? 2 : 4;
        let binNoCarry = 0;
        for (let k = 0; k < digits; k++) {
          let t = ((acc >> (k * 4)) & 15) + ((v >> (k * 4)) & 15) + c;
          if (k === digits - 1) binNoCarry = res | ((t & 15) << (k * 4));
          c = 0;
          if (t > 9) { t += 6; c = 1; }
          res |= (t & 15) << (k * 4);
        }
        const top = w8 ? 0x80 : 0x8000;
        fv = (~(acc ^ v) & (acc ^ binNoCarry) & top) ? 1 : 0;
        fc = c; r = res;
      }
      if (w8) { a = (a & 0xff00) | r; nz8(r); } else { a = r; nz16(r); }
    }
    function sbc(v) {
      const w8 = fm;
      if (!fd) { adc(w8 ? v ^ 0xff : v ^ 0xffff); return; }
      const acc = w8 ? a & 0xff : a;
      const digits = w8 ? 2 : 4;
      let c = fc, res = 0, binNoBorrow = 0;
      const nv = w8 ? v ^ 0xff : v ^ 0xffff;
      for (let k = 0; k < digits; k++) {
        let t = ((acc >> (k * 4)) & 15) + ((nv >> (k * 4)) & 15) + c;
        if (k === digits - 1) binNoBorrow = res | ((t & 15) << (k * 4));
        c = t > 15 ? 1 : 0;
        if (!c) t -= 6;
        res |= (t & 15) << (k * 4);
      }
      const top = w8 ? 0x80 : 0x8000;
      fv = (~(acc ^ nv) & (acc ^ binNoBorrow) & top) ? 1 : 0;
      fc = c;
      if (w8) { a = (a & 0xff00) | (res & 0xff); nz8(res); } else { a = res & 0xffff; nz16(res); }
    }
    function cmp(reg, v, w8) { const r = (w8 ? reg & 0xff : reg) - v; fc = r >= 0 ? 1 : 0; if (w8) nz8(r); else nz16(r); }
    function setA(v) { if (fm) a = (a & 0xff00) | (v & 0xff); else a = v & 0xffff; }
    function aluOp(g, v) {
      switch (g) {
        case 0: setA((fm ? a & 0xff : a) | v); nzM(fm ? a & 0xff : a); break;
        case 1: setA((fm ? a & 0xff : a) & v); nzM(fm ? a & 0xff : a); break;
        case 2: setA((fm ? a & 0xff : a) ^ v); nzM(fm ? a & 0xff : a); break;
        case 3: adc(v); break;
        case 5: setA(v); nzM(v); break;
        case 6: cmp(a, v, fm); break;
        case 7: sbc(v); break;
      }
    }
    // read-modify-write ops
    function rmw(op, v, w8) {
      const top = w8 ? 0x80 : 0x8000, mask = w8 ? 0xff : 0xffff;
      let r;
      switch (op) {
        case 0: fc = v & top ? 1 : 0; r = (v << 1) & mask; break;               // ASL
        case 1: r = ((v << 1) | fc) & mask; fc = v & top ? 1 : 0; break;      // ROL
        case 2: fc = v & 1; r = v >> 1; break;                                 // LSR
        case 3: r = (v >> 1) | (fc ? top : 0); fc = v & 1; break;             // ROR
        case 4: r = (v - 1) & mask; break;                                     // DEC
        default: r = (v + 1) & mask;                                           // INC
      }
      if (w8) nz8(r); else nz16(r);
      return r;
    }
    function branch(cond) {
      const o = fetch();
      if (!cond) return;
      io();
      const np = (pc + ((o << 24) >> 24)) & 0xffff;
      if (e && (np & 0xff00) !== (pc & 0xff00)) io();
      pc = np;
    }
    function interrupt(vecNative, vecEmu, isBrk) {
      if (!e) push(pb);
      push(pc >> 8); push(pc & 0xff);
      push(e ? (getP() & ~0x10) | (isBrk ? 0x10 : 0) | 0x20 : getP());
      fi = 1; fd = 0; pb = 0;
      const v = e ? vecEmu : vecNative;
      pc = rd(v) | (rd(v + 1) << 8);
    }

    // operand mode tables for the 8 "group one" instructions
    const G1MODE = {};
    [[0x01, 9], [0x03, 13], [0x05, 0], [0x07, 11], [0x0d, 3], [0x0f, 6], [0x11, 10], [0x12, 8], [0x13, 14], [0x15, 1], [0x17, 12], [0x19, 5], [0x1d, 4], [0x1f, 7]]
      .forEach(([k, m]) => { G1MODE[k] = m; });

    function exec(op) {
      const low = op & 0x1f, g = op >> 5;
      if (G1MODE[low] !== undefined && op !== 0x89) {
        const mode = G1MODE[low];
        if (g === 4) { const ea = addr(mode, 1); wrOp(ea, a, fm); return; } // STA
        const ea = addr(mode, 0);
        aluOp(g, rdOp(ea, fm));
        return;
      }
      if (low === 0x09 && op !== 0x89) { const v = fm ? fetch() : fetch16(); aluOp(g, v); return; }
      switch (op) {
        // ---- read-modify-write ----
        case 0x06: case 0x26: case 0x46: case 0x66: case 0xc6: case 0xe6:
        case 0x0e: case 0x2e: case 0x4e: case 0x6e: case 0xce: case 0xee:
        case 0x16: case 0x36: case 0x56: case 0x76: case 0xd6: case 0xf6:
        case 0x1e: case 0x3e: case 0x5e: case 0x7e: case 0xde: case 0xfe: {
          const kind = [0, 1, 2, 3, -1, -1, 4, 5][op >> 5];
          const mode = [0, 3, 1, 4][((op >> 3) & 3)];
          const ea = addr(mode, 2);
          const v = rdOp(ea, fm);
          io();
          const r = rmw(kind, v, fm);
          if (fm) wrOp(ea, r, true);
          else { // 16-bit writes go high byte first
            const base = ea & 0xffffff, hiA = ea & B0 ? (base + 1) & 0xffff : (base + 1) & 0xffffff;
            wr(hiA, r >> 8); wr(base, r);
          }
          return;
        }
        case 0x0a: case 0x2a: case 0x4a: case 0x6a: case 0x3a: case 0x1a: {
          io();
          const kind = { 0x0a: 0, 0x2a: 1, 0x4a: 2, 0x6a: 3, 0x3a: 4, 0x1a: 5 }[op];
          setA(rmw(kind, fm ? a & 0xff : a, fm));
          return;
        }
        case 0x04: case 0x0c: case 0x14: case 0x1c: { // TSB / TRB
          const ea = addr(op & 8 ? 3 : 0, 2);
          const v = rdOp(ea, fm);
          io();
          const acc = fm ? a & 0xff : a;
          fz = (v & acc) === 0 ? 1 : 0;
          const r = op & 0x10 ? v & ~acc : v | acc;
          if (fm) wrOp(ea, r, true);
          else { const base = ea & 0xffffff, hiA = ea & B0 ? (base + 1) & 0xffff : (base + 1) & 0xffffff; wr(hiA, r >> 8); wr(base, r); }
          return;
        }
        // ---- BIT ----
        case 0x89: { const v = fm ? fetch() : fetch16(); fz = ((fm ? a & 0xff : a) & v) === 0 ? 1 : 0; return; }
        case 0x24: case 0x2c: case 0x34: case 0x3c: {
          const ea = addr([0, 3, 1, 4][(op >> 3) & 3], 0);
          const v = rdOp(ea, fm);
          fz = ((fm ? a & 0xff : a) & v) === 0 ? 1 : 0;
          if (fm) { fn = (v >> 7) & 1; fv = (v >> 6) & 1; } else { fn = (v >> 15) & 1; fv = (v >> 14) & 1; }
          return;
        }
        // ---- index loads / stores / compares ----
        case 0xa2: x = fx ? fetch() : fetch16(); nzX(x); return;
        case 0xa0: y = fx ? fetch() : fetch16(); nzX(y); return;
        case 0xa6: case 0xae: case 0xb6: case 0xbe: { const ea = addr([0, 3, 2, 5][(op >> 3) & 3], 0); x = rdOp(ea, fx); nzX(x); return; }
        case 0xa4: case 0xac: case 0xb4: case 0xbc: { const ea = addr([0, 3, 1, 4][(op >> 3) & 3], 0); y = rdOp(ea, fx); nzX(y); return; }
        case 0x86: case 0x8e: case 0x96: { const ea = addr(op === 0x86 ? 0 : op === 0x8e ? 3 : 2, 1); wrOp(ea, x, fx); return; }
        case 0x84: case 0x8c: case 0x94: { const ea = addr(op === 0x84 ? 0 : op === 0x8c ? 3 : 1, 1); wrOp(ea, y, fx); return; }
        case 0x64: case 0x74: case 0x9c: case 0x9e: { const ea = addr(op === 0x64 ? 0 : op === 0x74 ? 1 : op === 0x9c ? 3 : 4, 1); wrOp(ea, 0, fm); return; }
        case 0xe0: cmp(x, fx ? fetch() : fetch16(), fx); return;
        case 0xc0: cmp(y, fx ? fetch() : fetch16(), fx); return;
        case 0xe4: case 0xec: { const ea = addr(op === 0xe4 ? 0 : 3, 0); cmp(x, rdOp(ea, fx), fx); return; }
        case 0xc4: case 0xcc: { const ea = addr(op === 0xc4 ? 0 : 3, 0); cmp(y, rdOp(ea, fx), fx); return; }
        // ---- inc/dec index ----
        case 0xe8: io(); x = (x + 1) & (fx ? 0xff : 0xffff); nzX(x); return;
        case 0xc8: io(); y = (y + 1) & (fx ? 0xff : 0xffff); nzX(y); return;
        case 0xca: io(); x = (x - 1) & (fx ? 0xff : 0xffff); nzX(x); return;
        case 0x88: io(); y = (y - 1) & (fx ? 0xff : 0xffff); nzX(y); return;
        // ---- branches ----
        case 0x10: branch(!fn); return;
        case 0x30: branch(fn); return;
        case 0x50: branch(!fv); return;
        case 0x70: branch(fv); return;
        case 0x90: branch(!fc); return;
        case 0xb0: branch(fc); return;
        case 0xd0: branch(!fz); return;
        case 0xf0: branch(fz); return;
        case 0x80: branch(true); return;
        case 0x82: { const o = fetch16(); io(); pc = (pc + o) & 0xffff; return; }
        // ---- jumps / calls ----
        case 0x4c: pc = fetch16(); return;
        case 0x5c: { const t = fetch24(); pc = t & 0xffff; pb = t >> 16; return; }
        case 0x6c: { const p = fetch16(); pc = rd(p) | (rd((p + 1) & 0xffff) << 8); return; }
        case 0x7c: { const p = fetch16(); io(); const q = (pb << 16) | ((p + x) & 0xffff); pc = rd(q) | (rd((pb << 16) | ((p + x + 1) & 0xffff)) << 8); return; }
        case 0xdc: { const p = fetch16(); pc = rd(p) | (rd((p + 1) & 0xffff) << 8); pb = rd((p + 2) & 0xffff); return; }
        case 0x20: { const t = fetch16(); io(); const r = (pc - 1) & 0xffff; push(r >> 8); push(r & 0xff); pc = t; return; }
        case 0xfc: { const l = fetch(); const r = pc; pushN(r >> 8); pushN(r & 0xff); const h = fetch(); io(); const p = (l | (h << 8)); const q = (p + x) & 0xffff; pc = rd((pb << 16) | q) | (rd((pb << 16) | ((q + 1) & 0xffff)) << 8); fixS(); return; }
        case 0x22: { const l = fetch16(); pushN(pb); io(); const b = fetch(); const r = (pc - 1) & 0xffff; pushN(r >> 8); pushN(r & 0xff); pc = l; pb = b; fixS(); return; }
        case 0x60: { io(); io(); const l = pull(); pc = ((l | (pull() << 8)) + 1) & 0xffff; io(); return; }
        case 0x6b: { io(); io(); const l = pullN(); const h = pullN(); pb = pullN(); pc = ((l | (h << 8)) + 1) & 0xffff; fixS(); return; }
        case 0x40: {
          io(); io();
          setP(pull());
          const l = pull(); pc = l | (pull() << 8);
          if (!e) pb = pull();
          return;
        }
        // ---- stack ----
        case 0x48: io(); if (fm) push(a & 0xff); else { push(a >> 8); push(a & 0xff); } return;
        case 0xda: io(); if (fx) push(x & 0xff); else { push(x >> 8); push(x & 0xff); } return;
        case 0x5a: io(); if (fx) push(y & 0xff); else { push(y >> 8); push(y & 0xff); } return;
        case 0x68: io(); io(); if (fm) { const v = pull(); a = (a & 0xff00) | v; nz8(v); } else { const l = pull(); a = l | (pull() << 8); nz16(a); } return;
        case 0xfa: io(); io(); if (fx) { x = pull(); nz8(x); } else { const l = pull(); x = l | (pull() << 8); nz16(x); } return;
        case 0x7a: io(); io(); if (fx) { y = pull(); nz8(y); } else { const l = pull(); y = l | (pull() << 8); nz16(y); } return;
        case 0x08: io(); push(e ? getP() | 0x30 : getP()); return;
        case 0x28: io(); io(); setP(pull()); return;
        case 0x8b: io(); push(db); return;
        case 0xab: io(); io(); db = pullN(); nz8(db); fixS(); return;
        case 0x0b: io(); pushN(d >> 8); pushN(d & 0xff); fixS(); return;
        case 0x2b: { io(); io(); const l = pullN(); d = l | (pullN() << 8); nz16(d); fixS(); return; }
        case 0x4b: io(); push(pb); return;
        case 0xf4: { const v = fetch16(); pushN(v >> 8); pushN(v & 0xff); fixS(); return; }
        case 0xd4: { const p = dpBase(fetch()); const v = rd(p) | (rd((p + 1) & 0xffff) << 8); pushN(v >> 8); pushN(v & 0xff); fixS(); return; }
        case 0x62: { const o = fetch16(); io(); const v = (pc + o) & 0xffff; pushN(v >> 8); pushN(v & 0xff); fixS(); return; }
        // ---- transfers ----
        case 0xaa: io(); x = fx ? a & 0xff : a; nzX(x); return;
        case 0xa8: io(); y = fx ? a & 0xff : a; nzX(y); return;
        case 0x8a: io(); if (fm) { a = (a & 0xff00) | (x & 0xff); nz8(a); } else { a = x; nz16(a); } return;
        case 0x98: io(); if (fm) { a = (a & 0xff00) | (y & 0xff); nz8(a); } else { a = y; nz16(a); } return;
        case 0xba: io(); x = fx ? s & 0xff : s; nzX(x); return;
        case 0x9a: io(); s = e ? 0x100 | (x & 0xff) : x; return;
        case 0x9b: io(); y = x; nzX(y); return;
        case 0xbb: io(); x = y; nzX(x); return;
        case 0x5b: io(); d = a; nz16(d); return;
        case 0x7b: io(); a = d; nz16(a); return;
        case 0x1b: io(); s = e ? 0x100 | (a & 0xff) : a; return;
        case 0x3b: io(); a = s; nz16(a); return;
        case 0xeb: io(); io(); a = ((a >> 8) | (a << 8)) & 0xffff; nz8(a & 0xff); return;
        case 0xfb: { io(); const t = fc; fc = e; e = t; if (e) { fm = 1; fx = 1; x &= 0xff; y &= 0xff; s = 0x100 | (s & 0xff); } return; }
        // ---- flags ----
        case 0x18: io(); fc = 0; return;
        case 0x38: io(); fc = 1; return;
        case 0x58: io(); fi = 0; return;
        case 0x78: io(); fi = 1; return;
        case 0xb8: io(); fv = 0; return;
        case 0xd8: io(); fd = 0; return;
        case 0xf8: io(); fd = 1; return;
        case 0xc2: { const v = fetch(); io(); setP(getP() & ~v); return; }
        case 0xe2: { const v = fetch(); io(); setP(getP() | v); return; }
        // ---- misc ----
        case 0xea: io(); return;
        case 0x42: fetch(); return;
        case 0x00: fetch(); interrupt(0xffe6, 0xfffe, true); return;
        case 0x02: fetch(); interrupt(0xffe4, 0xfff4, true); return;
        case 0xcb: io(); io(); io(); waiting = true; return;
        case 0xdb: io(); io(); io(); stopped = true; return;
        case 0x54: case 0x44: { // MVN / MVP
          const dst = fetch(), src = fetch();
          db = dst;
          const v = rd((src << 16) | x);
          wr((dst << 16) | y, v);
          io(); io();
          const step = op === 0x54 ? 1 : -1, im = fx ? 0xff : 0xffff;
          x = (x + step) & im; y = (y + step) & im;
          a = (a - 1) & 0xffff;
          if (a !== 0xffff) pc = (pc - 3) & 0xffff;
          return;
        }
      }
    }

    this.nmiPending = false;
    this.irqLine = false;
    this.step = function () {
      if (stopped) { io(); return; }
      if (self.nmiPending) {
        self.nmiPending = false; waiting = false;
        io(); io();
        interrupt(0xffea, 0xfffa, false);
        return;
      }
      if (self.irqLine && !fi) {
        waiting = false;
        io(); io();
        interrupt(0xffee, 0xfffe, false);
        return;
      }
      if (waiting) { if (self.irqLine) waiting = false; else { io(); return; } }
      exec(fetch());
    };
    this.reset = function () {
      e = 1; fm = 1; fx = 1; fi = 1; fd = 0; db = 0; pb = 0; d = 0; s = 0x1ff; x &= 0xff; y &= 0xff;
      waiting = false; stopped = false;
      pc = rd(0xfffc) | (rd(0xfffd) << 8);
    };
    this.isWaiting = () => waiting;
    this.getState = () => [a, x, y, s, d, db, pb, pc, getP(), e, waiting ? 1 : 0, stopped ? 1 : 0, self.nmiPending ? 1 : 0, self.irqLine ? 1 : 0];
    this.setState = (st) => {
      [a, x, y, s, d, db, pb, pc] = st; e = st[9]; fm = 0; fx = 0;
      const p = st[8]; e = st[9];
      fn = (p >> 7) & 1; fv = (p >> 6) & 1; fm = (p >> 5) & 1; fx = (p >> 4) & 1; fd = (p >> 3) & 1; fi = (p >> 2) & 1; fz = (p >> 1) & 1; fc = p & 1;
      if (e) { s = 0x100 | (s & 0xff); fm = 1; fx = 1; }
      if (fx) { x &= 0xff; y &= 0xff; }
      waiting = !!st[10]; stopped = !!st[11]; self.nmiPending = !!st[12]; self.irqLine = !!st[13];
    };
    this.regs = () => ({ a, x, y, s, d, dbr: db, pbr: pb, pc, p: getP(), e });
    this.getPC = () => (pb << 16) | pc;
  }
  global.RP65816 = RP65816;
})(typeof window !== "undefined" ? window : globalThis);
