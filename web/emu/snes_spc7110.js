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

  // The chip's adaptive probability states (53 of them, in four chains that a
  // context climbs while its guesses keep coming true). Each entry: [size of the
  // less likely symbol's share of the range (out of 256), next state after that
  // symbol, next state after a likely symbol that needed renormalising, 1 when the
  // less likely symbol makes the context swap which symbol it expects].
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
  // How the three neighbouring pixels (left a, above b, above-left c) relate:
  // all equal, a = b, b = c, a = c, or all different.
  function shape(a, b, c) {
    if (a === b) return b === c ? 0 : 1;
    if (b === c) return 2;
    return a === c ? 3 : 4;
  }
  // Context of the k-th decision (k = 0..3) for a mode 2 pixel, from the
  // decisions so far (`path`, earliest first) and the neighbour shape. The first
  // two decisions have fixed contexts; after a first decision of 0 the neighbour
  // shape is mixed in, and some branches share contexts.
  function ctxMode2(k, path, ref) {
    const p0 = (path >> (k - 1)) & 1;
    switch (k) {
      case 0: return 0;
      case 1: return 1 + (path & 1);
      case 2: return p0 ? 13 + (path & 1) : (path & 1 ? 8 : 3) + ref;
      default: {
        const p1 = (path >> 1) & 1, last = path & 1;
        if (!p0) return p1 ? 25 + last : 15 + 2 * ref + last;
        return 27 + 2 * p1 + last;
      }
    }
  }
  // A colour list kept in most-recently-used order.
  function bump(list, v) { const i = list.indexOf(v); if (i > 0) list.copyWithin(1, 0, i); list[0] = v; }

  class Decompressor {
    constructor(read) {
      this.readByte = read;
      this.idx = new Uint8Array(32); // probability state per context
      this.inv = new Uint8Array(32); // per context: 1 when the likely symbol is "1"
      this.queue = []; this.mode = 0; this.addr = 0; this.ready = false;
    }
    start(mode, addr, skip) {
      this.mode = mode > 2 ? 0 : mode; this.addr = addr;
      this.idx.fill(0); this.inv.fill(0); this.queue = [];
      // arithmetic decoder: an 8-bit window on the code stream and the current range
      this.code = this.nextIn(); this.inByte = this.nextIn(); this.inLeft = 8; this.range = 0xff;
      this.out = 0; this.out1 = 0; // recently decoded bits / pixels (newest lowest)
      this.order = this.mode === 2 ? Array.from({ length: 16 }, (_, i) => i) : [0, 1, 2, 3];
      this.ready = true;
      for (let i = 0; i < skip; i++) this.next();
    }
    nextIn() { const v = this.readByte(this.addr); this.addr = (this.addr + 1) & 0xffffff; return v; }
    // Decode one binary decision in context `con`. The context's state gives the
    // part of the range that belongs to the less likely symbol (at the top);
    // returns 1 when that symbol was coded, then adapts the context.
    decide(con) {
      const [prob, onLps, onMps, swap] = EVOLUTION[this.idx[con]];
      const split = this.range - prob; // likely symbol: codes 0..split
      let lps = 0;
      if (this.code > split) { lps = 1; this.code -= split + 1; this.range = prob - 1; } else this.range = split;
      let renorm = false;
      while (this.range < 0x7f) { // keep the range above half: shift in more code bits
        renorm = true;
        this.range = (this.range << 1) | 1;
        this.code = ((this.code << 1) | (this.inByte >> 7)) & 0xff;
        this.inByte = (this.inByte << 1) & 0xff;
        if (--this.inLeft === 0) { this.inByte = this.nextIn(); this.inLeft = 8; }
      }
      if (lps) { this.inv[con] ^= swap; this.idx[con] = onLps; } else if (renorm) this.idx[con] = onMps;
      return lps;
    }
    // decision in context `con` as a plain 0 / 1 (the likely symbol's value folded in)
    bitIn(con) { const inv = this.inv[con]; return this.decide(con) ^ inv; }
    next() {
      if (!this.ready) return 0;
      if (!this.queue.length) this.fill();
      return this.queue.shift();
    }
    // the colour order a pixel's index refers to: the neighbours first, then by recent use
    pixelOrder(a, b, c) {
      bump(this.order, a);
      const r = this.real = this.order.slice();
      bump(r, c); bump(r, b); bump(r, a);
      return r;
    }
    fill() {
      if (this.mode === 0) { // plain bytes, 4 bits at a time down a binary tree of contexts
        for (let half = 0; half < 2; half++) {
          let node = 0;
          for (let k = 0; k < 4; k++) {
            // a decision of 0 repeats the bit 16 places back, 1 flips it
            const d = this.bitIn(node + half * 15);
            this.out = ((this.out << 1) | (d ^ ((this.out >> 15) & 1))) >>> 0;
            node = 2 * node + 1 + d;
          }
        }
        this.queue.push(this.out & 0xff);
        return;
      }
      if (this.mode === 1) { // 2 bitplanes: each pixel is 2 decisions, a row of 8 pixels gives 2 bytes
        for (let px = 0; px < 8; px++) {
          const a = this.out & 3, b = (this.out >>> 14) & 3, c = (this.out >>> 16) & 3;
          const r = this.pixelOrder(a, b, c), sh = shape(a, b, c);
          const d0 = this.bitIn(sh);
          const d1 = this.bitIn(5 + 2 * sh + d0);
          this.out = ((this.out << 2) | r[(d0 << 1) | d1]) >>> 0;
        }
        this.queue.push(...planesOf(this.out, 2));
        return;
      }
      // mode 2: 4 bitplanes; a tile's planes 2-3 follow its planes 0-1
      const hi = [];
      for (let row = 0; row < 8; row++) {
        for (let px = 0; px < 8; px++) {
          const a = this.out & 15, b = (this.out >>> 28) & 15, c = this.out1 & 15;
          const r = this.pixelOrder(a, b, c), sh = shape(a, b, c);
          let path = 0;
          for (let k = 0; k < 4; k++) path = (path << 1) | this.bitIn(ctxMode2(k, path, sh));
          this.out1 = ((this.out1 << 4) | (this.out >>> 28)) >>> 0;
          this.out = ((this.out << 4) | r[path]) >>> 0;
        }
        const pl = planesOf(this.out, 4);
        this.queue.push(pl[0], pl[1]); hi.push(pl[2], pl[3]);
      }
      this.queue.push(...hi);
    }
    saveState() {
      return { mode: this.mode, addr: this.addr, ready: this.ready, idx: this.idx.slice(), inv: this.inv.slice(), queue: this.queue.slice(),
        r: [this.code, this.inByte, this.inLeft, this.range, this.out, this.out1], order: this.order ? this.order.slice() : null };
    }
    loadState(o) {
      this.mode = o.mode; this.addr = o.addr; this.ready = o.ready; this.idx.set(o.idx); this.inv.set(o.inv); this.queue = o.queue.slice();
      [this.code, this.inByte, this.inLeft, this.range, this.out, this.out1] = o.r; this.order = o.order ? o.order.slice() : null;
    }
  }
  // Split the 8 newest pixels of `bits` (bpp bits each, oldest highest) into bitplane bytes.
  function planesOf(bits, bpp) {
    const pl = new Array(bpp).fill(0);
    for (let i = 0; i < 8; i++) {
      const v = (bits >>> ((7 - i) * bpp)) & ((1 << bpp) - 1);
      for (let k = 0; k < bpp; k++) pl[k] |= ((v >> k) & 1) << (7 - i);
    }
    return pl;
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
  global.RPSNESSPC7110 = { SPC7110, Decompressor, RTC4513, EVOLUTION };
})(typeof window !== "undefined" ? window : globalThis);
