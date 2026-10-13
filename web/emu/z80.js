// RetroPixels Z80 core — original implementation written from the documented
// behaviour of the Zilog Z80 (including the undocumented flag bits 3/5, MEMPTR
// and the Q latch used by SCF/CCF). Used as the Genesis sound CPU.
//
//   const z = new RPZ80({read(a), write(a,v), in(port), out(port,v), vector()})
//   z.step() -> T-states used;  z.irq = true/false (level);  z.nmi()
(function (global) {
  'use strict';
  const CF = 1, NF = 2, PF = 4, XF = 8, HF = 16, YF = 32, ZF = 64, SF = 128;
  const SZ = new Uint8Array(256), SZP = new Uint8Array(256);
  for (let i = 0; i < 256; i++) {
    SZ[i] = (i & (SF | YF | XF)) | (i ? 0 : ZF);
    let p = i; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    SZP[i] = SZ[i] | ((p & 1) ? 0 : PF);
  }
  const IM_MODE = [0, 0, 1, 2, 0, 0, 1, 2];
  const sx = (v) => (v << 24) >> 24;

  function RPZ80(bus) {
    const rd = (ad) => bus.read(ad & 0xffff);
    const wr = (ad, v) => bus.write(ad & 0xffff, v & 0xff);
    const pin = (port) => (bus.in ? bus.in(port & 0xffff) : 0xff) & 0xff;
    const pout = (port, v) => { if (bus.out) bus.out(port & 0xffff, v & 0xff); };

    let a, f, b, c, d, e, h, l, ix, iy, sp, pc, iv, rv, wz;
    let af2, bc2, de2, hl2, iff1, iff2, im, halted, eiBlock, q, qn, nmiPending;
    const self = this;

    function setF(v) { f = v; qn = 1; }
    function fetch() { const v = rd(pc); pc = (pc + 1) & 0xffff; return v; }
    function fetch16() { const lo = fetch(); return lo | (fetch() << 8); }
    function m1() { rv = (rv & 0x80) | ((rv + 1) & 0x7f); return fetch(); }
    function push(v) { sp = (sp - 1) & 0xffff; wr(sp, v >> 8); sp = (sp - 1) & 0xffff; wr(sp, v); }
    function pop() { const lo = rd(sp); sp = (sp + 1) & 0xffff; const hi = rd(sp); sp = (sp + 1) & 0xffff; return lo | (hi << 8); }

    function getR(r) {
      switch (r) { case 0: return b; case 1: return c; case 2: return d; case 3: return e;
        case 4: return h; case 5: return l; default: return a; }
    }
    function setR(r, v) {
      switch (r) { case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break;
        case 4: h = v; break; case 5: l = v; break; default: a = v; }
    }
    function getRX(r, x) {
      if (x && (r === 4 || r === 5)) { const v = x === 1 ? ix : iy; return r === 4 ? v >> 8 : v & 0xff; }
      return getR(r);
    }
    function setRX(r, v, x) {
      if (x && (r === 4 || r === 5)) {
        let w = x === 1 ? ix : iy;
        w = r === 4 ? (w & 0xff) | (v << 8) : (w & 0xff00) | v;
        if (x === 1) ix = w; else iy = w;
        return;
      }
      setR(r, v);
    }
    function getHL(x) { return x === 0 ? (h << 8) | l : x === 1 ? ix : iy; }
    function setHL(x, v) { if (x === 0) { h = v >> 8; l = v & 0xff; } else if (x === 1) ix = v; else iy = v; }
    function getRP(p, x) {
      switch (p) { case 0: return (b << 8) | c; case 1: return (d << 8) | e; case 2: return getHL(x); default: return sp; }
    }
    function setRP(p, v, x) {
      switch (p) { case 0: b = v >> 8; c = v & 0xff; break; case 1: d = v >> 8; e = v & 0xff; break;
        case 2: setHL(x, v); break; default: sp = v; }
    }
    function memAddr(x) {
      if (!x) return (h << 8) | l;
      const ad = ((x === 1 ? ix : iy) + sx(fetch())) & 0xffff;
      wz = ad;
      return ad;
    }
    function cond(y) {
      switch (y) { case 0: return !(f & ZF); case 1: return !!(f & ZF); case 2: return !(f & CF); case 3: return !!(f & CF);
        case 4: return !(f & PF); case 5: return !!(f & PF); case 6: return !(f & SF); default: return !!(f & SF); }
    }

    // ---- 8-bit arithmetic ----
    function add8(v, cy) {
      const r = a + v + cy;
      setF(SZ[r & 0xff] | ((r >> 8) & 1) | ((a ^ v ^ r) & HF) | (((a ^ ~v) & (a ^ r) & 0x80) >> 5));
      a = r & 0xff;
    }
    function sub8(v, cy) {
      const r = a - v - cy;
      setF(SZ[r & 0xff] | NF | ((r >> 8) & 1) | ((a ^ v ^ r) & HF) | (((a ^ v) & (a ^ r) & 0x80) >> 5));
      return r & 0xff;
    }
    function alu(op, v) {
      switch (op) {
        case 0: add8(v, 0); break;
        case 1: add8(v, f & CF); break;
        case 2: a = sub8(v, 0); break;
        case 3: a = sub8(v, f & CF); break;
        case 4: a &= v; setF(SZP[a] | HF); break;
        case 5: a ^= v; setF(SZP[a]); break;
        case 6: a |= v; setF(SZP[a]); break;
        default: sub8(v, 0); setF((f & ~(XF | YF)) | (v & (XF | YF)));
      }
    }
    function inc8(v) { const r = (v + 1) & 0xff; setF((f & CF) | SZ[r] | ((r & 0xf) ? 0 : HF) | (v === 0x7f ? PF : 0)); return r; }
    function dec8(v) { const r = (v - 1) & 0xff; setF((f & CF) | NF | SZ[r] | ((v & 0xf) ? 0 : HF) | (v === 0x80 ? PF : 0)); return r; }
    function rot(y, v) {
      let r, cy;
      switch (y) {
        case 0: cy = v >> 7; r = ((v << 1) | cy) & 0xff; break;
        case 1: cy = v & 1; r = (v >> 1) | (cy << 7); break;
        case 2: cy = v >> 7; r = ((v << 1) | (f & CF)) & 0xff; break;
        case 3: cy = v & 1; r = (v >> 1) | ((f & CF) << 7); break;
        case 4: cy = v >> 7; r = (v << 1) & 0xff; break;
        case 5: cy = v & 1; r = (v >> 1) | (v & 0x80); break;
        case 6: cy = v >> 7; r = ((v << 1) | 1) & 0xff; break;
        default: cy = v & 1; r = v >> 1;
      }
      setF(SZP[r] | cy);
      return r;
    }
    function bit(y, v, src35) {
      const r = v & (1 << y);
      setF((f & CF) | HF | (r ? 0 : ZF | PF) | (r & SF) | (src35 & (XF | YF)));
    }
    function addHL(x, v) {
      const hv = getHL(x), r = hv + v;
      wz = (hv + 1) & 0xffff;
      setF((f & (SF | ZF | PF)) | ((r >> 16) & 1) | (((hv ^ v ^ r) >> 8) & HF) | ((r >> 8) & (XF | YF)));
      setHL(x, r & 0xffff);
    }
    function adcHL(v) {
      const hv = (h << 8) | l, r = hv + v + (f & CF);
      wz = (hv + 1) & 0xffff;
      setF(((r >> 8) & (SF | XF | YF)) | ((r & 0xffff) ? 0 : ZF) | (((hv ^ v ^ r) >> 8) & HF) |
        (((~(hv ^ v) & (hv ^ r)) & 0x8000) >> 13) | ((r >> 16) & 1));
      h = (r >> 8) & 0xff; l = r & 0xff;
    }
    function sbcHL(v) {
      const hv = (h << 8) | l, r = hv - v - (f & CF);
      wz = (hv + 1) & 0xffff;
      setF(NF | ((r >> 8) & (SF | XF | YF)) | ((r & 0xffff) ? 0 : ZF) | (((hv ^ v ^ r) >> 8) & HF) |
        ((((hv ^ v) & (hv ^ r)) & 0x8000) >> 13) | ((r >> 16) & 1));
      h = (r >> 8) & 0xff; l = r & 0xff;
    }

    // ---- unprefixed / DD / FD ----
    function exec(op, x) {
      const y = (op >> 3) & 7, z = op & 7, p = y >> 1, odd = y & 1;
      switch (op >> 6) {
        case 0:
          switch (z) {
            case 0:
              switch (y) {
                case 0: return 4;
                case 1: { const t = (a << 8) | f; a = af2 >> 8; f = af2 & 0xff; af2 = t; return 4; }
                case 2: { const dd = fetch(); b = (b - 1) & 0xff; if (b) { pc = (pc + sx(dd)) & 0xffff; wz = pc; return 13; } return 8; }
                case 3: { const dd = fetch(); pc = (pc + sx(dd)) & 0xffff; wz = pc; return 12; }
                default: { const dd = fetch(); if (cond(y - 4)) { pc = (pc + sx(dd)) & 0xffff; wz = pc; return 12; } return 7; }
              }
            case 1:
              if (!odd) { setRP(p, fetch16(), x); return 10; }
              addHL(x, getRP(p, x)); return 11;
            case 2:
              switch (y) {
                case 0: { const ad = (b << 8) | c; wr(ad, a); wz = ((ad + 1) & 0xff) | (a << 8); return 7; }
                case 1: { const ad = (b << 8) | c; a = rd(ad); wz = (ad + 1) & 0xffff; return 7; }
                case 2: { const ad = (d << 8) | e; wr(ad, a); wz = ((ad + 1) & 0xff) | (a << 8); return 7; }
                case 3: { const ad = (d << 8) | e; a = rd(ad); wz = (ad + 1) & 0xffff; return 7; }
                case 4: { const ad = fetch16(), v = getHL(x); wr(ad, v); wr(ad + 1, v >> 8); wz = (ad + 1) & 0xffff; return 16; }
                case 5: { const ad = fetch16(); setHL(x, rd(ad) | (rd(ad + 1) << 8)); wz = (ad + 1) & 0xffff; return 16; }
                case 6: { const ad = fetch16(); wr(ad, a); wz = ((ad + 1) & 0xff) | (a << 8); return 13; }
                default: { const ad = fetch16(); a = rd(ad); wz = (ad + 1) & 0xffff; return 13; }
              }
            case 3: setRP(p, (getRP(p, x) + (odd ? 0xffff : 1)) & 0xffff, x); return 6;
            case 4:
              if (y === 6) { const ad = memAddr(x); wr(ad, inc8(rd(ad))); return x ? 19 : 11; }
              setRX(y, inc8(getRX(y, x)), x); return 4;
            case 5:
              if (y === 6) { const ad = memAddr(x); wr(ad, dec8(rd(ad))); return x ? 19 : 11; }
              setRX(y, dec8(getRX(y, x)), x); return 4;
            case 6:
              if (y === 6) { const ad = memAddr(x); wr(ad, fetch()); return x ? 15 : 10; }
              setRX(y, fetch(), x); return 7;
            default:
              switch (y) {
                case 0: a = ((a << 1) | (a >> 7)) & 0xff; setF((f & (SF | ZF | PF)) | (a & (XF | YF | CF))); break;
                case 1: { const cy = a & 1; a = (a >> 1) | (cy << 7); setF((f & (SF | ZF | PF)) | (a & (XF | YF)) | cy); break; }
                case 2: { const cy = a >> 7; a = ((a << 1) | (f & CF)) & 0xff; setF((f & (SF | ZF | PF)) | (a & (XF | YF)) | cy); break; }
                case 3: { const cy = a & 1; a = (a >> 1) | ((f & CF) << 7); setF((f & (SF | ZF | PF)) | (a & (XF | YF)) | cy); break; }
                case 4: {
                  let t = 0, cy = f & CF;
                  if ((f & HF) || (a & 0xf) > 9) t = 6;
                  if (cy || a > 0x99) { t |= 0x60; cy = 1; }
                  const r = ((f & NF) ? a - t : a + t) & 0xff;
                  setF(SZP[r] | cy | (f & NF) | ((a ^ r) & HF));
                  a = r; break;
                }
                case 5: a ^= 0xff; setF((f & (SF | ZF | PF | CF)) | HF | NF | (a & (XF | YF))); break;
                case 6: setF((f & (SF | ZF | PF)) | CF | (((q ^ f) | a) & (XF | YF))); break;
                default: setF(((f & (SF | ZF | PF | CF)) | ((f & CF) << 4) | (((q ^ f) | a) & (XF | YF))) ^ CF);
              }
              return 4;
          }
        case 1: {
          if (op === 0x76) { halted = true; return 4; }
          if (z === 6) { setR(y, rd(memAddr(x))); return x ? 15 : 7; }
          if (y === 6) { const ad = memAddr(x); wr(ad, getR(z)); return x ? 15 : 7; }
          setRX(y, getRX(z, x), x); return 4;
        }
        case 2:
          if (z === 6) { alu(y, rd(memAddr(x))); return x ? 15 : 7; }
          alu(y, getRX(z, x)); return 4;
        default:
          switch (z) {
            case 0: if (cond(y)) { pc = pop(); wz = pc; return 11; } return 5;
            case 1:
              if (!odd) {
                const v = pop();
                if (p === 3) { a = v >> 8; f = v & 0xff; } else setRP(p, v, x);
                return 10;
              }
              switch (p) {
                case 0: pc = pop(); wz = pc; return 10;
                case 1: {
                  let t = (b << 8) | c; b = bc2 >> 8; c = bc2 & 0xff; bc2 = t;
                  t = (d << 8) | e; d = de2 >> 8; e = de2 & 0xff; de2 = t;
                  t = (h << 8) | l; h = hl2 >> 8; l = hl2 & 0xff; hl2 = t;
                  return 4;
                }
                case 2: pc = getHL(x); return 4;
                default: sp = getHL(x); return 6;
              }
            case 2: { const ad = fetch16(); wz = ad; if (cond(y)) pc = ad; return 10; }
            case 3:
              switch (y) {
                case 0: pc = fetch16(); wz = pc; return 10;
                case 1: return x ? execDDCB(x) : execCB();
                case 2: { const n = fetch(); pout((a << 8) | n, a); wz = ((n + 1) & 0xff) | (a << 8); return 11; }
                case 3: { const port = (a << 8) | fetch(); a = pin(port); wz = (port + 1) & 0xffff; return 11; }
                case 4: {
                  const v = rd(sp) | (rd(sp + 1) << 8), hv = getHL(x);
                  wr(sp + 1, hv >> 8); wr(sp, hv); setHL(x, v); wz = v; return 19;
                }
                case 5: { let t = d; d = h; h = t; t = e; e = l; l = t; return 4; }
                case 6: iff1 = iff2 = 0; return 4;
                default: iff1 = iff2 = 1; eiBlock = true; return 4;
              }
            case 4: { const ad = fetch16(); wz = ad; if (cond(y)) { push(pc); pc = ad; return 17; } return 10; }
            case 5:
              if (!odd) { push(p === 3 ? (a << 8) | f : getRP(p, x)); return 11; }
              { const ad = fetch16(); wz = ad; push(pc); pc = ad; return 17; } // p===0 (prefixes handled in step)
            case 6: alu(y, fetch()); return 7;
            default: push(pc); pc = y << 3; wz = pc; return 11;
          }
      }
    }

    function execCB() {
      const op = m1(), y = (op >> 3) & 7, z = op & 7;
      const mem = z === 6, ad = mem ? (h << 8) | l : 0;
      const v = mem ? rd(ad) : getR(z);
      let r;
      switch (op >> 6) {
        case 0: r = rot(y, v); break;
        case 1: bit(y, v, mem ? wz >> 8 : v); return mem ? 12 : 8;
        case 2: r = v & ~(1 << y); break;
        default: r = v | (1 << y);
      }
      if (mem) { wr(ad, r); return 15; }
      setR(z, r); return 8;
    }
    function execDDCB(x) {
      const ad = ((x === 1 ? ix : iy) + sx(fetch())) & 0xffff;
      wz = ad;
      const op = fetch(), y = (op >> 3) & 7, z = op & 7, v = rd(ad);
      let r;
      switch (op >> 6) {
        case 0: r = rot(y, v); break;
        case 1: bit(y, v, ad >> 8); return 16;
        case 2: r = v & ~(1 << y); break;
        default: r = v | (1 << y);
      }
      wr(ad, r);
      if (z !== 6) setR(z, r);
      return 19;
    }

    function blockIOFlags(v, k) {
      setF(SZ[b] | ((v & 0x80) ? NF : 0) | (k > 0xff ? HF | CF : 0) | (SZP[(k & 7) ^ b] & PF));
    }
    function repeatFlags() { f = (f & ~(XF | YF)) | ((pc >> 8) & (XF | YF)); }
    function repeatIOFlags(v) {
      repeatFlags();
      if (f & CF) {
        f &= ~HF;
        if (v & 0x80) { f ^= (SZP[(b - 1) & 7] ^ PF) & PF; if ((b & 0xf) === 0) f |= HF; }
        else { f ^= (SZP[(b + 1) & 7] ^ PF) & PF; if ((b & 0xf) === 0xf) f |= HF; }
      } else f ^= (SZP[b & 7] ^ PF) & PF;
    }

    function execED() {
      const op = m1(), y = (op >> 3) & 7, z = op & 7, p = y >> 1, odd = y & 1;
      if ((op >> 6) === 1) {
        switch (z) {
          case 0: { const port = (b << 8) | c, v = pin(port); wz = (port + 1) & 0xffff; setF((f & CF) | SZP[v]); if (y !== 6) setR(y, v); return 12; }
          case 1: { const port = (b << 8) | c; pout(port, y === 6 ? 0 : getR(y)); wz = (port + 1) & 0xffff; return 12; }
          case 2: if (odd) adcHL(getRP(p, 0)); else sbcHL(getRP(p, 0)); return 15;
          case 3: {
            const ad = fetch16();
            if (odd) setRP(p, rd(ad) | (rd(ad + 1) << 8), 0);
            else { const v = getRP(p, 0); wr(ad, v); wr(ad + 1, v >> 8); }
            wz = (ad + 1) & 0xffff; return 20;
          }
          case 4: { const v = a; a = 0; a = sub8(v, 0); return 8; }
          case 5: iff1 = iff2; pc = pop(); wz = pc; return 14;
          case 6: im = IM_MODE[y]; return 8;
          default:
            switch (y) {
              case 0: iv = a; return 9;
              case 1: rv = a; return 9;
              case 2: a = iv; setF((f & CF) | SZ[a] | (iff2 ? PF : 0)); return 9;
              case 3: a = rv; setF((f & CF) | SZ[a] | (iff2 ? PF : 0)); return 9;
              case 4: { const ad = (h << 8) | l, v = rd(ad); wr(ad, (a << 4) | (v >> 4)); a = (a & 0xf0) | (v & 0xf); setF((f & CF) | SZP[a]); wz = (ad + 1) & 0xffff; return 18; }
              case 5: { const ad = (h << 8) | l, v = rd(ad); wr(ad, (v << 4) | (a & 0xf)); a = (a & 0xf0) | (v >> 4); setF((f & CF) | SZP[a]); wz = (ad + 1) & 0xffff; return 18; }
              default: return 8;
            }
        }
      }
      if ((op >> 6) === 2 && y >= 4 && z <= 3) {
        const dec = odd, rep = y >= 6, step = dec ? 0xffff : 1;
        let hl = (h << 8) | l;
        switch (z) {
          case 0: { // LDI/LDD/LDIR/LDDR
            const v = rd(hl), de = (d << 8) | e;
            wr(de, v);
            hl = (hl + step) & 0xffff; h = hl >> 8; l = hl & 0xff;
            const de2_ = (de + step) & 0xffff; d = de2_ >> 8; e = de2_ & 0xff;
            const bc = (((b << 8) | c) - 1) & 0xffff; b = bc >> 8; c = bc & 0xff;
            const n = (v + a) & 0xff;
            setF((f & (SF | ZF | CF)) | (bc ? PF : 0) | (n & XF) | ((n & 2) << 4));
            if (rep && bc) { pc = (pc - 2) & 0xffff; wz = (pc + 1) & 0xffff; repeatFlags(); return 21; }
            return 16;
          }
          case 1: { // CPI/CPD/CPIR/CPDR
            const v = rd(hl), r = (a - v) & 0xff;
            hl = (hl + step) & 0xffff; h = hl >> 8; l = hl & 0xff;
            const bc = (((b << 8) | c) - 1) & 0xffff; b = bc >> 8; c = bc & 0xff;
            const hf = (a ^ v ^ r) & HF, n = (r - (hf ? 1 : 0)) & 0xff;
            setF((f & CF) | NF | (SZ[r] & (SF | ZF)) | hf | (bc ? PF : 0) | (n & XF) | ((n & 2) << 4));
            wz = (wz + step) & 0xffff;
            if (rep && bc && r) { pc = (pc - 2) & 0xffff; wz = (pc + 1) & 0xffff; repeatFlags(); return 21; }
            return 16;
          }
          case 2: { // INI/IND/INIR/INDR
            const port = (b << 8) | c, v = pin(port);
            wz = (port + step) & 0xffff;
            wr(hl, v);
            b = (b - 1) & 0xff;
            hl = (hl + step) & 0xffff; h = hl >> 8; l = hl & 0xff;
            blockIOFlags(v, v + ((c + (dec ? -1 : 1)) & 0xff));
            if (rep && b) { pc = (pc - 2) & 0xffff; wz = (pc + 1) & 0xffff; repeatIOFlags(v); return 21; }
            return 16;
          }
          default: { // OUTI/OUTD/OTIR/OTDR
            const v = rd(hl);
            b = (b - 1) & 0xff;
            const port = (b << 8) | c;
            wz = (port + step) & 0xffff;
            pout(port, v);
            hl = (hl + step) & 0xffff; h = hl >> 8; l = hl & 0xff;
            blockIOFlags(v, v + l);
            if (rep && b) { pc = (pc - 2) & 0xffff; wz = (pc + 1) & 0xffff; repeatIOFlags(v); return 21; }
            return 16;
          }
        }
      }
      return 8;
    }

    function step() {
      if (nmiPending) {
        nmiPending = false;
        halted = false;
        rv = (rv & 0x80) | ((rv + 1) & 0x7f);
        iff1 = 0; push(pc); pc = 0x66; wz = pc; q = 0;
        return 11;
      }
      if (self.irq && iff1 && !eiBlock) {
        halted = false;
        iff1 = iff2 = 0;
        rv = (rv & 0x80) | ((rv + 1) & 0x7f);
        push(pc); q = 0;
        if (im === 2) {
          const vec = (iv << 8) | (bus.vector ? bus.vector() & 0xff : 0xff);
          pc = rd(vec) | (rd(vec + 1) << 8); wz = pc;
          return 19;
        }
        pc = 0x38; wz = pc;
        return 13;
      }
      eiBlock = false;
      if (halted) { rv = (rv & 0x80) | ((rv + 1) & 0x7f); q = 0; return 4; }
      qn = 0;
      let op = m1(), t = 0, x = 0;
      while (op === 0xdd || op === 0xfd) { x = op === 0xdd ? 1 : 2; t += 4; q = 0; op = m1(); }
      if (op === 0xed) t += execED();
      else t += exec(op, x);
      q = qn ? f : 0;
      return t;
    }

    this.irq = false;
    this.step = step;
    this.nmi = () => { nmiPending = true; };
    this.reset = function () {
      a = f = 0xff; b = c = d = e = h = l = 0xff;
      ix = iy = sp = 0xffff; pc = 0; iv = rv = 0; wz = 0;
      af2 = bc2 = de2 = hl2 = 0xffff;
      iff1 = iff2 = 0; im = 0; halted = false; eiBlock = false; q = qn = 0; nmiPending = false;
    };
    this.isHalted = () => halted;
    this.getPC = () => pc;
    const KEYS = ['a', 'f', 'b', 'c', 'd', 'e', 'h', 'l', 'ix', 'iy', 'sp', 'pc', 'i', 'r', 'wz',
      'af_', 'bc_', 'de_', 'hl_', 'iff1', 'iff2', 'im', 'halted', 'ei', 'q', 'nmi'];
    this.getState = function () {
      return [a, f, b, c, d, e, h, l, ix, iy, sp, pc, iv, rv, wz, af2, bc2, de2, hl2,
        iff1, iff2, im, halted ? 1 : 0, eiBlock ? 1 : 0, q, nmiPending ? 1 : 0, self.irq ? 1 : 0];
    };
    this.setState = function (s) {
      [a, f, b, c, d, e, h, l, ix, iy, sp, pc, iv, rv, wz, af2, bc2, de2, hl2, iff1, iff2, im] = s;
      halted = !!s[22]; eiBlock = !!s[23]; q = s[24]; nmiPending = !!s[25]; self.irq = !!s[26];
    };
    this.regs = function () {
      const s = this.getState(), o = {};
      KEYS.forEach((k, i) => { o[k] = s[i]; });
      return o;
    };
    this.reset();
  }
  global.RPZ80 = RPZ80;
})(typeof window !== 'undefined' ? window : globalThis);
