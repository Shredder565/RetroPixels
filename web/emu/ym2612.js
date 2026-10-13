// RetroPixels — Yamaha YM2612 (OPN2) FM sound chip, used by the Genesis emulator.
//
// Original code written from the chip's documented behaviour: 6 channels of
// 4 operators, 8 algorithms, envelope generator with key scaling, detune,
// LFO (AM / PM), channel 3 special mode, timers A / B, SSG-EG and the DAC.
// One call to tick() produces one native sample (clock / 144).
"use strict";

(function (global) {
  // Log-sine and exponent tables, generated from their defining formulas.
  const LOGSIN = new Uint16Array(256), EXP = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    LOGSIN[i] = Math.round(-Math.log2(Math.sin((i + 0.5) * Math.PI / 512)) * 256);
    EXP[i] = Math.round(2048 * Math.pow(2, -i / 256));
  }
  // Detune amounts per key code (from the chip's data sheet).
  const DT = [
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8],
    [1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16],
    [2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22],
  ];
  // Envelope increments: 64 rates x 8 steps.
  const EG_INC = new Uint8Array(64 * 8);
  {
    const P = [[0, 1, 0, 1, 0, 1, 0, 1], [0, 1, 0, 1, 1, 1, 0, 1], [0, 1, 1, 1, 0, 1, 1, 1], [0, 1, 1, 1, 1, 1, 1, 1]];
    const Q = [[1, 1, 1, 1, 1, 1, 1, 1], [1, 1, 1, 2, 1, 1, 1, 2], [1, 2, 1, 2, 1, 2, 1, 2], [1, 2, 2, 2, 1, 2, 2, 2]];
    for (let r = 0; r < 64; r++) for (let s = 0; s < 8; s++) {
      let v;
      if (r < 2) v = 0;
      else if (r < 48) v = P[r & 3][s];
      else if (r < 60) { const b = 1 << ((r - 48) >> 2); v = Q[r & 3][s] * b; }
      else v = 8;
      EG_INC[r * 8 + s] = v;
    }
  }
  const LFO_PERIOD = [108, 77, 71, 67, 62, 44, 8, 5];
  const AM_SHIFT = [8, 3, 1, 0];
  const PM_CENTS = [0, 3.4, 6.7, 10, 14, 20, 40, 80];
  // PM multiplier table: [fms][step 0..31]
  const PM_MUL = [];
  for (let f = 0; f < 8; f++) {
    const row = new Float64Array(32);
    for (let s = 0; s < 32; s++) {
      const q = s & 7, tri = (s & 8) ? 7 - q : q, val = (s & 16) ? -tri : tri;
      row[s] = Math.pow(2, PM_CENTS[f] * val / 7 / 1200);
    }
    PM_MUL.push(row);
  }
  const KEY_SLOT = [0, 2, 1, 3]; // key-on bit (S1,S2,S3,S4) -> register-order slot
  const ATTACK = 0, DECAY = 1, SUSTAIN = 2, RELEASE = 3;

  class YM2612 {
    constructor() {
      // per operator (24 = 6 channels x 4 slots in register order: S1,S3,S2,S4)
      this.dt = new Uint8Array(24); this.mul = new Uint8Array(24); this.tl = new Uint8Array(24);
      this.ks = new Uint8Array(24); this.ar = new Uint8Array(24); this.amOn = new Uint8Array(24);
      this.d1r = new Uint8Array(24); this.d2r = new Uint8Array(24); this.sl = new Uint8Array(24);
      this.rr = new Uint8Array(24); this.ssg = new Uint8Array(24);
      this.phase = new Uint32Array(24); this.inc = new Uint32Array(24); this.kc = new Uint8Array(24);
      this.att = new Uint16Array(24); this.egState = new Uint8Array(24); this.key = new Uint8Array(24);
      this.ssgInv = new Uint8Array(24);
      // per channel
      this.fnum = new Uint16Array(6); this.block = new Uint8Array(6); this.fb = new Uint8Array(6);
      this.alg = new Uint8Array(6); this.panL = new Uint8Array(6); this.panR = new Uint8Array(6);
      this.ams = new Uint8Array(6); this.fms = new Uint8Array(6);
      this.fbOut = new Int32Array(12); // two previous S1 outputs per channel
      this.c3fnum = new Uint16Array(3); this.c3block = new Uint8Array(3);
      this.latch = new Uint8Array(2); this.c3latch = new Uint8Array(1);
      this.regs = new Uint8Array(512);
      this.out = new Int32Array(2); // last sample, L / R
      this.chOut = new Int32Array(6);
      this.reset();
    }
    reset() {
      for (const k of ["dt", "mul", "tl", "ks", "ar", "amOn", "d1r", "d2r", "sl", "rr", "ssg", "phase", "inc", "kc", "egState", "key", "ssgInv",
        "fnum", "block", "fb", "alg", "ams", "fms", "fbOut", "c3fnum", "c3block", "latch", "c3latch", "regs"]) this[k].fill(0);
      this.att.fill(0x3ff); this.egState.fill(RELEASE); this.panL.fill(1); this.panR.fill(1);
      this.addr = [0, 0]; this.status = 0;
      this.timerA = 0; this.timerB = 0; this.cntA = 0; this.cntB = 0; this.cntBdiv = 0; this.mode27 = 0;
      this.dacOn = 0; this.dac = 0x80;
      this.lfoOn = 0; this.lfoFreq = 0; this.lfoCnt = 0; this.lfoStep = 0;
      this.egDiv = 0; this.egCnt = 0;
      this.dirty = 0x3f;
    }

    readStatus() { return this.status; }
    write(port, v) {
      v &= 0xff;
      if (!(port & 1)) { this.addr[port >> 1] = v; return; }
      const part = port >> 1, r = this.addr[part];
      if (part === 0 && r < 0x30) { this.writeGlobal(r, v); return; }
      if (r < 0x30) return;
      this.regs[part * 256 + r] = v;
      const chi = r & 3;
      if (chi === 3) return;
      const ch = part * 3 + chi;
      if (r < 0xa0) {
        const slot = ch * 4 + ((r >> 2) & 3);
        switch (r & 0xf0) {
          case 0x30: this.dt[slot] = (v >> 4) & 7; this.mul[slot] = v & 15; break;
          case 0x40: this.tl[slot] = v & 0x7f; break;
          case 0x50: this.ks[slot] = v >> 6; this.ar[slot] = v & 31; break;
          case 0x60: this.amOn[slot] = v >> 7; this.d1r[slot] = v & 31; break;
          case 0x70: this.d2r[slot] = v & 31; break;
          case 0x80: this.sl[slot] = v >> 4; this.rr[slot] = v & 15; break;
          case 0x90: this.ssg[slot] = v & 15; break;
        }
        this.dirty |= 1 << ch;
        return;
      }
      switch (r & 0xfc) {
        case 0xa0: this.fnum[ch] = ((this.latch[part] & 7) << 8) | v; this.block[ch] = (this.latch[part] >> 3) & 7; this.dirty |= 1 << ch; break;
        case 0xa4: this.latch[part] = v & 0x3f; break;
        case 0xa8: if (part === 0) { this.c3fnum[chi] = ((this.c3latch[0] & 7) << 8) | v; this.c3block[chi] = (this.c3latch[0] >> 3) & 7; this.dirty |= 4; } break;
        case 0xac: if (part === 0) this.c3latch[0] = v & 0x3f; break;
        case 0xb0: this.fb[ch] = (v >> 3) & 7; this.alg[ch] = v & 7; break;
        case 0xb4: this.panL[ch] = v >> 7; this.panR[ch] = (v >> 6) & 1; this.ams[ch] = (v >> 4) & 3; this.fms[ch] = v & 7; break;
      }
    }
    writeGlobal(r, v) {
      this.regs[r] = v;
      switch (r) {
        case 0x22: this.lfoOn = (v >> 3) & 1; this.lfoFreq = v & 7; if (!this.lfoOn) { this.lfoCnt = 0; this.lfoStep = 0; } this.dirty = 0x3f; break;
        case 0x24: this.timerA = (this.timerA & 3) | (v << 2); break;
        case 0x25: this.timerA = (this.timerA & 0x3fc) | (v & 3); break;
        case 0x26: this.timerB = v; break;
        case 0x27: {
          if ((v ^ this.mode27) & 0xc0) this.dirty |= 4;
          if ((v & 1) && !(this.mode27 & 1)) this.cntA = this.timerA;
          if ((v & 2) && !(this.mode27 & 2)) { this.cntB = this.timerB; this.cntBdiv = 0; }
          if (v & 0x10) this.status &= ~1;
          if (v & 0x20) this.status &= ~2;
          this.mode27 = v;
          break;
        }
        case 0x28: {
          let ch = v & 3; if (ch === 3) return;
          if (v & 4) ch += 3;
          for (let k = 0; k < 4; k++) this.keyOnOff(ch * 4 + KEY_SLOT[k], (v >> (4 + k)) & 1);
          break;
        }
        case 0x2a: this.dac = v; break;
        case 0x2b: this.dacOn = v >> 7; break;
      }
    }
    keyOnOff(s, on) {
      if (on && !this.key[s]) {
        this.phase[s] = 0; this.ssgInv[s] = 0;
        this.egState[s] = ATTACK;
        this.updateInc((s >> 2));
        if (this.rateFor(s, this.ar[s] * 2) >= 62) { this.att[s] = 0; this.egState[s] = DECAY; }
      } else if (!on && this.key[s]) {
        if ((this.ssg[s] & 8) && (this.ssgInv[s] ^ ((this.ssg[s] >> 2) & 1))) this.att[s] = (0x200 - this.att[s]) & 0x3ff;
        this.egState[s] = RELEASE;
      }
      this.key[s] = on;
    }
    rateFor(s, r) { if (!r) return 0; const v = r + (this.kc[s] >> (3 - this.ks[s])); return v > 63 ? 63 : v; }

    // Recompute phase increments of one channel.
    updateInc(ch) {
      const special = ch === 2 && (this.mode27 & 0xc0);
      const pm = this.lfoOn && this.fms[ch] ? PM_MUL[this.fms[ch]][this.lfoStep >> 2] : 1;
      for (let k = 0; k < 4; k++) {
        const s = ch * 4 + k;
        let fn = this.fnum[ch], bl = this.block[ch];
        if (special && k !== 3) { const i = k === 0 ? 1 : k === 1 ? 0 : 2; fn = this.c3fnum[i]; bl = this.c3block[i]; }
        const f11 = (fn >> 10) & 1, n3 = (f11 && (fn & 0x380)) || (!f11 && (fn & 0x380) === 0x380) ? 1 : 0;
        this.kc[s] = (bl << 2) | (f11 << 1) | n3;
        let base = ((pm === 1 ? fn : Math.floor(fn * pm) & 0xfff) << bl) >> 1;
        const d = this.dt[s];
        if (d & 3) { const amt = DT[d & 3][this.kc[s]]; base += (d & 4) ? -amt : amt; }
        base &= 0x1ffff;
        this.inc[s] = (this.mul[s] ? base * this.mul[s] : base >> 1) & 0xfffff;
      }
    }

    egStep(s) {
      let st = this.egState[s], att = this.att[s];
      const ssgOn = this.ssg[s] & 8;
      if (ssgOn && att >= 0x200 && st !== ATTACK && st !== RELEASE && this.key[s]) {
        const g = this.ssg[s];
        if (g & 1) { if (g & 2) this.ssgInv[s] = 1; }
        else if (g & 2) this.ssgInv[s] ^= 1;
        else this.phase[s] = 0;
        if (!(g & 1)) { st = ATTACK; if (this.rateFor(s, this.ar[s] * 2) >= 62) { att = 0; st = DECAY; } }
      }
      let r;
      switch (st) {
        case ATTACK: r = this.rateFor(s, this.ar[s] * 2); break;
        case DECAY: r = this.rateFor(s, this.d1r[s] * 2); break;
        case SUSTAIN: r = this.rateFor(s, this.d2r[s] * 2); break;
        default: r = this.rateFor(s, this.rr[s] * 4 + 2);
      }
      const shift = r < 48 ? 11 - (r >> 2) : 0;
      if (shift > 0 && (this.egCnt & ((1 << shift) - 1))) { this.egState[s] = st; this.att[s] = att; return; }
      const inc = EG_INC[r * 8 + ((this.egCnt >> shift) & 7)];
      if (st === ATTACK) {
        if (r >= 62) att = 0;
        else if (att && inc) att += ((~att) * inc) >> 4;
        if (att <= 0) { att = 0; st = DECAY; }
      } else if (ssgOn) {
        if (att < 0x200) att += 4 * inc;
        if (st === RELEASE && att >= 0x200) att = 0x3ff;
      } else {
        att += inc;
        if (att > 0x3ff) att = 0x3ff;
      }
      if (st === DECAY) { const slv = this.sl[s] === 15 ? 0x3e0 : this.sl[s] << 5; if (att >= slv) st = SUSTAIN; }
      this.egState[s] = st; this.att[s] = att;
    }

    opOut(s, mod, am) {
      let a = this.att[s];
      if ((this.ssg[s] & 8) && this.egState[s] !== RELEASE && (this.ssgInv[s] ^ ((this.ssg[s] >> 2) & 1))) a = (0x200 - a) & 0x3ff;
      a += this.tl[s] << 3;
      if (this.amOn[s]) a += am;
      if (a >= 0x3ff) return 0;
      const p = ((this.phase[s] >> 10) + mod) & 0x3ff;
      let q = p & 0xff; if (p & 0x100) q ^= 0xff;
      const level = LOGSIN[q] + (a << 2);
      const v = level >= 3328 ? 0 : (EXP[level & 0xff] << 2) >> (level >> 8);
      return (p & 0x200) ? -v : v;
    }

    // Produce one sample into this.out (each side about ±1536).
    tick() {
      // LFO
      if (this.lfoOn) {
        if (++this.lfoCnt >= LFO_PERIOD[this.lfoFreq]) {
          this.lfoCnt = 0;
          const old = this.lfoStep >> 2;
          this.lfoStep = (this.lfoStep + 1) & 127;
          if ((this.lfoStep >> 2) !== old) for (let c = 0; c < 6; c++) if (this.fms[c]) this.dirty |= 1 << c;
        }
      }
      const amBase = this.lfoOn ? (this.lfoStep < 64 ? (this.lfoStep ^ 63) * 2 : (this.lfoStep & 63) * 2) : 0;
      if (this.dirty) { for (let c = 0; c < 6; c++) if (this.dirty & (1 << c)) this.updateInc(c); this.dirty = 0; }
      // envelope (every 3 samples)
      if (++this.egDiv === 3) {
        this.egDiv = 0;
        this.egCnt = (this.egCnt + 1) & 0xfff;
        for (let s = 0; s < 24; s++) this.egStep(s);
      }
      let L = 0, R = 0;
      for (let ch = 0; ch < 6; ch++) {
        const b = ch * 4;
        let outv;
        if (ch === 5 && this.dacOn) outv = (this.dac - 128) << 6;
        else {
          const am = this.ams[ch] ? amBase >> AM_SHIFT[this.ams[ch]] : 0;
          const fbn = this.fb[ch];
          const f0 = this.fbOut[ch * 2], f1 = this.fbOut[ch * 2 + 1];
          const s1 = this.opOut(b, fbn ? (f0 + f1) >> (10 - fbn) : 0, am);
          this.fbOut[ch * 2] = f1; this.fbOut[ch * 2 + 1] = s1;
          const S2 = b + 2, S3 = b + 1, S4 = b + 3;
          let s2, s3, s4;
          switch (this.alg[ch]) {
            case 0: s2 = this.opOut(S2, s1 >> 1, am); s3 = this.opOut(S3, s2 >> 1, am); outv = this.opOut(S4, s3 >> 1, am); break;
            case 1: s2 = this.opOut(S2, 0, am); s3 = this.opOut(S3, (s1 + s2) >> 1, am); outv = this.opOut(S4, s3 >> 1, am); break;
            case 2: s2 = this.opOut(S2, 0, am); s3 = this.opOut(S3, s2 >> 1, am); outv = this.opOut(S4, (s1 + s3) >> 1, am); break;
            case 3: s2 = this.opOut(S2, s1 >> 1, am); s3 = this.opOut(S3, 0, am); outv = this.opOut(S4, (s2 + s3) >> 1, am); break;
            case 4: s2 = this.opOut(S2, s1 >> 1, am); s3 = this.opOut(S3, 0, am); outv = s2 + this.opOut(S4, s3 >> 1, am); break;
            case 5: outv = this.opOut(S2, s1 >> 1, am) + this.opOut(S3, s1 >> 1, am) + this.opOut(S4, s1 >> 1, am); break;
            case 6: outv = this.opOut(S2, s1 >> 1, am) + this.opOut(S3, 0, am) + this.opOut(S4, 0, am); break;
            default: outv = s1 + this.opOut(S2, 0, am) + this.opOut(S3, 0, am) + this.opOut(S4, 0, am);
          }
          if (outv > 8191) outv = 8191; else if (outv < -8192) outv = -8192;
        }
        outv >>= 5;
        this.chOut[ch] = outv;
        if (this.panL[ch]) L += outv;
        if (this.panR[ch]) R += outv;
      }
      this.out[0] = L; this.out[1] = R;
      // phase
      for (let s = 0; s < 24; s++) this.phase[s] = (this.phase[s] + this.inc[s]) & 0xfffff;
      // timers
      if (this.mode27 & 1) {
        if (++this.cntA >= 1024) {
          this.cntA = this.timerA;
          if (this.mode27 & 4) this.status |= 1;
          if ((this.mode27 & 0xc0) === 0x80) for (let k = 0; k < 4; k++) { const s = 8 + k; if (!this.key[s]) { this.keyOnOff(s, 1); this.keyOnOff(s, 0); this.egState[s] = ATTACK; } }
        }
      }
      if (this.mode27 & 2) {
        if (++this.cntBdiv >= 16) {
          this.cntBdiv = 0;
          if (++this.cntB >= 256) { this.cntB = this.timerB; if (this.mode27 & 8) this.status |= 2; }
        }
      }
    }

    saveState() {
      const o = {};
      for (const k of ["dt", "mul", "tl", "ks", "ar", "amOn", "d1r", "d2r", "sl", "rr", "ssg", "phase", "inc", "kc", "att", "egState", "key", "ssgInv",
        "fnum", "block", "fb", "alg", "panL", "panR", "ams", "fms", "fbOut", "c3fnum", "c3block", "latch", "c3latch", "regs"]) o[k] = this[k].slice();
      for (const k of ["status", "timerA", "timerB", "cntA", "cntB", "cntBdiv", "mode27", "dacOn", "dac", "lfoOn", "lfoFreq", "lfoCnt", "lfoStep", "egDiv", "egCnt"]) o[k] = this[k];
      o.addr = this.addr.slice();
      return o;
    }
    loadState(o) {
      for (const k in o) {
        if (ArrayBuffer.isView(this[k])) this[k].set(o[k]);
        else if (k === "addr") this.addr = o.addr.slice();
        else this[k] = o[k];
      }
      this.dirty = 0x3f;
    }
  }

  global.RPYM2612 = YM2612;
})(typeof window !== "undefined" ? window : globalThis);
