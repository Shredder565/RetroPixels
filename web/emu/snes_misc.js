// RetroPixels — smaller SNES cartridge chips: OBC1 (Metal Combat) and the
// Sharp S-RTC clock (Daikaijuu Monogatari II). Original code, written from the
// documented behaviour of the chips.
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;

  // ===================================================================
  // OBC1: 8 KB battery RAM at $6000-$7FFF with a window ($7FF0-$7FF6)
  // that reads and writes sprite (OAM) entries kept in that RAM.
  // ===================================================================
  class OBC1 {
    constructor(snes) {
      this.snes = snes; this.cart = snes.cart;
      this.own = this.cart.sramSize >= 0x2000 ? null : new Uint8Array(0x2000);
      Kit.claim(snes, 0x00, 0x3f, 0x6000, 0x7fff, true);
    }
    reset() {}
    ram(i) { return this.own ? this.own[i & 0x1fff] : this.cart.sram[i & this.cart.sramMask]; }
    set(i, v) {
      if (this.own) { this.own[i & 0x1fff] = v; return; }
      const k = i & this.cart.sramMask;
      if (this.cart.sram[k] !== v) { this.cart.sram[k] = v; this.cart.sramDirty = true; }
    }
    base() { return this.ram(0x1ff5) & 1 ? 0x1800 : 0x1c00; }
    index() { return this.ram(0x1ff6) & 0x7f; }
    read(bank, off) {
      if (off < 0x6000 || off >= 0x8000) return -1;
      const a = off - 0x6000;
      if (a >= 0x1ff0 && a <= 0x1ff3) return this.ram(this.base() + (this.index() << 2) + (a & 3));
      if (a === 0x1ff4) return this.ram(this.base() + 0x200 + (this.index() >> 2));
      return this.ram(a);
    }
    write(bank, off, v) {
      if (off < 0x6000 || off >= 0x8000) return false;
      const a = off - 0x6000;
      if (a >= 0x1ff0 && a <= 0x1ff3) this.set(this.base() + (this.index() << 2) + (a & 3), v);
      else if (a === 0x1ff4) {
        // the two extra bits (X high bit, size) of this sprite
        const i = this.base() + 0x200 + (this.index() >> 2), sh = (this.index() & 3) * 2;
        this.set(i, (this.ram(i) & ~(3 << sh)) | ((v & 3) << sh));
      } else this.set(a, v);
      return true;
    }
    getSRAM() { return this.own ? this.own.slice() : this.cart.sram.slice(0, this.cart.sramSize); }
    saveState() { return { own: this.own ? this.own.slice() : null }; }
    loadState(o) { if (this.own && o.own) this.own.set(o.own); }
  }

  // ===================================================================
  // Sharp S-RTC: a clock read and set 4 bits at a time through $2800 (read)
  // and $2801 (write). Thirteen digits: seconds, minutes, hours, day (ones and
  // tens each), month, year ones and tens, century (9 = 1900s), weekday.
  // RetroPixels keeps the time as an offset from the computer's clock and
  // stores that offset after the battery RAM.
  // ===================================================================
  class SRTC {
    constructor(snes, opts) {
      this.snes = snes; this.cart = snes.cart;
      this.offset = 0;
      const n = this.cart.sramSize;
      if (opts && opts.sram && opts.sram.length >= n + 8 && opts.sram[n + 4] === 0x53) this.offset = new DataView(opts.sram.buffer, opts.sram.byteOffset + n, 8).getInt32(0, true);
      Kit.claim(snes, 0x00, 0x3f, 0x2000, 0x2fff, true);
      this.reset();
    }
    reset() { this.mode = 0; this.index = -1; this.regs = new Uint8Array(13); }
    latch() {
      const d = new Date((Math.floor(Date.now() / 1000) + this.offset) * 1000), r = this.regs;
      const s = d.getUTCSeconds(), m = d.getUTCMinutes(), h = d.getUTCHours(), day = d.getUTCDate(), y = d.getUTCFullYear();
      r[0] = s % 10; r[1] = (s / 10) | 0; r[2] = m % 10; r[3] = (m / 10) | 0; r[4] = h % 10; r[5] = (h / 10) | 0;
      r[6] = day % 10; r[7] = (day / 10) | 0; r[8] = d.getUTCMonth() + 1; r[9] = y % 10; r[10] = ((y / 10) | 0) % 10; r[11] = ((y / 100) | 0) - 10; r[12] = d.getUTCDay();
    }
    commit() {
      const r = this.regs;
      const year = (r[11] + 10) * 100 + r[10] * 10 + r[9];
      const t = Date.UTC(year, Math.max(0, r[8] - 1), r[6] + r[7] * 10 || 1, r[4] + r[5] * 10, r[2] + r[3] * 10, r[0] + r[1] * 10);
      if (!isNaN(t)) {
        this.offset = Math.floor(t / 1000) - Math.floor(Date.now() / 1000);
        r[12] = new Date(t).getUTCDay(); // the chip works out the weekday itself
      }
    }
    read(bank, off) {
      if (off !== 0x2800) return -1;
      if (this.mode !== 1) return 0x00;
      if (this.index < 0) { this.latch(); this.index = 0; return 0x0f; }
      if (this.index > 12) { this.index = -1; return 0x0f; }
      return this.regs[this.index++];
    }
    write(bank, off, v) {
      if (off !== 0x2801) return false;
      v &= 15;
      if (v === 0x0d) { this.mode = 1; this.index = -1; return true; } // start reading
      if (v === 0x0e) { this.mode = 2; return true; } // command follows
      if (v === 0x0f) return true;
      if (this.mode === 2) {
        if (v === 0x00) { this.mode = 3; this.index = 0; } // set the time
        else if (v === 0x04) { this.mode = 0; this.regs.fill(0); this.regs[8] = 1; this.regs[6] = 1; this.regs[11] = 9; this.commit(); }
        else this.mode = 0;
        return true;
      }
      if (this.mode === 3 && this.index >= 0 && this.index < 12) {
        this.regs[this.index++] = v;
        if (this.index === 12) { this.commit(); this.index = 13; }
      }
      return true;
    }
    getSRAM() {
      const c = this.cart, tail = new Uint8Array(8);
      new DataView(tail.buffer).setInt32(0, this.offset, true); tail[4] = 0x53;
      return Kit.joinSRAM(c.sramSize ? c.sram.slice(0, c.sramSize) : new Uint8Array(0), tail);
    }
    saveState() { return { offset: this.offset, mode: this.mode, index: this.index, regs: this.regs.slice() }; }
    loadState(o) { this.offset = o.offset; this.mode = o.mode; this.index = o.index; this.regs.set(o.regs); }
  }

  global.RPSNESChips.push(
    { name: "OBC1", detect: (c) => c.chip === 0x25, create: (snes) => new OBC1(snes) },
    { name: "S-RTC", detect: (c) => c.chip === 0x55, create: (snes, opts) => new SRTC(snes, opts) },
  );
  global.RPSNESMisc = { OBC1, SRTC };
})(typeof window !== "undefined" ? window : globalThis);
