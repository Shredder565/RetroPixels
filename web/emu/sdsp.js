// RetroPixels — Sony S-DSP (SNES sound generator). Original code written from the
// chip's documented behaviour: 8 voices of BRR-compressed samples, ADSR / GAIN
// envelopes, pitch modulation, noise, echo with an 8-tap FIR filter.
// One call to sample() produces one 32 kHz stereo sample.
"use strict";

(function (global) {
  // Envelope / noise rate counter: rate r fires every RATE[r] samples. Rates
  // 1-30 come in groups of three (2048, 1536, 1280 samples, halving each group),
  // rate 31 fires every sample and rate 0 never. OFFSET staggers the three rates
  // of a group so they don't all fire on the same sample.
  const RATE = new Int32Array(32), OFFSET = new Int32Array(32);
  for (let r = 1; r < 32; r++) {
    const k = (r - 1) % 3;
    RATE[r] = r === 31 ? 1 : [2048, 1536, 1280][k] >> ((r - 1) / 3 | 0);
    OFFSET[r] = [0, 1040, 536][k];
  }
  // 4-tap interpolation weights (a smooth bell-shaped kernel), 256 fractional positions
  const KERN = new Int32Array(256 * 4);
  for (let f = 0; f < 256; f++) {
    const t = f / 256, d = [1 + t, t, 1 - t, 2 - t], w = d.map((x) => Math.exp(-(x * x) / (2 * 0.62 * 0.62)));
    const sum = w[0] + w[1] + w[2] + w[3];
    let acc = 0;
    for (let k = 0; k < 4; k++) { KERN[f * 4 + k] = Math.round(w[k] / sum * 2048); acc += KERN[f * 4 + k]; }
    KERN[f * 4 + 1] += 2048 - acc;
  }
  const clamp16 = (v) => (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
  const ATTACK = 0, DECAY = 1, SUSTAIN = 2, RELEASE = 3;

  class SDSP {
    constructor(ram) {
      this.ram = ram;
      this.regs = new Uint8Array(128);
      this.buf = new Int16Array(8 * 19); // per voice: 3 previous + 16 current decoded samples
      this.v = new Int32Array(8 * 10);    // per voice ints: brrAddr, pos, env, mode, keyDelay, out, active, hidden flag...
      this.echoHist = new Int16Array(16); // 8 L + 8 R
      this.out = new Int16Array(2);
      this.reset();
    }
    reset() {
      this.regs.fill(0); this.buf.fill(0); this.v.fill(0); this.echoHist.fill(0);
      this.regs[0x6c] = 0xe0;
      for (let i = 0; i < 8; i++) this.v[i * 10 + 3] = RELEASE;
      this.counter = 0; this.noise = 0x4000; this.echoPos = 0; this.echoLen = 0; this.firPos = 0;
      this.konPending = 0; this.everyOther = 0;
    }
    read(r) { return this.regs[r & 0x7f]; }
    write(r, val) {
      r &= 0x7f;
      if (r === 0x7c) { this.regs[r] = 0; return; } // ENDX: any write clears it
      this.regs[r] = val;
      if (r === 0x4c) this.konPending |= val;
    }
    tick(r) { return r && ((this.counter + OFFSET[r]) % RATE[r]) === 0; }

    decodeBlock(i) {
      const V = this.v, o = i * 10, ram = this.ram, b = this.buf, bo = i * 19;
      const addr = V[o];
      const hdr = ram[addr & 0xffff], shift = hdr >> 4, filter = (hdr >> 2) & 3;
      // keep the last three samples for interpolation / the filters
      b[bo] = b[bo + 16]; b[bo + 1] = b[bo + 17]; b[bo + 2] = b[bo + 18];
      // the buffer holds doubled samples; the filters work on the plain 15-bit ones
      let p1 = b[bo + 2] >> 1, p2 = b[bo + 1] >> 1;
      for (let k = 0; k < 16; k++) {
        const byte = ram[(addr + 1 + (k >> 1)) & 0xffff];
        let n = (k & 1) ? byte & 15 : byte >> 4;
        if (n >= 8) n -= 16;
        let s = shift <= 12 ? (n << shift) >> 1 : (n < 0 ? -2048 : 0);
        // prediction from the previous two samples (filter 1: old * 15/16;
        // 2: old * 61/32 - older * 15/16; 3: old * 115/64 - older * 13/16)
        if (filter === 1) s += p1 + ((-p1) >> 4);
        else if (filter === 2) s += 2 * p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4);
        else if (filter === 3) s += 2 * p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4);
        s = clamp16(s);
        const w = (s << 17) >> 16; // stored doubled, wrapping to 16 bits
        b[bo + 3 + k] = w;
        p2 = p1; p1 = w >> 1;
      }
      V[o + 7] = hdr; // remember flags of the block being played
    }
    keyOn(i) {
      const V = this.v, o = i * 10, R = this.regs;
      const dir = R[0x5d] << 8, src = R[i * 16 + 4];
      const ent = (dir + src * 4) & 0xffff;
      V[o] = this.ram[ent] | (this.ram[(ent + 1) & 0xffff] << 8);
      V[o + 1] = 0; V[o + 2] = 0; V[o + 3] = ATTACK; V[o + 4] = 5; V[o + 6] = 1;
      this.buf.fill(0, i * 19, i * 19 + 19);
      this.decodeBlock(i);
      R[0x7c] &= ~(1 << i);
    }

    envelope(i) {
      const V = this.v, o = i * 10, R = this.regs, base = i * 16;
      let env = V[o + 2], mode = V[o + 3];
      if (mode === RELEASE) { env -= 8; if (env < 0) env = 0; V[o + 2] = env; return; }
      const adsr1 = R[base + 5], adsr2 = R[base + 6];
      if (adsr1 & 0x80) {
        if (mode === ATTACK) {
          const r = (adsr1 & 15) * 2 + 1;
          if (this.tick(r)) env += r === 31 ? 1024 : 32;
          if (env >= 0x7ff) { env = 0x7ff; mode = DECAY; }
        } else if (mode === DECAY) {
          const r = ((adsr1 >> 4) & 7) * 2 + 16;
          if (this.tick(r)) env -= ((env - 1) >> 8) + 1;
          if ((env >> 8) === (adsr2 >> 5)) mode = SUSTAIN;
        } else {
          const r = adsr2 & 31;
          if (this.tick(r)) env -= ((env - 1) >> 8) + 1;
        }
      } else {
        const g = R[base + 7];
        if (!(g & 0x80)) env = (g & 0x7f) << 4;
        else {
          const r = g & 31;
          if (this.tick(r)) {
            switch ((g >> 5) & 3) {
              case 0: env -= 32; break;
              case 1: env -= ((env - 1) >> 8) + 1; break;
              case 2: env += 32; break;
              default: env += env < 0x600 ? 32 : 8;
            }
          }
        }
      }
      if (env < 0) env = 0; else if (env > 0x7ff) env = 0x7ff;
      V[o + 2] = env; V[o + 3] = mode;
    }

    // Produce one stereo sample into this.out.
    sample() {
      const R = this.regs, V = this.v, b = this.buf;
      this.counter = this.counter === 0 ? 0x77ff : this.counter - 1;
      // key on / off (handled every other sample like the real chip)
      this.everyOther ^= 1;
      if (this.everyOther) {
        const kon = this.konPending; this.konPending = 0;
        R[0x4c] = 0;
        const koff = R[0x5c];
        for (let i = 0; i < 8; i++) {
          if (koff & (1 << i)) V[i * 10 + 3] = RELEASE;
          if (kon & (1 << i)) this.keyOn(i);
        }
      }
      if (R[0x6c] & 0x80) { for (let i = 0; i < 8; i++) { V[i * 10 + 3] = RELEASE; V[i * 10 + 2] = 0; } }
      // noise
      if (this.tick(R[0x6c] & 31)) { const fb = (this.noise ^ (this.noise >> 1)) & 1; this.noise = (this.noise >> 1) | (fb << 14); }
      const pmon = R[0x2d], non = R[0x3d], eon = R[0x4d];
      let L = 0, Rr = 0, eL = 0, eR = 0, prevOut = 0;
      for (let i = 0; i < 8; i++) {
        const o = i * 10, base = i * 16;
        if (!V[o + 6]) { R[base + 8] = 0; R[base + 9] = 0; prevOut = 0; continue; }
        if (V[o + 4] > 0) { V[o + 4]--; R[base + 8] = 0; R[base + 9] = 0; prevOut = 0; continue; }
        let pitch = (R[base + 2] | (R[base + 3] << 8)) & 0x3fff;
        if ((pmon & (1 << i)) && i > 0) { pitch += ((prevOut >> 5) * pitch) >> 10; if (pitch < 0) pitch = 0; else if (pitch > 0x3fff) pitch = 0x3fff; }
        // interpolate
        const pos = V[o + 1], idx = pos >> 12, f = (pos >> 4) & 0xff, bo = i * 19 + idx;
        let s;
        if (non & (1 << i)) s = ((this.noise << 17) >> 16);
        else {
          s = (KERN[f * 4] * b[bo] + KERN[f * 4 + 1] * b[bo + 1] + KERN[f * 4 + 2] * b[bo + 2] + KERN[f * 4 + 3] * b[bo + 3]) >> 11;
          s = clamp16(s) & ~1;
        }
        this.envelope(i);
        const env = V[o + 2];
        const out = (s * env) >> 11;
        V[o + 5] = out; prevOut = out;
        R[base + 8] = env >> 4; R[base + 9] = (out >> 8) & 0xff;
        const vl = (R[base] << 24) >> 24, vr = (R[base + 1] << 24) >> 24;
        const ol = (out * vl) >> 7, or = (out * vr) >> 7;
        L += ol; Rr += or;
        if (eon & (1 << i)) { eL += ol; eR += or; }
        // advance
        let np = pos + pitch;
        while ((np >> 12) >= 16) {
          np -= 16 << 12;
          const hdr = V[o + 7];
          if (hdr & 1) { // end of sample
            R[0x7c] |= 1 << i;
            if (hdr & 2) { const dir = R[0x5d] << 8, ent = (dir + R[base + 4] * 4 + 2) & 0xffff; V[o] = this.ram[ent] | (this.ram[(ent + 1) & 0xffff] << 8); }
            else { V[o + 3] = RELEASE; V[o + 2] = 0; V[o + 6] = 0; }
          } else V[o] = (V[o] + 9) & 0xffff;
          if (V[o + 6]) this.decodeBlock(i);
        }
        V[o + 1] = np;
      }
      L = clamp16(L); Rr = clamp16(Rr);
      // echo
      const ram = this.ram, esa = R[0x6d] << 8;
      const ea = (esa + this.echoPos) & 0xffff;
      const inL = ((ram[ea] | (ram[(ea + 1) & 0xffff] << 8)) << 16) >> 17;
      const inR = ((ram[(ea + 2) & 0xffff] | (ram[(ea + 3) & 0xffff] << 8)) << 16) >> 17;
      const H = this.echoHist;
      this.firPos = (this.firPos + 1) & 7;
      H[this.firPos] = inL; H[8 + this.firPos] = inR;
      let fl = 0, fr = 0;
      for (let k = 0; k < 8; k++) {
        const c = (R[k * 16 + 0x0f] << 24) >> 24, hp = (this.firPos + k + 1) & 7;
        fl += (H[hp] * c) >> 6; fr += (H[8 + hp] * c) >> 6;
      }
      fl = clamp16(fl) & ~1; fr = clamp16(fr) & ~1;
      const mvl = (R[0x0c] << 24) >> 24, mvr = (R[0x1c] << 24) >> 24, evl = (R[0x2c] << 24) >> 24, evr = (R[0x3c] << 24) >> 24;
      let outL = clamp16(((L * mvl) >> 7) + ((fl * evl) >> 7));
      let outR = clamp16(((Rr * mvr) >> 7) + ((fr * evr) >> 7));
      if (R[0x6c] & 0x40) { outL = 0; outR = 0; }
      this.out[0] = outL; this.out[1] = outR;
      // write echo
      if (!(R[0x6c] & 0x20)) {
        const efb = (R[0x0d] << 24) >> 24;
        const wl = clamp16(eL + ((fl * efb) >> 7)) & ~1, wr = clamp16(eR + ((fr * efb) >> 7)) & ~1;
        ram[ea] = wl & 0xff; ram[(ea + 1) & 0xffff] = (wl >> 8) & 0xff; ram[(ea + 2) & 0xffff] = wr & 0xff; ram[(ea + 3) & 0xffff] = (wr >> 8) & 0xff;
      }
      if (this.echoPos === 0) this.echoLen = (R[0x7d] & 15) * 2048 || 4;
      this.echoPos += 4;
      if (this.echoPos >= this.echoLen) this.echoPos = 0;
    }

    saveState() {
      return { regs: this.regs.slice(), buf: this.buf.slice(), v: this.v.slice(), hist: this.echoHist.slice(),
        m: [this.counter, this.noise, this.echoPos, this.echoLen, this.firPos, this.konPending, this.everyOther] };
    }
    loadState(o) {
      this.regs.set(o.regs); this.buf.set(o.buf); this.v.set(o.v); this.echoHist.set(o.hist);
      [this.counter, this.noise, this.echoPos, this.echoLen, this.firPos, this.konPending, this.everyOther] = o.m;
    }
  }
  global.RPSDSP = SDSP;
})(typeof window !== "undefined" ? window : globalThis);
