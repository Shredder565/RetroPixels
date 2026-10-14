// RetroPixels — Konami K051960 / K051937 sprite chips. Original code, written from the
// chips' documented behaviour. Used by TMNT and other Konami boards of 1988-1990.
//
// The K051960 holds 128 sprites of 8 bytes each and draws them (with the K051937 doing
// the pixels). Sprites are made of 16x16 cells (1x1 up to 8x8 cells), can be flipped and
// shrunk, and are sorted by a 7-bit priority. A sprite's cells use codes in Konami's
// zig-zag order: one step right adds 1, 4, 16 (for cell 1, 2, 4), one step down 2, 8, 32.
//
// Sprite bytes:
//   0  bit 7 active, bits 0-6 priority (lower values are drawn on top)
//   1  bits 5-7 size, bits 0-4 code bits 8-12
//   2  code bits 0-7
//   3  attribute (colour etc.; meaning is up to the board, see spriteCallback)
//   4  bits 2-7 vertical shrink, bit 1 flip upside down, bit 0 Y bit 8
//   5  Y bits 0-7 (Y counts up the screen: the top edge is at 256 - Y)
//   6  bits 2-7 horizontal shrink, bit 1 flip sideways, bit 0 X bit 8
//   7  X bits 0-7
// Sizes (cells across x down): 0 1x1, 1 2x1, 2 1x2, 3 2x2, 4 4x2, 5 2x4, 6 4x4, 7 8x8.
// Shrink s draws each cell (128 - s) / 128 of its size.
//
// ===========================================================================
// INTERFACE
// ===========================================================================
//   const k = new RPK051960({ rom, spriteCallback, decode, spriteStart })
//     rom             the sprite ROMs as one Uint8Array, 128 bytes per 16x16 code: four 8x8
//                     blocks (top-left, top-right, bottom-left, bottom-right), each 4 bytes a row
//                     where byte n holds bit n of the pixels' colour numbers (leftmost pixel in bit 7)
//     decode          optional: decode(rom) -> Uint8Array of one byte per pixel, 8 pixels per 32-bit
//                     ROM word, for boards whose ROMs hold their pixels differently
//     spriteStart     optional, for boards whose ROMs keep each whole sprite as one picture stored
//                     row after row (rather than in 16x16 cells): spriteStart(code, size) -> the ROM
//                     word (4 bytes, 8 pixels) where the sprite's top row starts; a sprite W cells
//                     across then has 2 * W words a row. code is the board's code (after spriteCallback).
//     readbackAddr    optional (word) -> word: the board's wiring of the chip's ROM address lines,
//                     applied to the readback address (reg 3 : reg 2 : latched low byte)
//     spriteCallback  t => { ... } with t = { code, color (byte 3), pri: 0, shadow: false };
//                     the board's wiring sets t.code, t.color (pens become color * 16 + pen),
//                     t.pri (copied to the priority buffer) and t.shadow (pen 15 marks shadow)
//   k.ram                         Uint8Array(0x400): the sprite list (the board maps it for its CPU)
//   k.read(off) / k.write(off, v) the sprite list, off 0..0x3ff
//   k.readReg(r) / k.writeReg(r, v)   K051937 registers 0-7: reg 0 bit 5 = ROM readback on
//                     (reads of 4-7 then return the 4 bytes of the ROM word whose address is reg 3 :
//                     reg 2 : the last sprite-list address read, bits 2-9); reg 1 bit 0 = flip
//                     screen; reading reg 0 returns a bit 0 that changes each read (drawing status)
//   k.dx, k.dy        board-specific offsets added to every sprite's position
//   k.render(out, w, h, opts)  draws the sprites into out (Uint16Array w*h, color * 16 + pen, 0 untouched
//                     where no sprite). opts: x0, y0 = sprite-space position of the window's
//                     top-left; pri (Uint8Array w*h) receives t.pri; shadow (Uint8Array w*h) is set to 1
//                     where a shadow pen lands (and cleared again where a sprite drawn later covers it).
//   k.saveState() / k.loadState(o)
"use strict";

(function (global) {
  const WIDTH = [1, 2, 1, 2, 4, 2, 4, 8], HEIGHT = [1, 1, 2, 2, 2, 4, 4, 8];
  const XOFF = [0, 1, 4, 5, 16, 17, 20, 21], YOFF = [0, 2, 8, 10, 32, 34, 40, 42];

  class K051960 {
    constructor(opts) {
      opts = opts || {};
      this.rom = opts.rom || new Uint8Array(128);
      this.spriteCallback = opts.spriteCallback || null;
      this.ram = new Uint8Array(0x400);
      this.regs = new Uint8Array(8);
      this.spriteStart = opts.spriteStart || null;
      this.pens = opts.decode ? opts.decode(this.rom) : K051960.decode(this.rom);
      this.readbackAddr = opts.readbackAddr || null;
      this.packed = !!(opts.decode && opts.spriteStart); // pens kept 8 to a ROM word, in ROM order
      this.codes = this.pens.length >> 8;
      this.dx = 0; this.dy = 0;
      this.t = { code: 0, color: 0, pri: 0, shadow: false };
      this.order = new Int32Array(128);
      this.toggle = 0; this.romLow = 0;
    }

    // 16x16 cells: four 8x8 blocks of 32 bytes; row byte n = colour bit n, leftmost pixel in bit 7
    static decode(rom) {
      const n = rom.length >> 7, pens = new Uint8Array(n * 256);
      for (let c = 0; c < n; c++) for (let q = 0; q < 4; q++) {
        const bx = (q & 1) * 8, by = (q >> 1) * 8;
        for (let y = 0; y < 8; y++) {
          const o = c * 128 + q * 32 + y * 4, b0 = rom[o], b1 = rom[o + 1], b2 = rom[o + 2], b3 = rom[o + 3];
          const d = c * 256 + (by + y) * 16 + bx;
          for (let x = 0; x < 8; x++) {
            const s = 7 - x;
            pens[d + x] = ((b0 >> s) & 1) | (((b1 >> s) & 1) << 1) | (((b2 >> s) & 1) << 2) | (((b3 >> s) & 1) << 3);
          }
        }
      }
      return pens;
    }

    reset() { this.ram.fill(0); this.regs.fill(0); this.toggle = 0; this.romLow = 0; }
    read(off) {
      // with ROM reading on, a sprite-list read also latches the low byte of the ROM address (A2-A9)
      if (this.regs[0] & 0x20) this.romLow = (off >> 2) & 0xff;
      return this.ram[off & 0x3ff];
    }
    write(off, v) { this.ram[off & 0x3ff] = v & 0xff; }
    get flipScreen() { return !!(this.regs[1] & 1); }
    readReg(r) {
      r &= 7;
      if (r === 0) { this.toggle ^= 1; return this.toggle; }
      if (r >= 4 && (this.regs[0] & 0x20)) {
        let w = (this.regs[3] << 16) | (this.regs[2] << 8) | this.romLow;
        if (this.readbackAddr) w = this.readbackAddr(w);
        if (this.packed) {
          // a packed board: the chip still shows the word in its planar form (byte n = bit n of the 8 pixels)
          const at = (w * 8) % this.pens.length, n = r & 3;
          let v = 0;
          for (let x = 0; x < 8; x++) v |= ((this.pens[at + x] >> n) & 1) << (7 - x);
          return v;
        }
        return this.rom.length ? this.rom[(w * 4 + (r & 3)) % this.rom.length] : 0;
      }
      return 0;
    }
    writeReg(r, v) { this.regs[r & 7] = v & 0xff; }

    render(out, w, h, opts) {
      opts = opts || {};
      const x0 = opts.x0 || 0, y0 = opts.y0 || 0, priBuf = opts.pri || null, shBuf = opts.shadow || null;
      const ram = this.ram, pens = this.pens, t = this.t, cb = this.spriteCallback, codes = this.codes;
      // sort: lower priority value on top, so draw higher values first; equal values: lower index on top
      let n = 0;
      for (let i = 0; i < 128; i++) if (ram[i * 8] & 0x80) this.order[n++] = i;
      const ord = Array.from(this.order.subarray(0, n));
      ord.sort((a, b) => ((ram[b * 8] & 0x7f) - (ram[a * 8] & 0x7f)) || (b - a));
      for (const i of ord) {
        const o = i * 8;
        const size = ram[o + 1] >> 5;
        t.code = ram[o + 2] | ((ram[o + 1] & 0x1f) << 8);
        t.color = ram[o + 3]; t.pri = 0; t.shadow = false;
        if (cb) cb(t);
        const code = t.code, color = t.color << 4, pri = t.pri, shadow = t.shadow;
        const zy = ram[o + 4] >> 2, zx = ram[o + 6] >> 2;
        const flipY = !!(ram[o + 4] & 2), flipX = !!(ram[o + 6] & 2);
        const xr = ((ram[o + 6] & 1) << 8) | ram[o + 7], yr = ((ram[o + 4] & 1) << 8) | ram[o + 5];
        const cw = WIDTH[size], ch = HEIGHT[size];
        const sw = Math.max(1, Math.round(16 * cw * (128 - zx) / 128)), shh = Math.max(1, Math.round(16 * ch * (128 - zy) / 128));
        const left = ((xr + this.dx - x0) % 512 + 512) % 512, top = ((256 - yr + this.dy - y0) % 512 + 512) % 512;
        // whole-picture ROM layout: pixel (x, y) of the sprite sits at word start + y * rowWords + x / 8
        const rowWords = 2 * cw, npens = pens.length;
        const start = this.spriteStart ? this.spriteStart(code, size) : -1;
        for (let py = 0; py < shh; py++) {
          let sy = top + py; if (sy >= 512) sy -= 512;
          if (sy >= h) continue;
          let srcY = Math.floor(py * 16 * ch / shh); if (flipY) srcY = 16 * ch - 1 - srcY;
          const cy = srcY >> 4, ry = srcY & 15, rowAt = start >= 0 ? (start + srcY * rowWords) * 8 : 0;
          for (let px = 0; px < sw; px++) {
            let sx = left + px; if (sx >= 512) sx -= 512;
            if (sx >= w) continue;
            let srcX = Math.floor(px * 16 * cw / sw); if (flipX) srcX = 16 * cw - 1 - srcX;
            let pen;
            if (start >= 0) pen = pens[(rowAt + srcX) % npens];
            else {
              const cx = srcX >> 4, rx = srcX & 15;
              const c = (code + XOFF[cx] + YOFF[cy]) % codes;
              pen = pens[c * 256 + ry * 16 + rx];
            }
            if (!pen) continue;
            const at = sy * w + sx;
            if (shadow && pen === 15) { if (shBuf) shBuf[at] = 1; continue; }
            out[at] = color | pen;
            if (priBuf) priBuf[at] = pri;
            if (shBuf) shBuf[at] = 0; // a sprite drawn over a shadow hides it
          }
        }
      }
    }

    saveState() { return { ram: this.ram.slice(), regs: this.regs.slice(), toggle: this.toggle, romLow: this.romLow }; }
    loadState(o) { this.ram.set(o.ram); this.regs.set(o.regs); this.toggle = o.toggle || 0; this.romLow = o.romLow || 0; }
  }

  global.RPK051960 = K051960;
})(typeof window !== "undefined" ? window : globalThis);
