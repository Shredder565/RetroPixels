// RetroPixels — Sega's SN76489-compatible sound chip (PSG): three square-wave
// channels and a noise channel. Used by the Genesis and the Master System.
// Original code, written from the chip's documented behaviour.
"use strict";

(function (global) {
  class PSG {
    constructor() {
      this.vol = new Float32Array(16);
      for (let i = 0; i < 15; i++) this.vol[i] = Math.pow(10, -i * 2 / 20);
      this.reg = new Uint16Array(8); this.cnt = new Int32Array(4); this.pol = new Int8Array(4);
      this.reset();
    }
    reset() {
      this.reg.fill(0); for (let c = 0; c < 4; c++) this.reg[c * 2 + 1] = 15;
      this.cnt.fill(0); this.pol.fill(1); this.lfsr = 0x8000; this.latch = 0;
    }
    write(v) {
      if (v & 0x80) {
        this.latch = (v >> 4) & 7;
        const r = this.latch;
        if (r & 1) this.reg[r] = v & 15;
        else if (r === 6) { this.reg[6] = v & 7; this.lfsr = 0x8000; }
        else this.reg[r] = (this.reg[r] & 0x3f0) | (v & 15);
      } else {
        const r = this.latch;
        if (r & 1) this.reg[r] = v & 15;
        else if (r === 6) { this.reg[6] = v & 7; this.lfsr = 0x8000; }
        else this.reg[r] = (this.reg[r] & 15) | ((v & 0x3f) << 4);
      }
    }
    // Advance one step; returns the mixed output.
    step() {
      let out = 0;
      for (let c = 0; c < 3; c++) {
        const per = this.reg[c * 2];
        if (--this.cnt[c] <= 0) { this.cnt[c] = per; this.pol[c] = -this.pol[c]; }
        out += (per <= 1 ? 1 : this.pol[c]) * this.vol[this.reg[c * 2 + 1]];
      }
      const nr = this.reg[6] & 3, nper = nr === 3 ? this.reg[4] : 0x10 << nr;
      if (--this.cnt[3] <= 0) {
        this.cnt[3] = nper;
        this.pol[3] = -this.pol[3];
        if (this.pol[3] > 0) {
          const fb = (this.reg[6] & 4) ? ((this.lfsr ^ (this.lfsr >> 3)) & 1) : (this.lfsr & 1);
          this.lfsr = (this.lfsr >> 1) | (fb << 15);
        }
      }
      out += ((this.lfsr & 1) ? 1 : -1) * this.vol[this.reg[7]] * 0.5;
      return out;
    }
    saveState() { return { reg: this.reg.slice(), cnt: this.cnt.slice(), pol: this.pol.slice(), lfsr: this.lfsr, latch: this.latch }; }
    loadState(o) { this.reg.set(o.reg); this.cnt.set(o.cnt); this.pol.set(o.pol); this.lfsr = o.lfsr; this.latch = o.latch; }
  }

  global.RPSN76489 = PSG;
})(typeof window !== "undefined" ? window : globalThis);
