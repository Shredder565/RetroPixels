// RetroPixels — Yamaha YM2151 (OPM) FM sound chip, used by arcade boards. Original code,
// written from the chip's documented behaviour (the YM2151 data sheet's register
// list): 8 channels of 4 operators, 8 algorithms, operator-1 feedback, detune
// (DT1 fine, DT2 coarse), envelope generator with key scaling, LFO with four
// wave shapes driving vibrato (PM) and tremolo (AM), the noise generator on
// channel 8's last operator, timers A and B with their interrupt, and the two
// output pins CT1 / CT2 that boards use as a little port.
//
// ===========================================================================
// INTERFACE
// ===========================================================================
//   const ym = new RPYM2151({ clock: 3579545, onIRQ(on), onPort(ct) })
//       clock    the chip's input clock in Hz (default 3,579,545)
//       onIRQ    optional: called with true / false when the IRQ pin changes
//       onPort   optional: called with (CT2 << 1 | CT1) when register 0x1B's top bits change
//   ym.rate                       native sample rate = clock / 64
//   ym.write(port, v)             port 0 = register address, port 1 = data
//   ym.read()                     status: bit 0 timer A flag, bit 1 timer B flag (never busy)
//   ym.tick()                     make one native sample: ym.out[0] / ym.out[1] = left / right
//                                 (integers, about ±8192 per loud channel); timers count here
//   ym.clocks(n)                  helper: run n input clocks (ticks every 64), mixing into
//                                 ym.acc[0..1] / ym.accN so a board can average them itself
//   ym.irq                        current state of the IRQ pin
//   ym.reset(), ym.saveState(), ym.loadState(o)
// Board wiring note: a board that only needs timers and sound calls tick() at clock/64.
"use strict";

(function (global) {
  // Log-sine and exponent tables, computed from their defining formulas (as the chip's ROMs hold).
  const LOGSIN = new Uint16Array(256), EXP = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    LOGSIN[i] = Math.round(-Math.log2(Math.sin((i + 0.5) * Math.PI / 512)) * 256);
    EXP[i] = Math.round(2048 * Math.pow(2, -i / 256));
  }
  // DT1: small detune per 5-bit key code, in phase-increment steps (data sheet amounts).
  const DT1 = [
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8],
    [1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16],
    [2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22],
  ];
  // DT2: coarse detune in cents (data sheet: x1, x1.41, x1.57, x1.73)
  const DT2_CENTS = [0, 600, 781, 950];
  // PMS: vibrato depth in cents at full PMD
  const PMS_CENTS = [0, 5, 10, 20, 50, 100, 400, 700];
  // KC note code -> semitone above C# (codes 3, 7, 11, 15 repeat the note below)
  const NOTE = [0, 1, 2, 2, 3, 4, 5, 5, 6, 7, 8, 8, 9, 10, 11, 11];

  // Envelope increments: 64 rates x 8 steps (same envelope generator family as the OPN chips).
  const EG_INC = new Uint8Array(64 * 8);
  {
    const P = [[0, 1, 0, 1, 0, 1, 0, 1], [0, 1, 0, 1, 1, 1, 0, 1], [0, 1, 1, 1, 0, 1, 1, 1], [0, 1, 1, 1, 1, 1, 1, 1]];
    const Q = [[1, 1, 1, 1, 1, 1, 1, 1], [1, 1, 1, 2, 1, 1, 1, 2], [1, 2, 1, 2, 1, 2, 1, 2], [1, 2, 2, 2, 1, 2, 2, 2]];
    for (let r = 0; r < 64; r++) for (let s = 0; s < 8; s++) {
      let v;
      if (r < 2) v = 0;
      else if (r < 48) v = P[r & 3][s];
      else if (r < 60) v = Q[r & 3][s] * (1 << ((r - 48) >> 2));
      else v = 8;
      EG_INC[r * 8 + s] = v;
    }
  }
  const ATTACK = 0, DECAY = 1, SUSTAIN = 2, RELEASE = 3;
  // Operators are kept in register order: slot = group * 8 + channel, group 0 M1, 1 M2, 2 C1, 3 C2.
  const M1 = 0, M2 = 8, C1 = 16, C2 = 24;
  // key-on register bits 3..6 = M1, C1, M2, C2
  const KEY_GROUP = [0, 2, 1, 3];

  class YM2151 {
    constructor(opts) {
      opts = opts || {};
      this.clock = opts.clock || 3579545;
      this.rate = this.clock / 64;
      this.onIRQ = opts.onIRQ || null;
      this.onPort = opts.onPort || null;
      const N = 32;
      this.dt1 = new Uint8Array(N); this.mul = new Uint8Array(N); this.tl = new Uint8Array(N);
      this.ks = new Uint8Array(N); this.ar = new Uint8Array(N); this.amOn = new Uint8Array(N);
      this.d1r = new Uint8Array(N); this.dt2 = new Uint8Array(N); this.d2r = new Uint8Array(N);
      this.d1l = new Uint8Array(N); this.rr = new Uint8Array(N);
      this.phase = new Uint32Array(N); this.inc = new Uint32Array(N);
      this.att = new Uint16Array(N); this.egState = new Uint8Array(N); this.key = new Uint8Array(N);
      this.kc = new Uint8Array(8); this.kf = new Uint8Array(8); this.fb = new Uint8Array(8);
      this.alg = new Uint8Array(8); this.panL = new Uint8Array(8); this.panR = new Uint8Array(8);
      this.pms = new Uint8Array(8); this.ams = new Uint8Array(8);
      this.fbOut = new Int32Array(16);
      this.regs = new Uint8Array(256);
      this.out = new Int32Array(2);
      this.acc = new Float64Array(2); this.accN = 0;
      this.reset();
    }

    reset() {
      for (const k of ["dt1", "mul", "tl", "ks", "ar", "amOn", "d1r", "dt2", "d2r", "d1l", "rr", "phase", "inc", "key",
        "kc", "kf", "fb", "alg", "pms", "ams", "fbOut", "regs"]) this[k].fill(0);
      this.att.fill(0x3ff); this.egState.fill(RELEASE); this.panL.fill(1); this.panR.fill(1);
      this.addr = 0; this.status = 0; this.irq = false;
      this.timerA = 0; this.timerB = 0; this.cntA = 0; this.cntB = 0; this.cntBdiv = 0; this.ctrl14 = 0;
      this.lfrq = 0; this.amd = 0; this.pmd = 0; this.lfoWave = 0; this.lfoPhase = 0; this.lfoAM = 0; this.lfoPM = 0;
      this.lfoRand = 0x1ffff; this.ct = 0;
      this.noiseOn = 0; this.nfrq = 0; this.noiseCnt = 0; this.noiseLfsr = 1; this.noiseBit = 0;
      this.egDiv = 0; this.egCnt = 0; this.clk = 0;
      this.dirty = 0xff;
    }

    read() { return this.status; }

    setIrq() {
      const on = !!(this.status & 3);
      if (on !== this.irq) { this.irq = on; if (this.onIRQ) this.onIRQ(on); }
    }

    write(port, v) {
      v &= 0xff;
      if (!(port & 1)) { this.addr = v; return; }
      const r = this.addr;
      this.regs[r] = v;
      if (r < 0x20) { this.writeGlobal(r, v); return; }
      if (r < 0x40) {
        const ch = r & 7;
        switch (r & 0x38) {
          case 0x20: this.panR[ch] = v >> 7; this.panL[ch] = (v >> 6) & 1; this.fb[ch] = (v >> 3) & 7; this.alg[ch] = v & 7; break;
          case 0x28: this.kc[ch] = v & 0x7f; this.dirty |= 1 << ch; break;
          case 0x30: this.kf[ch] = v >> 2; this.dirty |= 1 << ch; break;
          case 0x38: this.pms[ch] = (v >> 4) & 7; this.ams[ch] = v & 3; this.dirty |= 1 << ch; break;
        }
        return;
      }
      const s = r & 0x1f;
      switch (r & 0xe0) {
        case 0x40: this.dt1[s] = (v >> 4) & 7; this.mul[s] = v & 15; break;
        case 0x60: this.tl[s] = v & 0x7f; break;
        case 0x80: this.ks[s] = v >> 6; this.ar[s] = v & 31; break;
        case 0xa0: this.amOn[s] = v >> 7; this.d1r[s] = v & 31; break;
        case 0xc0: this.dt2[s] = v >> 6; this.d2r[s] = v & 31; break;
        case 0xe0: this.d1l[s] = v >> 4; this.rr[s] = v & 15; break;
      }
      this.dirty |= 1 << (s & 7);
    }

    writeGlobal(r, v) {
      switch (r) {
        case 0x01: if (v & 2) { this.lfoPhase = 0; } break; // LFO reset
        case 0x08: {
          const ch = v & 7;
          for (let k = 0; k < 4; k++) this.keyOnOff(KEY_GROUP[k] * 8 + ch, (v >> (3 + k)) & 1);
          break;
        }
        case 0x0f: this.noiseOn = v >> 7; this.nfrq = v & 31; break;
        case 0x10: this.timerA = (this.timerA & 3) | (v << 2); break;
        case 0x11: this.timerA = (this.timerA & 0x3fc) | (v & 3); break;
        case 0x12: this.timerB = v; break;
        case 0x14: {
          if ((v & 1) && !(this.ctrl14 & 1)) this.cntA = this.timerA;
          if ((v & 2) && !(this.ctrl14 & 2)) { this.cntB = this.timerB; this.cntBdiv = 0; }
          if (v & 0x10) this.status &= ~1;
          if (v & 0x20) this.status &= ~2;
          this.ctrl14 = v;
          this.setIrq();
          break;
        }
        case 0x18: this.lfrq = v; break;
        case 0x19: if (v & 0x80) this.pmd = v & 0x7f; else this.amd = v & 0x7f; break;
        case 0x1b: {
          this.lfoWave = v & 3;
          const ct = v >> 6;
          if (ct !== this.ct) { this.ct = ct; if (this.onPort) this.onPort(ct); }
          break;
        }
      }
    }

    keyOnOff(s, on) {
      if (on && !this.key[s]) {
        this.phase[s] = 0;
        this.egState[s] = ATTACK;
        this.updateInc(s & 7);
        if (this.rateFor(s, this.ar[s] * 2) >= 62) { this.att[s] = 0; this.egState[s] = DECAY; }
      } else if (!on && this.key[s]) this.egState[s] = RELEASE;
      this.key[s] = on;
    }
    // 5-bit key code used by key scaling and DT1: octave and the top two bits of the note
    keyCode(ch) { return (this.kc[ch] >> 2) & 31; }
    rateFor(s, r) {
      if (!r) return 0;
      const v = r + (this.keyCode(s & 7) >> (3 - this.ks[s]));
      return v > 63 ? 63 : v;
    }

    // Recompute the phase increments of one channel's four operators.
    updateInc(ch) {
      const kc = this.kc[ch], oct = (kc >> 4) & 7;
      let semis = oct * 12 + NOTE[kc & 15] + this.kf[ch] / 64 - (4 * 12 + 8); // A4 = 440 Hz
      if (this.pms[ch] && this.pmd) semis += PMS_CENTS[this.pms[ch]] * (this.lfoPM / 128) * (this.pmd / 127) / 100;
      const code = this.keyCode(ch);
      for (let g = 0; g < 4; g++) {
        const s = g * 8 + ch;
        const hz = 440 * Math.pow(2, semis / 12 + DT2_CENTS[this.dt2[s]] / 1200) * (this.clock / 3579545);
        let base = Math.round(hz * 1048576 / this.rate);
        const d = this.dt1[s];
        if (d & 3) { const amt = DT1[d & 3][code]; base += (d & 4) ? -amt : amt; }
        if (base < 0) base = 0;
        this.inc[s] = (this.mul[s] ? base * this.mul[s] : base >> 1) & 0xfffff;
      }
    }

    egStep(s) {
      let st = this.egState[s], att = this.att[s], r;
      switch (st) {
        case ATTACK: r = this.rateFor(s, this.ar[s] * 2); break;
        case DECAY: r = this.rateFor(s, this.d1r[s] * 2); break;
        case SUSTAIN: r = this.rateFor(s, this.d2r[s] * 2); break;
        default: r = this.rateFor(s, this.rr[s] * 4 + 2);
      }
      const shift = r < 48 ? 11 - (r >> 2) : 0;
      if (shift > 0 && (this.egCnt & ((1 << shift) - 1))) return;
      const inc = EG_INC[r * 8 + ((this.egCnt >> shift) & 7)];
      if (st === ATTACK) {
        if (r >= 62) att = 0;
        else if (att && inc) att += ((~att) * inc) >> 4;
        if (att <= 0) { att = 0; st = DECAY; }
      } else {
        att += inc;
        if (att > 0x3ff) att = 0x3ff;
      }
      if (st === DECAY) { const lv = this.d1l[s] === 15 ? 0x3e0 : this.d1l[s] << 5; if (att >= lv) st = SUSTAIN; }
      this.egState[s] = st; this.att[s] = att;
    }

    // attenuation (10-bit units of 0.09375 dB) -> linear level, about 0..8188
    static level(a) { const l = a << 2; return l >= 3328 ? 0 : (EXP[l & 0xff] << 2) >> (l >> 8); }

    opOut(s, mod, am) {
      let a = this.att[s] + (this.tl[s] << 3);
      if (this.amOn[s]) a += am;
      if (a >= 0x3ff) return 0;
      const p = ((this.phase[s] >> 10) + mod) & 0x3ff;
      let q = p & 0xff; if (p & 0x100) q ^= 0xff;
      const level = LOGSIN[q] + (a << 2);
      const v = level >= 3328 ? 0 : (EXP[level & 0xff] << 2) >> (level >> 8);
      return (p & 0x200) ? -v : v;
    }

    // the noise generator replaces channel 8's C2 operator when it is switched on. Its level is not
    // taken through the log/exponent tables like a tone's: the 10-bit attenuation (envelope + TL + AM)
    // is used directly, inverted, so the noise fades in a straight line and is quieter than a full tone
    // (at no attenuation a square of +-2046, against a tone's +-8188)
    noiseOut(am) {
      let a = this.att[31] + (this.tl[31] << 3);
      if (this.amOn[31]) a += am;
      if (a >= 0x3ff) return 0;
      const v = (0x3ff - a) << 1;
      return this.noiseBit ? v : -v;
    }

    stepLFO() {
      // LFRQ: each 16 steps doubles the rate; 0xff is about 53 Hz at 3.58 MHz
      const hz = (16 + (this.lfrq & 15)) / 32 * Math.pow(2, this.lfrq >> 4) * 0.0016666 * (this.clock / 3579545);
      const old = this.lfoPhase;
      this.lfoPhase += hz / this.rate;
      if (this.lfoPhase >= 1) this.lfoPhase -= Math.floor(this.lfoPhase);
      const p = this.lfoPhase;
      let am, pm;
      switch (this.lfoWave) {
        case 0: am = 255 - Math.floor(p * 256); pm = p < 0.5 ? Math.floor(p * 256) : Math.floor(p * 256) - 256; break; // saw
        case 1: am = p < 0.5 ? 255 : 0; pm = p < 0.5 ? 127 : -128; break; // square
        case 2: am = p < 0.5 ? 255 - Math.floor(p * 510) : Math.floor((p - 0.5) * 510); // triangle
          pm = p < 0.25 ? Math.floor(p * 508) : p < 0.75 ? Math.floor((0.5 - p) * 508) : Math.floor((p - 1) * 508); break;
        default: // noise: a new random value each time the LFO wraps a 1/32 step
          if (Math.floor(p * 32) !== Math.floor(old * 32)) {
            const b = ((this.lfoRand >> 0) ^ (this.lfoRand >> 3)) & 1;
            this.lfoRand = (this.lfoRand >> 1) | (b << 16);
          }
          am = this.lfoRand & 0xff; pm = (this.lfoRand & 0xff) - 128;
      }
      this.lfoAM = am;
      if (pm !== this.lfoPM) { this.lfoPM = pm; if (this.pmd) for (let c = 0; c < 8; c++) if (this.pms[c]) this.dirty |= 1 << c; }
    }

    // Produce one sample (at clock / 64) into this.out.
    tick() {
      this.stepLFO();
      if (this.dirty) { for (let c = 0; c < 8; c++) if (this.dirty & (1 << c)) this.updateInc(c); this.dirty = 0; }
      if (++this.egDiv === 3) {
        this.egDiv = 0;
        this.egCnt = (this.egCnt + 1) & 0xfff;
        for (let s = 0; s < 32; s++) this.egStep(s);
      }
      // noise: shifts at clock / (32 * (32 - NFRQ)), i.e. every (32 - NFRQ) / 2 samples
      this.noiseCnt += 2;
      const np = 32 - this.nfrq;
      while (this.noiseCnt >= np) {
        this.noiseCnt -= np;
        const b = (this.noiseLfsr ^ (this.noiseLfsr >> 3)) & 1;
        this.noiseLfsr = (this.noiseLfsr >> 1) | (b << 16);
        this.noiseBit = this.noiseLfsr & 1;
      }
      const amBase = (this.lfoAM * this.amd) >> 7; // 0..253
      let L = 0, R = 0;
      for (let ch = 0; ch < 8; ch++) {
        const am = this.ams[ch] ? amBase << (this.ams[ch] - 1) : 0;
        const fbn = this.fb[ch];
        const f0 = this.fbOut[ch * 2], f1 = this.fbOut[ch * 2 + 1];
        const m1 = this.opOut(M1 + ch, fbn ? (f0 + f1) >> (10 - fbn) : 0, am);
        this.fbOut[ch * 2] = f1; this.fbOut[ch * 2 + 1] = m1;
        const c2 = (mod) => (ch === 7 && this.noiseOn ? this.noiseOut(am) : this.opOut(C2 + ch, mod, am));
        let o, c1, m2;
        switch (this.alg[ch]) {
          case 0: c1 = this.opOut(C1 + ch, m1 >> 1, am); m2 = this.opOut(M2 + ch, c1 >> 1, am); o = c2(m2 >> 1); break;
          case 1: c1 = this.opOut(C1 + ch, 0, am); m2 = this.opOut(M2 + ch, (m1 + c1) >> 1, am); o = c2(m2 >> 1); break;
          case 2: c1 = this.opOut(C1 + ch, 0, am); m2 = this.opOut(M2 + ch, c1 >> 1, am); o = c2((m1 + m2) >> 1); break;
          case 3: c1 = this.opOut(C1 + ch, m1 >> 1, am); m2 = this.opOut(M2 + ch, 0, am); o = c2((c1 + m2) >> 1); break;
          case 4: c1 = this.opOut(C1 + ch, m1 >> 1, am); m2 = this.opOut(M2 + ch, 0, am); o = c1 + c2(m2 >> 1); break;
          case 5: o = this.opOut(C1 + ch, m1 >> 1, am) + this.opOut(M2 + ch, m1 >> 1, am) + c2(m1 >> 1); break;
          case 6: o = this.opOut(C1 + ch, m1 >> 1, am) + this.opOut(M2 + ch, 0, am) + c2(0); break;
          default: o = m1 + this.opOut(C1 + ch, 0, am) + this.opOut(M2 + ch, 0, am) + c2(0);
        }
        // (a channel's carriers add up without being cut off at one operator's range; only the
        // final sum is held to 16 bits)
        if (this.panL[ch]) L += o;
        if (this.panR[ch]) R += o;
      }
      this.out[0] = L > 32767 ? 32767 : L < -32768 ? -32768 : L;
      this.out[1] = R > 32767 ? 32767 : R < -32768 ? -32768 : R;
      for (let s = 0; s < 32; s++) this.phase[s] = (this.phase[s] + this.inc[s]) & 0xfffff;
      this.timers();
    }

    // Timer A counts once a sample (period 1024 - TA samples); timer B every 16 samples (period 16 * (256 - TB)).
    timers() {
      const c = this.ctrl14;
      if (c & 1) {
        if (++this.cntA >= 1024) {
          this.cntA = this.timerA;
          if (c & 4) { this.status |= 1; this.setIrq(); }
          if (c & 0x80) for (let ch = 0; ch < 8; ch++) for (let g = 0; g < 4; g++) { // CSM: key every operator on and off
            const s = g * 8 + ch; if (!this.key[s]) { this.keyOnOff(s, 1); this.keyOnOff(s, 0); this.egState[s] = ATTACK; }
          }
        }
      }
      if (c & 2) {
        if (++this.cntBdiv >= 16) {
          this.cntBdiv = 0;
          if (++this.cntB >= 256) { this.cntB = this.timerB; if (c & 8) { this.status |= 2; this.setIrq(); } }
        }
      }
    }

    // Run n input clocks; each 64 make a sample, summed into acc / accN.
    clocks(n) {
      this.clk += n;
      while (this.clk >= 64) {
        this.clk -= 64;
        this.tick();
        this.acc[0] += this.out[0]; this.acc[1] += this.out[1]; this.accN++;
      }
    }

    saveState() {
      const o = {};
      for (const k of ["dt1", "mul", "tl", "ks", "ar", "amOn", "d1r", "dt2", "d2r", "d1l", "rr", "phase", "inc", "att", "egState", "key",
        "kc", "kf", "fb", "alg", "panL", "panR", "pms", "ams", "fbOut", "regs"]) o[k] = this[k].slice();
      for (const k of ["addr", "status", "irq", "timerA", "timerB", "cntA", "cntB", "cntBdiv", "ctrl14", "lfrq", "amd", "pmd", "lfoWave",
        "lfoPhase", "lfoAM", "lfoPM", "lfoRand", "ct", "noiseOn", "nfrq", "noiseCnt", "noiseLfsr", "noiseBit", "egDiv", "egCnt", "clk"]) o[k] = this[k];
      return o;
    }
    loadState(o) {
      for (const k in o) {
        if (ArrayBuffer.isView(this[k])) this[k].set(o[k]);
        else this[k] = o[k];
      }
      this.dirty = 0xff;
    }
  }

  global.RPYM2151 = YM2151;
})(typeof window !== "undefined" ? window : globalThis);
