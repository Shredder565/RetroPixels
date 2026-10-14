// RetroPixels — Konami K052109 tilemap chip (with its K051962 pixel partner). Original code,
// written from the chip's documented behaviour. Used by many Konami arcade boards
// (TMNT, The Simpsons, TMNT2, Sunset Riders, X-Men...).
//
// What the chip does: three 64x32 maps of 8x8 tiles with 16 colours each — a fixed
// layer (0) and two scrolling layers A (1) and B (2). Layers A and B scroll as a
// whole, per line or per 8 lines horizontally, and per 8-pixel column vertically.
// Each map entry is an attribute byte plus a code byte; two attribute bits pick
// one of four ROM bank registers, and the board's own wiring (the tile callback)
// turns code, attribute and bank into the final tile number and colour.
//
// ===========================================================================
// INTERFACE
// ===========================================================================
//   const k = new RPK052109({ rom, tileCallback, extraCode, decode })
//     rom           the tile ROMs as one Uint8Array: 32 bytes per tile, 4 bytes per row of
//                   8 pixels; by default byte n of a row holds bit n of each pixel's colour
//                   number, leftmost pixel in bit 7. (A board whose ROM halves are laid out
//                   differently passes decode(rom) -> Uint8Array of one byte per pixel, 64 per tile.)
//     tileCallback  t => { ... } (the board's wiring). Called with t = { layer, bank, code, color,
//                   flipX, flipY, pri }: code = the code byte (plus the extra code byte << 8 when
//                   extraCode is set), color = the attribute byte with its bits 2-3 replaced by the
//                   low 2 bits of the selected bank register, bank = that register's top 2 bits.
//                   The callback sets t.code (final tile number), t.color (colour number: the
//                   tile's pens become color * 16 + pen) and may change t.flipX / t.flipY / t.pri.
//     extraCode     the board has the third RAM plane (0x4000-0x5fff) as code bits 8-15
//
//   k.read(off) / k.write(off, v)   the chip's own address space, off = 0x0000..0x5fff (bytes):
//       0x0000-0x07ff fixed layer attributes, 0x0800 layer A, 0x1000 layer B, 0x1800-0x1fff registers,
//       0x2000-0x3fff the same for the code bytes, 0x4000-0x5fff the extra plane.
//       Registers (they live in the RAM too and read back):
//         0x180c-0x1833 layer A column Y scroll (one per 8-pixel column, 0x180c = global)
//         0x1a00-0x1bff layer A X scroll: low byte, high bit (bit 0 of the next byte) per line
//         0x380c-0x3833 / 0x3a00-0x3bff the same for layer B
//         0x1c80 scroll mode: bits 0-2 layer A, 3-5 layer B: (mode & 3) 2 = X per 8 lines,
//                3 = X per line, else one X for all; bit 2 = Y per column
//         0x1d00 interrupt enables: bit 0 NMI, bit 1 FIRQ, bit 2 IRQ (boards read k.irqEnabled...)
//         0x1d80 tile ROM banks 0 (low nibble) and 1 (high nibble); 0x1f00 banks 2 and 3
//         0x1e00 ROM test bank (for reading the ROMs back); 0x1e80 bit 0 flip screen,
//                bit 1 lets attribute bit 0 flip tiles sideways, bit 2 lets attribute bit 1 flip them upside down
//   k.rmrd             set by the board while its "ROM read" line is on: read() then returns ROM bytes
//                      for every offset (the RAM is hidden meanwhile)
//   k.readRom(off)     the ROM byte shown at offset off while rmrd is on: register 0x1e00 picks an
//                      8 KB-row window (8192 rows of 4 bytes), off & 0x1fff the row and off >> 13 the
//                      byte of the row (RetroPixels' reading, used only by games' ROM tests)
//   opts.romRead       optional (off, chip) -> byte: a board whose wiring shows the ROMs differently
//                      while rmrd is on (see arcade_tmnt.js) passes its own reading instead of readRom
//   k.irqEnabled / k.firqEnabled / k.nmiEnabled / k.flipScreen   (from the registers)
//   k.dx[3], k.dy[3]   board-specific scroll offsets for layers 0..2 (pixels, default 0)
//   k.render(layer, out, w, h, opts)
//       draws one layer into out (Uint16Array of w*h, value = color * 16 + pen) for the w x h
//       window whose top-left is map pixel (opts.x0, opts.y0) of the unscrolled screen
//       (e.g. x0 = 96, y0 = 16 for a 320x224 picture: map columns 12..51, lines 16..239).
//       Pen 0 is see-through and left alone unless opts.opaque. opts.pri (Uint8Array w*h) gets
//       t.pri of every pixel drawn when given; opts.priMask: draw only tiles whose t.pri matches
//       opts.priValue under that mask.
//   k.saveState() / k.loadState(o)
"use strict";

(function (global) {
  class K052109 {
    constructor(opts) {
      opts = opts || {};
      this.ram = new Uint8Array(0x6000);
      this.rom = opts.rom || new Uint8Array(32);
      this.tileCallback = opts.tileCallback || null;
      this.extraCode = !!opts.extraCode;
      this.pens = opts.decode ? opts.decode(this.rom) : K052109.decode(this.rom);
      this.tileCount = this.pens.length >> 6;
      this.dx = [0, 0, 0]; this.dy = [0, 0, 0];
      this.rmrd = false;
      this.romRead = opts.romRead || null;
      // per-layer tile cache filled at render time
      this.tCode = new Int32Array(2048); this.tColor = new Uint16Array(2048);
      this.tFlags = new Uint8Array(2048); this.tPri = new Uint8Array(2048);
      this.t = { layer: 0, bank: 0, code: 0, color: 0, flipX: false, flipY: false, pri: 0 };
      this.reset();
    }

    // 4 bytes a row: byte n = colour bit n of the row's 8 pixels (leftmost in bit 7)
    static decode(rom) {
      const n = rom.length >> 5, pens = new Uint8Array(n * 64);
      for (let t = 0; t < n; t++) for (let y = 0; y < 8; y++) {
        const o = t * 32 + y * 4, b0 = rom[o], b1 = rom[o + 1], b2 = rom[o + 2], b3 = rom[o + 3];
        for (let x = 0; x < 8; x++) {
          const s = 7 - x;
          pens[t * 64 + y * 8 + x] = ((b0 >> s) & 1) | (((b1 >> s) & 1) << 1) | (((b2 >> s) & 1) << 2) | (((b3 >> s) & 1) << 3);
        }
      }
      return pens;
    }

    reset() { this.ram.fill(0); this.rmrd = false; }

    get scrollCtrl() { return this.ram[0x1c80]; }
    get irqEnabled() { return !!(this.ram[0x1d00] & 4); }
    get firqEnabled() { return !!(this.ram[0x1d00] & 2); }
    get nmiEnabled() { return !!(this.ram[0x1d00] & 1); }
    get flipScreen() { return !!(this.ram[0x1e80] & 1); }
    bankReg(i) { return i < 2 ? (this.ram[0x1d80] >> (i * 4)) & 15 : (this.ram[0x1f00] >> ((i - 2) * 4)) & 15; }

    read(off) {
      off &= 0x7fff;
      if (off >= 0x6000) return 0;
      if (this.rmrd) return this.romRead ? this.romRead(off, this) : this.readRom(off);
      return this.ram[off];
    }
    write(off, v) {
      off &= 0x7fff;
      if (off < 0x6000) this.ram[off] = v & 0xff;
    }
    // ROM test: the bank register 0x1e00 picks which window of the tile ROMs shows
    readRom(off) {
      const len = this.rom.length;
      return len ? this.rom[((((this.ram[0x1e00] << 13) | (off & 0x1fff)) * 4 + ((off >> 13) & 3)) >>> 0) % len] : 0xff;
    }

    // Look up every tile of one layer through the board's wiring.
    buildLayer(layer) {
      const ram = this.ram, t = this.t, cb = this.tileCallback, flipCtl = ram[0x1e80], base = layer * 0x800;
      const max = this.tileCount;
      for (let i = 0; i < 2048; i++) {
        let attr = ram[base + i];
        let code = ram[0x2000 + base + i];
        if (this.extraCode) code |= ram[0x4000 + base + i] << 8;
        const bankFull = this.bankReg((attr >> 2) & 3);
        attr = (attr & 0xf3) | ((bankFull & 3) << 2);
        t.layer = layer; t.bank = bankFull >> 2; t.code = code; t.color = attr; t.pri = 0;
        t.flipX = !!((flipCtl & 2) && (attr & 1));
        t.flipY = !!((flipCtl & 4) && (attr & 2));
        if (cb) cb(t);
        this.tCode[i] = ((t.code % max) + max) % max;
        this.tColor[i] = t.color;
        this.tFlags[i] = (t.flipX ? 1 : 0) | (t.flipY ? 2 : 0);
        this.tPri[i] = t.pri;
      }
    }

    render(layer, out, w, h, opts) {
      opts = opts || {};
      const x0 = opts.x0 || 0, y0 = opts.y0 || 0, opaque = !!opts.opaque, pri = opts.pri || null;
      const priMask = opts.priMask || 0, priValue = opts.priValue || 0;
      this.buildLayer(layer);
      const ram = this.ram, pens = this.pens, tCode = this.tCode, tColor = this.tColor, tFlags = this.tFlags, tPri = this.tPri;
      const flip = this.flipScreen;
      let mode = 0, sbase = 0;
      if (layer === 1) { mode = this.scrollCtrl & 7; sbase = 0x1800; }
      else if (layer === 2) { mode = (this.scrollCtrl >> 3) & 7; sbase = 0x3800; }
      const xmode = mode & 3, colY = !!(mode & 4);
      const dx = this.dx[layer], dy = this.dy[layer];
      for (let sy = 0; sy < h; sy++) {
        const line = (flip ? (y0 + h - 1 - sy) : (y0 + sy)) & 255;
        let xs = 0, ysG = 0;
        if (layer) {
          const li = xmode === 3 ? line : xmode === 2 ? line & 0xf8 : 0;
          xs = ram[sbase + 0x200 + li * 2] | ((ram[sbase + 0x201 + li * 2] & 1) << 8);
          ysG = ram[sbase + 0x0c];
        }
        const row = sy * w;
        for (let sx = 0; sx < w; sx++) {
          const scrX = flip ? (x0 + w - 1 - sx) : (x0 + sx);
          let ys = ysG;
          if (colY) ys = ram[sbase + ((scrX >> 3) & 63)];
          const mx = (scrX + xs + dx) & 511, my = (line + ys + dy) & 255;
          const i = (my >> 3) * 64 + (mx >> 3);
          if (priMask && (tPri[i] & priMask) !== priValue) continue;
          const f = tFlags[i];
          let px = mx & 7, py = my & 7;
          if (f & 1) px ^= 7;
          if (f & 2) py ^= 7;
          const pen = pens[tCode[i] * 64 + py * 8 + px];
          if (pen || opaque) {
            out[row + sx] = (tColor[i] << 4) | pen;
            if (pri) pri[row + sx] = tPri[i];
          }
        }
      }
    }

    saveState() { return { ram: this.ram.slice(), rmrd: this.rmrd, dx: this.dx.slice(), dy: this.dy.slice() }; }
    loadState(o) { this.ram.set(o.ram); this.rmrd = !!o.rmrd; if (o.dx) this.dx = o.dx.slice(); if (o.dy) this.dy = o.dy.slice(); }
  }

  global.RPK052109 = K052109;
})(typeof window !== "undefined" ? window : globalThis);
