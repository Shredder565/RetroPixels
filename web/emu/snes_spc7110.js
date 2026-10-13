// RetroPixels — SNES SPC7110 cartridge chip (Far East of Eden Zero, Momotarou
// Dentetsu Happy, Super Power League 4) and its Epson RTC-4513 clock. Original code.
//
// The SPC7110 sits between the SNES and a large "data ROM" (everything after the
// first 1 MB of the file). It gives the game:
//   - a decompressor for graphics packed with an adaptive binary arithmetic code
//     (modes 0, 1 and 2: plain bytes, 2 and 4 bitplane tiles), read at $4800 / $50:xxxx
//   - a data port that reads the data ROM through a moving pointer ($4810-$481A)
//   - a multiply / divide unit ($4820-$482F)
//   - switchable 1 MB windows of the data ROM in banks $D0-$FF ($4831-$4833)
//   - battery RAM at $6000-$7FFF, enabled through $4830
//   - in Far East of Eden Zero, a real-time clock at $4840-$4842
// All of it is written from the documented behaviour of the chip.
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;
  const s16 = Kit.s16;

  // Arithmetic decoder states: [LPS probability, next on LPS, next on MPS, swap MPS on LPS]
  const EVOLUTION = [
    [0x5a, 1, 1, 1], [0x25, 6, 2, 0], [0x11, 8, 3, 0], [0x08, 10, 4, 0], [0x03, 12, 5, 0], [0x01, 15, 5, 0],
    [0x5a, 7, 7, 1], [0x3f, 19, 8, 0], [0x2c, 21, 9, 0], [0x20, 22, 10, 0], [0x17, 23, 11, 0], [0x11, 25, 12, 0], [0x0c, 26, 13, 0], [0x09, 28, 14, 0],
    [0x07, 29, 15, 0], [0x05, 31, 16, 0], [0x04, 32, 17, 0], [0x03, 34, 18, 0], [0x02, 35, 5, 0],
    [0x5a, 20, 20, 1], [0x48, 39, 21, 0], [0x3a, 40, 22, 0], [0x2e, 42, 23, 0], [0x26, 44, 24, 0], [0x1f, 45, 25, 0], [0x19, 46, 26, 0], [0x15, 25, 27, 0],
    [0x11, 26, 28, 0], [0x0e, 26, 29, 0], [0x0b, 27, 30, 0], [0x09, 28, 31, 0], [0x08, 29, 32, 0], [0x07, 30, 33, 0], [0x05, 31, 34, 0], [0x04, 33, 35, 0],
    [0x04, 33, 36, 0], [0x03, 34, 37, 0], [0x02, 35, 38, 0], [0x02, 36, 5, 0],
    [0x58, 39, 40, 1], [0x4d, 47, 41, 0], [0x43, 48, 42, 0], [0x3b, 49, 43, 0], [0x34, 50, 44, 0], [0x2e, 51, 45, 0], [0x29, 44, 46, 0], [0x25, 45, 24, 0],
    [0x56, 47, 48, 1], [0x4f, 47, 49, 0], [0x47, 48, 50, 0], [0x41, 49, 51, 0], [0x3c, 50, 52, 0], [0x37, 51, 43, 0],
  ];
  // Mode 2 walks a small tree of contexts while reading the 4 bits of a pixel.
  const TREE2 = [[1, 2], [3, 8], [13, 14], [15, 16], [17, 18], [19, 20], [21, 22], [23, 24], [25, 26], [25, 26], [25, 26], [25, 26], [25, 26], [27, 28], [29, 30]];
  while (TREE2.length < 32) TREE2.push([31, 31]);

  // How the three neighbouring pixels (left, above, above-left) relate.
  function shape(a, b, c) {
    if (a === b && b === c) return 0;
    if (a === b) return 1;
    if (b === c) return 2;
    if (a === c) return 3;
    return 4;
  }
  // move value v to the front of list l
  function toFront(l, v) { const i = l.indexOf(v); for (let k = i; k > 0; k--) l[k] = l[k - 1]; l[0] = v; }

  class Decompressor {
    constructor(read) { this.readByte = read; this.idx = new Uint8Array(32); this.inv = new Uint8Array(32); this.queue = []; this.mode = 0; this.addr = 0; this.ready = false; }
    start(mode, addr, skip) {
      this.mode = mode > 2 ? 0 : mode; this.addr = addr;
      this.idx.fill(0); this.inv.fill(0); this.queue = [];
      this.val = this.nextIn(); this.inb = this.nextIn(); this.inCount = 8; this.span = 0xff;
      this.out = 0; this.out1 = 0; this.lpsH = 0; this.invH = 0;
      this.order = this.mode === 2 ? Array.from({ length: 16 }, (_, i) => i) : [0, 1, 2, 3];
      this.ready = true;
      for (let i = 0; i < skip; i++) this.next();
    }
    nextIn() { const v = this.readByte(this.addr); this.addr = (this.addr + 1) & 0xffffff; return v; }
    // decode one binary decision in context `con`; returns 1 for the less likely symbol
    decide(con) {
      const st = EVOLUTION[this.idx[con]], prob = st[0];
      let lps;
      if (this.val <= this.span - prob) { this.span -= prob; lps = 0; } else { this.val -= this.span - (prob - 1); this.span = prob - 1; lps = 1; }
      let shift = 0;
      while (this.span < 0x7f) {
        shift++;
        this.span = (this.span << 1) | 1;
        this.val = ((this.val << 1) | (this.inb >> 7)) & 0xff;
        this.inb = (this.inb << 1) & 0xff;
        if (--this.inCount === 0) { this.inb = this.nextIn(); this.inCount = 8; }
      }
      this.lpsH = ((this.lpsH << 1) | lps) >>> 0;
      this.invH = ((this.invH << 1) | this.inv[con]) >>> 0;
      if (lps && st[3]) this.inv[con] ^= 1;
      if (lps) this.idx[con] = st[1]; else if (shift) this.idx[con] = st[2];
      return lps;
    }
    next() {
      if (!this.ready) return 0;
      if (!this.queue.length) this.fill();
      return this.queue.shift();
    }
    fill() {
      if (this.mode === 0) { // plain bytes; each bit guessed from the bit 16 places back
        for (let bit = 0; bit < 8; bit++) {
          const mask = (1 << (bit & 3)) - 1;
          let con = mask + ((this.invH & mask) ^ (this.lpsH & mask));
          if (bit > 3) con += 15;
          const mps = ((this.out >> 15) & 1) ^ this.inv[con];
          const lps = this.decide(con);
          this.out = ((this.out << 1) | (lps ? 1 - mps : mps)) >>> 0;
        }
        this.queue.push(this.out & 0xff);
        return;
      }
      if (this.mode === 1) { // 2 bitplanes, one 8-pixel row at a time
        for (let px = 0; px < 8; px++) {
          const a = this.out & 3, b = (this.out >>> 14) & 3, c = (this.out >>> 16) & 3;
          let con = shape(a, b, c);
          toFront(this.order, a);
          const real = this.real = this.order.slice(); toFront(real, c); toFront(real, b); toFront(real, a);
          this.decide(con);
          con = 5 + con * 2 + ((this.lpsH ^ this.invH) & 1);
          this.decide(con);
          this.out = ((this.out << 2) | real[(this.lpsH ^ this.invH) & 3]) >>> 0;
        }
        let p0 = 0, p1 = 0;
        for (let i = 0; i < 8; i++) { const v = (this.out >>> (14 - 2 * i)) & 3; p0 |= (v & 1) << (7 - i); p1 |= (v >> 1) << (7 - i); }
        this.queue.push(p0, p1);
        return;
      }
      // mode 2: 4 bitplanes; planes 2-3 of a tile follow its planes 0-1
      const hi = [];
      for (let row = 0; row < 8; row++) {
        for (let px = 0; px < 8; px++) {
          const a = this.out & 15, b = (this.out >>> 28) & 15, c = this.out1 & 15;
          const ref = shape(a, b, c);
          toFront(this.order, a);
          const real = this.real = this.order.slice(); toFront(real, c); toFront(real, b); toFront(real, a);
          let con = 0;
          for (let bit = 0; bit < 4; bit++) {
            this.decide(con);
            con = TREE2[con][(this.lpsH ^ this.invH) & 1] + (con === 1 ? ref : 0);
          }
          this.out1 = ((this.out1 << 4) | (this.out >>> 28)) >>> 0;
          this.out = ((this.out << 4) | real[(this.lpsH ^ this.invH) & 15]) >>> 0;
        }
        const pl = [0, 0, 0, 0];
        for (let i = 0; i < 8; i++) { const v = (this.out >>> (28 - 4 * i)) & 15; for (let k = 0; k < 4; k++) pl[k] |= ((v >> k) & 1) << (7 - i); }
        this.queue.push(pl[0], pl[1]); hi.push(pl[2], pl[3]);
      }
      this.queue.push(...hi);
    }
    saveState() {
      return { mode: this.mode, addr: this.addr, ready: this.ready, idx: this.idx.slice(), inv: this.inv.slice(), queue: this.queue.slice(),
        r: [this.val, this.inb, this.inCount, this.span, this.out, this.out1, this.lpsH, this.invH], order: this.order ? this.order.slice() : null };
    }
    loadState(o) {
      this.mode = o.mode; this.addr = o.addr; this.ready = o.ready; this.idx.set(o.idx); this.inv.set(o.inv); this.queue = o.queue.slice();
      [this.val, this.inb, this.inCount, this.span, this.out, this.out1, this.lpsH, this.invH] = o.r; this.order = o.order ? o.order.slice() : null;
    }
  }

  // ===================================================================
  // Epson RTC-4513 (serial, 4-bit registers)
  // ===================================================================
  // Registers 0-12: seconds, minutes, hours, day, month, year (ones / tens),
  // weekday; 13-15 control (15 bit 2 = 24-hour mode). RetroPixels keeps the
  // clock as an offset from the computer's clock.
  class RTC4513 {
    constructor() { this.offset = 0; this.ctl = [0, 0, 4]; this.reset(); }
    reset() { this.ce = 0; this.state = 0; this.mode = 0; this.index = 0; this.regs = new Uint8Array(16); this.dirty = false; }
    now() { return Math.floor(Date.now() / 1000) + this.offset; }
    latch() {
      const d = new Date(this.now() * 1000), r = this.regs;
      const sec = d.getUTCSeconds(), min = d.getUTCMinutes(), hour = d.getUTCHours(), day = d.getUTCDate(), mon = d.getUTCMonth() + 1, yr = d.getUTCFullYear() % 100;
      let h = hour, pm = 0;
      if (!(this.ctl[2] & 4)) { pm = hour >= 12 ? 4 : 0; h = hour % 12; }
      r[0] = sec % 10; r[1] = (sec / 10) | 0; r[2] = min % 10; r[3] = (min / 10) | 0;
      r[4] = h % 10; r[5] = ((h / 10) | 0) | pm; r[6] = day % 10; r[7] = (day / 10) | 0;
      r[8] = mon % 10; r[9] = (mon / 10) | 0; r[10] = yr % 10; r[11] = (yr / 10) | 0; r[12] = d.getUTCDay();
      r[13] = this.ctl[0]; r[14] = this.ctl[1]; r[15] = this.ctl[2];
    }
    // the game set the time: work out the new offset from the registers
    commit() {
      const r = this.regs;
      this.ctl = [r[13] & 15, r[14] & 15, r[15] & 15];
      let hour = r[4] + (r[5] & 3) * 10;
      if (!(this.ctl[2] & 4)) hour = (hour % 12) + (r[5] & 4 ? 12 : 0);
      const yr = r[10] + r[11] * 10;
      const t = Date.UTC(yr >= 90 ? 1900 + yr : 2000 + yr, Math.max(0, r[8] + (r[9] & 1) * 10 - 1), r[6] + (r[7] & 3) * 10 || 1, hour, r[2] + (r[3] & 7) * 10, r[0] + (r[1] & 7) * 10);
      if (!isNaN(t)) this.offset = Math.floor(t / 1000) - Math.floor(Date.now() / 1000);
    }
    setCE(v) {
      const ce = v & 1;
      if (ce && !this.ce) { this.state = 1; this.dirty = false; }
      if (!ce && this.ce) { if (this.dirty) this.commit(); this.state = 0; }
      this.ce = ce;
    }
    write(v) {
      v &= 15;
      if (!this.ce) return;
      if (this.state === 1) { if (v === 0x03 || v === 0x0c) { this.mode = v; this.state = 2; } return; }
      if (this.state === 2) { this.index = v; this.state = 3; this.latch(); return; }
      if (this.state === 3 && this.mode === 0x03) { this.regs[this.index] = v; this.index = (this.index + 1) & 15; this.dirty = true; }
    }
    read() {
      if (!this.ce || this.state !== 3 || this.mode !== 0x0c) return 0;
      const v = this.regs[this.index]; this.index = (this.index + 1) & 15; return v;
    }
    // 8 bytes kept with the battery RAM: offset (seconds, 32-bit) and control registers
    bytes() { const o = new Uint8Array(8); new DataView(o.buffer).setInt32(0, this.offset, true); o[4] = 0x52; o[5] = this.ctl[0]; o[6] = this.ctl[1]; o[7] = this.ctl[2]; return o; }
    fromBytes(b) { if (b.length >= 8 && b[4] === 0x52) { this.offset = new DataView(b.buffer, b.byteOffset, 8).getInt32(0, true); this.ctl = [b[5], b[6], b[7]]; } }
    saveState() { return { offset: this.offset, ctl: this.ctl.slice(), ce: this.ce, state: this.state, mode: this.mode, index: this.index, regs: this.regs.slice(), dirty: this.dirty }; }
    loadState(o) { Object.assign(this, { offset: o.offset, ctl: o.ctl.slice(), ce: o.ce, state: o.state, mode: o.mode, index: o.index, dirty: o.dirty }); this.regs.set(o.regs); }
  }

  // ===================================================================
  // The chip
  // ===================================================================
  class SPC7110 {
    constructor(snes, opts) {
      this.snes = snes; this.cart = snes.cart;
      const rom = this.cart.rom;
      this.dataSize = Math.max(1, rom.length - 0x100000);
      this.dec = new Decompressor((a) => this.dataByte(a));
      this.rtc = this.cart.chip === 0xf9 ? new RTC4513() : null;
      if (this.rtc && opts && opts.sram && opts.sram.length >= this.cart.sramSize + 8) this.rtc.fromBytes(opts.sram.subarray(this.cart.sramSize, this.cart.sramSize + 8));
      Kit.claim(snes, 0x00, 0x3f, 0x4000, 0x4fff, true); // registers
      Kit.claim(snes, 0x00, 0x3f, 0x6000, 0xffff, true); // battery RAM, program ROM
      Kit.claim(snes, 0x40, 0x4f, 0x0000, 0xffff, false); // program ROM
      Kit.claim(snes, 0x50, 0x50, 0x0000, 0xffff, false); // decompressed data
      Kit.claim(snes, 0xc0, 0xff, 0x0000, 0xffff, false); // program ROM + data ROM windows
      this.reset();
    }
    reset() {
      this.r = new Uint8Array(0x50); // $4800-$484F register images
      this.r[0x31] = 0; this.r[0x32] = 1; this.r[0x33] = 2;
      this.portSet = 0; this.latch14 = false; this.latch15 = false;
      this.dec.ready = false;
      if (this.rtc) this.rtc.reset();
    }
    dataByte(a) { return this.cart.rom[0x100000 + (a % this.dataSize)] | 0; }
    progByte(a) { return this.cart.rom[a & 0xfffff] | 0; }
    get ptr() { return this.r[0x11] | (this.r[0x12] << 8) | (this.r[0x13] << 16); }
    set ptr(v) { this.r[0x11] = v & 0xff; this.r[0x12] = (v >> 8) & 0xff; this.r[0x13] = (v >> 16) & 0xff; }
    get adj() { return this.r[0x14] | (this.r[0x15] << 8); }
    set adj(v) { this.r[0x14] = v & 0xff; this.r[0x15] = (v >> 8) & 0xff; }
    get step() { return this.r[0x16] | (this.r[0x17] << 8); }

    // ---- bus ----
    read(bank, off) {
      const b = bank & 0x7f;
      if (bank >= 0xd0) return this.dataByte(((this.r[0x31 + ((bank - 0xd0) >> 4)] & 7) << 20) | ((bank & 0x0f) << 16) | off);
      if (bank >= 0xc0 || (b >= 0x40 && b < 0x50)) return this.progByte(((bank & 0x0f) << 16) | off);
      if (b === 0x50) return this.decRead();
      if (b >= 0x40) return -1;
      if (off >= 0x8000) return this.progByte(((bank & 0x0f) << 16) | off);
      if (off >= 0x6000) {
        if (!this.cart.sramSize || !(this.r[0x30] & 0x80)) return 0;
        return this.cart.sram[(((bank & 0x1f) << 13) | (off - 0x6000)) & this.cart.sramMask];
      }
      if (off >= 0x4800 && off < 0x4850) return this.readReg(off - 0x4800);
      return -1;
    }
    write(bank, off, v) {
      const b = bank & 0x7f;
      if (bank >= 0xc0 || (b >= 0x40 && b <= 0x50)) return true;
      if (b >= 0x40) return false;
      if (off >= 0x8000) return true;
      if (off >= 0x6000) {
        if (this.cart.sramSize && (this.r[0x30] & 0x80)) {
          const i = (((bank & 0x1f) << 13) | (off - 0x6000)) & this.cart.sramMask;
          if (this.cart.sram[i] !== v) { this.cart.sram[i] = v; this.cart.sramDirty = true; }
        }
        return true;
      }
      if (off >= 0x4800 && off < 0x4850) { this.writeReg(off - 0x4800, v); return true; }
      return false;
    }

    // ---- decompressor ----
    decRead() {
      const n = (this.r[0x09] | (this.r[0x0a] << 8)) - 1;
      this.r[0x09] = n & 0xff; this.r[0x0a] = (n >> 8) & 0xff;
      return this.dec.next();
    }
    decStart() {
      const table = this.r[0x01] | (this.r[0x02] << 8) | (this.r[0x03] << 16);
      const ent = table + (this.r[0x04] << 2);
      const mode = this.dataByte(ent);
      const addr = (this.dataByte(ent + 1) << 16) | (this.dataByte(ent + 2) << 8) | this.dataByte(ent + 3);
      this.dec.start(mode, addr, (this.r[0x05] | (this.r[0x06] << 8)) << Math.min(2, mode));
      this.r[0x0c] = 0x80;
    }

    // ---- data port ----
    portRead() {
      if (this.portSet !== 7) return 0;
      const m = this.r[0x18], addr = this.ptr;
      let adj = this.adj; if (m & 8) adj = s16(adj);
      let a = addr;
      if (m & 2) { a = addr + adj; this.adj = adj + 1; }
      const v = this.dataByte(a & 0xffffff);
      if (!(m & 2)) {
        let inc = m & 1 ? this.step : 1; if (m & 4) inc = s16(inc);
        if (!(m & 0x10)) this.ptr = addr + inc; else this.adj = adj + inc;
      }
      return v;
    }
    portReadAdjusted() {
      if (this.portSet !== 7) return 0;
      const m = this.r[0x18], addr = this.ptr;
      let adj = this.adj; if (m & 8) adj = s16(adj);
      const v = this.dataByte((addr + adj) & 0xffffff);
      if ((m & 0x60) === 0x60) { if (!(m & 0x10)) this.ptr = addr + adj; else this.adj = adj + adj; }
      return v;
    }
    // writing the adjust value can move the pointer straight away
    adjustWritten() {
      const m = this.r[0x18];
      if (!(this.latch14 && this.latch15) || !(m & 2) || (m & 0x10)) return;
      if ((m & 0x60) === 0x20) { let inc = this.adj & 0xff; if (m & 8) inc = Kit.s8(inc); this.ptr = this.ptr + inc; }
      else if ((m & 0x60) === 0x40) { let inc = this.adj; if (m & 8) inc = s16(inc); this.ptr = this.ptr + inc; }
    }

    // ---- registers ----
    readReg(i) {
      switch (i) {
        case 0x00: return this.decRead();
        case 0x0c: { const v = this.r[0x0c]; this.r[0x0c] &= 0x7f; return v; }
        case 0x10: return this.portRead();
        case 0x1a: return this.portReadAdjusted();
        case 0x2f: return 0; // ALU never busy
        case 0x41: return this.rtc ? this.rtc.read() : 0;
        case 0x42: return 0x80; // clock ready
      }
      return this.r[i];
    }
    writeReg(i, v) {
      this.r[i] = v;
      switch (i) {
        case 0x06: this.decStart(); break;
        case 0x11: this.portSet |= 1; break;
        case 0x12: this.portSet |= 2; break;
        case 0x13: this.portSet |= 4; break;
        case 0x14: this.latch14 = true; this.adjustWritten(); break;
        case 0x15: this.latch15 = true; this.adjustWritten(); break;
        case 0x18: this.latch14 = this.latch15 = false; break;
        case 0x25: this.multiply(); break;
        case 0x27: this.divide(); break;
        case 0x2e: this.r[0x2e] = v & 1; break;
        case 0x40: if (this.rtc) this.rtc.setCE(v); break;
        case 0x41: if (this.rtc) this.rtc.write(v); break;
      }
    }
    multiply() {
      const r = this.r, signed = r[0x2e] & 1;
      let a = r[0x20] | (r[0x21] << 8), b = r[0x24] | (r[0x25] << 8);
      if (signed) { a = s16(a); b = s16(b); }
      const p = (a * b) >>> 0;
      r[0x28] = p & 0xff; r[0x29] = (p >>> 8) & 0xff; r[0x2a] = (p >>> 16) & 0xff; r[0x2b] = p >>> 24;
    }
    divide() {
      const r = this.r, signed = r[0x2e] & 1;
      let n = (r[0x20] | (r[0x21] << 8) | (r[0x22] << 16) | (r[0x23] << 24)) >>> 0, d = r[0x26] | (r[0x27] << 8);
      if (signed) { n |= 0; d = s16(d); }
      let q, m;
      if (d === 0) { q = 0; m = n & 0xffff; } else { q = Math.trunc(n / d); m = n - q * d; }
      q >>>= 0; m &= 0xffff;
      r[0x28] = q & 0xff; r[0x29] = (q >>> 8) & 0xff; r[0x2a] = (q >>> 16) & 0xff; r[0x2b] = q >>> 24;
      r[0x2c] = m & 0xff; r[0x2d] = m >> 8;
    }

    getSRAM() {
      const c = this.cart;
      const s = c.sramSize ? c.sram.slice(0, c.sramSize) : new Uint8Array(0);
      if (!this.rtc) return c.sramSize ? s : null;
      return Kit.joinSRAM(s, this.rtc.bytes());
    }
    saveState() {
      return { r: this.r.slice(), portSet: this.portSet, l: [this.latch14 ? 1 : 0, this.latch15 ? 1 : 0], dec: this.dec.saveState(), rtc: this.rtc ? this.rtc.saveState() : null };
    }
    loadState(o) {
      this.r.set(o.r); this.portSet = o.portSet; this.latch14 = !!o.l[0]; this.latch15 = !!o.l[1]; this.dec.loadState(o.dec);
      if (this.rtc && o.rtc) this.rtc.loadState(o.rtc);
    }
  }

  global.RPSNESChips.push({ name: "SPC7110", detect: (c) => c.chip === 0xf9 || (c.chip === 0xf5 && Kit.mapMode(c) === 0x3a), create: (snes, opts) => new SPC7110(snes, opts) });
  global.RPSNESSPC7110 = { SPC7110, Decompressor, RTC4513, EVOLUTION, TREE2 };
})(typeof window !== "undefined" ? window : globalThis);
