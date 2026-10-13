// RetroPixels — Game Boy / Game Boy Color emulator. Original code.
//
// SM83 CPU (timed per memory access), PPU with DMG and CGB modes, four-channel
// stereo APU, timers, serial port and OAM/HDMA transfers. The cartridge chips
// (memory bank controllers, clocks, sensors...) are in gb_carts.js.
// No boot ROM is used; the machine starts in the state the boot ROM leaves it in.
"use strict";

(function (global) {
  const FPS = 4194304 / 70224; // 59.7275

  // ===================================================================
  // Sound
  // ===================================================================
  const DUTY = [0x01, 0x81, 0x87, 0x7e];
  const NOISE_DIV = [8, 16, 32, 48, 64, 80, 96, 112];
  const READ_OR = [0x80, 0x3f, 0x00, 0xff, 0xbf, 0xff, 0x3f, 0x00, 0xff, 0xbf, 0x7f, 0xff, 0x9f, 0xff, 0xbf, 0xff, 0xff, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x70];
  class APU {
    constructor(sampleRate, gb) {
      this.gb = gb; // for the model (Game Boy or Color), which changes a few details
      this.sampleRate = sampleRate;
      this.regs = new Uint8Array(0x30);
      this.wave = new Uint8Array(16);
      this.on = true;
      this.ch = [0, 1, 2, 3].map(() => ({ on: false, dac: false, len: 0, lenOn: false, vol: 0, envDir: 0, envPeriod: 0, envTimer: 0,
        freq: 0, timer: 0, pos: 0, duty: 0, lfsr: 0x7fff, sweepPeriod: 0, sweepTimer: 0, sweepShift: 0, sweepNeg: false, sweepOn: false,
        shadow: 0, negUsed: false, sample: 0, fetched: 0 }));
      this.seqStep = 0; // the next frame-sequencer step (0-7), clocked by the divider
      this.acc = 0; this.step = 4194304 / sampleRate; this.sumL = 0; this.sumR = 0; this.n = 0;
      this.hpL = 0; this.hpR = 0; this.prevL = 0; this.prevR = 0;
      this.out = new Float32Array(8192); this.outLen = 0;
      this.now = 0; // T-cycles since start, for wave RAM access timing
    }
    get cgb() { return this.gb ? this.gb.cgb : false; }
    // While channel 3 plays, the CPU sees wave RAM only through the byte the channel is reading:
    // always on the Color, and on the original only in the same moment the channel reads it.
    waveIndex() {
      const c = this.ch[2];
      if (!c.on) return -1;
      if (this.cgb || this.now - c.fetched < 2) return c.pos >> 1;
      return -2;
    }
    read(a) {
      if (a >= 0xff30) { const i = this.waveIndex(); return i === -1 ? this.wave[a - 0xff30] : i === -2 ? 0xff : this.wave[i]; }
      const r = a - 0xff10;
      if (a === 0xff26) return 0x70 | (this.on ? 0x80 : 0) | this.ch.reduce((m, c, i) => m | (c.on ? 1 << i : 0), 0);
      if (r >= READ_OR.length) return 0xff;
      return this.regs[r] | READ_OR[r];
    }
    write(a, v) {
      if (a >= 0xff30) { const i = this.waveIndex(); if (i === -1) this.wave[a - 0xff30] = v; else if (i >= 0) this.wave[i] = v; return; }
      const r = a - 0xff10;
      if (a === 0xff26) {
        const on = !!(v & 0x80);
        if (!on && this.on) this.powerOff();
        if (on && !this.on) { this.seqStep = 0; for (const c of this.ch) { c.pos = 0; c.sample = 0; } }
        this.on = on; return;
      }
      if (r >= 0x17) return;
      if (!this.on) {
        // powered off: only the original Game Boy still accepts the length counters
        if (!this.cgb && (r === 1 || r === 6 || r === 11 || r === 16)) {
          const ch = (r / 5) | 0, c = this.ch[ch];
          if (ch === 2) c.len = 256 - v; else c.len = 64 - (v & 0x3f);
        }
        return;
      }
      this.regs[r] = v;
      const ch = (r / 5) | 0, sub = r % 5;
      if (ch > 3) return;
      const c = this.ch[ch];
      switch (sub) {
        case 0:
          if (ch === 0) {
            c.sweepPeriod = (v >> 4) & 7; c.sweepShift = v & 7;
            const neg = !!(v & 8);
            if (c.sweepNeg && !neg && c.negUsed) c.on = false; // leaving subtract mode after using it
            c.sweepNeg = neg;
          }
          if (ch === 2) { c.dac = !!(v & 0x80); if (!c.dac) c.on = false; }
          break;
        case 1:
          if (ch === 2) c.len = 256 - v; else { c.len = 64 - (v & 0x3f); c.duty = v >> 6; }
          break;
        case 2:
          if (ch !== 2) { c.dac = (v & 0xf8) !== 0; if (!c.dac) c.on = false; }
          break;
        case 3: if (ch !== 3) c.freq = (c.freq & 0x700) | v; break;
        case 4: {
          if (ch !== 3) c.freq = (c.freq & 0xff) | ((v & 7) << 8);
          const wasOn = c.lenOn, nowOn = !!(v & 0x40);
          c.lenOn = nowOn;
          // Turning the length counter on in the first half of a length period clocks it once.
          const extra = (this.seqStep & 1) === 1;
          if (extra && nowOn && !wasOn && c.len > 0) { if (--c.len === 0 && !(v & 0x80)) c.on = false; }
          if (v & 0x80) this.trigger(ch, extra && nowOn);
          break;
        }
      }
    }
    powerOff() {
      for (let i = 0; i < 0x16; i++) this.regs[i] = 0;
      for (const c of this.ch) {
        Object.assign(c, { on: false, dac: false, lenOn: false, vol: 0, envDir: 0, envPeriod: 0, freq: 0, duty: 0,
          sweepPeriod: 0, sweepShift: 0, sweepNeg: false, sweepOn: false });
        if (this.cgb) c.len = 0; // the original keeps its length counters
      }
    }
    trigger(i, extraClock) {
      const c = this.ch[i], R = this.regs, base = i * 5;
      // DMG: restarting channel 3 just as it reads wave RAM scrambles the first bytes
      if (i === 2 && c.on && !this.cgb && c.timer <= 2) {
        const next = ((c.pos + 1) & 31) >> 1;
        if (next < 4) this.wave[0] = this.wave[next];
        else { const b = next & ~3; for (let k = 0; k < 4; k++) this.wave[k] = this.wave[b + k]; }
      }
      c.on = c.dac;
      if (!c.len) { c.len = i === 2 ? 256 : 64; if (extraClock) c.len--; }
      if (i === 2) { c.timer = (2048 - c.freq) * 2 + 6; c.pos = 0; }
      else if (i === 3) {
        const v = R[base + 3];
        c.timer = NOISE_DIV[v & 7] << (v >> 4); c.lfsr = 0x7fff;
      } else c.timer = (2048 - c.freq) * 4 + (c.timer & 3);
      if (i !== 2) { const e = R[base + 2]; c.vol = e >> 4; c.envDir = e & 8 ? 1 : -1; c.envPeriod = e & 7; c.envTimer = c.envPeriod || 8; }
      if (i === 0) {
        c.shadow = c.freq; c.sweepTimer = c.sweepPeriod || 8; c.sweepOn = !!(c.sweepPeriod || c.sweepShift); c.negUsed = false;
        if (c.sweepShift && this.sweepCalc(c) > 2047) c.on = false;
      }
    }
    sweepCalc(c) {
      const d = c.shadow >> c.sweepShift;
      if (c.sweepNeg) { c.negUsed = true; return c.shadow - d; }
      return c.shadow + d;
    }
    // frame sequencer: 512 Hz, from the divider
    clockSeq() {
      if (!this.on) return;
      const s = this.seqStep;
      if ((s & 1) === 0) for (const c of this.ch) if (c.lenOn && c.len > 0 && --c.len === 0) c.on = false;
      if (s === 2 || s === 6) {
        const c = this.ch[0];
        if (--c.sweepTimer <= 0) {
          c.sweepTimer = c.sweepPeriod || 8;
          if (c.sweepOn && c.sweepPeriod) {
            const f = this.sweepCalc(c);
            if (f > 2047) c.on = false;
            else if (c.sweepShift) { c.shadow = f; c.freq = f; if (this.sweepCalc(c) > 2047) c.on = false; }
          }
        }
      }
      if (s === 7) for (const i of [0, 1, 3]) {
        const c = this.ch[i];
        if (c.envPeriod && --c.envTimer <= 0) { c.envTimer = c.envPeriod; const nv = c.vol + c.envDir; if (nv >= 0 && nv <= 15) c.vol = nv; }
      }
      this.seqStep = (s + 1) & 7;
    }
    // advance n T-cycles (4.19 MHz)
    tick(n) {
      const t0 = this.now;
      this.now += n;
      if (!this.on) { this.mix(n, 0, 0); return; }
      for (let i = 0; i < 4; i++) {
        const c = this.ch[i];
        c.timer -= n;
        while (c.timer <= 0) {
          if (i === 2) {
            const at = this.now + c.timer; // when this read happened
            c.timer += (2048 - c.freq) * 2; c.pos = (c.pos + 1) & 31;
            const b = this.wave[c.pos >> 1]; c.sample = c.pos & 1 ? b & 15 : b >> 4; c.fetched = at > t0 ? at : t0;
          } else if (i === 3) {
            const v = this.regs[3 * 5 + 3];
            c.timer += NOISE_DIV[v & 7] << (v >> 4);
            const x = (c.lfsr ^ (c.lfsr >> 1)) & 1;
            c.lfsr = (c.lfsr >> 1) | (x << 14);
            if (v & 8) c.lfsr = (c.lfsr & ~0x40) | (x << 6);
          } else { c.timer += (2048 - c.freq) * 4; c.pos = (c.pos + 1) & 7; }
        }
      }
      // channel outputs (0..15) -> DAC (-1..1)
      const c0 = this.ch[0], c1 = this.ch[1], c2 = this.ch[2], c3 = this.ch[3];
      const o0 = c0.on && (DUTY[c0.duty] >> (7 - c0.pos)) & 1 ? c0.vol : 0;
      const o1 = c1.on && (DUTY[c1.duty] >> (7 - c1.pos)) & 1 ? c1.vol : 0;
      let o2 = 0;
      if (c2.on) { const vc = (this.regs[2 * 5 + 2] >> 5) & 3; o2 = vc ? c2.sample >> (vc - 1) : 0; }
      const o3 = c3.on && !(c3.lfsr & 1) ? c3.vol : 0;
      const pan = this.regs[0x15], vol = this.regs[0x14];
      let L = 0, R = 0;
      const os = [o0, o1, o2, o3];
      for (let i = 0; i < 4; i++) {
        const dac = this.ch[i].dac ? os[i] / 7.5 - 1 : 0;
        if (pan & (0x10 << i)) L += dac;
        if (pan & (1 << i)) R += dac;
      }
      L *= (((vol >> 4) & 7) + 1) / 8; R *= ((vol & 7) + 1) / 8;
      this.mix(n, L / 4, R / 4);
    }
    mix(n, L, R) {
      this.sumL += L * n; this.sumR += R * n; this.n += n; this.acc += n;
      while (this.acc >= this.step) {
        this.acc -= this.step;
        const l = this.n ? this.sumL / this.n : 0, r = this.n ? this.sumR / this.n : 0;
        this.sumL = this.sumR = 0; this.n = 0;
        this.hpL = 0.996 * (this.hpL + l - this.prevL); this.prevL = l;
        this.hpR = 0.996 * (this.hpR + r - this.prevR); this.prevR = r;
        if (this.outLen < this.out.length - 1) { this.out[this.outLen++] = this.hpL * 0.6; this.out[this.outLen++] = this.hpR * 0.6; }
      }
    }
    saveState() { return { regs: this.regs, wave: this.wave, on: this.on, ch: this.ch.map(c => Object.assign({}, c)), seqStep: this.seqStep, now: this.now }; }
    loadState(o) { this.regs.set(o.regs); this.wave.set(o.wave); this.on = o.on; o.ch.forEach((c, i) => Object.assign(this.ch[i], c)); this.seqStep = o.seqStep; this.now = o.now || 0; }
  }

  // ===================================================================
  // The machine (CPU + memory + PPU + timers)
  // ===================================================================
  const DMG_PALETTES = {
    green: [0xff0fbc9b, 0xff0fac8b, 0xff306230, 0xff0f380f].map((x) => x), // ABGR
    gray: [0xffffffff, 0xffaaaaaa, 0xff555555, 0xff000000],
  };
  // ABGR from RGB helpers
  const abgr = (r, g, b) => 0xff000000 | (b << 16) | (g << 8) | r;
  DMG_PALETTES.green = [abgr(0xe0, 0xf0, 0xc8), abgr(0x98, 0xc0, 0x78), abgr(0x48, 0x70, 0x50), abgr(0x10, 0x24, 0x20)];
  DMG_PALETTES.gray = [abgr(0xf4, 0xf4, 0xf4), abgr(0xaa, 0xaa, 0xaa), abgr(0x55, 0x55, 0x55), abgr(0x10, 0x10, 0x10)];

  class GB {
    constructor(rom, opts = {}) {
      if (rom.length < 0x150) throw new Error("This isn't a Game Boy game file");
      this.clock = 0; // real time in 4.19 MHz clocks (for cartridge clocks and the camera)
      this.tilt = { x: 0, y: 0 }; this.stick = { x: 0, y: 0 }; this.tiltKeys = 0;
      this.cart = global.RPGBCarts.create(rom, this);
      this.cgb = this.cart.cgb;
      this.battery = this.cart.battery;
      if (opts.sram && this.battery) this.cart.loadSram(opts.sram);
      this.dmgPalette = DMG_PALETTES[opts.palette || "green"];
      this.vram = new Uint8Array(0x4000); this.vbank = 0;
      this.wram = new Uint8Array(0x8000); this.wbank = 1;
      this.oam = new Uint8Array(0xa0); this.hram = new Uint8Array(0x7f); this.io = new Uint8Array(0x80);
      this.bgPal = new Uint8Array(64); this.obPal = new Uint8Array(64); this.bcps = 0; this.ocps = 0;
      this.ie = 0; this.if = 0xe1;
      this.apu = new APU(opts.sampleRate || 48000, this);
      this.screen = new Uint32Array(160 * 144);
      this.width = 160; this.height = 144; this.fps = FPS;
      this.keys = 0; // bit set = pressed: A B Select Start Right Left Up Down (GB order)
      this.serialOut = ""; this.serialBits = 0; this.serialTimer = 0;
      this.reset();
    }

    reset() {
      // Registers as left by the boot ROM.
      if (this.cgb) { this.a = 0x11; this.f = 0x80; this.b = 0x00; this.c = 0x00; this.d = 0xff; this.e = 0x56; this.h = 0x00; this.l = 0x0d; }
      else { this.a = 0x01; this.f = 0xb0; this.b = 0x00; this.c = 0x13; this.d = 0x00; this.e = 0xd8; this.h = 0x01; this.l = 0x4d; }
      this.sp = 0xfffe; this.pc = 0x100;
      this.ime = false; this.eiDelay = 0; this.halted = false; this.haltBug = false; this.stopped = false;
      this.div = this.cgb ? 0x1ea0 : 0xabc8; this.tima = 0; this.tma = 0; this.tac = 0xf8; this.timaReload = 0;
      this.ly = 0; this.lineDot = 0; this.mode = 2; this.statLine = false; this.winLine = 0; this.frameDone = false;
      this.lyReg = 0; this.lyCmp = 0; this.m3end = 256; this.firstLine = false; this.oamLock = false; this.vramLock = false; this.oamWLock = false; this.vramWLock = false; this.lycFlag = false;
      this.double = false; this.key1 = 0;
      this.hdmaSrc = 0; this.hdmaDst = 0; this.hdmaLen = 0; this.hdmaActive = false;
      this.cycles = 0;
      this.dmaActive = false; this.dmaDelay = 0; this.dmaIdx = 0; this.dmaBase = 0; this.dmaReq = 0; this.dmaByte = 0xff;
      const io = this.io;
      io.fill(0);
      this.joyp = 0xcf;
      io[0x40] = 0x91; io[0x41] = 0x85; io[0x47] = 0xfc; io[0x48] = 0xff; io[0x49] = 0xff;
      // APU post-boot values
      const apuInit = [[0xff10, 0x80], [0xff11, 0xbf], [0xff12, 0xf3], [0xff14, 0xbf], [0xff16, 0x3f], [0xff17, 0x00], [0xff19, 0xbf],
        [0xff1a, 0x7f], [0xff1b, 0xff], [0xff1c, 0x9f], [0xff1e, 0xbf], [0xff20, 0xff], [0xff21, 0x00], [0xff22, 0x00], [0xff23, 0xbf],
        [0xff24, 0x77], [0xff25, 0xf3]];
      this.apu.powerOff(); this.apu.on = true; this.apu.seqStep = 0;
      for (const [a, v] of apuInit) { const r = a - 0xff10; if (r % 5 === 4 && r < 0x14) this.apu.regs[r] = v; else this.apu.write(a, v); } // no triggers
      this.apu.ch[0].on = true; this.apu.ch[0].vol = 0; // the start-up chime's channel is left on, silent
      if (this.cgb) for (let i = 0; i < 64; i++) this.bgPal[i] = 0xff;
    }

    // ---- timing: every memory access costs one M-cycle (4 T-cycles) ----
    tick() {
      const t = this.double ? 2 : 4; // PPU/APU see half as much time per M-cycle at double speed
      this.cycles += 4; this.clock += t;
      if (this.dmaActive || this.dmaDelay) this.oamDmaStep();
      this.timerTick();
      const ifBefore = this.if;
      this.ppuTick(t);
      this.ppuNewIf = this.if & ~ifBefore;
      this.apu.tick(t);
    }
    // DIV is a 16-bit counter; TIMA counts falling edges of one of its bits.
    timerTick() {
      // after TIMA overflows it reads 0 for one M-cycle, then TMA is loaded
      this.timaJustLoaded = false;
      if (this.timaReload) {
        if (--this.timaReload === 0) { this.tima = this.tma; this.if |= 4; this.timaJustLoaded = true; }
      }
      const old = this.div;
      this.div = (this.div + 4) & 0xffff;
      this.timerEdge(old, this.div);
      this.apuEdge(old, this.div);
      // serial port with its own clock: one bit per falling edge of a divider bit (8192 Hz, or 262144 Hz on Color)
      if (this.serialBits) {
        const bit = this.cgb && (this.io[2] & 2) ? 3 : 8;
        if (((old >> bit) & 1) && !((this.div >> bit) & 1) && --this.serialBits === 0) { this.io[1] = 0xff; this.io[2] &= 0x7f; this.if |= 8; }
      }
    }
    // the sound frame sequencer steps on falling edges of divider bit 12 (bit 13 at double speed)
    apuEdge(old, now) { const b = this.double ? 13 : 12; if (((old >> b) & 1) && !((now >> b) & 1)) this.apu.clockSeq(); }
    timerBit() { return [9, 3, 5, 7][this.tac & 3]; }
    timerEdge(old, now) {
      if (!(this.tac & 4)) return;
      const bit = this.timerBit();
      if (((old >> bit) & 1) && !((now >> bit) & 1)) this.incTima();
    }
    incTima() {
      this.tima = (this.tima + 1) & 0xff;
      if (this.tima === 0) this.timaReload = 1;
    }
    rd(a) { this.tick(); return this.dmaActive && a < 0xff00 ? this.dmaBusRead(a) : this.read(a); }
    wr(a, v) { this.tick(); if (this.dmaActive && a < 0xff00 && this.dmaBusBlocked(a)) return; this.write(a, v); }
    // ---- OAM DMA: one byte per M-cycle after a cycle of set-up; meanwhile the CPU
    // can't use the bus the copy reads from (it sees the byte being copied), nor OAM.
    oamDmaStep() {
      if (this.dmaDelay && --this.dmaDelay === 0) { this.dmaActive = true; this.dmaIdx = 0; this.dmaBase = this.dmaReq; }
      if (this.dmaActive) {
        if (this.dmaIdx >= 160) this.dmaActive = false;
        else { const a = this.dmaBase + this.dmaIdx; this.dmaByte = this.dmaRead(a >= 0xe000 ? a - 0x2000 : a); this.oam[this.dmaIdx++] = this.dmaByte; }
      }
    }
    dmaRead(a) {
      if (a < 0x8000) return this.cart.romRead(a);
      if (a < 0xa000) return this.vram[this.vbank * 0x2000 + (a & 0x1fff)];
      if (a < 0xc000) return this.cart.ramRead(a);
      if (a < 0xd000) return this.wram[a & 0xfff];
      return this.wram[this.wbank * 0x1000 + (a & 0xfff)];
    }
    // the copy's source is on the video bus (8000-9FFF) or the cartridge / work RAM bus
    dmaBusBlocked(a) {
      if (a >= 0xfe00) return true;
      const srcVram = this.dmaBase >= 0x8000 && this.dmaBase < 0xa000, isVram = a >= 0x8000 && a < 0xa000;
      if (srcVram !== isVram) return false;
      // the Color model has work RAM on a bus of its own
      if (this.cgb && !srcVram && ((a >= 0xc000) !== (this.dmaBase >= 0xc000))) return false;
      return true;
    }
    dmaBusRead(a) { return !this.dmaBusBlocked(a) ? this.read(a) : a >= 0xfe00 ? 0xff : this.dmaByte; }
    idle() { this.tick(); }

    read(a) {
      if (a < 0x8000) return this.cart.romRead(a);
      if (a < 0xa000) return this.vramLock ? 0xff : this.vram[this.vbank * 0x2000 + (a & 0x1fff)];
      if (a < 0xc000) return this.cart.ramRead(a);
      if (a < 0xd000) return this.wram[a & 0xfff];
      if (a < 0xe000) return this.wram[this.wbank * 0x1000 + (a & 0xfff)];
      if (a < 0xfe00) return this.read(a - 0x2000);
      if (a < 0xfea0) return this.oamLock ? 0xff : this.oam[a - 0xfe00];
      if (a < 0xff00) return 0xff;
      if (a < 0xff80) return this.ioRead(a);
      if (a < 0xffff) return this.hram[a - 0xff80];
      return this.ie;
    }
    write(a, v) {
      if (a < 0x8000) { this.cart.romWrite(a, v); return; }
      if (a < 0xa000) { if (!this.vramWLock) this.vram[this.vbank * 0x2000 + (a & 0x1fff)] = v; return; }
      if (a < 0xc000) { this.cart.ramWrite(a, v); return; }
      if (a < 0xd000) { this.wram[a & 0xfff] = v; return; }
      if (a < 0xe000) { this.wram[this.wbank * 0x1000 + (a & 0xfff)] = v; return; }
      if (a < 0xfe00) { this.write(a - 0x2000, v); return; }
      if (a < 0xfea0) { if (!this.oamWLock) this.oam[a - 0xfe00] = v; return; }
      if (a < 0xff00) return;
      if (a < 0xff80) { this.ioWrite(a, v); return; }
      if (a < 0xffff) { this.hram[a - 0xff80] = v; return; }
      this.ie = v;
    }
    lcdOn() { return (this.io[0x40] & 0x80) !== 0; }

    ioRead(a) {
      const r = a & 0x7f;
      switch (r) {
        case 0x00: {
          let v = 0xcf | (this.joyp & 0x30);
          if (!(this.joyp & 0x10)) v &= ~((this.keys >> 4) & 15);   // directions
          if (!(this.joyp & 0x20)) v &= ~(this.keys & 15);           // buttons
          return v;
        }
        case 0x01: return this.io[1];
        case 0x02: return this.io[2] | (this.cgb ? 0x7c : 0x7e);
        case 0x04: return this.div >> 8;
        case 0x05: return this.tima;
        case 0x06: return this.tma;
        case 0x07: return this.tac | 0xf8;
        case 0x0f: return this.if | 0xe0;
        case 0x41: return 0x80 | (this.io[0x41] & 0x78) | (this.lycFlag ? 4 : 0) | (this.lcdOn() ? this.mode : 0);
        case 0x44: return this.lyReg;
        case 0x4d: return this.cgb ? (this.double ? 0x80 : 0) | (this.key1 & 1) | 0x7e : 0xff;
        case 0x4f: return this.cgb ? 0xfe | this.vbank : 0xff;
        case 0x55: return this.cgb ? (this.hdmaActive ? ((this.hdmaLen - 1) & 0x7f) : 0xff) : 0xff;
        case 0x68: return this.cgb ? this.bcps | 0x40 : 0xff;
        case 0x69: return this.cgb ? this.bgPal[this.bcps & 0x3f] : 0xff;
        case 0x6a: return this.cgb ? this.ocps | 0x40 : 0xff;
        case 0x6b: return this.cgb ? this.obPal[this.ocps & 0x3f] : 0xff;
        case 0x70: return this.cgb ? 0xf8 | this.wbank : 0xff;
      }
      if (r >= 0x10 && r < 0x40) return this.apu.read(a);
      if (r >= 0x40 && r <= 0x4b) return this.io[r];
      return 0xff;
    }
    ioWrite(a, v) {
      const r = a & 0x7f;
      switch (r) {
        case 0x00: this.joyp = v & 0x30; return;
        case 0x01: this.io[1] = v; return;
        case 0x02:
          this.io[2] = v;
          if ((v & 0x81) === 0x81) { this.serialOut += String.fromCharCode(this.io[1]); this.serialBits = 8; } else this.serialBits = 0;
          return;
        case 0x04: { const old = this.div; this.div = 0; this.timerEdge(old, 0); this.apuEdge(old, 0); return; }
        case 0x05: if (!this.timaJustLoaded) { this.tima = v; this.timaReload = 0; } return; // a write in the 0 cycle cancels the reload
        case 0x06: this.tma = v; if (this.timaJustLoaded) this.tima = v; return;
        case 0x07: {
          // changing the timer setting can itself produce a falling edge
          const oldBit = (this.tac & 4) && ((this.div >> this.timerBit()) & 1);
          this.tac = v & 7;
          const newBit = (this.tac & 4) && ((this.div >> this.timerBit()) & 1);
          if (oldBit && !newBit) this.incTima();
          return;
        }
        case 0x0f: this.if = v & 0x1f; return;
        case 0x40: {
          const was = this.lcdOn();
          this.io[0x40] = v;
          if (was && !(v & 0x80)) {
            this.ly = this.lyReg = this.lyCmp = 0; this.lineDot = 0; this.mode = 0; this.oamLock = this.vramLock = this.oamWLock = this.vramWLock = false;
            this.screen.fill(this.cgb ? 0xffffffff : this.dmgPalette[0]);
          }
          // Switched on: line 0 starts in mode 0 (no OAM scan) and runs a little late.
          if (!was && (v & 0x80)) { this.ly = this.lyReg = this.lyCmp = 0; this.lineDot = 4; this.mode = 0; this.winLine = 0; this.firstLine = true; this.checkStat(); }
          return;
        }
        case 0x41: this.io[0x41] = v & 0x78; this.checkStat(); return;
        case 0x44: return;
        case 0x45: this.io[0x45] = v; this.checkStat(); return;
        case 0x46: // OAM DMA: the first byte is copied two M-cycles later
          this.dmaReq = v << 8; this.dmaDelay = 2;
          this.io[0x46] = v; return;
        case 0x4d: if (this.cgb) this.key1 = v & 1; return;
        case 0x4f: if (this.cgb) this.vbank = v & 1; return;
        case 0x51: this.hdmaSrc = (this.hdmaSrc & 0xff) | (v << 8); return;
        case 0x52: this.hdmaSrc = (this.hdmaSrc & 0xff00) | (v & 0xf0); return;
        case 0x53: this.hdmaDst = (this.hdmaDst & 0xff) | ((v & 0x1f) << 8); return;
        case 0x54: this.hdmaDst = (this.hdmaDst & 0xff00) | (v & 0xf0); return;
        case 0x55: {
          if (!this.cgb) return;
          if (this.hdmaActive && !(v & 0x80)) { this.hdmaActive = false; return; }
          this.hdmaLen = (v & 0x7f) + 1;
          if (v & 0x80) this.hdmaActive = true;
          else { while (this.hdmaLen > 0) this.hdmaBlock(); } // general-purpose DMA: all at once
          return;
        }
        case 0x68: this.bcps = v & 0xbf; return;
        case 0x69: this.bgPal[this.bcps & 0x3f] = v; if (this.bcps & 0x80) this.bcps = 0x80 | ((this.bcps + 1) & 0x3f); return;
        case 0x6a: this.ocps = v & 0xbf; return;
        case 0x6b: this.obPal[this.ocps & 0x3f] = v; if (this.ocps & 0x80) this.ocps = 0x80 | ((this.ocps + 1) & 0x3f); return;
        case 0x70: if (this.cgb) this.wbank = (v & 7) || 1; return;
      }
      if (r >= 0x10 && r < 0x40) { this.apu.write(a, v); return; }
      if (r >= 0x40 && r <= 0x4b) this.io[r] = v;
    }
    hdmaBlock() {
      for (let i = 0; i < 16; i++) {
        this.vram[this.vbank * 0x2000 + ((this.hdmaDst + i) & 0x1fff)] = this.read(this.hdmaSrc + i);
      }
      this.hdmaSrc = (this.hdmaSrc + 16) & 0xffff; this.hdmaDst = (this.hdmaDst + 16) & 0x1fff;
      if (--this.hdmaLen <= 0) { this.hdmaActive = false; this.hdmaLen = 0; }
      // the CPU is paused while a block is copied
      for (let i = 0; i < (this.double ? 16 : 8); i++) this.tick();
    }

    // ---- PPU ----
    // The STAT interrupt line is the OR of the enabled sources; an interrupt is requested on its rising edge.
    checkStat() {
      // With the screen off nothing changes: the LY=LYC flag and the interrupt line keep their state.
      if (!this.lcdOn()) return;
      const st = this.io[0x41], d = this.lineDot, ly = this.ly;
      this.lycFlag = this.lyCmp === this.io[0x45];
      let line = false;
      {
        const vis = ly < 144;
        line = ((st & 0x40) && this.lycFlag) ||
          ((st & 0x08) && vis && (this.mode === 0 || (this.mode === 3 && d >= this.m3end - 4))) ||
          ((st & 0x10) && !vis) ||
          ((st & 0x20) && this.mode2Source(ly, d));
      }
      if (line && !this.statLine) this.if |= 2;
      this.statLine = !!line;
    }
    // The mode 2 interrupt source: from LY's change to the end of the OAM scan, plus a
    // short pulse at the start of line 144 (together with the vertical blank interrupt).
    mode2Source(ly, d) {
      if (this.firstLine) return false;
      return ly < 144 ? d < 84 : ly === 144 && d < 4;
    }
    // Timing of a visible line, in dots from the moment LY changes:
    //   0-3 LY changes (STAT still shows mode 0, OAM already locked), 4-83 mode 2 (VRAM locked from 80),
    //   84 mode 3, which ends at 256 + SCX&7 + objects + window, then mode 0 until 456.
    ppuTick(t) {
      if (!this.lcdOn()) {
        this.lcdOffDots = (this.lcdOffDots || 0) + t;
        if (this.lcdOffDots >= 70224) { this.lcdOffDots -= 70224; this.frameDone = true; }
        return;
      }
      this.lineDot += t;
      if (this.lineDot >= 456) {
        this.lineDot -= 456;
        this.firstLine = false;
        if (++this.ly > 153) { this.ly = 0; this.winLine = 0; }
      }
      const d = this.lineDot, ly = this.ly;
      if (ly < 144) {
        let mode;
        if (this.firstLine) mode = d < 84 ? 0 : d < this.m3end ? 3 : 0;
        else mode = d < 4 ? 0 : d < 84 ? 2 : d < this.m3end ? 3 : 0;
        if (mode === 3 && this.mode !== 3) this.m3end = this.mode3Length();
        if (mode === 0 && this.mode === 3) {
          this.renderLine();
          if (this.hdmaActive) this.hdmaPending = true;
        }
        this.mode = mode;
        this.oamLock = this.firstLine ? (d >= 84 && d < this.m3end) : d < this.m3end;
        this.vramLock = d >= (this.firstLine ? 84 : 80) && d < this.m3end;
        // writes are refused a little later than reads
        this.oamWLock = this.firstLine ? this.oamLock : (d >= 4 && d < 80) || (d >= 84 && d < this.m3end);
        this.vramWLock = d >= 84 && d < this.m3end;
        this.lyReg = ly;
        this.lyCmp = d < 4 && !this.firstLine ? -1 : ly;
        if (mode === 3 && d >= this.m3end) this.m3end = 456;
      } else {
        if (ly === 144 && d < 4) {
          if (this.lyReg !== 144) { this.if |= 1; this.frameDone = true; } // vertical blank begins
          this.lyReg = 144; this.lyCmp = -1; this.mode = 0;
        } else {
          this.mode = 1;
          // line 153 reports LY = 0 after its first M-cycle
          if (ly === 153) { this.lyReg = d < 4 ? 153 : 0; this.lyCmp = d < 4 ? -1 : d < 8 ? 153 : 0; }
          else { this.lyReg = ly; this.lyCmp = d < 4 ? -1 : ly; }
        }
        this.oamLock = this.vramLock = this.oamWLock = this.vramWLock = false;
      }
      this.checkStat();
    }
    // Mode 3 length in dots (from dot 0 of the line to the start of mode 0)
    mode3Length() {
      const lcdc = this.io[0x40], scx = this.io[0x43] & 7;
      let len = 256 + scx;
      // window: the fetcher restarts when it reaches the window
      const wx = this.io[0x4b];
      if ((lcdc & 0x20) && this.ly >= this.io[0x4a] && wx < 167) len += 6;
      if (lcdc & 2) {
        const h = lcdc & 4 ? 16 : 8, xs = [];
        for (let i = 0; i < 40 && xs.length < 10; i++) {
          const sy = this.oam[i * 4] - 16;
          if (this.ly >= sy && this.ly < sy + h) xs.push(this.oam[i * 4 + 1]);
        }
        xs.sort((a, b) => a - b);
        // each object costs 6 dots, plus a wait for the background fetch of the first object in a tile
        const seen = new Set();
        let pen = 0;
        for (const x of xs) {
          if (x >= 168) continue;
          const px = x + scx, tile = px >> 3;
          pen += 6;
          if (!seen.has(tile)) { seen.add(tile); pen += Math.max(0, 5 - (px & 7)); }
        }
        if (pen) len += pen - 3; // the last fetch overlaps the end of the line
      }
      return len;
    }

    color(palIdx, c, obj) { // CGB 15-bit colour -> ABGR (with gentle correction)
      const p = obj ? this.obPal : this.bgPal, i = palIdx * 8 + c * 2;
      const v = p[i] | (p[i + 1] << 8);
      const r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
      const R = (r * 26 + g * 4 + b * 2) >> 2, G = (g * 24 + b * 8) >> 2, B = (r * 6 + g * 4 + b * 22) >> 2;
      return abgr(Math.min(255, R), Math.min(255, G), Math.min(255, B));
    }

    renderLine() {
      const y = this.ly, lcdc = this.io[0x40], cgb = this.cgb;
      const row = y * 160, scr = this.screen;
      const bgIdx = this.lineBg || (this.lineBg = new Uint8Array(160));   // colour index 0-3 for priority
      const bgPri = this.lineBgPri || (this.lineBgPri = new Uint8Array(160));
      const scx = this.io[0x43], scy = this.io[0x42], wx = this.io[0x4b] - 7, wy = this.io[0x4a];
      const bgp = this.io[0x47];
      const bgOn = cgb || (lcdc & 1);
      const winOn = (lcdc & 0x20) && (cgb || (lcdc & 1)) && y >= wy && wx < 160;
      const tileData = lcdc & 0x10 ? 0x0000 : 0x0800;
      const signed = !(lcdc & 0x10);
      let usedWin = false;
      for (let x = 0; x < 160; x++) {
        let mapBase, px, py;
        const inWin = winOn && x >= wx;
        if (inWin) { mapBase = lcdc & 0x40 ? 0x1c00 : 0x1800; px = x - wx; py = this.winLine; usedWin = true; }
        else if (bgOn) { mapBase = lcdc & 0x08 ? 0x1c00 : 0x1800; px = (x + scx) & 255; py = (y + scy) & 255; }
        else { bgIdx[x] = 0; bgPri[x] = 0; scr[row + x] = cgb ? 0xffffffff : this.dmgPalette[bgp & 3]; continue; }
        const mapAddr = mapBase + ((py >> 3) << 5) + (px >> 3);
        const tile = this.vram[mapAddr];
        const attr = cgb ? this.vram[0x2000 + mapAddr] : 0;
        let ty = py & 7; if (attr & 0x40) ty = 7 - ty;
        let tx = px & 7; if (attr & 0x20) tx = 7 - tx;
        const t = signed ? 0x1000 + ((tile << 24) >> 24) * 16 : tile * 16;
        const bank = attr & 8 ? 0x2000 : 0;
        const lo = this.vram[bank + t + ty * 2], hi = this.vram[bank + t + ty * 2 + 1];
        const c = ((lo >> (7 - tx)) & 1) | (((hi >> (7 - tx)) & 1) << 1);
        bgIdx[x] = c; bgPri[x] = attr & 0x80 ? 1 : 0;
        scr[row + x] = cgb ? this.color(attr & 7, c, false) : this.dmgPalette[(bgp >> (c * 2)) & 3];
        void tileData;
      }
      if (usedWin) this.winLine++;
      if (lcdc & 2) this.renderSprites(y, row, bgIdx, bgPri);
    }
    renderSprites(y, row, bgIdx, bgPri) {
      const lcdc = this.io[0x40], h = lcdc & 4 ? 16 : 8, cgb = this.cgb, scr = this.screen;
      const list = [];
      for (let i = 0; i < 40 && list.length < 10; i++) {
        const sy = this.oam[i * 4] - 16;
        if (y >= sy && y < sy + h) list.push(i);
      }
      // Priority: CGB by OAM order; DMG by X position, then OAM order.
      if (!cgb) list.sort((a, b) => (this.oam[a * 4 + 1] - this.oam[b * 4 + 1]) || (a - b));
      const drawn = this.spriteDrawn || (this.spriteDrawn = new Uint8Array(160));
      drawn.fill(0);
      for (const i of list) {
        const sy = this.oam[i * 4] - 16, sx = this.oam[i * 4 + 1] - 8, attr = this.oam[i * 4 + 3];
        let tile = this.oam[i * 4 + 2]; if (h === 16) tile &= 0xfe;
        let ty = y - sy; if (attr & 0x40) ty = h - 1 - ty;
        const bank = cgb && (attr & 8) ? 0x2000 : 0;
        const addr = bank + tile * 16 + ty * 2;
        const lo = this.vram[addr], hi = this.vram[addr + 1];
        for (let px = 0; px < 8; px++) {
          const x = sx + px;
          if (x < 0 || x >= 160 || drawn[x]) continue;
          const bit = attr & 0x20 ? px : 7 - px;
          const c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
          if (!c) continue;
          drawn[x] = 1;
          // behind-background flags (and the CGB master priority bit)
          if (bgIdx[x] && (cgb ? (lcdc & 1) && ((attr & 0x80) || bgPri[x]) : (attr & 0x80))) continue;
          scr[row + x] = cgb ? this.color(attr & 7, c, true) : this.dmgPalette[(this.io[attr & 0x10 ? 0x49 : 0x48] >> (c * 2)) & 3];
        }
      }
    }

    // ---- CPU (SM83) ----
    get af() { return (this.a << 8) | this.f; }
    get bc() { return (this.b << 8) | this.c; } set bc(v) { this.b = (v >> 8) & 255; this.c = v & 255; }
    get de() { return (this.d << 8) | this.e; } set de(v) { this.d = (v >> 8) & 255; this.e = v & 255; }
    get hl() { return (this.h << 8) | this.l; } set hl(v) { this.h = (v >> 8) & 255; this.l = v & 255; }
    fetch() { const v = this.rd(this.pc); this.pc = (this.pc + 1) & 0xffff; return v; }
    fetch16() { const lo = this.fetch(); return lo | (this.fetch() << 8); }
    push16(v) { this.idle(); this.sp = (this.sp - 1) & 0xffff; this.wr(this.sp, v >> 8); this.sp = (this.sp - 1) & 0xffff; this.wr(this.sp, v & 255); }
    pop16() { const lo = this.rd(this.sp); this.sp = (this.sp + 1) & 0xffff; const hi = this.rd(this.sp); this.sp = (this.sp + 1) & 0xffff; return lo | (hi << 8); }
    getR(i) {
      switch (i) { case 0: return this.b; case 1: return this.c; case 2: return this.d; case 3: return this.e; case 4: return this.h; case 5: return this.l; case 6: return this.rd(this.hl); default: return this.a; }
    }
    setR(i, v) {
      v &= 255;
      switch (i) { case 0: this.b = v; break; case 1: this.c = v; break; case 2: this.d = v; break; case 3: this.e = v; break; case 4: this.h = v; break; case 5: this.l = v; break; case 6: this.wr(this.hl, v); break; default: this.a = v; }
    }
    flags(z, n, h, c) { this.f = (z ? 0x80 : 0) | (n ? 0x40 : 0) | (h ? 0x20 : 0) | (c ? 0x10 : 0); }
    get fz() { return (this.f & 0x80) !== 0; } get fc() { return (this.f & 0x10) !== 0; }
    alu(op, v) {
      const a = this.a, c = this.fc ? 1 : 0;
      let r;
      switch (op) {
        case 0: r = a + v; this.flags(!(r & 255), 0, ((a & 15) + (v & 15)) > 15, r > 255); this.a = r & 255; break;             // ADD
        case 1: r = a + v + c; this.flags(!(r & 255), 0, ((a & 15) + (v & 15) + c) > 15, r > 255); this.a = r & 255; break;   // ADC
        case 2: r = a - v; this.flags(!(r & 255), 1, (a & 15) < (v & 15), r < 0); this.a = r & 255; break;                    // SUB
        case 3: r = a - v - c; this.flags(!(r & 255), 1, (a & 15) < (v & 15) + c, r < 0); this.a = r & 255; break;            // SBC
        case 4: this.a = a & v; this.flags(!this.a, 0, 1, 0); break;                                                          // AND
        case 5: this.a = a ^ v; this.flags(!this.a, 0, 0, 0); break;                                                          // XOR
        case 6: this.a = a | v; this.flags(!this.a, 0, 0, 0); break;                                                          // OR
        case 7: r = a - v; this.flags(!(r & 255), 1, (a & 15) < (v & 15), r < 0); break;                                      // CP
      }
    }
    cond(i) { switch (i) { case 0: return !this.fz; case 1: return this.fz; case 2: return !this.fc; default: return this.fc; } }
    get rr() { return null; }
    getRR(i) { switch (i) { case 0: return this.bc; case 1: return this.de; case 2: return this.hl; default: return this.sp; } }
    setRR(i, v) { v &= 0xffff; switch (i) { case 0: this.bc = v; break; case 1: this.de = v; break; case 2: this.hl = v; break; default: this.sp = v; } }

    // idles: wait cycles before the pushes (2 when waking from HALT, 1 after a dropped opcode fetch).
    // mask: requests too recent to be seen yet.
    interrupts(idles, mask = 0) {
      const pending = this.ie & this.if & 0x1f & ~mask;
      if (!pending) return false;
      if (this.halted) { this.halted = false; }
      if (!this.ime) return false;
      this.ime = false;
      // "EI, HALT" with an interrupt already waiting: the handler returns to the HALT
      if (this.haltBug) { this.haltBug = false; this.pc = (this.pc - 1) & 0xffff; }
      for (let i = 0; i < idles; i++) this.idle();
      this.sp = (this.sp - 1) & 0xffff; this.wr(this.sp, this.pc >> 8);
      // Which interrupt wins is decided after the high byte is pushed.
      const p2 = this.ie & this.if & 0x1f;
      this.sp = (this.sp - 1) & 0xffff; this.wr(this.sp, this.pc & 255);
      if (!p2) { this.pc = 0; return true; }
      const bit = p2 & -p2;
      this.if &= ~bit;
      this.pc = 0x40 + 8 * Math.log2(bit);
      this.idle();
      return true;
    }

    step() {
      if (this.hdmaPending) { this.hdmaPending = false; if (this.hdmaActive && !this.halted) this.hdmaBlock(); }
      // EI takes effect after the instruction that follows it
      if (this.eiDelay && --this.eiDelay === 0) this.ime = true;
      if (this.halted || this.stopped) {
        if (this.interrupts(2)) return;
        if (this.halted || this.stopped) { this.idle(); if (this.stopped && (this.keys & 0xff)) this.stopped = false; return; }
      }
      // Opcode fetch. An interrupt that arrives during it is still taken (the fetched byte is dropped),
      // except the video chip's, which come too late in the M-cycle.
      const op = this.rd(this.pc);
      if (this.interrupts(1, this.ppuNewIf)) return;
      if (this.haltBug) this.haltBug = false; else this.pc = (this.pc + 1) & 0xffff;
      this.exec(op);
    }

    exec(op) {
      if (op >= 0x40 && op < 0x80) {
        if (op === 0x76) { // HALT
          if (!this.ime && (this.ie & this.if & 0x1f)) this.haltBug = true; else this.halted = true;
          return;
        }
        this.setR((op >> 3) & 7, this.getR(op & 7)); return;
      }
      if (op >= 0x80 && op < 0xc0) { this.alu((op >> 3) & 7, this.getR(op & 7)); return; }
      const x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
      if (x === 0) {
        switch (z) {
          case 0:
            if (y === 0) return;                                                    // NOP
            if (y === 1) { const a = this.fetch16(); this.wr(a, this.sp & 255); this.wr((a + 1) & 0xffff, this.sp >> 8); return; }
            if (y === 2) { this.fetch(); if (this.cgb && (this.key1 & 1)) { this.double = !this.double; this.key1 = 0; this.div = 0; } else this.stopped = true; return; } // STOP
            { const e = (this.fetch() << 24) >> 24; if (y === 3 || this.cond(y - 4)) { this.idle(); this.pc = (this.pc + e) & 0xffff; } return; } // JR
          case 1:
            if (!q) { this.setRR(p, this.fetch16()); return; }
            { const hl = this.hl, v = this.getRR(p), r = hl + v; this.idle();
              this.f = (this.f & 0x80) | (((hl & 0xfff) + (v & 0xfff)) > 0xfff ? 0x20 : 0) | (r > 0xffff ? 0x10 : 0); this.hl = r; return; }
          case 2: {
            const addr = p === 0 ? this.bc : p === 1 ? this.de : this.hl;
            if (!q) this.wr(addr, this.a); else this.a = this.rd(addr);
            if (p === 2) this.hl = this.hl + 1; else if (p === 3) this.hl = this.hl - 1;
            return;
          }
          case 3: this.idle(); this.setRR(p, this.getRR(p) + (q ? -1 : 1)); return;
          case 4: { const v = this.getR(y), r = (v + 1) & 255; this.f = (this.f & 0x10) | (r ? 0 : 0x80) | ((v & 15) === 15 ? 0x20 : 0); this.setR(y, r); return; }
          case 5: { const v = this.getR(y), r = (v - 1) & 255; this.f = (this.f & 0x10) | 0x40 | (r ? 0 : 0x80) | ((v & 15) === 0 ? 0x20 : 0); this.setR(y, r); return; }
          case 6: this.setR(y, this.fetch()); return;
          case 7: {
            const a = this.a, c = this.fc ? 1 : 0;
            switch (y) {
              case 0: this.a = ((a << 1) | (a >> 7)) & 255; this.flags(0, 0, 0, a & 0x80); return;      // RLCA
              case 1: this.a = ((a >> 1) | (a << 7)) & 255; this.flags(0, 0, 0, a & 1); return;         // RRCA
              case 2: this.a = ((a << 1) | c) & 255; this.flags(0, 0, 0, a & 0x80); return;            // RLA
              case 3: this.a = ((a >> 1) | (c << 7)) & 255; this.flags(0, 0, 0, a & 1); return;       // RRA
              case 4: { // DAA
                let r = a, carry = this.fc;
                if (!(this.f & 0x40)) { if (carry || r > 0x99) { r += 0x60; carry = true; } if ((this.f & 0x20) || (r & 15) > 9) r += 6; }
                else { if (carry) r -= 0x60; if (this.f & 0x20) r -= 6; }
                r &= 255; this.f = (r ? 0 : 0x80) | (this.f & 0x40) | (carry ? 0x10 : 0); this.a = r; return;
              }
              case 5: this.a = a ^ 255; this.f |= 0x60; return;                                             // CPL
              case 6: this.f = (this.f & 0x80) | 0x10; return;                                              // SCF
              case 7: this.f = (this.f & 0x90) ^ 0x10; return;                                              // CCF
            }
          }
        }
      }
      // x === 3
      switch (z) {
        case 0:
          if (y < 4) { this.idle(); if (this.cond(y)) { this.pc = this.pop16(); this.idle(); } return; }
          if (y === 4) { this.wr(0xff00 | this.fetch(), this.a); return; }
          if (y === 6) { this.a = this.rd(0xff00 | this.fetch()); return; }
          { const e = (this.fetch() << 24) >> 24, sp = this.sp, r = (sp + e) & 0xffff;
            this.flags(0, 0, ((sp & 15) + (e & 15)) > 15, ((sp & 255) + (e & 255)) > 255);
            if (y === 5) { this.idle(); this.idle(); this.sp = r; } else { this.idle(); this.hl = r; } return; }
        case 1:
          if (!q) { const v = this.pop16(); if (p === 3) { this.a = v >> 8; this.f = v & 0xf0; } else this.setRR(p, v); return; }
          if (p === 0) { this.pc = this.pop16(); this.idle(); return; }                                      // RET
          if (p === 1) { this.pc = this.pop16(); this.idle(); this.ime = true; return; }                     // RETI
          if (p === 2) { this.pc = this.hl; return; }                                                         // JP HL
          this.idle(); this.sp = this.hl; return;                                                             // LD SP,HL
        case 2:
          if (y < 4) { const a = this.fetch16(); if (this.cond(y)) { this.idle(); this.pc = a; } return; }
          if (y === 4) { this.wr(0xff00 | this.c, this.a); return; }
          if (y === 5) { this.wr(this.fetch16(), this.a); return; }
          if (y === 6) { this.a = this.rd(0xff00 | this.c); return; }
          this.a = this.rd(this.fetch16()); return;
        case 3:
          if (y === 0) { const a = this.fetch16(); this.idle(); this.pc = a; return; }                       // JP nn
          if (y === 1) { this.execCB(this.fetch()); return; }
          if (y === 6) { this.ime = false; this.eiDelay = 0; return; }                                        // DI
          if (y === 7) { if (!this.ime && !this.eiDelay) this.eiDelay = 2; return; }                                                          // EI (takes effect after the next instruction)
          this.locked = true; this.pc = (this.pc - 1) & 0xffff; return;                                       // illegal opcode: CPU stops
        case 4:
          if (y < 4) { const a = this.fetch16(); if (this.cond(y)) { this.push16(this.pc); this.pc = a; } return; }
          this.locked = true; this.pc = (this.pc - 1) & 0xffff; return;
        case 5:
          if (!q) { const v = p === 3 ? (this.a << 8) | this.f : this.getRR(p); this.push16(v); return; }
          if (p === 0) { const a = this.fetch16(); this.push16(this.pc); this.pc = a; return; }              // CALL nn
          this.locked = true; this.pc = (this.pc - 1) & 0xffff; return;
        case 6: this.alu(y, this.fetch()); return;
        case 7: this.push16(this.pc); this.pc = y * 8; return;                                                // RST
      }
    }

    execCB(op) {
      const r = op & 7, y = (op >> 3) & 7, x = op >> 6;
      const v = this.getR(r);
      if (x === 1) { this.f = (this.f & 0x10) | 0x20 | ((v >> y) & 1 ? 0 : 0x80); return; } // BIT
      if (x === 2) { this.setR(r, v & ~(1 << y)); return; }                                 // RES
      if (x === 3) { this.setR(r, v | (1 << y)); return; }                                  // SET
      let res, c;
      const cin = this.fc ? 1 : 0;
      switch (y) {
        case 0: c = v >> 7; res = (v << 1) | c; break;          // RLC
        case 1: c = v & 1; res = (v >> 1) | (c << 7); break;    // RRC
        case 2: c = v >> 7; res = (v << 1) | cin; break;        // RL
        case 3: c = v & 1; res = (v >> 1) | (cin << 7); break;  // RR
        case 4: c = v >> 7; res = v << 1; break;                // SLA
        case 5: c = v & 1; res = (v >> 1) | (v & 0x80); break;  // SRA
        case 6: c = 0; res = ((v & 15) << 4) | (v >> 4); break; // SWAP
        default: c = v & 1; res = v >> 1; break;                // SRL
      }
      res &= 255;
      this.flags(!res, 0, 0, c);
      this.setR(r, res);
    }

    // ---- frontend interface ----
    // RetroPixels' button order: A, B, Select, Start, Up, Down, Left, Right
    setInput(player, bits) {
      if (player) return;
      this.tiltKeys = bits >> 8;
      // GB order: A B Select Start (low nibble), Right Left Up Down (high nibble)
      const k = (bits & 15) | ((bits & 0x80) ? 0x10 : 0) | ((bits & 0x40) ? 0x20 : 0) | ((bits & 0x10) ? 0x40 : 0) | ((bits & 0x20) ? 0x80 : 0);
      if (k & ~this.keys) this.if |= 0x10; // joypad interrupt on a new press
      this.keys = k;
    }
    // Tilt carts (MBC7) read the right stick, or the tilt keys
    setAnalog(player, x, y, rx, ry) { if (!player) { this.stick.x = rx; this.stick.y = ry; } }
    runFrame() {
      this.frameDone = false;
      const k = this.tiltKeys;
      this.tilt.x = Math.max(-1, Math.min(1, this.stick.x + ((k & 8) ? 1 : 0) - ((k & 4) ? 1 : 0)));
      this.tilt.y = Math.max(-1, Math.min(1, this.stick.y + ((k & 2) ? 1 : 0) - ((k & 1) ? 1 : 0)));
      let guard = 0;
      while (!this.frameDone && guard++ < 300000) {
        if (this.locked) { this.idle(); continue; }
        this.step();
      }
      this.cart.frame();
    }
    // 0..1: how hard the cartridge's rumble motor ran this frame
    get rumble() { return this.cart.hasRumble ? this.cart.rumbleLevel : undefined; }
    get usesRightStick() { return this.cart.kind === "MBC7"; }
    get controlsNote() { return this.usesRightStick ? "Tilt the game with the right stick, or with the tilt keys (I J K L to start with). Clicking the sticks (L3/R3) is used for save states." : null; }
    // extra controls shown for this game only (tilt keys for MBC7 carts)
    get buttons() { return this.cart.kind === "MBC7" ? GB.info.buttons.concat(["Tilt Up", "Tilt Down", "Tilt Left", "Tilt Right"]) : null; }
    takeAudio() { const a = this.apu.out.slice(0, this.apu.outLen); this.apu.outLen = 0; return a; }
    getSRAM() { return this.cart.sram(); }
    saveState() {
      const regs = {};
      for (const k of ["a", "f", "b", "c", "d", "e", "h", "l", "sp", "pc", "ime", "eiDelay", "halted", "haltBug", "stopped", "div", "tima", "tma", "tac",
        "timaReload", "timaJustLoaded", "ly", "lyReg", "lyCmp", "m3end", "firstLine", "oamLock", "vramLock", "oamWLock", "vramWLock", "lycFlag", "serialBits", "lineDot", "mode", "statLine", "winLine", "double", "key1", "hdmaSrc", "hdmaDst", "hdmaLen", "hdmaActive", "vbank", "wbank",
        "bcps", "ocps", "ie", "if", "joyp", "cycles", "clock", "locked", "lcdOffDots",
        "dmaActive", "dmaDelay", "dmaIdx", "dmaBase", "dmaReq", "dmaByte"]) regs[k] = this[k];
      return { v: 1, regs, vram: this.vram, wram: this.wram, oam: this.oam, hram: this.hram, io: this.io, bgPal: this.bgPal, obPal: this.obPal,
        cart: this.cart.saveState(), apu: this.apu.saveState() };
    }
    loadState(s) {
      Object.assign(this, s.regs);
      this.vram.set(s.vram); this.wram.set(s.wram); this.oam.set(s.oam); this.hram.set(s.hram); this.io.set(s.io);
      this.bgPal.set(s.bgPal); this.obPal.set(s.obPal);
      this.cart.loadState(s.cart); this.apu.loadState(s.apu);
    }
  }
  GB.info = { system: "gb", name: "Game Boy", width: 160, height: 144, aspect: 10 / 9, fps: FPS, stereo: true, players: 1,
    buttons: ["A", "B", "Select", "Start", "Up", "Down", "Left", "Right"] };

  global.RPCores = global.RPCores || {};
  global.RPCores.gb = GB;
})(typeof window !== "undefined" ? window : globalThis);
