// RetroPixels — Game Boy / Game Boy Color cartridge chips. Original code.
//
// The memory bank controllers and the extra hardware some cartridges carry,
// written from the documented behaviour of each chip:
//   MBC1 (and MBC1 multi-game carts), MBC2, MBC3 with its clock, MBC30, MBC5
//   (and rumble), MBC6 (with its flash memory), MBC7 (tilt sensor + EEPROM),
//   HuC1 (infrared port left dark), HuC3 (with its clock), MMM01, Bandai TAMA5
//   and the Pocket Camera (no real camera: it photographs a built-in test card).
// Each chip keeps the same small interface the Game Boy core uses:
//   romRead(a) romWrite(a, v) ramRead(a) ramWrite(a, v), sram() / loadSram(bytes)
//   for battery saves, saveState() / loadState(o), and frame() once per frame.
"use strict";

(function (global) {
  const RAM_SIZES = [0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000];
  const CLOCK_HZ = 4194304; // the Game Boy's crystal; the cartridge clocks count real time in these units
  const SKIP = new Set(["rom", "gb"]);

  // ===================================================================
  // Common part
  // ===================================================================
  class Cart {
    constructor(rom, gb) {
      this.rom = rom; this.gb = gb;
      this.type = rom[0x147];
      this.ramSize = RAM_SIZES[rom[0x149]] || 0;
      this.romBanks = Math.max(2, Math.ceil(rom.length / 0x4000));
      this.ram = new Uint8Array(this.ramSize);
      this.ramOn = false;
      this.rom0 = 0; this.romX = 0x4000; this.ramOff = 0;
      this.battery = [3, 6, 9, 0x0d, 0x0f, 0x10, 0x13, 0x1b, 0x1e, 0x22, 0xfc, 0xfd, 0xfe, 0xff].includes(this.type);
      this.cgb = (rom[0x143] & 0x80) !== 0;
      this.title = String.fromCharCode(...rom.slice(0x134, 0x143)).replace(/\0.*$/, "").trim();
      this.rumbleLevel = 0;
    }
    bankOffset(bank) { return (bank % this.romBanks) * 0x4000; }
    romRead(a) { return a < 0x4000 ? this.rom[this.rom0 + a] : this.rom[this.romX + (a & 0x3fff)]; }
    romWrite() { /* no registers */ }
    ramRead(a) {
      if (!this.ramOn || !this.ram.length) return 0xff;
      return this.ram[(this.ramOff + (a & 0x1fff)) % this.ram.length];
    }
    ramWrite(a, v) {
      if (!this.ramOn || !this.ram.length) return;
      this.ram[(this.ramOff + (a & 0x1fff)) % this.ram.length] = v;
    }
    frame() { /* nothing to do each frame */ }
    // real time in CPU clocks (the Game Boy core counts it; double speed doesn't change it)
    now() { return this.gb ? this.gb.clock : 0; }
    // Battery save: the RAM, plus whatever extra the chip keeps (clock, EEPROM, flash...)
    extraSave() { return null; }
    loadExtra() { /* nothing extra */ }
    sram() {
      if (!this.battery) return null;
      const extra = this.extraSave();
      if (!extra) return this.ram.slice();
      const out = new Uint8Array(this.ram.length + extra.length);
      out.set(this.ram); out.set(extra, this.ram.length);
      return out;
    }
    loadSram(s) {
      this.ram.set(s.subarray(0, Math.min(this.ram.length, s.length)));
      if (s.length > this.ram.length) this.loadExtra(s.subarray(this.ram.length));
    }
    // Save state: every field except the ROM itself
    saveState() {
      const o = {};
      for (const k of Object.keys(this)) if (!SKIP.has(k)) o[k] = this[k];
      return o;
    }
    loadState(o) {
      if (o.romX === undefined && o.romBank !== undefined) { this.loadLegacy(o); return; }
      for (const k of Object.keys(o)) {
        if (SKIP.has(k)) continue;
        const cur = this[k], v = o[k];
        if (ArrayBuffer.isView(cur) && v) cur.set(v.subarray ? v.subarray(0, cur.length) : v);
        else if (cur && typeof cur === "object" && v && typeof v === "object") this[k] = JSON.parse(JSON.stringify(v));
        else this[k] = v;
      }
    }
    // save states made before RetroPixels had these cartridge files
    loadLegacy(o) {
      if (o.ram) this.ram.set(o.ram.subarray(0, this.ram.length));
      this.ramOn = !!o.ramOn;
      this.romX = this.bankOffset(o.romBank || 1);
      this.ramOff = (o.ramBank & 0x0f) * 0x2000;
    }
  }

  // ===================================================================
  // MBC1 (and MBC1M: multi-game carts wired with a 4-bit low bank)
  // ===================================================================
  class MBC1 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      // A 1 MB MBC1 cart with Nintendo's logo at the start of a 256 KB quarter is a multi-game cart.
      this.multi = false;
      if (rom.length === 0x100000) {
        let logos = 0;
        for (let q = 1; q < 4; q++) {
          let same = true;
          for (let i = 0; i < 48; i++) if (rom[q * 0x40000 + 0x104 + i] !== rom[0x104 + i]) { same = false; break; }
          if (same) logos++;
        }
        this.multi = logos >= 2;
      }
      this.bank1 = 1; this.bank2 = 0; this.mode = 0;
      this.update();
    }
    update() {
      const s = this.multi ? 4 : 5, low = this.multi ? this.bank1 & 15 : this.bank1;
      this.romX = this.bankOffset((this.bank2 << s) | low);
      this.rom0 = this.mode ? this.bankOffset(this.bank2 << s) : 0;
      this.ramOff = this.mode ? this.bank2 * 0x2000 : 0;
    }
    romWrite(a, v) {
      if (a < 0x2000) this.ramOn = (v & 0x0f) === 0x0a;
      else if (a < 0x4000) this.bank1 = (v & 0x1f) || 1;
      else if (a < 0x6000) this.bank2 = v & 3;
      else this.mode = v & 1;
      this.update();
    }
    loadLegacy(o) {
      super.loadLegacy(o);
      this.bank1 = o.low5 || 1; this.bank2 = o.bank2 || 0; this.mode = o.mode || 0; this.update();
    }
  }

  // ===================================================================
  // MBC2: 16 ROM banks and 512 half-bytes of built-in RAM
  // ===================================================================
  class MBC2 extends Cart {
    constructor(rom, gb) { super(rom, gb); this.ram = new Uint8Array(512); this.ramSize = 512; }
    romWrite(a, v) {
      if (a >= 0x4000) return;
      if (a & 0x100) this.romX = this.bankOffset((v & 0x0f) || 1);
      else this.ramOn = (v & 0x0f) === 0x0a;
    }
    ramRead(a) { return this.ramOn ? 0xf0 | this.ram[a & 0x1ff] : 0xff; }
    ramWrite(a, v) { if (this.ramOn) this.ram[a & 0x1ff] = v & 0x0f; }
  }

  // ===================================================================
  // Real-time clock of the MBC3 (counts in emulated time; catches up with
  // the computer's clock after a pause and between play sessions)
  // ===================================================================
  class Clock {
    constructor() {
      this.s = 0; this.m = 0; this.h = 0; this.d = 0; this.halt = false; this.carry = false;
      this.sub = 0;        // clocks into the current second
      this.last = 0;       // emulated clock time up to which we've counted
      this.wall = 0;       // computer time (ms) at the last frame, to notice pauses
    }
    // one second passes; registers that hold out-of-range values count on to their bit limit
    tickSecond() {
      this.s = (this.s + 1) & 63;
      if (this.s === 60) { this.s = 0; this.m = (this.m + 1) & 63; if (this.m === 60) { this.m = 0; this.h = (this.h + 1) & 31; if (this.h === 24) { this.h = 0; this.d++; if (this.d > 511) { this.d = 0; this.carry = true; } } } }
    }
    addSeconds(n) {
      if (this.halt || n <= 0) return;
      // step until every value is in range, then jump
      while (n > 0 && (this.s >= 60 || this.m >= 60 || this.h >= 24)) { this.tickSecond(); n--; }
      if (n <= 0) return;
      let t = this.s + 60 * (this.m + 60 * (this.h + 24 * this.d)) + n;
      this.s = t % 60; t = Math.floor(t / 60);
      this.m = t % 60; t = Math.floor(t / 60);
      this.h = t % 24; t = Math.floor(t / 24);
      if (t > 511) this.carry = true;
      this.d = t % 512;
    }
    // bring the clock up to emulated time `now`
    sync(now) {
      const dt = now - this.last; this.last = now;
      if (this.halt || dt <= 0) return;
      this.sub += dt;
      if (this.sub >= CLOCK_HZ) { const n = Math.floor(this.sub / CLOCK_HZ); this.sub -= n * CLOCK_HZ; this.addSeconds(n); }
    }
    // once per frame: if the computer's clock ran on without us (paused, hidden tab), catch up
    frame(emuMs) {
      const w = Date.now();
      if (this.wall) {
        const lost = (w - this.wall) - emuMs;
        if (lost > 1500) this.addSeconds(Math.floor(lost / 1000));
      }
      this.wall = w;
    }
  }

  // ===================================================================
  // MBC3 (with optional clock) and MBC30 (8-bit ROM bank, 8 RAM banks)
  // ===================================================================
  class MBC3 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.hasRtc = this.type === 0x0f || this.type === 0x10;
      this.mbc30 = rom.length > 0x200000 || this.ramSize > 0x8000;
      this.sel = 0;
      this.clock = new Clock();
      this.latched = [0, 0, 0, 0, 0]; this.latchPrev = 0xff;
      this.romX = 0x4000;
    }
    rtcSync() { this.clock.sync(this.now()); }
    romWrite(a, v) {
      if (a < 0x2000) this.ramOn = (v & 0x0f) === 0x0a;
      else if (a < 0x4000) this.romX = this.bankOffset((v & (this.mbc30 ? 0xff : 0x7f)) || 1);
      else if (a < 0x6000) { this.sel = v & 0x0f; if (this.sel < 8) this.ramOff = (this.sel & (this.mbc30 ? 7 : 3)) * 0x2000; }
      else { if (this.latchPrev === 0 && v === 1 && this.hasRtc) this.latch(); this.latchPrev = v; }
    }
    latch() {
      this.rtcSync();
      const c = this.clock;
      this.latched = [c.s, c.m, c.h, c.d & 0xff, ((c.d >> 8) & 1) | (c.halt ? 0x40 : 0) | (c.carry ? 0x80 : 0)];
    }
    ramRead(a) {
      if (!this.ramOn) return 0xff;
      if (this.sel >= 8) return this.hasRtc && this.sel <= 0x0c ? this.latched[this.sel - 8] : 0xff;
      return super.ramRead(a);
    }
    ramWrite(a, v) {
      if (!this.ramOn) return;
      if (this.sel >= 8) { if (this.hasRtc && this.sel <= 0x0c) this.writeRtc(this.sel - 8, v); return; }
      super.ramWrite(a, v);
    }
    writeRtc(reg, v) {
      this.rtcSync();
      const c = this.clock;
      switch (reg) {
        case 0: c.s = v & 63; c.sub = 0; break; // writing the seconds restarts the second
        case 1: c.m = v & 63; break;
        case 2: c.h = v & 31; break;
        case 3: c.d = (c.d & 0x100) | v; break;
        case 4: c.d = (c.d & 0xff) | ((v & 1) << 8); c.halt = !!(v & 0x40); c.carry = !!(v & 0x80); break;
      }
      const mask = [63, 63, 31, 255, 0xc1][reg];
      this.latched[reg] = v & mask;
    }
    frame() { if (this.hasRtc) { this.rtcSync(); this.clock.frame(70224 * 1000 / CLOCK_HZ); } }
    // Clock in battery saves: the common 48-byte layout (registers, latched registers, time stamp)
    extraSave() {
      if (!this.hasRtc) return null;
      this.rtcSync();
      const c = this.clock, out = new Uint8Array(48), dv = new DataView(out.buffer);
      const cur = [c.s, c.m, c.h, c.d & 0xff, ((c.d >> 8) & 1) | (c.halt ? 0x40 : 0) | (c.carry ? 0x80 : 0)];
      for (let i = 0; i < 5; i++) { dv.setUint32(i * 4, cur[i], true); dv.setUint32(20 + i * 4, this.latched[i], true); }
      const t = Math.floor(Date.now() / 1000);
      dv.setUint32(40, t >>> 0, true); dv.setUint32(44, Math.floor(t / 4294967296), true);
      return out;
    }
    loadExtra(x) {
      if (!this.hasRtc) return;
      const dv = new DataView(x.buffer, x.byteOffset, x.length), c = this.clock;
      if (x.length >= 44) {
        const r = []; for (let i = 0; i < 5; i++) r.push(dv.getUint32(i * 4, true) & 0xff);
        for (let i = 0; i < 5; i++) this.latched[i] = dv.getUint32(20 + i * 4, true) & 0xff;
        c.s = r[0] & 63; c.m = r[1] & 63; c.h = r[2] & 31; c.d = r[3] | ((r[4] & 1) << 8); c.halt = !!(r[4] & 0x40); c.carry = !!(r[4] & 0x80);
        const t = dv.getUint32(40, true) + (x.length >= 48 ? dv.getUint32(44, true) * 4294967296 : 0);
        c.addSeconds(Math.floor(Date.now() / 1000) - t);
      } else if (x.length >= 13) { // RetroPixels' earlier layout: when the clock read zero, and the halted value
        const base = dv.getFloat64(0, true), halted = !!dv.getUint8(12);
        c.halt = false;
        c.addSeconds(halted ? dv.getUint32(8, true) : Math.floor((Date.now() - base) / 1000));
        c.halt = halted;
      }
      this.clock.last = this.now();
    }
    loadLegacy(o) {
      super.loadLegacy(o);
      this.sel = o.ramBank || 0;
      this.romX = this.bankOffset(o.romBank || 1);
      this.ramOff = (this.sel < 8 ? this.sel : 0) * 0x2000;
    }
    loadState(o) { super.loadState(o); this.clock = Object.assign(new Clock(), this.clock); }
  }

  // ===================================================================
  // MBC5 (9-bit ROM bank; rumble carts use RAM bank bit 3 for the motor)
  // ===================================================================
  class MBC5 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.hasRumble = this.type >= 0x1c && this.type <= 0x1e;
      this.bank = 1; this.romX = this.bankOffset(1);
      this.motor = false; this.motorSince = 0; this.motorTime = 0; this.frameStart = 0;
    }
    romWrite(a, v) {
      if (a < 0x2000) this.ramOn = (v & 0x0f) === 0x0a;
      else if (a < 0x3000) { this.bank = (this.bank & 0x100) | v; this.romX = this.bankOffset(this.bank); }
      else if (a < 0x4000) { this.bank = (this.bank & 0xff) | ((v & 1) << 8); this.romX = this.bankOffset(this.bank); }
      else if (a < 0x6000) {
        if (this.hasRumble) { this.ramOff = (v & 7) * 0x2000; this.setMotor(!!(v & 8)); }
        else this.ramOff = (v & 0x0f) * 0x2000;
      }
    }
    setMotor(on) {
      if (on === this.motor) return;
      const t = this.now();
      if (this.motor) this.motorTime += t - Math.max(this.motorSince, this.frameStart);
      this.motor = on; this.motorSince = t;
    }
    // how hard the motor ran during the last frame (games pulse it to set the strength)
    frame() {
      if (!this.hasRumble) return;
      const t = this.now(), span = Math.max(1, t - this.frameStart);
      if (this.motor) this.motorTime += t - Math.max(this.motorSince, this.frameStart);
      this.rumbleLevel = Math.min(1, this.motorTime / span);
      this.motorTime = 0; this.frameStart = t;
    }
    loadLegacy(o) { super.loadLegacy(o); this.bank = o.romBank || 1; }
  }

  // ===================================================================
  // MBC6 (Net de Get): two 8 KB ROM/flash windows, two 4 KB RAM windows,
  // and 1 MB of flash memory programmed with the usual unlock sequence
  // ===================================================================
  class MBC6 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.ram = new Uint8Array(0x8000); this.ramSize = 0x8000;
      this.flash = new Uint8Array(0x100000).fill(0xff);
      this.bankA = 0; this.bankB = 0; this.flashA = false; this.flashB = false;
      this.ramA = 0; this.ramB = 0; this.flashOn = false; this.flashWrite = false;
      this.fState = 0; this.fCmd = 0; this.fId = false;
      this.battery = true;
    }
    romRead(a) {
      if (a < 0x4000) return this.rom[a];
      const b = a < 0x6000 ? 0 : 1, bank = b ? this.bankB : this.bankA, fl = b ? this.flashB : this.flashA, off = a & 0x1fff;
      if (fl) {
        if (this.fId) return off === 0 ? 0xc2 : off === 1 ? 0x81 : 0xff; // Macronix, MX29F008
        return this.flash[((bank & 0x7f) * 0x2000 + off) & 0xfffff];
      }
      return this.rom[(bank * 0x2000 + off) % this.rom.length];
    }
    romWrite(a, v) {
      if (a < 0x400) this.ramOn = (v & 0x0f) === 0x0a;
      else if (a < 0x800) this.ramA = v & 7;
      else if (a < 0xc00) this.ramB = v & 7;
      else if (a < 0x1000) this.flashOn = !!(v & 1);
      else if (a === 0x1000) this.flashWrite = !!(v & 1);
      else if (a >= 0x2000 && a < 0x2800) this.bankA = v & 0x7f;
      else if (a >= 0x2800 && a < 0x3000) this.flashA = v === 8;
      else if (a >= 0x3000 && a < 0x3800) this.bankB = v & 0x7f;
      else if (a >= 0x3800 && a < 0x4000) this.flashB = v === 8;
      else if (a >= 0x4000) { // writes into a flash window go to the flash chip
        const b = a < 0x6000 ? 0 : 1;
        if ((b ? this.flashB : this.flashA) && this.flashOn) this.flashCmd(((b ? this.bankB : this.bankA) & 0x7f) * 0x2000 + (a & 0x1fff), v);
      }
    }
    flashCmd(fa, v) {
      fa &= 0xfffff;
      if (this.fCmd === 0xa0) { if (this.flashWrite) this.flash[fa] &= v; this.fCmd = 0; return; }
      if (v === 0xf0) { this.fState = 0; this.fCmd = 0; this.fId = false; return; }
      const lo = fa & 0x7fff;
      if (this.fState === 0 && lo === 0x5555 && v === 0xaa) { this.fState = 1; return; }
      if (this.fState === 1 && lo === 0x2aaa && v === 0x55) { this.fState = 2; return; }
      if (this.fState === 2) {
        this.fState = 0;
        if (this.fCmd === 0x80 && v === 0x30) { if (this.flashWrite) this.flash.fill(0xff, fa & ~0x1ffff, (fa & ~0x1ffff) + 0x20000); this.fCmd = 0; return; }
        if (this.fCmd === 0x80 && v === 0x10) { if (this.flashWrite) this.flash.fill(0xff); this.fCmd = 0; return; }
        if (v === 0x90) this.fId = true;
        else if (v === 0x80 || v === 0xa0) this.fCmd = v;
        return;
      }
      this.fState = 0;
    }
    ramRead(a) {
      if (!this.ramOn) return 0xff;
      return this.ram[(a < 0xb000 ? this.ramA : this.ramB) * 0x1000 + (a & 0xfff)];
    }
    ramWrite(a, v) { if (this.ramOn) this.ram[(a < 0xb000 ? this.ramA : this.ramB) * 0x1000 + (a & 0xfff)] = v; }
    extraSave() { return this.flash; }
    loadExtra(x) { this.flash.set(x.subarray(0, this.flash.length)); }
  }

  // ===================================================================
  // MBC7 (Kirby Tilt 'n' Tumble, Command Master): two-axis tilt sensor and
  // a 93LC56 serial EEPROM (128 words of 16 bits)
  // ===================================================================
  class MBC7 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.ram = new Uint8Array(256).fill(0xff); this.ramSize = 256; // the EEPROM is the battery save
      this.battery = true;
      this.ramg1 = false; this.ramg2 = false;
      this.accX = 0x8000; this.accY = 0x8000; this.latched = false;
      // EEPROM interface
      this.cs = 0; this.clk = 0; this.di = 0; this.do = 1;
      this.eState = 0; this.eBits = 0; this.eCount = 0; this.eAddr = 0; this.eOut = 0; this.eOutBits = 0; this.wen = false;
      this.romX = this.bankOffset(1);
    }
    romWrite(a, v) {
      if (a < 0x2000) { this.ramg1 = (v & 0x0f) === 0x0a; if (!this.ramg1) this.ramg2 = false; }
      else if (a < 0x4000) this.romX = this.bankOffset(v & 0x7f);
      else if (a < 0x6000) this.ramg2 = this.ramg1 && v === 0x40;
    }
    ramRead(a) {
      if (!this.ramg1 || !this.ramg2 || a >= 0xb000) return 0xff;
      switch ((a >> 4) & 0x0f) {
        case 2: return this.accX & 0xff;
        case 3: return this.accX >> 8;
        case 4: return this.accY & 0xff;
        case 5: return this.accY >> 8;
        case 6: return 0x00;
        case 8: return (this.cs << 7) | (this.clk << 6) | (this.di << 1) | this.do;
        default: return 0xff;
      }
    }
    ramWrite(a, v) {
      if (!this.ramg1 || !this.ramg2 || a >= 0xb000) return;
      switch ((a >> 4) & 0x0f) {
        case 0: if (v === 0x55) { this.accX = 0x8000; this.accY = 0x8000; this.latched = false; } break;
        case 1:
          if (v === 0xaa && !this.latched) {
            // centre 0x81D0, about 0x70 per g; tilting right or toward you lowers the value
            const t = this.gb ? this.gb.tilt : { x: 0, y: 0 };
            this.accX = (0x81d0 - Math.round(t.x * 0x70)) & 0xffff;
            this.accY = (0x81d0 - Math.round(t.y * 0x70)) & 0xffff;
            this.latched = true;
          }
          break;
        case 8: this.eeprom(v); break;
      }
    }
    // Microwire serial protocol: start bit, 2-bit opcode, 8-bit address (word address in the low 7 bits)
    eeprom(v) {
      const cs = (v >> 7) & 1, clk = (v >> 6) & 1, di = (v >> 1) & 1;
      if (!cs) { this.eState = 0; this.do = 1; this.cs = 0; this.clk = clk; this.di = di; return; }
      if (!this.cs) { this.eState = 0; this.eCount = 0; this.eBits = 0; } // chip selected: wait for a start bit
      const rising = clk && !this.clk;
      this.cs = cs; this.clk = clk; this.di = di;
      if (!rising) return;
      switch (this.eState) {
        case 0: if (di) { this.eState = 1; this.eCount = 0; this.eBits = 0; } break;
        case 1: // opcode + address
          this.eBits = (this.eBits << 1) | di;
          if (++this.eCount === 10) {
            const op = this.eBits >> 8, addr = this.eBits & 0x7f;
            this.eAddr = addr; this.eCount = 0;
            if (op === 2) { this.eState = 2; this.eOut = this.word(addr); this.eOutBits = 16; this.do = 0; } // read: a dummy 0 first
            else if (op === 1) { this.eState = 3; this.eBits = 0; }                                         // write
            else if (op === 3) { if (this.wen) this.setWord(addr, 0xffff); this.eState = 4; this.do = 1; }     // erase
            else {
              const sub = (this.eBits >> 6) & 3;
              if (sub === 0) { this.wen = false; this.eState = 4; }
              else if (sub === 3) { this.wen = true; this.eState = 4; }
              else if (sub === 2) { if (this.wen) this.ram.fill(0xff); this.eState = 4; }
              else { this.eState = 5; this.eBits = 0; } // write all
            }
          }
          break;
        case 2: // shifting data out, words follow on
          if (this.eOutBits === 0) { this.eAddr = (this.eAddr + 1) & 0x7f; this.eOut = this.word(this.eAddr); this.eOutBits = 16; }
          this.do = (this.eOut >> 15) & 1; this.eOut = (this.eOut << 1) & 0xffff; this.eOutBits--;
          break;
        case 3: case 5:
          this.eBits = (this.eBits << 1) | di;
          if (++this.eCount === 16) {
            if (this.wen) {
              if (this.eState === 3) this.setWord(this.eAddr, this.eBits);
              else for (let i = 0; i < 128; i++) this.setWord(i, this.eBits);
            }
            this.eState = 4; this.do = 1; // done at once (always ready)
          }
          break;
      }
    }
    word(i) { return (this.ram[i * 2] << 8) | this.ram[i * 2 + 1]; }
    setWord(i, w) { this.ram[i * 2] = w >> 8; this.ram[i * 2 + 1] = w & 0xff; }
  }

  // ===================================================================
  // HuC1 (Hudson): MBC1-like banking plus an infrared port, which here never sees light
  // ===================================================================
  class HuC1 extends Cart {
    constructor(rom, gb) { super(rom, gb); this.ir = false; this.ramOn = true; this.romX = this.bankOffset(1); }
    romWrite(a, v) {
      if (a < 0x2000) { this.ir = (v & 0x0f) === 0x0e; this.ramOn = !this.ir; }
      else if (a < 0x4000) this.romX = this.bankOffset((v & 0x3f) || 1);
      else if (a < 0x6000) this.ramOff = (v & 3) * 0x2000;
    }
    ramRead(a) { return this.ir ? 0xc0 : super.ramRead(a); }
    ramWrite(a, v) { if (!this.ir) super.ramWrite(a, v); }
  }

  // ===================================================================
  // HuC3 (Hudson): banking, infrared (dark), and a clock reached through a
  // nibble-wide command port. Its memory holds minutes-of-day and days.
  // ===================================================================
  class HuC3 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.mode = 0; this.mem = new Uint8Array(256); this.addr = 0; this.result = 0; this.lastCmd = 0;
      this.minutes = 0; this.days = 0; this.sub = 0; this.last = 0; this.wall = 0;
      this.romX = this.bankOffset(1);
    }
    clockSync() {
      const now = this.now(), dt = now - this.last; this.last = now;
      if (dt > 0) { this.sub += dt; const per = CLOCK_HZ * 60; if (this.sub >= per) { const n = Math.floor(this.sub / per); this.sub -= n * per; this.addMinutes(n); } }
    }
    addMinutes(n) { const t = this.minutes + n; this.minutes = t % 1440; this.days = (this.days + Math.floor(t / 1440)) & 0xfff; }
    romWrite(a, v) {
      if (a < 0x2000) { this.mode = v & 0x0f; this.ramOn = this.mode === 0x0a; }
      else if (a < 0x4000) this.romX = this.bankOffset(v & 0x7f);
      else if (a < 0x6000) this.ramOff = (v & 0x0f) * 0x2000;
    }
    ramRead(a) {
      switch (this.mode) {
        case 0x0a: case 0x00: return this.ram.length ? this.ram[(this.ramOff + (a & 0x1fff)) % this.ram.length] : 0xff;
        case 0x0c: return 0x80 | (this.lastCmd << 4) | this.result;
        case 0x0d: return 0x01; // the clock is always ready
        case 0x0e: return 0xc0; // no infrared light
        default: return 0xff;
      }
    }
    ramWrite(a, v) {
      if (this.mode === 0x0a) { super.ramWrite(a, v); return; }
      if (this.mode !== 0x0b) return;
      const cmd = (v >> 4) & 7, arg = v & 0x0f;
      this.lastCmd = cmd;
      switch (cmd) {
        case 1: this.result = this.mem[this.addr] & 0x0f; this.addr = (this.addr + 1) & 0xff; break;
        case 2: this.result = this.mem[this.addr] & 0x0f; break;
        case 3: this.mem[this.addr] = arg; this.addr = (this.addr + 1) & 0xff; break;
        case 4: this.addr = (this.addr & 0xf0) | arg; break;
        case 5: this.addr = (this.addr & 0x0f) | (arg << 4); break;
        case 6:
          if (arg === 0) { // copy the clock into memory 0-5
            this.clockSync();
            for (let i = 0; i < 3; i++) { this.mem[i] = (this.minutes >> (i * 4)) & 0x0f; this.mem[3 + i] = (this.days >> (i * 4)) & 0x0f; }
          } else if (arg === 1) { // set the clock from memory 0-5
            this.clockSync();
            let m = 0, d = 0;
            for (let i = 0; i < 3; i++) { m |= (this.mem[i] & 0x0f) << (i * 4); d |= (this.mem[3 + i] & 0x0f) << (i * 4); }
            this.minutes = m % 1440; this.days = d; this.sub = 0;
          } else if (arg === 2) this.result = 1;
          break;
      }
    }
    frame() {
      this.clockSync();
      const w = Date.now();
      if (this.wall) { const lost = (w - this.wall) - 70224 * 1000 / CLOCK_HZ; if (lost > 1500) { this.sub += lost / 1000 * CLOCK_HZ; this.clockSync(); } }
      this.wall = w;
    }
    // battery: RAM, then RetroPixels' clock block (minutes, days, time stamp, clock memory)
    extraSave() {
      this.clockSync();
      const out = new Uint8Array(16 + 256), dv = new DataView(out.buffer);
      dv.setUint32(0, this.minutes, true); dv.setUint32(4, this.days, true); dv.setFloat64(8, Date.now(), true);
      out.set(this.mem, 16);
      return out;
    }
    loadExtra(x) {
      if (x.length < 16) return;
      const dv = new DataView(x.buffer, x.byteOffset, x.length);
      this.minutes = dv.getUint32(0, true) % 1440; this.days = dv.getUint32(4, true) & 0xfff;
      if (x.length >= 272) this.mem.set(x.subarray(16, 272));
      const away = Date.now() - dv.getFloat64(8, true);
      if (away > 0) this.addMinutes(Math.floor(away / 60000));
      this.last = this.now();
    }
  }

  // ===================================================================
  // MMM01 (multi-game carts): starts with the menu (the last 32 KB of ROM)
  // mapped in; once the menu picks a game it locks the outer bank bits.
  // ===================================================================
  class MMM01 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.mapped = false;
      this.romLow = 0; this.romMid = 0; this.romHigh = 0; this.fixedLow = 0;
      this.ramLow = 0; this.ramMid = 0; this.mask = 0; this.mode = 0; this.modeLock = false;
      this.update();
    }
    update() {
      const size = this.romBanks * 0x4000;
      if (!this.mapped) { this.rom0 = Math.max(0, size - 0x8000); this.romX = Math.max(0x4000, size - 0x4000); this.ramOff = 0; return; }
      // outer bank bits chosen by the menu, low bits by the game (the menu can freeze some of them too)
      const outer = (this.romHigh << 7) | (this.romMid << 5), fixed = (this.mask << 1) & 0x1e;
      const keep = this.fixedLow & fixed;
      this.romX = this.bankOffset(outer | keep | ((this.romLow || 1) & ~fixed & 0x1f));
      this.rom0 = this.bankOffset(outer | keep);
      this.ramOff = ((this.ramMid << 2) | (this.mode ? this.ramLow : 0)) * 0x2000;
    }
    romWrite(a, v) {
      if (a < 0x2000) {
        this.ramOn = (v & 0x0f) === 0x0a;
        if (!this.mapped && (v & 0x40)) { this.mapped = true; this.fixedLow = this.romLow; }
      } else if (a < 0x4000) {
        this.romLow = v & 0x1f;
        if (!this.mapped) this.romMid = (v >> 5) & 3;
      } else if (a < 0x6000) {
        this.ramLow = v & 3;
        if (!this.mapped) { this.ramMid = (v >> 2) & 3; this.romHigh = (v >> 4) & 3; this.modeLock = !!(v & 0x40); }
      } else {
        if (!this.modeLock) this.mode = v & 1;
        if (!this.mapped) this.mask = (v >> 2) & 0x0f;
      }
      this.update();
    }
  }

  // ===================================================================
  // Bandai TAMA5 (Tamagotchi 3): registers reached a nibble at a time through
  // A000 (data) and A001 (register number); 32 bytes of memory and a clock.
  // ===================================================================
  class TAMA5 extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.battery = true;
      this.ram = new Uint8Array(32); this.ramSize = 32;
      this.reg = 0; this.r = new Uint8Array(16); this.out = 0;
      this.clock = new Clock(); this.month = 1; this.year = 0; this.dayOfMonth = 1;
      this.romX = this.bankOffset(1);
    }
    romWrite() { /* everything goes through A000/A001 */ }
    bcd(v) { return ((v / 10) | 0) * 16 + (v % 10); }
    ramRead(a) {
      if (a & 1) return 0xff;
      switch (this.reg) {
        case 0x0a: return 0xf1;                         // ready
        case 0x0c: return 0xf0 | (this.out & 0x0f);
        case 0x0d: return 0xf0 | (this.out >> 4);
        default: return 0xff;
      }
    }
    ramWrite(a, v) {
      if (a & 1) { this.reg = v & 0x0f; return; }
      const r = this.r; r[this.reg] = v & 0x0f;
      switch (this.reg) {
        case 0: case 1: this.romX = this.bankOffset(((r[1] & 1) << 4) | r[0]); break;
        case 7: { // run the operation
          const op = r[6] >> 1, addr = ((r[6] & 1) << 4) | r[7], data = (r[5] << 4) | r[4];
          if (op === 0) this.ram[addr] = data;
          else if (op === 1) this.out = this.ram[addr];
          else if (op === 2) this.out = this.readClock(addr);
          else if (op === 3) this.writeClock(addr, data);
          break;
        }
      }
    }
    // the clock chip's registers, in BCD: 0 seconds, 1 minutes, 2 hours, 3 day, 4 month, 5 year
    readClock(i) {
      this.clock.sync(this.now());
      const c = this.clock;
      return [this.bcd(c.s % 60), this.bcd(c.m % 60), this.bcd(c.h % 24), this.bcd(this.dayOfMonth), this.bcd(this.month), this.bcd(this.year)][i] || 0;
    }
    writeClock(i, v) {
      this.clock.sync(this.now());
      const n = (v >> 4) * 10 + (v & 0x0f), c = this.clock;
      if (i === 0) { c.s = n % 60; c.sub = 0; } else if (i === 1) c.m = n % 60; else if (i === 2) c.h = n % 24;
      else if (i === 3) this.dayOfMonth = n || 1; else if (i === 4) this.month = n || 1; else if (i === 5) this.year = n;
    }
    frame() {
      const before = this.clock.d;
      this.clock.sync(this.now()); this.clock.frame(70224 * 1000 / CLOCK_HZ);
      // a simple calendar: days roll into months of 30 days
      for (let k = before; k < this.clock.d; k++) { if (++this.dayOfMonth > 30) { this.dayOfMonth = 1; if (++this.month > 12) { this.month = 1; this.year = (this.year + 1) % 100; } } }
      this.clock.d &= 0xff;
    }
    extraSave() {
      this.clock.sync(this.now());
      const c = this.clock, out = new Uint8Array(16), dv = new DataView(out.buffer);
      out.set([c.s, c.m, c.h, this.dayOfMonth, this.month, this.year], 0);
      dv.setFloat64(8, Date.now(), true);
      return out;
    }
    loadExtra(x) {
      if (x.length < 16) return;
      const dv = new DataView(x.buffer, x.byteOffset, x.length), c = this.clock;
      [c.s, c.m, c.h, this.dayOfMonth, this.month, this.year] = x.subarray(0, 6);
      c.d = 0; const away = Math.floor((Date.now() - dv.getFloat64(8, true)) / 1000);
      if (away > 0) c.addSeconds(away);
      this.clock.last = this.now();
    }
    loadState(o) { super.loadState(o); this.clock = Object.assign(new Clock(), this.clock); }
  }

  // ===================================================================
  // Pocket Camera: MBC with 128 KB RAM and the camera's registers in RAM bank $10.
  // There's no camera, so a capture "photographs" a built-in test card,
  // shaded through the game's exposure and dithering settings.
  // ===================================================================
  const TEST_CARD = (() => {
    const p = new Uint8Array(128 * 112);
    for (let y = 0; y < 112; y++) for (let x = 0; x < 128; x++) {
      let v = 40 + ((x * 180) / 127) | 0;                            // left-to-right gradient
      const dx = x - 64, dy = y - 56, r = Math.sqrt(dx * dx + dy * dy);
      if (r < 40) v = (Math.floor(r / 6) & 1) ? 230 : 30;             // target rings
      if (y >= 100) v = [20, 90, 160, 235][Math.floor(x / 32)];       // grey-scale bars
      if (x < 2 || y < 2 || x > 125 || y > 109) v = 10;               // frame
      p[y * 128 + x] = v;
    }
    return p;
  })();
  class Camera extends Cart {
    constructor(rom, gb) {
      super(rom, gb);
      this.ram = new Uint8Array(0x20000); this.ramSize = 0x20000;
      this.regs = new Uint8Array(0x36); this.camSel = false; this.busyUntil = 0;
      this.romX = this.bankOffset(1);
    }
    romWrite(a, v) {
      if (a < 0x2000) this.ramOn = (v & 0x0f) === 0x0a;
      else if (a < 0x4000) this.romX = this.bankOffset(v & 0x3f);
      else if (a < 0x6000) { this.camSel = !!(v & 0x10); if (!this.camSel) this.ramOff = (v & 0x0f) * 0x2000; }
    }
    frame() { this.busy(); }
    busy() {
      if (this.busyUntil && this.now() >= this.busyUntil) { this.busyUntil = 0; this.regs[0] &= ~1; this.capture(); }
      return !!this.busyUntil;
    }
    ramRead(a) {
      if (this.camSel) return (a & 0x7f) === 0 ? (this.busy() ? 1 : 0) | (this.regs[0] & 6) : 0x00;
      return this.ram[(this.ramOff + (a & 0x1fff)) % this.ram.length]; // readable even when writes are off
    }
    ramWrite(a, v) {
      if (this.camSel) {
        const r = a & 0x7f;
        if (r >= 0x36) return;
        if (r === 0) {
          this.busy();
          this.regs[0] = v & 7;
          if ((v & 1) && !this.busyUntil) {
            const exposure = (this.regs[2] << 8) | this.regs[3];
            this.busyUntil = this.now() + 32446 + exposure * 16;
          }
        } else this.regs[r] = v;
        return;
      }
      if (this.ramOn) super.ramWrite(a, v);
    }
    // turn the test card into 16x14 tiles in RAM bank 0 at A100
    capture() {
      const R = this.regs, exposure = (R[2] << 8) | R[3];
      const gain = Math.min(2.5, Math.max(0.15, exposure / 0x1000 + 0.15));
      for (let ty = 0; ty < 14; ty++) for (let tx = 0; tx < 16; tx++) for (let row = 0; row < 8; row++) {
        let lo = 0, hi = 0;
        const y = ty * 8 + row;
        for (let col = 0; col < 8; col++) {
          const x = tx * 8 + col;
          let v = Math.min(255, TEST_CARD[y * 128 + x] * gain);
          const m = 6 + ((y & 3) * 4 + (x & 3)) * 3;
          let c = 0;
          if (v < R[m]) c = 3; else if (v < R[m + 1]) c = 2; else if (v < R[m + 2]) c = 1;
          lo |= (c & 1) << (7 - col); hi |= (c >> 1) << (7 - col);
        }
        const o = 0x100 + (ty * 16 + tx) * 16 + row * 2;
        this.ram[o] = lo; this.ram[o + 1] = hi;
      }
    }
  }

  // ===================================================================
  // Picking the chip from the cartridge header
  // ===================================================================
  function create(rom, gb) {
    const t = rom[0x147];
    let c;
    if (t === 0 || t === 8 || t === 9) { c = new Cart(rom, gb); c.ramOn = true; }
    else if (t >= 1 && t <= 3) c = new MBC1(rom, gb);
    else if (t === 5 || t === 6) c = new MBC2(rom, gb);
    else if (t >= 0x0b && t <= 0x0d) c = new MMM01(rom, gb);
    else if (t >= 0x0f && t <= 0x13) c = new MBC3(rom, gb);
    else if (t >= 0x19 && t <= 0x1e) c = new MBC5(rom, gb);
    else if (t === 0x20) c = new MBC6(rom, gb);
    else if (t === 0x22) c = new MBC7(rom, gb);
    else if (t === 0xfc) c = new Camera(rom, gb);
    else if (t === 0xfd) c = new TAMA5(rom, gb);
    else if (t === 0xfe) c = new HuC3(rom, gb);
    else if (t === 0xff) c = new HuC1(rom, gb);
    else throw new Error(`This Game Boy game uses a cartridge chip RetroPixels doesn't support yet (type $${t.toString(16)}).`);
    c.kind = c.constructor.name;
    return c;
  }

  global.RPGBCarts = { create, Cart, Clock, MBC1, MBC2, MBC3, MBC5, MBC6, MBC7, HuC1, HuC3, MMM01, TAMA5, Camera };
})(typeof window !== "undefined" ? window : globalThis);
