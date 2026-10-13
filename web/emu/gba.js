// RetroPixels — Game Boy Advance emulator. Original code.
//
// ARM7TDMI CPU (RPARM7), memory bus with wait states, video (modes 0-5,
// sprites, windows, blending, mosaic), sound (four PSG channels + two DMA
// sound FIFOs), DMA, timers, interrupts, keypad, cartridge saves (SRAM, Flash,
// EEPROM) and real-time clock, written from the documented hardware behaviour.
// No Nintendo BIOS is used: RetroPixels' own small BIOS handles interrupts and
// the BIOS calls (division, decompression, copy, affine set-up, waits...).
"use strict";

(function (global) {
  const FRAME_LINES = 228, LINE_CYCLES = 1232, HDRAW = 960;
  const CPU_HZ = 16777216;

  // ---- RetroPixels BIOS (ARM code, little-endian words) ----
  const BIOS_WORDS = [
    0xeafffffe, // 0x00 reset (unused: the game starts directly)
    0xe1b0f00e, // 0x04 undefined instruction: movs pc, lr
    0xe1b0f00e, // 0x08 software interrupt (handled by RetroPixels): movs pc, lr
    0xe25ef004, // 0x0C prefetch abort: subs pc, lr, #4
    0xe25ef008, // 0x10 data abort: subs pc, lr, #8
    0xeafffffe, // 0x14 reserved
    0xea000000, // 0x18 IRQ: b 0x20
    0xe25ef004, // 0x1C FIQ: subs pc, lr, #4
    0xe92d500f, // 0x20 stmfd sp!, {r0-r3, r12, lr}
    0xe3a00301, // 0x24 mov r0, #0x04000000
    0xe28fe000, // 0x28 add lr, pc, #0
    0xe510f004, // 0x2C ldr pc, [r0, #-4]     (the game's handler at 0x03FFFFFC)
    0xe8bd500f, // 0x30 ldmfd sp!, {r0-r3, r12, lr}
    0xe25ef004, // 0x34 subs pc, lr, #4
    0xe55ec002, // 0x38 (not executed: value the real chip's open bus shows after an interrupt)
    0xe55ec002, // 0x3C
  ];

  // 15-bit BGR -> ABGR
  const COLOR = new Uint32Array(0x8000);
  for (let i = 0; i < 0x8000; i++) {
    const r = i & 31, g = (i >> 5) & 31, b = (i >> 10) & 31;
    COLOR[i] = 0xff000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
  }

  // ===================================================================
  // Cartridge save memory
  // ===================================================================
  class Save {
    constructor(rom) {
      const text = new TextDecoder("latin1").decode(rom);
      this.type = "none";
      if (/EEPROM_V/.test(text)) this.type = "eeprom";
      else if (/FLASH1M_V/.test(text)) this.type = "flash128";
      else if (/FLASH(512)?_V/.test(text)) this.type = "flash64";
      else if (/SRAM(_F)?_V/.test(text)) this.type = "sram";
      this.rtc = /SIIRTC_V\d\d\d/.test(text);
      const size = { eeprom: 0x2000, flash128: 0x20000, flash64: 0x10000, sram: 0x8000, none: 0x8000 }[this.type];
      this.data = new Uint8Array(size).fill(0xff);
      this.eepromSize = 0; // 512 or 8192 once known
      this.dirty = false;
      // flash state
      this.fState = 0; this.fCmd = 0; this.fBank = 0; this.fId = false;
      // eeprom state
      this.eBits = []; this.eRead = []; this.eReadPos = 0;
    }
    load(bytes) {
      if (this.type === "eeprom" && bytes.length <= 512) this.eepromSize = 512;
      else if (this.type === "eeprom") this.eepromSize = 8192;
      this.data.set(bytes.subarray(0, this.data.length));
    }
    bytes() {
      if (this.type === "eeprom") return this.data.slice(0, this.eepromSize || 8192);
      return this.data.slice();
    }
    // ---- SRAM / Flash (0x0E000000) ----
    read8(a) {
      a &= 0xffff;
      if (this.type === "flash64" || this.type === "flash128") {
        if (this.fId && a < 2) return this.type === "flash128" ? (a ? 0x09 : 0xc2) : (a ? 0x1b : 0x32);
        return this.data[this.fBank * 0x10000 + a];
      }
      if (this.type === "sram" || this.type === "none") return this.data[a & 0x7fff];
      return 0xff;
    }
    write8(a, v) {
      a &= 0xffff;
      if (this.type === "sram" || this.type === "none") {
        if (this.type === "none") this.type = "sram";
        if (this.data[a & 0x7fff] !== v) { this.data[a & 0x7fff] = v; this.dirty = true; }
        return;
      }
      if (this.type !== "flash64" && this.type !== "flash128") return;
      if (this.fCmd === 0xa0) { this.data[this.fBank * 0x10000 + a] &= v; this.fCmd = 0; this.dirty = true; return; }
      if (this.fCmd === 0xb0 && a === 0) { this.fBank = this.type === "flash128" ? v & 1 : 0; this.fCmd = 0; return; }
      if (this.fState === 0 && a === 0x5555 && v === 0xaa) { this.fState = 1; return; }
      if (this.fState === 1 && a === 0x2aaa && v === 0x55) { this.fState = 2; return; }
      if (this.fState === 2) {
        this.fState = 0;
        if (this.fCmd === 0x80 && v === 0x30) { // erase 4 KB sector
          const base = this.fBank * 0x10000 + (a & 0xf000);
          this.data.fill(0xff, base, base + 0x1000); this.fCmd = 0; this.dirty = true; return;
        }
        if (a !== 0x5555) return;
        switch (v) {
          case 0x90: this.fId = true; break;
          case 0xf0: this.fId = false; break;
          case 0x80: this.fCmd = 0x80; break;
          case 0x10: if (this.fCmd === 0x80) { this.data.fill(0xff); this.dirty = true; } this.fCmd = 0; break;
          case 0xa0: this.fCmd = 0xa0; break;
          case 0xb0: this.fCmd = 0xb0; break;
        }
        return;
      }
      this.fState = 0;
    }
    // ---- EEPROM (serial, through DMA 3) ----
    eepromWrite(bit) {
      this.eBits.push(bit & 1);
      const n = this.eBits.length, b = this.eBits;
      if (n < 2) return;
      const cmd = (b[0] << 1) | b[1];
      if (cmd === 3) { // read request: 2 + addr + 1
        const ab = this.eepromSize === 512 ? 6 : this.eepromSize === 8192 ? 14 : 0;
        if (ab && n === 2 + ab + 1) this.eepromStartRead(ab);
      } else if (cmd === 2) { // write: 2 + addr + 64 + 1
        const ab = this.eepromSize === 512 ? 6 : this.eepromSize === 8192 ? 14 : 0;
        if (ab && n === 2 + ab + 64 + 1) this.eepromDoWrite(ab);
      } else this.eBits = [];
    }
    // Called when a DMA to the EEPROM finishes; the length tells us the chip size.
    eepromDmaDone(units) {
      if (this.type !== "eeprom") return;
      if (!this.eepromSize) {
        if (units === 9 || units === 73) this.eepromSize = 512;
        else if (units === 17 || units === 81) this.eepromSize = 8192;
        else return;
        // re-run the bits now that the size is known
        const bits = this.eBits; this.eBits = [];
        for (const x of bits) this.eepromWrite(x);
      }
    }
    eAddr(ab) { let a = 0; for (let i = 0; i < ab; i++) a = (a << 1) | this.eBits[2 + i]; return a & (this.eepromSize === 512 ? 0x3f : 0x3ff); }
    eepromStartRead(ab) {
      const a = this.eAddr(ab) * 8;
      this.eRead = [0, 0, 0, 0];
      for (let i = 0; i < 64; i++) this.eRead.push((this.data[a + (i >> 3)] >> (7 - (i & 7))) & 1);
      this.eReadPos = 0; this.eBits = [];
    }
    eepromDoWrite(ab) {
      const a = this.eAddr(ab) * 8;
      for (let i = 0; i < 8; i++) {
        let v = 0;
        for (let k = 0; k < 8; k++) v = (v << 1) | this.eBits[2 + ab + i * 8 + k];
        this.data[a + i] = v;
      }
      this.dirty = true; this.eBits = []; this.eRead = []; this.eReadPos = 0;
    }
    eepromRead() {
      if (this.eReadPos < this.eRead.length) return this.eRead[this.eReadPos++];
      return 1; // ready
    }
    saveState() {
      return { type: this.type, data: this.data.slice(), eepromSize: this.eepromSize, f: [this.fState, this.fCmd, this.fBank, this.fId ? 1 : 0],
        eBits: this.eBits.slice(), eRead: this.eRead.slice(), eReadPos: this.eReadPos };
    }
    loadState(o) {
      this.type = o.type; this.data.set(o.data); this.eepromSize = o.eepromSize;
      [this.fState, this.fCmd, this.fBank] = o.f; this.fId = !!o.f[3];
      this.eBits = o.eBits.slice(); this.eRead = o.eRead.slice(); this.eReadPos = o.eReadPos;
    }
  }

  // ===================================================================
  // Real-time clock (Seiko S-3511 style, on the cartridge GPIO port)
  // ===================================================================
  class RTC {
    constructor() { this.dir = 0; this.ctrl = 0; this.pins = 0; this.state = 0; this.bits = 0; this.nbits = 0; this.cmd = 0; this.out = []; this.status = 0x40; }
    bcd(v) { return ((v / 10) | 0) * 16 + (v % 10); }
    timeBytes(withDate) {
      const d = new Date();
      let h = d.getHours();
      const hb = (this.status & 0x40) ? this.bcd(h) : this.bcd(h % 12) | (h >= 12 ? 0x80 : 0);
      const t = [hb, this.bcd(d.getMinutes()), this.bcd(d.getSeconds())];
      return withDate ? [this.bcd(d.getFullYear() % 100), this.bcd(d.getMonth() + 1), this.bcd(d.getDate()), d.getDay(), ...t] : t;
    }
    read() { // GPIO data register
      let v = this.pins & ~this.dir & 0xf;
      return (this.pins & this.dir) | v;
    }
    write(v) {
      const old = this.pins;
      this.pins = (this.pins & ~this.dir) | (v & this.dir);
      const sck = this.pins & 1, sio = (this.pins >> 1) & 1, cs = (this.pins >> 2) & 1;
      if (!cs) { this.state = 0; this.nbits = 0; this.bits = 0; return; }
      if (!(old & 4)) { this.state = 1; this.nbits = 0; this.bits = 0; return; } // chip select rose
      if ((old & 1) && !sck) return;
      if (!(old & 1) && sck) { // rising edge
        if (this.state === 1) { // command byte "0110 ccc r", sent MSB first (or LSB first by some games)
          this.bits = ((this.bits << 1) | sio) & 0xff; this.nbits++;
          if (this.nbits === 8) {
            let c = this.bits;
            if ((c >> 4) !== 6) { let r = 0; for (let i = 0; i < 8; i++) r |= ((c >> i) & 1) << (7 - i); c = r; }
            this.cmd = (c >> 1) & 7;
            const read = c & 1;
            this.nbits = 0; this.bits = 0;
            if (read) {
              let bytes = [];
              if (this.cmd === 2) bytes = this.timeBytes(true);
              else if (this.cmd === 3) bytes = this.timeBytes(false);
              else if (this.cmd === 1) bytes = [this.status];
              this.out = []; for (const b of bytes) for (let i = 0; i < 8; i++) this.out.push((b >> i) & 1);
              this.state = 2;
            } else { this.state = this.cmd === 0 ? 0 : 3; if (this.cmd === 0) this.status = 0x40; }
          }
        } else if (this.state === 2) {
          const bit = this.out.length ? this.out.shift() : 1;
          this.pins = (this.pins & ~2) | (bit << 1);
        } else if (this.state === 3) {
          this.bits |= sio << this.nbits; this.nbits++;
          if (this.nbits === 8) { if (this.cmd === 1) this.status = this.bits; this.nbits = 0; this.bits = 0; }
        }
      } else if (this.state === 2 && !sck && this.out.length) {
        this.pins = (this.pins & ~2) | (this.out[0] << 1);
      }
    }
    saveState() { return { dir: this.dir, ctrl: this.ctrl, pins: this.pins, state: this.state, bits: this.bits, nbits: this.nbits, cmd: this.cmd, out: this.out.slice(), status: this.status }; }
    loadState(o) { Object.assign(this, o); this.out = o.out.slice(); }
  }

  // ===================================================================
  // Sound
  // ===================================================================
  const DUTY = [0x01, 0x81, 0x87, 0x7e];
  const NOISE_DIV = [8, 16, 32, 48, 64, 80, 96, 112];
  class APU {
    constructor(gba, sampleRate) {
      this.gba = gba;
      this.sampleRate = sampleRate;
      this.wave = new Uint8Array(32);
      this.fifo = [new Int8Array(32), new Int8Array(32)];
      this.out = new Float32Array(16384); this.outLen = 0;
      this.reset();
    }
    reset() {
      this.ch = [0, 1, 2, 3].map(() => ({ on: false, dac: false, len: 0, lenOn: false, vol: 0, envDir: 0, envPeriod: 0, envTimer: 0, envInit: 0,
        freq: 0, timer: 0, pos: 0, duty: 0, lfsr: 0x7fff, sweepPeriod: 0, sweepTimer: 0, sweepShift: 0, sweepNeg: false, sweepOn: false, shadow: 0 }));
      this.wave.fill(0);
      this.fifoR = [0, 0]; this.fifoW = [0, 0]; this.fifoN = [0, 0]; this.fifoOut = [0, 0];
      this.seqTimer = 32768; this.seqStep = 0;
      this.last = 0; // cycle time audio has been generated up to
      this.acc = 0; this.sumL = 0; this.sumR = 0; this.n = 0;
      this.hpL = 0; this.hpR = 0; this.prevL = 0; this.prevR = 0;
    }
    get io() { return this.gba.io16; }
    // ---- registers (16-bit, offsets from 0x60) ----
    write16(off, v) {
      const io = this.io;
      const master = io[0x84 >> 1] & 0x80;
      if (!master && off >= 0x60 && off < 0x82) return;
      switch (off) {
        case 0x60: { const c = this.ch[0]; c.sweepPeriod = (v >> 4) & 7; c.sweepNeg = !!(v & 8); c.sweepShift = v & 7; break; }
        case 0x62: case 0x68: { const c = this.ch[off === 0x62 ? 0 : 1]; c.len = 64 - (v & 0x3f); c.duty = (v >> 6) & 3; c.dac = (v & 0xf800) !== 0; if (!c.dac) c.on = false; break; }
        case 0x64: case 0x6c: {
          const i = off === 0x64 ? 0 : 1, c = this.ch[i];
          c.freq = v & 0x7ff; c.lenOn = !!(v & 0x4000);
          if (v & 0x8000) this.trigger(i);
          break;
        }
        case 0x70: { const c = this.ch[2]; c.dac = !!(v & 0x80); if (!c.dac) c.on = false; break; }
        case 0x72: this.ch[2].len = 256 - (v & 0xff); break;
        case 0x74: { const c = this.ch[2]; c.freq = v & 0x7ff; c.lenOn = !!(v & 0x4000); if (v & 0x8000) this.trigger(2); break; }
        case 0x78: { const c = this.ch[3]; c.len = 64 - (v & 0x3f); c.dac = (v & 0xf800) !== 0; if (!c.dac) c.on = false; break; }
        case 0x7c: { const c = this.ch[3]; c.lenOn = !!(v & 0x4000); if (v & 0x8000) this.trigger(3); break; }
        case 0x82:
          if (v & 0x800) { this.fifoR[0] = this.fifoW[0] = this.fifoN[0] = 0; }
          if (v & 0x8000) { this.fifoR[1] = this.fifoW[1] = this.fifoN[1] = 0; }
          break;
        case 0x84:
          if (!(v & 0x80) && master) { for (let a = 0x60; a < 0x82; a += 2) io[a >> 1] = 0; this.ch.forEach((c) => { c.on = false; c.dac = false; }); }
          if ((v & 0x80) && !master) this.seqStep = 0;
          break;
      }
    }
    trigger(i) {
      const c = this.ch[i], io = this.io;
      c.on = c.dac;
      if (!c.len) c.len = i === 2 ? 256 : 64;
      if (i === 2) { c.timer = (2048 - c.freq) * 8; c.pos = 0; }
      else if (i === 3) { const v = io[0x7c >> 1]; c.timer = (NOISE_DIV[v & 7] << (v >> 4 & 15)) * 4; c.lfsr = (v & 8) ? 0x7f : 0x7fff; }
      else c.timer = (2048 - c.freq) * 16;
      if (i !== 2) {
        const e = io[(i === 0 ? 0x62 : i === 1 ? 0x68 : 0x78) >> 1] >> 8;
        c.vol = e >> 4; c.envDir = e & 8 ? 1 : -1; c.envPeriod = e & 7; c.envTimer = c.envPeriod || 8;
      }
      if (i === 0) {
        c.shadow = c.freq; c.sweepTimer = c.sweepPeriod || 8; c.sweepOn = !!(c.sweepPeriod || c.sweepShift);
        if (c.sweepShift && this.sweepCalc(c) > 2047) c.on = false;
      }
    }
    sweepCalc(c) { const d = c.shadow >> c.sweepShift; return c.sweepNeg ? c.shadow - d : c.shadow + d; }
    clockSeq() {
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
    // wave RAM as seen by the CPU (the bank that isn't playing)
    waveRead(i) { const bank = (this.io[0x70 >> 1] >> 6) & 1; return this.wave[(bank ^ 1) * 16 + i]; }
    waveWrite(i, v) { const bank = (this.io[0x70 >> 1] >> 6) & 1; this.wave[(bank ^ 1) * 16 + i] = v; }
    // ---- DMA sound ----
    fifoWrite(k, v32, n = 4) {
      for (let b = 0; b < n; b++) {
        if (this.fifoN[k] < 32) { this.fifo[k][this.fifoW[k]] = (v32 >> (b * 8)) & 0xff; this.fifoW[k] = (this.fifoW[k] + 1) & 31; this.fifoN[k]++; }
      }
    }
    timerOverflow(t) {
      const cnt = this.io[0x82 >> 1];
      for (let k = 0; k < 2; k++) {
        const sel = (cnt >> (10 + k * 4)) & 1;
        if (sel !== t) continue;
        if (this.fifoN[k] > 0) {
          this.fifoOut[k] = this.fifo[k][this.fifoR[k]];
          this.fifoR[k] = (this.fifoR[k] + 1) & 31; this.fifoN[k]--;
        }
        if (this.fifoN[k] <= 16) this.gba.fifoRequest(k);
      }
    }
    // ---- output ----
    // Advance sound to cycle time t.
    advance(t) {
      let n = t - this.last;
      if (n <= 0) return;
      this.last = t;
      const io = this.io;
      const master = io[0x84 >> 1] & 0x80;
      // compute in chunks no longer than one output sample so changes are captured
      const step = CPU_HZ / this.sampleRate;
      while (n > 0) {
        const k = Math.min(n, 64);
        n -= k;
        let L = 0, R = 0;
        if (master) {
          this.seqTimer -= k;
          while (this.seqTimer <= 0) { this.seqTimer += 32768; this.clockSeq(); }
          const ch = this.ch;
          for (let i = 0; i < 4; i++) {
            const c = ch[i];
            if (!c.on) continue;
            c.timer -= k;
            while (c.timer <= 0) {
              if (i === 2) {
                c.timer += (2048 - c.freq) * 8;
                const dim = (io[0x70 >> 1] >> 5) & 1;
                c.pos = (c.pos + 1) & (dim ? 63 : 31);
              } else if (i === 3) {
                const v = io[0x7c >> 1];
                c.timer += (NOISE_DIV[v & 7] << ((v >> 4) & 15)) * 4;
                const x = (c.lfsr ^ (c.lfsr >> 1)) & 1;
                c.lfsr = (c.lfsr >> 1) | (x << 14);
                if (v & 8) c.lfsr = (c.lfsr & ~0x40) | (x << 6);
              } else { c.timer += (2048 - c.freq) * 16; c.pos = (c.pos + 1) & 7; }
            }
          }
          const o0 = ch[0].on && ((DUTY[ch[0].duty] >> (7 - ch[0].pos)) & 1) ? ch[0].vol : 0;
          const o1 = ch[1].on && ((DUTY[ch[1].duty] >> (7 - ch[1].pos)) & 1) ? ch[1].vol : 0;
          let o2 = 0;
          if (ch[2].on) {
            const c3l = io[0x70 >> 1], dim = (c3l >> 5) & 1, bank = (c3l >> 6) & 1;
            const p = dim ? ch[2].pos : (bank * 32 + ch[2].pos);
            const byte = this.wave[(p >> 1) & 31], s = (p & 1) ? byte & 15 : byte >> 4;
            const vh = io[0x72 >> 1];
            if (vh & 0x8000) o2 = (s * 3) >> 2;
            else { const vc = (vh >> 13) & 3; o2 = vc ? s >> (vc - 1) : 0; }
          }
          const o3 = ch[3].on && !(ch[3].lfsr & 1) ? ch[3].vol : 0;
          const cl = io[0x80 >> 1], ch2 = io[0x82 >> 1];
          const ps = [o0, o1, o2, o3];
          let pl = 0, pr = 0;
          for (let i = 0; i < 4; i++) { if (cl & (0x1000 << i)) pl += ps[i]; if (cl & (0x100 << i)) pr += ps[i]; }
          const ratio = [0.25, 0.5, 1, 1][ch2 & 3];
          pl *= (((cl >> 4) & 7) + 1) / 8 * ratio; pr *= ((cl & 7) + 1) / 8 * ratio;
          L = pl / 60 * 0.5; R = pr / 60 * 0.5;
          const a = this.fifoOut[0] / 128 * ((ch2 & 4) ? 1 : 0.5), b = this.fifoOut[1] / 128 * ((ch2 & 8) ? 1 : 0.5);
          if (ch2 & 0x200) L += a * 0.5; if (ch2 & 0x100) R += a * 0.5;
          if (ch2 & 0x2000) L += b * 0.5; if (ch2 & 0x1000) R += b * 0.5;
        }
        this.sumL += L * k; this.sumR += R * k; this.n += k; this.acc += k;
        while (this.acc >= step) {
          this.acc -= step;
          const l = this.n ? this.sumL / this.n : 0, r = this.n ? this.sumR / this.n : 0;
          this.sumL = this.sumR = 0; this.n = 0;
          this.hpL = 0.998 * (this.hpL + l - this.prevL); this.prevL = l;
          this.hpR = 0.998 * (this.hpR + r - this.prevR); this.prevR = r;
          if (this.outLen < this.out.length - 1) { this.out[this.outLen++] = this.hpL; this.out[this.outLen++] = this.hpR; }
        }
      }
    }
    saveState() {
      return { ch: this.ch.map((c) => Object.assign({}, c)), wave: this.wave.slice(), fifoA: this.fifo[0].slice(), fifoB: this.fifo[1].slice(),
        r: [this.fifoR[0], this.fifoR[1], this.fifoW[0], this.fifoW[1], this.fifoN[0], this.fifoN[1], this.fifoOut[0], this.fifoOut[1], this.seqTimer, this.seqStep, this.last, this.acc] };
    }
    loadState(o) {
      o.ch.forEach((c, i) => Object.assign(this.ch[i], c));
      this.wave.set(o.wave); this.fifo[0].set(o.fifoA); this.fifo[1].set(o.fifoB);
      const r = o.r;
      this.fifoR = [r[0], r[1]]; this.fifoW = [r[2], r[3]]; this.fifoN = [r[4], r[5]]; this.fifoOut = [r[6], r[7]];
      this.seqTimer = r[8]; this.seqStep = r[9]; this.last = r[10]; this.acc = r[11];
      this.sumL = this.sumR = 0; this.n = 0; this.outLen = 0;
    }
  }

  global.RPGBAParts = { BIOS_WORDS, COLOR, Save, RTC, APU, FRAME_LINES, LINE_CYCLES, HDRAW, CPU_HZ };
})(typeof window !== "undefined" ? window : globalThis);

// ---------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------
(function (global) {
  const { COLOR } = global.RPGBAParts;
  const OBJ_SIZE = [ // [shape][size] -> [w, h]
    [[8, 8], [16, 16], [32, 32], [64, 64]],
    [[16, 8], [32, 8], [32, 16], [64, 32]],
    [[8, 16], [8, 32], [16, 32], [32, 64]],
    [[8, 8], [8, 8], [8, 8], [8, 8]],
  ];
  // layer ids: 0-3 BGs, 4 OBJ, 5 backdrop
  class PPU {
    constructor(gba) {
      this.gba = gba;
      this.bg = [0, 1, 2, 3].map(() => new Uint32Array(240)); // 0 = transparent, else 0x10000 | color15
      this.obj = new Uint32Array(240);   // 0 = none, else 0x10000 | color | prio<<17 | semi<<19
      this.objWin = new Uint8Array(240);
      this.winMask = new Uint8Array(240);
      this.affX = new Int32Array(2); this.affY = new Int32Array(2); // internal reference points (BG2, BG3)
    }
    latchAffine() {
      const io = this.gba.io16;
      for (let k = 0; k < 2; k++) {
        const b = (0x28 + k * 0x10) >> 1;
        this.affX[k] = ((io[b] | (io[b + 1] << 16)) << 4) >> 4;
        this.affY[k] = ((io[b + 2] | (io[b + 3] << 16)) << 4) >> 4;
      }
    }
    // A write to BGxX/BGxY resets the internal point.
    writeRef(k, isY) {
      const io = this.gba.io16, b = (0x28 + k * 0x10) >> 1;
      if (isY) this.affY[k] = ((io[b + 2] | (io[b + 3] << 16)) << 4) >> 4;
      else this.affX[k] = ((io[b] | (io[b + 1] << 16)) << 4) >> 4;
    }

    renderLine(y, out) {
      const g = this.gba, io = g.io16, pal = g.pal16, vram = g.vram;
      const dispcnt = io[0];
      const o = y * 240;
      if (dispcnt & 0x80) { out.fill(0xffffffff, o, o + 240); this.stepAffine(); return; }
      const mode = dispcnt & 7;
      const mosaic = io[0x4c >> 1];
      const bgMosH = (mosaic & 15) + 1, bgMosV = ((mosaic >> 4) & 15) + 1;
      // which BGs exist in this mode
      let enabled = (dispcnt >> 8) & 0x1f;
      if (mode === 1) enabled &= 0x17;
      else if (mode === 2) enabled &= 0x1c;
      else if (mode >= 3) enabled &= 0x14;
      for (let k = 0; k < 4; k++) {
        if (!(enabled & (1 << k))) continue;
        const cnt = io[(0x08 >> 1) + k];
        const my = (cnt & 0x40) ? y - (y % bgMosV) : y;
        const buf = this.bg[k];
        if (mode === 0 || (mode === 1 && k < 2)) this.textBG(k, cnt, my, buf);
        else if (mode <= 2) this.affineBG(k, cnt, buf);
        else this.bitmapBG(mode, dispcnt, buf);
        if ((cnt & 0x40) && bgMosH > 1) for (let x = 0; x < 240; x++) buf[x] = buf[x - (x % bgMosH)];
      }
      this.stepAffine();
      if (enabled & 0x10) this.sprites(y, dispcnt, mode); else { this.obj.fill(0); this.objWin.fill(0); }
      // windows
      const winOn = dispcnt & 0xe000;
      const wm = this.winMask;
      if (winOn) {
        const winin = io[0x48 >> 1], winout = io[0x4a >> 1];
        wm.fill(winout & 0x3f);
        if (dispcnt & 0x8000) { const m = (winout >> 8) & 0x3f; for (let x = 0; x < 240; x++) if (this.objWin[x]) wm[x] = m; }
        for (let w = 1; w >= 0; w--) {
          if (!(dispcnt & (0x2000 << w))) continue;
          const h = io[(0x40 >> 1) + w], v = io[(0x44 >> 1) + w];
          const x1 = h >> 8, x2 = h & 0xff, y1 = v >> 8, y2 = v & 0xff;
          const inY = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
          if (!inY) continue;
          const m = (winin >> (w * 8)) & 0x3f;
          if (x1 <= x2) { for (let x = x1; x < x2 && x < 240; x++) wm[x] = m; }
          else { for (let x = 0; x < 240; x++) if (x >= x1 || x < x2) wm[x] = m; }
        }
      } else wm.fill(0x3f);
      // compose
      const prio = [io[4] & 3, io[5] & 3, io[6] & 3, io[7] & 3];
      const bldcnt = io[0x50 >> 1], bmode = (bldcnt >> 6) & 3, alpha = io[0x52 >> 1], bldy = Math.min(16, io[0x54 >> 1] & 31);
      const eva = Math.min(16, alpha & 31), evb = Math.min(16, (alpha >> 8) & 31);
      const backdrop = pal[0] & 0x7fff;
      // BG order by priority (stable by index)
      const order = [];
      for (let p = 0; p < 4; p++) for (let k = 0; k < 4; k++) if ((enabled & (1 << k)) && prio[k] === p) order.push(k);
      const bgs = this.bg, obj = this.obj;
      for (let x = 0; x < 240; x++) {
        const m = wm[x];
        let top = 5, topC = backdrop, topP = 4, second = 5, secC = backdrop;
        // find the top two layers
        const ov = obj[x];
        const objOk = ov && (m & 0x10);
        const objP = objOk ? (ov >>> 17) & 3 : 9;
        let placedObj = false;
        for (let i = 0; i < order.length; i++) {
          const k = order[i];
          if (!(m & (1 << k))) continue;
          const v = bgs[k][x];
          if (!v) continue;
          if (!placedObj && objP <= prio[k]) {
            placedObj = true;
            if (top === 5) { top = 4; topC = ov & 0x7fff; topP = objP; } else if (second === 5) { second = 4; secC = ov & 0x7fff; break; }
          }
          if (top === 5) { top = k; topC = v & 0x7fff; topP = prio[k]; }
          else { second = k; secC = v & 0x7fff; break; }
        }
        if (!placedObj && objOk) {
          if (top === 5) { top = 4; topC = ov & 0x7fff; }
          else if (second === 5) { second = 4; secC = ov & 0x7fff; }
        }
        let c = topC;
        const effects = m & 0x20;
        const semi = top === 4 && (ov & 0x80000);
        if (semi && (bldcnt & (0x100 << second))) c = blend(topC, secC, eva, evb);
        else if (effects && bmode && (bldcnt & (1 << top))) {
          if (bmode === 1) { if (bldcnt & (0x100 << second)) c = blend(topC, secC, eva, evb); }
          else if (bmode === 2) c = brighten(topC, bldy);
          else c = darken(topC, bldy);
        }
        out[o + x] = COLOR[c];
        void topP;
      }
    }
    stepAffine() {
      const io = this.gba.io16;
      for (let k = 0; k < 2; k++) {
        const b = (0x20 + k * 0x10) >> 1;
        this.affX[k] += (io[b + 1] << 16) >> 16; // PB
        this.affY[k] += (io[b + 3] << 16) >> 16; // PD
      }
    }
    textBG(k, cnt, y, buf) {
      const g = this.gba, io = g.io16, vram = g.vram, pal = g.pal16;
      const charBase = ((cnt >> 2) & 3) * 0x4000, screenBase = ((cnt >> 8) & 31) * 0x800;
      const c256 = cnt & 0x80, size = (cnt >> 14) & 3;
      const hofs = io[(0x10 >> 1) + k * 2] & 0x1ff, vofs = io[(0x12 >> 1) + k * 2] & 0x1ff;
      const wMask = size & 1 ? 511 : 255, hMask = size & 2 ? 511 : 255;
      const py = (y + vofs) & hMask;
      const ty = py >> 3, fy = py & 7;
      let lastTx = -1, entry = 0, rowAddr = 0, palBase = 0, hflip = 0;
      for (let x = 0; x < 240; x++) {
        const px = (x + hofs) & wMask, tx = px >> 3;
        if (tx !== lastTx) {
          lastTx = tx;
          let block = 0;
          if (size === 1) block = tx >> 5;
          else if (size === 2) block = ty >> 5;
          else if (size === 3) block = (tx >> 5) + (ty >> 5) * 2;
          const ea = screenBase + block * 0x800 + ((ty & 31) * 32 + (tx & 31)) * 2;
          entry = vram[ea & 0xffff] | (vram[(ea + 1) & 0xffff] << 8);
          const row = entry & 0x800 ? 7 - fy : fy;
          hflip = entry & 0x400 ? 7 : 0;
          const tile = entry & 0x3ff;
          rowAddr = charBase + (c256 ? tile * 64 + row * 8 : tile * 32 + row * 4);
          palBase = c256 ? 0 : (entry >> 12) * 16;
        }
        const fx = (px & 7) ^ hflip;
        let idx;
        if (rowAddr >= 0x10000) { buf[x] = 0; continue; }
        if (c256) idx = vram[rowAddr + fx];
        else { const b = vram[rowAddr + (fx >> 1)]; idx = fx & 1 ? b >> 4 : b & 15; }
        buf[x] = idx ? 0x10000 | (pal[palBase + idx] & 0x7fff) : 0;
      }
    }
    affineBG(k, cnt, buf) {
      const g = this.gba, io = g.io16, vram = g.vram, pal = g.pal16;
      const a = k - 2, b = (0x20 + a * 0x10) >> 1;
      const pa = (io[b] << 16) >> 16, pc = (io[b + 2] << 16) >> 16;
      const charBase = ((cnt >> 2) & 3) * 0x4000, screenBase = ((cnt >> 8) & 31) * 0x800;
      const size = 128 << ((cnt >> 14) & 3), wrap = cnt & 0x2000, tiles = size >> 3;
      let tx = this.affX[a], ty = this.affY[a];
      for (let x = 0; x < 240; x++, tx += pa, ty += pc) {
        let px = tx >> 8, py = ty >> 8;
        if (wrap) { px &= size - 1; py &= size - 1; }
        else if (px < 0 || py < 0 || px >= size || py >= size) { buf[x] = 0; continue; }
        const tile = vram[(screenBase + (py >> 3) * tiles + (px >> 3)) & 0xffff];
        const idx = vram[(charBase + tile * 64 + (py & 7) * 8 + (px & 7)) & 0xffff];
        buf[x] = idx ? 0x10000 | (pal[idx] & 0x7fff) : 0;
      }
    }
    bitmapBG(mode, dispcnt, buf) {
      const g = this.gba, io = g.io16, vram = g.vram, pal = g.pal16;
      const pa = (io[0x10] << 16) >> 16, pc = (io[0x12] << 16) >> 16;
      let tx = this.affX[0], ty = this.affY[0];
      const page = (dispcnt & 0x10) && mode !== 3 ? 0xa000 : 0;
      const w = mode === 5 ? 160 : 240, h = mode === 5 ? 128 : 160;
      for (let x = 0; x < 240; x++, tx += pa, ty += pc) {
        const px = tx >> 8, py = ty >> 8;
        if (px < 0 || py < 0 || px >= w || py >= h) { buf[x] = 0; continue; }
        if (mode === 4) { const idx = vram[page + py * 240 + px]; buf[x] = idx ? 0x10000 | (pal[idx] & 0x7fff) : 0; }
        else { const a = page + (py * w + px) * 2; buf[x] = 0x10000 | ((vram[a] | (vram[a + 1] << 8)) & 0x7fff); }
      }
    }
    sprites(y, dispcnt, mode) {
      const g = this.gba, oam = g.oam16, vram = g.vram, pal = g.pal16, io = g.io16;
      const obj = this.obj, win = this.objWin;
      obj.fill(0); win.fill(0);
      const map1d = dispcnt & 0x40;
      const mosaic = io[0x4c >> 1], mosH = ((mosaic >> 8) & 15) + 1, mosV = ((mosaic >> 12) & 15) + 1;
      const minTile = mode >= 3 ? 512 : 0;
      for (let i = 0; i < 128; i++) {
        const a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        const affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue; // hidden
        const shape = a0 >> 14, sz = a1 >> 14;
        const [w, h] = OBJ_SIZE[shape][sz];
        const dbl = affine && (a0 & 0x200);
        const bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        let sy = a0 & 0xff; if (sy >= 160) sy -= 256;
        let line = y - sy;
        if (line < 0 || line >= bh) continue;
        let sx = a1 & 0x1ff; if (sx >= 240) sx -= 512;
        const omode = (a0 >> 10) & 3;
        if (omode === 3) continue;
        const c256 = a0 & 0x2000, tile = a2 & 0x3ff, prio = (a2 >> 10) & 3, palBank = (a2 >> 12) * 16;
        const mos = a0 & 0x1000;
        if (mos) line -= line % mosV;
        const rowTiles = map1d ? (c256 ? w / 4 : w / 8) : 32;
        const pix = (tx, ty) => {
          let t = tile;
          if (c256) t = (t & ~1) + (ty >> 3) * rowTiles + (tx >> 3) * 2;
          else t = t + (ty >> 3) * rowTiles + (tx >> 3);
          t &= 0x3ff;
          if (t < minTile) return 0;
          if (c256) { const idx = vram[0x10000 + t * 32 + (ty & 7) * 8 + (tx & 7)]; return idx ? 0x10000 | (pal[256 + idx] & 0x7fff) : 0; }
          const b = vram[0x10000 + t * 32 + (ty & 7) * 4 + ((tx & 7) >> 1)];
          const idx = tx & 1 ? b >> 4 : b & 15;
          return idx ? 0x10000 | (pal[256 + palBank + idx] & 0x7fff) : 0;
        };
        const put = (x, v) => {
          if (omode === 2) { win[x] = 1; return; }
          const cur = obj[x];
          if (cur && ((cur >>> 17) & 3) <= prio) return;
          obj[x] = v | (prio << 17) | (omode === 1 ? 0x80000 : 0);
        };
        if (affine) {
          const p = ((a1 >> 9) & 31) * 16;
          const pa = (oam[p + 3] << 16) >> 16, pb = (oam[p + 7] << 16) >> 16, pc = (oam[p + 11] << 16) >> 16, pd = (oam[p + 15] << 16) >> 16;
          const cx = bw >> 1, cy = bh >> 1, dy = line - cy;
          for (let ix = 0; ix < bw; ix++) {
            const x = sx + ix;
            if (x < 0 || x >= 240) continue;
            const dx = ix - cx;
            const tx = ((pa * dx + pb * dy) >> 8) + (w >> 1), ty = ((pc * dx + pd * dy) >> 8) + (h >> 1);
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            const v = pix(tx, ty);
            if (v) put(x, v);
          }
        } else {
          const hf = a1 & 0x1000, vf = a1 & 0x2000;
          const ty = vf ? h - 1 - line : line;
          for (let ix = 0; ix < w; ix++) {
            const x = sx + ix;
            if (x < 0 || x >= 240) continue;
            let mx = ix; if (mos) mx -= x % mosH;
            const tx = hf ? w - 1 - mx : mx;
            const v = pix(tx, ty);
            if (v) put(x, v);
          }
        }
      }
    }
    saveState() { return { affX: this.affX.slice(), affY: this.affY.slice() }; }
    loadState(o) { this.affX.set(o.affX); this.affY.set(o.affY); }
  }
  function blend(a, b, eva, evb) {
    let r = ((a & 31) * eva + (b & 31) * evb) >> 4, gg = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4, bb = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31; if (gg > 31) gg = 31; if (bb > 31) bb = 31;
    return r | (gg << 5) | (bb << 10);
  }
  function brighten(a, e) {
    const r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    return (r + (((31 - r) * e) >> 4)) | ((g + (((31 - g) * e) >> 4)) << 5) | ((b + (((31 - b) * e) >> 4)) << 10);
  }
  function darken(a, e) {
    const r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    return (r - ((r * e) >> 4)) | ((g - ((g * e) >> 4)) << 5) | ((b - ((b * e) >> 4)) << 10);
  }
  global.RPGBAParts.PPU = PPU;
})(typeof window !== "undefined" ? window : globalThis);

// ---------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------
(function (global) {
  const P = global.RPGBAParts;
  const { BIOS_WORDS, Save, RTC, APU, PPU, LINE_CYCLES, HDRAW, FRAME_LINES } = P;
  const PRESCALE = [1, 64, 256, 1024];
  const WS_N = [4, 3, 2, 8];
  const BUTTONS = ["A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"];

  class GBA {
    constructor(romIn, opts) {
      opts = opts || {};
      let size = 4; while (size < romIn.length) size <<= 1;
      this.romSize = romIn.length;
      const romBuf = new ArrayBuffer(Math.max(size, 0x100));
      this.rom = new Uint8Array(romBuf); this.rom.set(romIn);
      this.rom16 = new Uint16Array(romBuf); this.rom32 = new Int32Array(romBuf);
      this.romMask = size - 1;
      const mk = (n) => { const b = new ArrayBuffer(n); return [new Uint8Array(b), new Uint16Array(b), new Int32Array(b)]; };
      [this.bios, this.bios16, this.bios32] = mk(0x4000);
      BIOS_WORDS.forEach((w, i) => { this.bios32[i] = w; });
      [this.ewram, this.ewram16, this.ewram32] = mk(0x40000);
      [this.iwram, this.iwram16, this.iwram32] = mk(0x8000);
      [this.pal, this.pal16, this.pal32] = mk(0x400);
      [this.vram, this.vram16, this.vram32] = mk(0x18000);
      [this.oam, this.oam16, this.oam32] = mk(0x400);
      [this.io, this.io16, this.io32] = mk(0x400);
      this.save = new Save(this.rom);
      if (opts.sram && opts.sram.length) this.save.load(opts.sram);
      this.rtc = this.save.rtc ? new RTC() : null;
      this.eepromBase = this.romSize > 0x1000000 ? 0x0dffff00 : 0x0d000000;
      this.sampleRate = opts.sampleRate || 48000;
      this.apu = new APU(this, this.sampleRate);
      this.ppu = new PPU(this);
      const self = this;
      this.cpu = new global.RPARM7({
        read8: (a) => self.read8(a), read16: (a) => self.read16(a), read32: (a) => self.read32(a),
        write8: (a, v) => self.write8(a, v), write16: (a, v) => self.write16(a, v), write32: (a, v) => self.write32(a, v),
        fetch16: (a) => self.fetch16(a), fetch32: (a) => self.fetch32(a),
        branched: (a, t) => self.branched(a, t),
        swi: (n) => self.swi(n),
      });
      this.width = 240; this.height = 160; this.fps = 16777216 / (LINE_CYCLES * FRAME_LINES);
      this.screen = new Uint32Array(240 * 160);
      this.keys = 0;
      // timing tables (per 16 MB region)
      this.n16 = new Uint8Array(16); this.s16 = new Uint8Array(16); this.n32 = new Uint8Array(16); this.s32 = new Uint8Array(16);
      this.f16 = new Uint8Array(16); this.f32 = new Uint8Array(16);
      this.reset();
    }

    reset() {
      for (const m of [this.ewram, this.iwram, this.pal, this.vram, this.oam, this.io]) m.fill(0);
      this.timers = [0, 1, 2, 3].map(() => ({ reload: 0, ctrl: 0, count: 0, start: 0 }));
      this.dma = [0, 1, 2, 3].map(() => ({ src: 0, dst: 0, count: 0, pending: false }));
      this.apu.reset(); this.ppu.affX.fill(0); this.ppu.affY.fill(0);
      this.time = 0; this.line = 0; this.lineStart = 0; this.nextTimer = Infinity;
      const c = this.cpu;
      c.reset();
      // state the real start-up code leaves behind
      c.switchMode(0x13); c.r[13] = 0x03007fe0; c.r[14] = 0;
      c.switchMode(0x12); c.r[13] = 0x03007fa0; c.r[14] = 0;
      c.switchMode(0x1f); c.r[13] = 0x03007f00;
      c.i = 0; c.f = 0; c.t = 0;
      c.cycles = 0;
      c.branch(0x08000000);
      this.io16[0] = 0x80;
      this.io16[0x20 >> 1] = 0x100; this.io16[0x26 >> 1] = 0x100; this.io16[0x30 >> 1] = 0x100; this.io16[0x36 >> 1] = 0x100;
      this.io16[0x88 >> 1] = 0x200;
      this.io16[0x134 >> 1] = 0x8000; // link port idle (general-purpose mode)
      this.io[0x300] = 1;
      this.updateWait(0);
      this.lastFetch = 0; this.biosLatch = 0xe129f000 | 0;
    }

    // ---------- timing ----------
    updateWait(w) {
      this.io16[0x204 >> 1] = w;
      const sram = 1 + WS_N[w & 3];
      const ws = [[1 + WS_N[(w >> 2) & 3], 1 + [2, 1][(w >> 4) & 1]], [1 + WS_N[(w >> 5) & 3], 1 + [4, 1][(w >> 7) & 1]], [1 + WS_N[(w >> 8) & 3], 1 + [8, 1][(w >> 10) & 1]]];
      const pf = (w & 0x4000) !== 0;
      const set = (r, n16, s16, n32, s32) => { this.n16[r] = n16; this.s16[r] = s16; this.n32[r] = n32; this.s32[r] = s32; this.f16[r] = s16; this.f32[r] = s32; };
      for (let r = 0; r < 16; r++) set(r, 1, 1, 1, 1);
      set(2, 3, 3, 6, 6); set(5, 1, 1, 2, 2); set(6, 1, 1, 2, 2);
      for (let k = 0; k < 3; k++) for (const r of [8 + k * 2, 9 + k * 2]) {
        const [n, s] = ws[k];
        set(r, n, s, n + s, s * 2);
        if (pf) { this.f16[r] = 1; this.f32[r] = 2; }
      }
      set(14, sram, sram, sram, sram); set(15, sram, sram, sram, sram);
    }
    branched(a, thumb) {
      const r = (a >>> 24) & 15;
      this.cpu.cycles += thumb ? this.n16[r] - this.f16[r] : this.n32[r] - this.f32[r];
    }

    // ---------- instruction fetch ----------
    fetch16(a) {
      const r = (a >>> 24) & 15;
      this.cpu.cycles += this.f16[r];
      let v;
      switch (r) {
        case 0: v = a < 0x4000 ? this.bios16[(a & 0x3fff) >> 1] : this.lastFetch & 0xffff; if (a < 0x4000) this.biosLatch = this.bios32[(a & 0x3ffc) >> 2]; break;
        case 2: v = this.ewram16[(a & 0x3ffff) >> 1]; break;
        case 3: v = this.iwram16[(a & 0x7fff) >> 1]; break;
        case 8: case 9: case 10: case 11: case 12: case 13: v = this.romRead16(a); break;
        default: v = this.read16raw(a);
      }
      this.lastFetch = v | (v << 16);
      return v;
    }
    fetch32(a) {
      const r = (a >>> 24) & 15;
      this.cpu.cycles += this.f32[r];
      let v;
      switch (r) {
        case 0: v = a < 0x4000 ? this.bios32[(a & 0x3ffc) >> 2] : this.lastFetch; if (a < 0x4000) this.biosLatch = v; break;
        case 2: v = this.ewram32[(a & 0x3fffc) >> 2]; break;
        case 3: v = this.iwram32[(a & 0x7ffc) >> 2]; break;
        case 8: case 9: case 10: case 11: case 12: case 13: v = this.romRead32(a); break;
        default: v = this.read32raw(a);
      }
      this.lastFetch = v;
      return v;
    }
    romRead16(a) {
      const o = a & 0x1fffffe;
      if (this.rtc && o >= 0xc4 && o <= 0xc8 && (this.rtc.ctrl & 1)) return o === 0xc4 ? this.rtc.read() : o === 0xc6 ? this.rtc.dir : this.rtc.ctrl;
      if (o >= this.rom.length) return (o >> 1) & 0xffff;
      return this.rom16[o >> 1];
    }
    romRead32(a) {
      const o = a & 0x1fffffc;
      if (o >= this.rom.length || (this.rtc && o >= 0xc4 && o <= 0xc8)) return (this.romRead16(o) | (this.romRead16(o + 2) << 16));
      return this.rom32[o >> 2];
    }

    // ---------- data access (charged) ----------
    read8(a) { a >>>= 0; this.cpu.cycles += this.n16[(a >>> 24) & 15]; return this.read8raw(a); }
    read16(a) { a >>>= 0; this.cpu.cycles += this.n16[(a >>> 24) & 15]; return this.read16raw(a); }
    read32(a) { a >>>= 0; this.cpu.cycles += this.n32[(a >>> 24) & 15]; return this.read32raw(a); }
    write8(a, v) { a >>>= 0; this.cpu.cycles += this.n16[(a >>> 24) & 15]; this.write8raw(a, v); }
    write16(a, v) { a >>>= 0; this.cpu.cycles += this.n16[(a >>> 24) & 15]; this.write16raw(a, v); }
    write32(a, v) { a >>>= 0; this.cpu.cycles += this.n32[(a >>> 24) & 15]; this.write32raw(a, v); }

    openBus(a) { return this.cpu.t ? this.lastFetch : this.lastFetch; }
    biosRead32(a) { return this.cpu.pc < 0x4000 ? this.bios32[(a & 0x3ffc) >> 2] : this.biosLatch; }

    read8raw(a) {
      switch (a >>> 24) {
        case 0: return a < 0x4000 ? (this.biosRead32(a) >>> ((a & 3) * 8)) & 0xff : (this.openBus(a) >>> ((a & 3) * 8)) & 0xff;
        case 2: return this.ewram[a & 0x3ffff];
        case 3: return this.iwram[a & 0x7fff];
        case 4: return (a & 0xffffff) < 0x400 ? (this.ioRead16(a & 0x3fe) >> ((a & 1) * 8)) & 0xff : 0;
        case 5: return this.pal[a & 0x3ff];
        case 6: return this.vram[this.vaddr(a)];
        case 7: return this.oam[a & 0x3ff];
        case 8: case 9: case 10: case 11: case 12: return (this.romRead16(a) >> ((a & 1) * 8)) & 0xff;
        case 13: if (this.save.type === "eeprom" && a >= this.eepromBase) return this.save.eepromRead(); return (this.romRead16(a) >> ((a & 1) * 8)) & 0xff;
        case 14: case 15: return this.save.read8(a);
        default: return (this.openBus(a) >>> ((a & 3) * 8)) & 0xff;
      }
    }
    read16raw(a) {
      switch (a >>> 24) {
        case 0: return a < 0x4000 ? (this.biosRead32(a) >>> ((a & 2) * 8)) & 0xffff : (this.openBus(a) >>> ((a & 2) * 8)) & 0xffff;
        case 2: return this.ewram16[(a & 0x3ffff) >> 1];
        case 3: return this.iwram16[(a & 0x7fff) >> 1];
        case 4: return (a & 0xffffff) < 0x400 ? this.ioRead16(a & 0x3fe) : 0;
        case 5: return this.pal16[(a & 0x3ff) >> 1];
        case 6: return this.vram16[this.vaddr(a) >> 1];
        case 7: return this.oam16[(a & 0x3ff) >> 1];
        case 8: case 9: case 10: case 11: case 12: return this.romRead16(a);
        case 13: if (this.save.type === "eeprom" && a >= this.eepromBase) return this.save.eepromRead(); return this.romRead16(a);
        case 14: case 15: return this.save.read8(a) * 0x101;
        default: return (this.openBus(a) >>> ((a & 2) * 8)) & 0xffff;
      }
    }
    read32raw(a) {
      switch (a >>> 24) {
        case 0: return a < 0x4000 ? this.biosRead32(a) : this.openBus(a);
        case 2: return this.ewram32[(a & 0x3fffc) >> 2];
        case 3: return this.iwram32[(a & 0x7ffc) >> 2];
        case 4: { const o = a & 0x3fc; return (a & 0xffffff) < 0x400 ? this.ioRead16(o) | (this.ioRead16(o + 2) << 16) : 0; }
        case 5: return this.pal32[(a & 0x3fc) >> 2];
        case 6: return this.vram32[this.vaddr(a) >> 2];
        case 7: return this.oam32[(a & 0x3fc) >> 2];
        case 8: case 9: case 10: case 11: case 12: return this.romRead32(a);
        case 13: if (this.save.type === "eeprom" && a >= this.eepromBase) return this.save.eepromRead(); return this.romRead32(a);
        case 14: case 15: return this.save.read8(a) * 0x1010101;
        default: return this.openBus(a);
      }
    }
    vaddr(a) { a &= 0x1ffff; return a >= 0x18000 ? a - 0x8000 : a; }

    write8raw(a, v) {
      v &= 0xff;
      switch (a >>> 24) {
        case 2: this.ewram[a & 0x3ffff] = v; return;
        case 3: this.iwram[a & 0x7fff] = v; return;
        case 4: if ((a & 0xffffff) < 0x400) this.ioWrite8(a & 0x3ff, v); return;
        case 5: this.pal16[(a & 0x3fe) >> 1] = v * 0x101; return;
        case 6: { const o = this.vaddr(a); const lim = (this.io16[0] & 7) >= 3 ? 0x14000 : 0x10000; if (o < lim) this.vram16[o >> 1] = v * 0x101; return; }
        case 8: if (this.rtc && (a & 0x1ffffff) >= 0xc4 && (a & 0x1ffffff) <= 0xc9) this.gpioWrite(a & 0x1fffffe, v); return;
        case 14: case 15: this.save.write8(a, v); return;
      }
    }
    write16raw(a, v) {
      v &= 0xffff;
      switch (a >>> 24) {
        case 2: this.ewram16[(a & 0x3ffff) >> 1] = v; return;
        case 3: this.iwram16[(a & 0x7fff) >> 1] = v; return;
        case 4: if ((a & 0xffffff) < 0x400) this.ioWrite16(a & 0x3fe, v); return;
        case 5: this.pal16[(a & 0x3ff) >> 1] = v; return;
        case 6: this.vram16[this.vaddr(a) >> 1] = v; return;
        case 7: this.oam16[(a & 0x3ff) >> 1] = v; return;
        case 8: if (this.rtc && (a & 0x1fffffe) >= 0xc4 && (a & 0x1fffffe) <= 0xc8) this.gpioWrite(a & 0x1fffffe, v); return;
        case 13: if (this.save.type === "eeprom" && a >= this.eepromBase) this.save.eepromWrite(v & 1); return;
        case 14: case 15: this.save.write8(a, (v >>> ((a & 1) * 8)) & 0xff); return;
      }
    }
    write32raw(a, v) {
      switch (a >>> 24) {
        case 2: this.ewram32[(a & 0x3fffc) >> 2] = v; return;
        case 3: this.iwram32[(a & 0x7ffc) >> 2] = v; return;
        case 4: if ((a & 0xffffff) < 0x400) { const o = a & 0x3fc; if (o === 0xa0 || o === 0xa4) { this.apu.fifoWrite(o === 0xa0 ? 0 : 1, v); return; } this.ioWrite16(o, v & 0xffff); this.ioWrite16(o + 2, (v >>> 16) & 0xffff); } return;
        case 5: this.pal32[(a & 0x3fc) >> 2] = v; return;
        case 6: this.vram32[this.vaddr(a) >> 2] = v; return;
        case 7: this.oam32[(a & 0x3fc) >> 2] = v; return;
        case 8: if (this.rtc) { this.write16raw(a & ~3, v & 0xffff); this.write16raw((a & ~3) + 2, v >>> 16); } return;
        case 13: if (this.save.type === "eeprom" && a >= this.eepromBase) this.save.eepromWrite(v & 1); return;
        case 14: case 15: this.save.write8(a, (v >>> ((a & 3) * 8)) & 0xff); return;
      }
    }
    gpioWrite(o, v) {
      const r = this.rtc;
      if (o === 0xc4) r.write(v & 15);
      else if (o === 0xc6) r.dir = v & 15;
      else if (o === 0xc8) r.ctrl = v & 1;
    }

    // ---------- I/O registers ----------
    ioRead16(o) {
      const io = this.io16;
      switch (o) {
        case 0x004: {
          const pos = this.cpu.cycles - this.lineStart;
          const vb = this.line >= 160 && this.line < 227 ? 1 : 0, hb = pos >= HDRAW ? 2 : 0;
          const lyc = io[2] >> 8, vc = this.line === lyc ? 4 : 0;
          return (io[2] & 0xff38) | vb | hb | vc;
        }
        case 0x006: return this.line;
        case 0x084: return (io[0x42] & 0x80) | this.apu.ch.reduce((m, c, i) => m | (c.on ? 1 << i : 0), 0);
        case 0x100: case 0x104: case 0x108: case 0x10c: return this.timerCount((o - 0x100) >> 2);
        case 0x130: return ~this.keys & 0x3ff;
        case 0x202: return io[0x101];
        case 0x10: case 0x12: case 0x14: case 0x16: case 0x18: case 0x1a: case 0x1c: case 0x1e: return 0;
      }
      if (o >= 0x90 && o < 0xa0) { const i = o - 0x90; return this.apu.waveRead(i) | (this.apu.waveRead(i + 1) << 8); }
      if (o >= 0xa0 && o < 0xe0) {
        // DMA registers: only the control halves read back
        if (o === 0xba || o === 0xc6 || o === 0xd2 || o === 0xde) return io[o >> 1];
        return 0;
      }
      return io[o >> 1];
    }
    ioWrite8(o, v) {
      if (o === 0x301) { this.cpu.halted = true; return; } // HALTCNT
      if (o === 0x202 || o === 0x203) { this.io16[0x101] &= ~(v << ((o & 1) * 8)); this.updateIrq(); return; }
      if (o >= 0xa0 && o < 0xa8) { this.apu.fifoWrite(o < 0xa4 ? 0 : 1, v, 1); return; }
      const cur = this.ioRead16raw(o & ~1);
      const nv = (o & 1) ? (cur & 0xff) | (v << 8) : (cur & 0xff00) | v;
      this.ioWrite16(o & ~1, nv);
    }
    ioRead16raw(o) { return this.io16[o >> 1]; }
    ioWrite16(o, v) {
      const io = this.io16;
      if (o >= 0x60 && o <= 0x84) {
        this.apu.advance(this.cpu.cycles);
        if (o === 0x84) { const old = io[0x42]; this.apu.write16(0x84, v); io[0x42] = (old & ~0x80) | (v & 0x80); return; }
        io[o >> 1] = v;
        this.apu.write16(o, v);
        return;
      }
      if (o >= 0x90 && o < 0xa0) { this.apu.advance(this.cpu.cycles); const i = o - 0x90; this.apu.waveWrite(i, v & 0xff); this.apu.waveWrite(i + 1, v >> 8); return; }
      if (o >= 0xa0 && o < 0xa8) { this.apu.fifoWrite(o < 0xa4 ? 0 : 1, v, 2); return; }
      switch (o) {
        case 0x004: io[2] = (io[2] & 7) | (v & 0xff38); return;
        case 0x006: return;
        case 0x028: case 0x02a: io[o >> 1] = v; this.ppu.writeRef(0, false); return;
        case 0x02c: case 0x02e: io[o >> 1] = v; this.ppu.writeRef(0, true); return;
        case 0x038: case 0x03a: io[o >> 1] = v; this.ppu.writeRef(1, false); return;
        case 0x03c: case 0x03e: io[o >> 1] = v; this.ppu.writeRef(1, true); return;
        case 0x0ba: case 0x0c6: case 0x0d2: case 0x0de: this.dmaControl((o - 0xba) / 12, v); return;
        case 0x100: case 0x104: case 0x108: case 0x10c: this.syncTimers(); this.timers[(o - 0x100) >> 2].reload = v; return;
        case 0x102: case 0x106: case 0x10a: case 0x10e: this.timerControl((o - 0x102) >> 2, v); return;
        case 0x128: { // SIOCNT: with no link cable attached, transfers finish on their own
          io[0x94] = v;
          const mode = (v >> 12) & 3, multi = mode === 2 && !(io[0x134 >> 1] & 0x8000);
          if ((v & 0x80) && (multi || (v & 1))) {
            if (multi) { io[0x90] = io[0x95]; io[0x91] = io[0x92] = io[0x93] = 0xffff; }
            else { io[0x90] = 0xffff; io[0x91] = 0xffff; }
            io[0x94] = v & ~0x80;
            if (v & 0x4000) this.raise(0x80);
          }
          if (mode === 2) io[0x94] &= ~0x0c; // nothing plugged in: this GBA is the parent and no one else is ready
          return;
        }
        case 0x130: return;
        case 0x132: io[0x99] = v; this.checkKeyIrq(); return;
        case 0x200: io[0x100] = v; this.updateIrq(); return;
        case 0x202: io[0x101] &= ~v; this.updateIrq(); return;
        case 0x204: this.updateWait(v & 0x5fff); return;
        case 0x208: io[0x104] = v & 1; this.updateIrq(); return;
        case 0x300: io[0x180] = (io[0x180] & 0xff00) | (v & 1); if (v & 0xff00) this.cpu.halted = true; return;
      }
      io[o >> 1] = v;
    }

    // ---------- interrupts ----------
    raise(bit) { this.io16[0x101] |= bit; this.updateIrq(); }
    updateIrq() {
      const io = this.io16, pend = io[0x100] & io[0x101] & 0x3fff;
      this.cpu.irqLine = !!(pend && (io[0x104] & 1));
      if (pend) this.cpu.halted = false;
    }
    checkKeyIrq() {
      const cnt = this.io16[0x99];
      if (!(cnt & 0x4000)) return;
      const sel = cnt & 0x3ff, pressed = this.keys & sel;
      if ((cnt & 0x8000) ? pressed === sel : pressed !== 0) this.raise(0x1000);
    }

    // ---------- timers ----------
    timerCount(i) {
      const t = this.timers[i];
      if ((t.ctrl & 0x80) && !(t.ctrl & 4) ) return (t.count + Math.floor((this.cpu.cycles - t.start) / PRESCALE[t.ctrl & 3])) & 0xffff;
      return t.count & 0xffff;
    }
    syncTimers() { this.runTimers(this.cpu.cycles); }
    timerControl(i, v) {
      this.syncTimers();
      const t = this.timers[i], was = t.ctrl & 0x80;
      if ((t.ctrl & 0x80) && !(t.ctrl & 4)) { t.count = this.timerCount(i); t.start = this.cpu.cycles; }
      t.ctrl = v & 0xc7;
      this.io16[(0x102 + i * 4) >> 1] = t.ctrl;
      if (!was && (v & 0x80)) { t.count = t.reload; t.start = this.cpu.cycles; }
      this.scheduleTimers();
    }
    scheduleTimers() {
      let next = Infinity;
      for (let i = 0; i < 4; i++) {
        const t = this.timers[i];
        if (!(t.ctrl & 0x80) || ((t.ctrl & 4) && i > 0)) continue;
        const ov = t.start + (0x10000 - t.count) * PRESCALE[t.ctrl & 3];
        if (ov < next) next = ov;
      }
      this.nextTimer = next;
    }
    timerOverflow(i, when) {
      const t = this.timers[i];
      if (t.ctrl & 0x40) this.raise(8 << i);
      if (i < 2) { this.apu.advance(when); this.apu.timerOverflow(i); }
      if (i < 3) {
        const n = this.timers[i + 1];
        if ((n.ctrl & 0x84) === 0x84) { n.count++; if (n.count > 0xffff) { n.count = n.reload; this.timerOverflow(i + 1, when); } }
      }
    }
    runTimers(now) {
      for (let guard = 0; guard < 100000 && this.nextTimer <= now; guard++) {
        // handle the earliest overflow
        let best = -1, bestT = Infinity;
        for (let i = 0; i < 4; i++) {
          const t = this.timers[i];
          if (!(t.ctrl & 0x80) || ((t.ctrl & 4) && i > 0)) continue;
          const ov = t.start + (0x10000 - t.count) * PRESCALE[t.ctrl & 3];
          if (ov < bestT) { bestT = ov; best = i; }
        }
        if (best < 0 || bestT > now) break;
        const t = this.timers[best];
        t.count = t.reload; t.start = bestT;
        this.timerOverflow(best, bestT);
        this.scheduleTimers();
      }
      this.scheduleTimers();
    }

    // ---------- DMA ----------
    dmaControl(i, v) {
      const io = this.io16, base = (0xb0 + i * 12) >> 1, d = this.dma[i];
      const old = io[base + 5];
      io[base + 5] = v;
      if (!(old & 0x8000) && (v & 0x8000)) {
        d.src = (io[base] | (io[base + 1] << 16)) & (i === 0 ? 0x07ffffff : 0x0fffffff);
        d.dst = (io[base + 2] | (io[base + 3] << 16)) & (i === 3 ? 0x0fffffff : 0x07ffffff);
        d.count = io[base + 4] || (i === 3 ? 0x10000 : 0x4000);
        if (i < 3) d.count &= 0x3fff, d.count = d.count || 0x4000;
        if (((v >> 12) & 3) === 0) this.runDma(i);
      }
    }
    triggerDma(timing) {
      for (let i = 0; i < 4; i++) {
        const cnt = this.io16[((0xb0 + i * 12) >> 1) + 5];
        if ((cnt & 0x8000) && ((cnt >> 12) & 3) === timing) this.runDma(i);
      }
    }
    fifoRequest(k) {
      const i = k + 1, cnt = this.io16[((0xb0 + i * 12) >> 1) + 5];
      if ((cnt & 0x8000) && ((cnt >> 12) & 3) === 3) this.runDma(i);
    }
    runDma(i) {
      const io = this.io16, base = (0xb0 + i * 12) >> 1, d = this.dma[i];
      const cnt = io[base + 5];
      const fifo = (i === 1 || i === 2) && ((cnt >> 12) & 3) === 3;
      const word = fifo || (cnt & 0x400);
      const dctl = fifo ? 2 : (cnt >> 5) & 3, sctl = (cnt >> 7) & 3;
      const unit = word ? 4 : 2;
      const sstep = sctl === 1 ? -unit : sctl === 2 ? 0 : unit;
      const dstep = dctl === 1 ? -unit : dctl === 2 ? 0 : unit;
      let n = fifo ? 4 : d.count;
      let src = d.src >>> 0, dst = d.dst >>> 0;
      const eeprom = this.save.type === "eeprom" && ((dst >>> 24) === 13 || (src >>> 24) === 13);
      const cyc = this.cpu.cycles;
      if (word) {
        for (let k = 0; k < n; k++) { this.write32(dst & ~3, this.read32(src & ~3)); src = (src + sstep) >>> 0; dst = (dst + dstep) >>> 0; }
      } else {
        for (let k = 0; k < n; k++) { this.write16(dst & ~1, this.read16(src & ~1)); src = (src + sstep) >>> 0; dst = (dst + dstep) >>> 0; }
      }
      this.cpu.cycles += 2;
      void cyc;
      d.src = src;
      d.dst = fifo ? d.dst : dst;
      if (eeprom) this.save.eepromDmaDone(n);
      if ((cnt & 0x200) && ((cnt >> 12) & 3) !== 0) {
        d.count = io[base + 4] || (i === 3 ? 0x10000 : 0x4000);
        if (i < 3) d.count = (d.count & 0x3fff) || 0x4000;
        if (dctl === 3) d.dst = (io[base + 2] | (io[base + 3] << 16)) & (i === 3 ? 0x0fffffff : 0x07ffffff);
      } else io[base + 5] = cnt & 0x7fff;
      if (cnt & 0x4000) this.raise(0x100 << i);
    }

    // ---------- running ----------
    runUntil(t) {
      const cpu = this.cpu;
      while (cpu.cycles < t) {
        const stop = Math.min(t, this.nextTimer);
        if (cpu.halted) { if (cpu.cycles < stop) cpu.cycles = stop; }
        else while (cpu.cycles < stop) { cpu.step(); if (cpu.halted) break; }
        if (cpu.cycles >= this.nextTimer) this.runTimers(cpu.cycles);
      }
    }
    runFrame() {
      const io = this.io16, ppu = this.ppu;
      for (let line = 0; line < FRAME_LINES; line++) {
        this.line = line;
        this.lineStart = this.time + line * LINE_CYCLES;
        const stat = io[2];
        if (line === (stat >> 8) && (stat & 0x20)) this.raise(4);
        if (line === 160) {
          if (stat & 8) this.raise(1);
          this.triggerDma(1);
        }
        if (line === 0) ppu.latchAffine();
        this.runUntil(this.lineStart + HDRAW);
        if (line < 160) ppu.renderLine(line, this.screen);
        if (io[2] & 0x10) this.raise(2);
        if (line < 160) this.triggerDma(2);
        this.runUntil(this.lineStart + LINE_CYCLES);
      }
      this.time += LINE_CYCLES * FRAME_LINES;
      this.apu.advance(this.cpu.cycles);
    }

    setInput(p, bits) {
      if (p !== 0) return;
      const was = this.keys;
      this.keys = bits & 0x3ff;
      if (this.keys !== was) this.checkKeyIrq();
    }
    takeAudio() { const a = this.apu.out.slice(0, this.apu.outLen); this.apu.outLen = 0; return a; }
    getSRAM() { return this.save.type === "none" && !this.save.dirty ? null : this.save.bytes(); }

    saveState() {
      return {
        v: 1, cpu: this.cpu.saveState(), ewram: this.ewram.slice(), iwram: this.iwram.slice(), pal: this.pal.slice(), vram: this.vram.slice(), oam: this.oam.slice(), io: this.io.slice(),
        timers: this.timers.map((t) => Object.assign({}, t)), dma: this.dma.map((d) => Object.assign({}, d)), apu: this.apu.saveState(), ppu: this.ppu.saveState(),
        save: this.save.saveState(), rtc: this.rtc ? this.rtc.saveState() : null,
        m: [this.time, this.line, this.lineStart, this.lastFetch, this.biosLatch, this.keys, this.waiting ? 1 : 0],
      };
    }
    loadState(s) {
      this.cpu.loadState(s.cpu);
      this.ewram.set(s.ewram); this.iwram.set(s.iwram); this.pal.set(s.pal); this.vram.set(s.vram); this.oam.set(s.oam); this.io.set(s.io);
      this.timers = s.timers.map((t) => Object.assign({}, t)); this.dma = s.dma.map((d) => Object.assign({}, d));
      this.apu.loadState(s.apu); this.ppu.loadState(s.ppu); this.save.loadState(s.save);
      if (this.rtc && s.rtc) this.rtc.loadState(s.rtc);
      [this.time, this.line, this.lineStart, this.lastFetch, this.biosLatch] = s.m;
      this.waiting = !!s.m[6];
      this.updateWait(this.io16[0x204 >> 1]);
      this.scheduleTimers();
    }
  }
  GBA.BUTTONS = BUTTONS;
  P.GBA = GBA;
})(typeof window !== "undefined" ? window : globalThis);

// ---------------------------------------------------------------------
// RetroPixels BIOS calls (written from GBATEK's description of each call)
// ---------------------------------------------------------------------
(function (global) {
  const P = global.RPGBAParts, GBA = P.GBA;
  const SIN = new Int16Array(256);
  for (let i = 0; i < 256; i++) SIN[i] = Math.round(Math.sin(i * Math.PI / 128) * 0x4000);

  GBA.prototype.swi = function (n) {
    const c = this.cpu, r = c.r;
    const rd8 = (a) => this.read8raw(a >>> 0), rd16 = (a) => this.read16raw((a >>> 0) & ~1), rd32 = (a) => this.read32raw((a >>> 0) & ~3);
    const wr8 = (a, v) => this.write8raw(a >>> 0, v), wr16 = (a, v) => this.write16raw((a >>> 0) & ~1, v), wr32 = (a, v) => this.write32raw((a >>> 0) & ~3, v);
    c.cycles += 24;
    this.biosLatch = 0xe3a02004 | 0;
    // like the real BIOS, copy / decompression calls refuse to read from the BIOS area
    if (((n >= 0x0b && n <= 0x0c) || (n >= 0x10 && n <= 0x18)) && ((r[0] >>> 0) & 0x0e000000) === 0) return true;
    switch (n) {
      case 0x00: { // SoftReset
        const toRam = rd8(0x03007ffa);
        for (let a = 0x03007e00; a < 0x03008000; a += 4) wr32(a, 0);
        c.switchMode(0x13); r[13] = 0x03007fe0; r[14] = 0;
        c.switchMode(0x12); r[13] = 0x03007fa0; r[14] = 0;
        c.switchMode(0x1f); r[13] = 0x03007f00;
        for (let k = 0; k < 13; k++) r[k] = 0;
        c.t = 0; c.i = 0;
        c.branch(toRam ? 0x02000000 : 0x08000000);
        break;
      }
      case 0x01: { // RegisterRamReset
        const f = r[0];
        if (f & 1) this.ewram.fill(0);
        if (f & 2) this.iwram.fill(0, 0, 0x7e00);
        if (f & 4) this.pal.fill(0);
        if (f & 8) this.vram.fill(0);
        if (f & 16) this.oam.fill(0);
        if (f & 64) for (let a = 0x60; a < 0xb0; a += 2) this.io16[a >> 1] = 0;
        if (f & 128) { for (let a = 0; a < 0x60; a += 2) this.io16[a >> 1] = 0; this.io16[0] = 0x80; this.io16[0x10] = this.io16[0x13] = this.io16[0x18] = this.io16[0x1b] = 0x100; }
        break;
      }
      case 0x02: case 0x03: c.halted = true; break; // Halt / Stop
      case 0x04: this.intrWait(r[0] & 1, r[1] & 0x3fff); break;
      case 0x05: this.intrWait(1, 1); break;
      case 0x06: case 0x07: { // Div / DivArm
        const num = n === 6 ? r[0] : r[1], den = n === 6 ? r[1] : r[0];
        if (den === 0) { r[0] = num < 0 ? -1 : 1; r[1] = num; r[3] = 1; break; }
        const q = (num / den) | 0;
        r[0] = q; r[1] = (num - Math.imul(q, den)) | 0; r[3] = Math.abs(q) | 0;
        c.cycles += 40;
        break;
      }
      case 0x08: r[0] = Math.floor(Math.sqrt(r[0] >>> 0)); c.cycles += 30; break;
      case 0x09: r[0] = arcTan(r[0]); break;
      case 0x0a: { // ArcTan2(x, y)
        const x = (r[0] << 16) >> 16, y = (r[1] << 16) >> 16;
        let a = Math.atan2(y, x); if (a < 0) a += 2 * Math.PI;
        r[0] = Math.round(a / (2 * Math.PI) * 0x10000) & 0xffff;
        break;
      }
      case 0x0b: { // CpuSet
        let src = r[0] >>> 0, dst = r[1] >>> 0;
        const cnt = r[2] & 0x1fffff, fill = r[2] & 0x1000000, word = r[2] & 0x4000000;
        if (word) { src &= ~3; dst &= ~3; const v0 = fill ? rd32(src) : 0; for (let k = 0; k < cnt; k++) { wr32(dst, fill ? v0 : rd32(src)); dst += 4; if (!fill) src += 4; } }
        else { src &= ~1; dst &= ~1; const v0 = fill ? rd16(src) : 0; for (let k = 0; k < cnt; k++) { wr16(dst, fill ? v0 : rd16(src)); dst += 2; if (!fill) src += 2; } }
        c.cycles += cnt * 4;
        break;
      }
      case 0x0c: { // CpuFastSet
        let src = (r[0] >>> 0) & ~3, dst = (r[1] >>> 0) & ~3;
        const cnt = ((r[2] & 0x1fffff) + 7) & ~7, fill = r[2] & 0x1000000;
        const v0 = fill ? rd32(src) : 0;
        for (let k = 0; k < cnt; k++) { wr32(dst, fill ? v0 : rd32(src)); dst += 4; if (!fill) src += 4; }
        c.cycles += cnt * 2;
        break;
      }
      case 0x0d: r[0] = 0xbaae187f | 0; break;
      case 0x0e: { // BgAffineSet
        let src = r[0] >>> 0, dst = r[1] >>> 0;
        for (let k = 0; k < r[2]; k++) {
          const ox = rd32(src), oy = rd32(src + 4), dx = (rd16(src + 8) << 16) >> 16, dy = (rd16(src + 10) << 16) >> 16;
          const sx = (rd16(src + 12) << 16) >> 16, sy = (rd16(src + 14) << 16) >> 16, th = (rd16(src + 16) >> 8) & 0xff;
          const s = SIN[th], co = SIN[(th + 64) & 255];
          const pa = (sx * co) >> 14, pb = -((sx * s) >> 14), pc = (sy * s) >> 14, pd = (sy * co) >> 14;
          wr16(dst, pa); wr16(dst + 2, pb); wr16(dst + 4, pc); wr16(dst + 6, pd);
          wr32(dst + 8, (ox - (pa * dx + pb * dy)) | 0); wr32(dst + 12, (oy - (pc * dx + pd * dy)) | 0);
          src += 20; dst += 16;
        }
        break;
      }
      case 0x0f: { // ObjAffineSet
        let src = r[0] >>> 0, dst = r[1] >>> 0;
        const step = r[3];
        for (let k = 0; k < r[2]; k++) {
          const sx = (rd16(src) << 16) >> 16, sy = (rd16(src + 2) << 16) >> 16, th = (rd16(src + 4) >> 8) & 0xff;
          const s = SIN[th], co = SIN[(th + 64) & 255];
          wr16(dst, (sx * co) >> 14); wr16(dst + step, -((sx * s) >> 14)); wr16(dst + step * 2, (sy * s) >> 14); wr16(dst + step * 3, (sy * co) >> 14);
          src += 8; dst += step * 4;
        }
        break;
      }
      case 0x10: { // BitUnPack
        let src = r[0] >>> 0, dst = r[1] >>> 0;
        const info = r[2] >>> 0, len = rd16(info), sw = rd8(info + 2), dw = rd8(info + 3), offw = rd32(info + 4);
        const off = offw & 0x7fffffff, zero = offw < 0;
        let acc = 0, bits = 0;
        for (let k = 0; k < len; k++) {
          const b = rd8(src + k);
          for (let s = 0; s < 8; s += sw) {
            let v = (b >> s) & ((1 << sw) - 1);
            if (v || zero) v += off;
            acc |= (v & (dw === 32 ? 0xffffffff : (1 << dw) - 1)) << bits;
            bits += dw;
            if (bits >= 32) { wr32(dst, acc); dst += 4; acc = 0; bits = 0; }
          }
        }
        break;
      }
      case 0x11: case 0x12: this.writeOut(r[1] >>> 0, lz77(rd8, r[0] >>> 0), n === 0x12); break;
      case 0x13: this.writeOut(r[1] >>> 0, huffman(rd8, rd32, r[0] >>> 0), false); break;
      case 0x14: case 0x15: this.writeOut(r[1] >>> 0, rle(rd8, r[0] >>> 0), n === 0x15); break;
      case 0x16: case 0x17: case 0x18: {
        const src = r[0] >>> 0, hdr = rd32(src), size = hdr >>> 8, out = new Uint8Array(size);
        if (n === 0x18) { let prev = 0; for (let k = 0; k + 1 < size; k += 2) { prev = (prev + rd16(src + 4 + k)) & 0xffff; out[k] = prev & 0xff; out[k + 1] = prev >> 8; } }
        else { let prev = 0; for (let k = 0; k < size; k++) { prev = (prev + rd8(src + 4 + k)) & 0xff; out[k] = prev; } }
        this.writeOut(r[1] >>> 0, out, n !== 0x16);
        break;
      }
      case 0x19: break; // SoundBias
      case 0x1f: { // MidiKey2Freq
        const freq = rd32(r[0] + 4) >>> 0;
        r[0] = Math.floor(freq / Math.pow(2, (180 - r[1] - r[2] / 256) / 12)) | 0;
        break;
      }
      default: break; // other sound-driver calls: nothing to do
    }
    return true;
  };
  GBA.prototype.intrWait = function (discard, mask) {
    const c = this.cpu;
    this.io16[0x104] = 1; // IME on
    let f = this.read16raw(0x03007ff8);
    if (!this.waiting) {
      this.waiting = true;
      if (discard) { f &= ~mask; this.write16raw(0x03007ff8, f); }
    }
    if (f & mask) { this.write16raw(0x03007ff8, f & ~mask); this.waiting = false; this.updateIrq(); return; }
    // not yet: sleep, and run this call again after the next interrupt
    c.branch((c.pc - (c.t ? 2 : 4)) >>> 0);
    c.halted = true;
    this.updateIrq();
  };
  // write decompressed bytes; VRAM destinations take 16-bit writes
  GBA.prototype.writeOut = function (dst, bytes, vram16) {
    if (vram16) for (let k = 0; k + 1 < bytes.length; k += 2) this.write16raw((dst + k) & ~1, bytes[k] | (bytes[k + 1] << 8));
    else for (let k = 0; k < bytes.length; k++) this.write8raw(dst + k, bytes[k]);
    this.cpu.cycles += bytes.length * 3;
  };
  function arcTan(v) {
    v = (v << 16) >> 16;
    const a = -((Math.imul(v, v)) >> 14);
    let b = ((Math.imul(0xa9, a)) >> 14) + 0x390;
    b = (Math.imul(b, a) >> 14) + 0x91c;
    b = (Math.imul(b, a) >> 14) + 0xfb6;
    b = (Math.imul(b, a) >> 14) + 0x16aa;
    b = (Math.imul(b, a) >> 14) + 0x2081;
    b = (Math.imul(b, a) >> 14) + 0x3651;
    b = (Math.imul(b, a) >> 14) + 0xa2f9;
    return Math.imul(v, b) >> 16;
  }
  function lz77(rd8, src) {
    const hdr = rd8(src) | (rd8(src + 1) << 8) | (rd8(src + 2) << 16) | (rd8(src + 3) << 24);
    const size = hdr >>> 8, out = new Uint8Array(size);
    let p = src + 4, o = 0;
    while (o < size) {
      const flags = rd8(p++);
      for (let b = 7; b >= 0 && o < size; b--) {
        if (flags & (1 << b)) {
          const b0 = rd8(p++), b1 = rd8(p++);
          const disp = (((b0 & 15) << 8) | b1) + 1, len = (b0 >> 4) + 3;
          for (let k = 0; k < len && o < size; k++, o++) out[o] = o - disp >= 0 ? out[o - disp] : 0;
        } else out[o++] = rd8(p++);
      }
    }
    return out;
  }
  function rle(rd8, src) {
    const hdr = rd8(src) | (rd8(src + 1) << 8) | (rd8(src + 2) << 16) | (rd8(src + 3) << 24);
    const size = hdr >>> 8, out = new Uint8Array(size);
    let p = src + 4, o = 0;
    while (o < size) {
      const f = rd8(p++);
      if (f & 0x80) { const len = (f & 0x7f) + 3, v = rd8(p++); for (let k = 0; k < len && o < size; k++) out[o++] = v; }
      else { const len = (f & 0x7f) + 1; for (let k = 0; k < len && o < size; k++) out[o++] = rd8(p++); }
    }
    return out;
  }
  function huffman(rd8, rd32, src) {
    const hdr = rd32(src) >>> 0, bitsPer = hdr & 15, size = hdr >>> 8, out = new Uint8Array(size);
    const treeSize = (rd8(src + 4) + 1) * 2, root = src + 5;
    let p = src + 4 + treeSize;
    let o = 0, acc = 0, accBits = 0, node = root, nodeVal = rd8(root);
    while (o < size) {
      const word = rd32(p) >>> 0; p += 4;
      for (let b = 31; b >= 0 && o < size; b--) {
        const bit = (word >>> b) & 1;
        const next = (node & ~1) + (nodeVal & 0x3f) * 2 + 2 + bit;
        const isData = bit ? (nodeVal & 0x40) : (nodeVal & 0x80);
        if (isData) {
          acc |= rd8(next) << accBits; accBits += bitsPer;
          if (accBits >= 32) { for (let k = 0; k < 4 && o < size; k++) out[o++] = (acc >>> (k * 8)) & 0xff; acc = 0; accBits = 0; }
          node = root; nodeVal = rd8(root);
        } else { node = next; nodeVal = rd8(next); }
      }
    }
    return out;
  }

  GBA.info = {
    system: "gba", name: "Game Boy Advance", width: 240, height: 160, aspect: 3 / 2, fps: 59.7275, stereo: true, players: 1,
    buttons: GBA.BUTTONS,
  };
  global.RPCores = global.RPCores || {};
  global.RPCores.gba = GBA;
})(typeof window !== "undefined" ? window : globalThis);
