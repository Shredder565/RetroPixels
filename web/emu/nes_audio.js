// RetroPixels — Famicom cartridge sound chips. Original code.
//
// Some Famicom cartridges carry their own sound hardware, mixed into the
// console's audio: Konami VRC6 (2 pulses + sawtooth) and VRC7 (6-channel FM
// synthesizer), Namco 163 (up to 8 wavetable channels), Sunsoft 5B (3 square
// channels with noise and envelope) and the MMC5 (2 pulses + 8-bit PCM).
// Each chip is clocked once per CPU cycle and its output() is added to the mix.
"use strict";

(function (global) {
  const TAU = Math.PI * 2;

  // -------------------------------------------------------------------
  // Konami VRC6
  // -------------------------------------------------------------------
  class VRC6 {
    constructor() {
      this.pulses = [0, 1].map(() => ({ vol: 0, duty: 0, mode: 0, period: 0, on: false, timer: 0, step: 15 }));
      this.saw = { rate: 0, period: 0, on: false, timer: 0, phase: 0, acc: 0 };
      this.halt = false; this.shift = 0; this.level = 0;
    }
    // ch: 0 = pulse 1 ($9000), 1 = pulse 2 ($A000), 2 = sawtooth ($B000)
    write(ch, r, v) {
      if (ch === 0 && r === 3) { this.halt = !!(v & 1); this.shift = v & 4 ? 8 : v & 2 ? 4 : 0; return; }
      if (ch < 2) {
        const p = this.pulses[ch];
        if (r === 0) { p.mode = v >> 7; p.duty = (v >> 4) & 7; p.vol = v & 15; }
        else if (r === 1) p.period = (p.period & 0xf00) | v;
        else if (r === 2) { p.period = (p.period & 0xff) | ((v & 15) << 8); p.on = !!(v & 0x80); if (!p.on) p.step = 15; }
      } else {
        const s = this.saw;
        if (r === 0) s.rate = v & 0x3f;
        else if (r === 1) s.period = (s.period & 0xf00) | v;
        else if (r === 2) { s.period = (s.period & 0xff) | ((v & 15) << 8); s.on = !!(v & 0x80); if (!s.on) { s.acc = 0; s.phase = 0; } }
      }
    }
    clock() {
      if (this.halt) return;
      let out = 0;
      for (const p of this.pulses) {
        if (p.on) {
          if (--p.timer < 0) { p.timer = p.period >> this.shift; p.step = (p.step - 1) & 15; }
          if (p.mode || p.step <= p.duty) out += p.vol;
        }
      }
      const s = this.saw;
      if (s.on) {
        if (--s.timer < 0) {
          s.timer = s.period >> this.shift;
          s.phase++;
          if (s.phase === 14) { s.phase = 0; s.acc = 0; } else if (!(s.phase & 1)) s.acc = (s.acc + s.rate) & 0xff;
        }
        out += s.acc >> 3;
      }
      this.level = out * 0.0095;
    }
    output() { return this.level; }
  }

  // -------------------------------------------------------------------
  // Namco 163: 128 bytes of internal RAM hold both the channel settings
  // and the 4-bit wave samples. One channel is updated every 15 CPU cycles.
  // -------------------------------------------------------------------
  class N163 {
    constructor() { this.ram = new Uint8Array(128); this.addr = 0; this.disabled = false; this.sub = 0; this.cur = 7; this.outs = new Float32Array(8); this.level = 0; }
    writeData(v) { this.ram[this.addr & 0x7f] = v; if (this.addr & 0x80) this.addr = 0x80 | ((this.addr + 1) & 0x7f); }
    readData() { const v = this.ram[this.addr & 0x7f]; if (this.addr & 0x80) this.addr = 0x80 | ((this.addr + 1) & 0x7f); return v; }
    clock() {
      if (this.disabled || ++this.sub < 15) return;
      this.sub = 0;
      const ram = this.ram, count = ((ram[0x7f] >> 4) & 7) + 1, first = 8 - count;
      if (this.cur < first) this.cur = 7;
      const b = 0x40 + this.cur * 8;
      const freq = ram[b] | (ram[b + 2] << 8) | ((ram[b + 4] & 3) << 16);
      const len = (256 - (ram[b + 4] & 0xfc)) << 16;
      let phase = ram[b + 1] | (ram[b + 3] << 8) | (ram[b + 5] << 16);
      phase = (phase + freq) % len;
      ram[b + 1] = phase & 0xff; ram[b + 3] = (phase >> 8) & 0xff; ram[b + 5] = (phase >> 16) & 0xff;
      const idx = ((phase >> 16) + ram[b + 6]) & 0xff;
      const sample = (ram[idx >> 1] >> ((idx & 1) << 2)) & 15;
      this.outs[this.cur] = (sample - 8) * (ram[b + 7] & 15);
      this.cur = this.cur === first ? 7 : this.cur - 1;
      // The chip plays the active channels one after another; averaging them gives the same mix.
      let sum = 0; for (let i = first; i < 8; i++) sum += this.outs[i];
      this.level = (sum / count) * 0.0022;
    }
    output() { return this.disabled ? 0 : this.level; }
  }

  // -------------------------------------------------------------------
  // Sunsoft 5B (a YM2149-style chip in Gimmick!): 3 square channels, noise,
  // and a 32-step volume envelope. Volume steps are logarithmic.
  // -------------------------------------------------------------------
  const VOL5B = new Float32Array(32);
  for (let i = 1; i < 32; i++) VOL5B[i] = Math.pow(10, -(31 - i) * 1.5 / 20);
  class S5B {
    constructor() {
      this.regs = new Uint8Array(16); this.addr = 0; this.pre = 0;
      this.tone = [0, 1, 2].map(() => ({ count: 0, out: 0 }));
      this.noiseCount = 0; this.lfsr = 1; this.envCount = 0; this.envPos = 0; this.envAttack = false; this.envHold = false; this.envVal = 0;
      this.level = 0;
    }
    writeAddr(v) { this.addr = v; }
    writeData(v) {
      if (this.addr & 0xf0) return;
      const r = this.addr & 15; this.regs[r] = v;
      if (r === 13) { this.envPos = 0; this.envHold = false; this.envAttack = !!(v & 4); this.envVal = this.envAttack ? 0 : 31; this.envCount = 0; }
    }
    envStep() {
      if (this.envHold) return;
      if (++this.envPos > 31) {
        const s = this.regs[13];
        if (!(s & 8)) { this.envHold = true; this.envVal = 0; return; }
        if (s & 1) { if (s & 2) this.envAttack = !this.envAttack; this.envHold = true; this.envVal = this.envAttack ? 31 : 0; return; }
        if (s & 2) this.envAttack = !this.envAttack;
        this.envPos = 0;
      }
      this.envVal = this.envAttack ? this.envPos : 31 - this.envPos;
    }
    clock() {
      if (++this.pre < 16) return;
      this.pre = 0;
      const r = this.regs;
      for (let i = 0; i < 3; i++) {
        const t = this.tone[i], period = (r[i * 2] | ((r[i * 2 + 1] & 15) << 8)) || 1;
        if (++t.count >= period) { t.count = 0; t.out ^= 1; }
      }
      const np = ((r[6] & 0x1f) || 1) * 2;
      if (++this.noiseCount >= np) { this.noiseCount = 0; const bit = (this.lfsr ^ (this.lfsr >> 3)) & 1; this.lfsr = (this.lfsr >> 1) | (bit << 16); }
      const ep = (r[11] | (r[12] << 8)) || 1;
      if (++this.envCount >= ep) { this.envCount = 0; this.envStep(); }
      let out = 0;
      const noise = this.lfsr & 1, mix = r[7];
      for (let i = 0; i < 3; i++) {
        const on = ((this.tone[i].out | ((mix >> i) & 1)) & (noise | ((mix >> (i + 3)) & 1)));
        if (!on) continue;
        const v = r[8 + i];
        out += v & 0x10 ? VOL5B[this.envVal] : (v & 15 ? VOL5B[(v & 15) * 2 + 1] : 0);
      }
      this.level = out * 0.45;
    }
    output() { return this.level; }
  }

  // -------------------------------------------------------------------
  // MMC5: two pulse channels like the console's (without sweep) and a raw 8-bit PCM output.
  // -------------------------------------------------------------------
  const LENGTHS = [10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30];
  const DUTY = [0x02, 0x06, 0x1e, 0xf9]; // 8-step duty patterns as bit masks
  class MMC5Audio {
    constructor() {
      this.p = [0, 1].map(() => ({ duty: 0, loop: false, constant: false, vol: 0, period: 0, timer: 0, step: 0, len: 0, on: false,
        envStart: false, envDiv: 0, envDecay: 0 }));
      this.pcm = 0; this.pcmRead = false; this.frame = 0; this.odd = 0; this.level = 0;
    }
    write(a, v) {
      if (a >= 0x5000 && a <= 0x5007) {
        const p = this.p[(a >> 2) & 1];
        switch (a & 3) {
          case 0: p.duty = v >> 6; p.loop = !!(v & 0x20); p.constant = !!(v & 0x10); p.vol = v & 15; break;
          case 2: p.period = (p.period & 0x700) | v; break;
          case 3: p.period = (p.period & 0xff) | ((v & 7) << 8); if (p.on) p.len = LENGTHS[v >> 3]; p.step = 0; p.envStart = true; break;
        }
      } else if (a === 0x5010) this.pcmRead = !!(v & 1);
      else if (a === 0x5011) { if (!this.pcmRead && v) this.pcm = v; }
      else if (a === 0x5015) { for (let i = 0; i < 2; i++) { this.p[i].on = !!(v & (1 << i)); if (!this.p[i].on) this.p[i].len = 0; } }
    }
    status() { return (this.p[0].len ? 1 : 0) | (this.p[1].len ? 2 : 0); }
    clock() {
      this.odd ^= 1;
      let out = 0;
      const quarter = ++this.frame >= 7457; if (quarter) this.frame = 0;
      for (const p of this.p) {
        if (this.odd && --p.timer < 0) { p.timer = p.period; p.step = (p.step - 1) & 7; }
        if (quarter) { // envelope and length counter run at a steady 240 Hz
          if (p.envStart) { p.envStart = false; p.envDecay = 15; p.envDiv = p.vol; }
          else if (p.envDiv > 0) p.envDiv--;
          else { p.envDiv = p.vol; if (p.envDecay > 0) p.envDecay--; else if (p.loop) p.envDecay = 15; }
          if (p.len > 0 && !p.loop) p.len--;
        }
        if (p.len && (DUTY[p.duty] >> p.step) & 1) out += p.constant ? p.vol : p.envDecay;
      }
      this.level = out * 0.0095 + this.pcm * 0.0021;
    }
    output() { return this.level; }
  }

  // -------------------------------------------------------------------
  // Konami VRC7: a 6-channel, 2-operator FM synthesizer (a cut-down OPLL).
  // Each channel's modulator operator bends the phase of its carrier
  // operator; both follow attack/decay/sustain/release envelopes.
  // The chip produces one sample every 36 CPU cycles (about 49.7 kHz).
  // -------------------------------------------------------------------
  // The chip's 15 built-in instruments (instrument 0 is the game's own).
  const VRC7_PATCHES = [
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
    [0x03, 0x21, 0x05, 0x06, 0xe8, 0x81, 0x42, 0x27],
    [0x13, 0x41, 0x14, 0x0d, 0xd8, 0xf6, 0x23, 0x12],
    [0x11, 0x11, 0x08, 0x08, 0xfa, 0xb2, 0x20, 0x12],
    [0x31, 0x61, 0x0c, 0x07, 0xa8, 0x64, 0x61, 0x27],
    [0x32, 0x21, 0x1e, 0x06, 0xe1, 0x76, 0x01, 0x28],
    [0x02, 0x01, 0x06, 0x00, 0xa3, 0xe2, 0xf4, 0xf4],
    [0x21, 0x61, 0x1d, 0x07, 0x82, 0x81, 0x11, 0x07],
    [0x23, 0x21, 0x22, 0x17, 0xa2, 0x72, 0x01, 0x17],
    [0x35, 0x11, 0x25, 0x00, 0x40, 0x73, 0x72, 0x01],
    [0xb5, 0x01, 0x0f, 0x0f, 0xa8, 0xa5, 0x51, 0x02],
    [0x17, 0xc1, 0x24, 0x07, 0xf8, 0xf8, 0x22, 0x12],
    [0x71, 0x23, 0x11, 0x06, 0x65, 0x74, 0x18, 0x16],
    [0x01, 0x02, 0xd3, 0x05, 0xc9, 0x95, 0x03, 0x02],
    [0x61, 0x63, 0x0c, 0x00, 0x94, 0xc0, 0x33, 0xf6],
    [0x21, 0x72, 0x0d, 0x00, 0xc1, 0xd5, 0x56, 0x06],
  ];
  const FM_RATE = 1789773 / 36;
  const MULT = [0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15];
  const KSL_DB = [0, 9, 12, 13.875, 15, 16.125, 16.875, 17.625, 18, 18.75, 19.125, 19.5, 19.875, 20.25, 20.625, 21];
  const SIN = new Float64Array(4096);
  for (let i = 0; i < 4096; i++) SIN[i] = Math.sin(i * TAU / 4096);
  const DB_MAX = 48; // the envelope's range; quieter than this is silence
  const DB_TO_AMP = new Float64Array(4097); // attenuation in 1/64 dB steps up to 64 dB
  for (let i = 0; i <= 4096; i++) DB_TO_AMP[i] = Math.pow(10, -(i / 64) / 20);
  const amp = (db) => db >= 64 ? 0 : DB_TO_AMP[(db * 64) | 0];
  // Envelope speeds for a rate index 0-63 (4 x rate + key scaling).
  const DECAY = new Float64Array(64), ATTACK = new Float64Array(64);
  for (let r = 0; r < 64; r++) {
    // decay: dB per sample, doubling every 4 rate steps
    DECAY[r] = r < 4 ? 0 : 1.61 * Math.pow(2, (r >> 2) - 15) * (1 + (r & 3) / 4);
    // attack: exponential approach to full volume; time halves every 4 rate steps
    const ms = 2826 / Math.pow(2, (r - 4) / 4);
    ATTACK[r] = r < 4 ? 0 : r >= 60 ? 1 : Math.min(1, 4.85 / (ms / 1000 * FM_RATE));
  }

  class FMOp {
    constructor() { this.phase = 0; this.env = DB_MAX; this.state = 0; this.out = 0; this.prev = 0; }
  }

  class VRC7 {
    constructor() {
      this.regs = new Uint8Array(0x40); this.addr = 0; this.mute = false; this.div = 0; this.level = 0;
      this.amPhase = 0; this.pmPhase = 0;
      this.ops = []; for (let i = 0; i < 12; i++) this.ops.push(new FMOp());
    }
    writeAddr(v) { this.addr = v & 0x3f; }
    writeData(v) {
      const r = this.addr, old = this.regs[r];
      this.regs[r] = v;
      if (r >= 0x20 && r <= 0x25) {
        const ch = r & 7;
        if ((v & 0x10) && !(old & 0x10)) { // key on: restart both operators
          for (const op of [this.ops[ch * 2], this.ops[ch * 2 + 1]]) { op.state = 1; op.phase = 0; }
        } else if (!(v & 0x10) && (old & 0x10)) {
          for (const op of [this.ops[ch * 2], this.ops[ch * 2 + 1]]) if (op.state) op.state = 4;
        }
      }
    }
    patch(ch) { const inst = this.regs[0x30 + ch] >> 4; return inst ? VRC7_PATCHES[inst] : this.regs; }
    // Advance one operator's envelope; returns its attenuation in dB.
    envelope(op, p, isCar, ch, fnum, block) {
      const flags = p[isCar], adr = p[4 + isCar], slrr = p[6 + isCar];
      const rks = flags & 0x10 ? (block << 1) | (fnum >> 8) : block >> 1;
      const rate = (R) => R ? Math.min(63, R * 4 + rks) : 0;
      switch (op.state) {
        case 1: { // attack
          const k = ATTACK[rate(adr >> 4)];
          op.env -= op.env * k + (k > 0 ? 0.001 : 0);
          if (op.env <= 0.05) { op.env = 0; op.state = 2; }
          break;
        }
        case 2: { // decay to the sustain level
          op.env += DECAY[rate(adr & 15)];
          const sl = (slrr >> 4) * 3;
          if (op.env >= sl) { op.env = sl; op.state = 3; }
          break;
        }
        case 3: // sustain: held for sustained instruments, otherwise keeps fading at the release rate
          if (!(flags & 0x20)) op.env += DECAY[rate(slrr & 15)];
          break;
        case 4: { // released
          const sus = this.regs[0x20 + ch] & 0x20;
          op.env += DECAY[rate(sus ? 5 : (flags & 0x20 ? slrr & 15 : 7))];
          break;
        }
      }
      if (op.env >= DB_MAX) { op.env = DB_MAX; if (op.state === 4) op.state = 0; }
      return op.env;
    }
    sample() {
      this.amPhase += 3.7 / FM_RATE; if (this.amPhase >= 1) this.amPhase -= 1;
      this.pmPhase += 6.4 / FM_RATE; if (this.pmPhase >= 1) this.pmPhase -= 1;
      const amDb = 4.8 * (1 + SIN[(this.amPhase * 4096) | 0]) / 2;
      const pm = 1 + 0.0081 * SIN[(this.pmPhase * 4096) | 0];
      let mix = 0;
      for (let ch = 0; ch < 6; ch++) {
        const mod = this.ops[ch * 2], car = this.ops[ch * 2 + 1];
        if (!mod.state && !car.state) continue;
        const r = this.regs, p = this.patch(ch);
        const fnum = r[0x10 + ch] | ((r[0x20 + ch] & 1) << 8), block = (r[0x20 + ch] >> 1) & 7;
        const base = fnum * Math.pow(2, block - 1) / 262144;
        const kslBase = Math.max(0, KSL_DB[fnum >> 5] - 6 * (7 - block));
        // modulator
        let inc = base * MULT[p[0] & 15]; if (p[0] & 0x40) inc *= pm;
        mod.phase = (mod.phase + inc) % 1;
        let att = this.envelope(mod, p, 0, ch, fnum, block) + (p[2] & 0x3f) * 0.75 + kslBase * [0, 0.25, 0.5, 1][p[2] >> 6];
        if (p[0] & 0x80) att += amDb;
        const fb = p[3] & 7;
        // feedback: the modulator's last two outputs bend its own phase by up to 4 pi (in cycles: 2)
        const fbShift = fb ? (mod.out + mod.prev) / 2 * Math.pow(2, fb - 6) : 0;
        let s = SIN[(((mod.phase + fbShift) % 1 + 1) * 4096 | 0) & 4095];
        if ((p[3] & 0x08) && s < 0) s = 0;
        mod.prev = mod.out; mod.out = s * amp(att);
        // carrier
        inc = base * MULT[p[1] & 15]; if (p[1] & 0x40) inc *= pm;
        car.phase = (car.phase + inc) % 1;
        att = this.envelope(car, p, 1, ch, fnum, block) + (r[0x30 + ch] & 15) * 3 + kslBase * [0, 0.25, 0.5, 1][p[3] >> 6];
        if (p[1] & 0x80) att += amDb;
        s = SIN[(((car.phase + mod.out * 2) % 1 + 1) * 4096 | 0) & 4095]; // modulation depth: up to 4 pi
        if ((p[3] & 0x10) && s < 0) s = 0;
        car.out = s * amp(att);
        mix += car.out;
      }
      return mix;
    }
    clock() {
      if (++this.div < 36) return;
      this.div = 0;
      this.level = this.mute ? 0 : this.sample() * 0.075;
    }
    output() { return this.level; }
  }

  global.RPNesAudio = { VRC6, VRC7, N163, S5B, MMC5Audio };
})(typeof window !== "undefined" ? window : globalThis);
