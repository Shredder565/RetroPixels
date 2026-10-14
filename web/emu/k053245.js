// RetroPixels — Konami sprite chips: K053245 + K053244 (TMNT2 / Turtles in Time)
// and K053247 + K053246 (The Simpsons). Original code, written from the chips'
// documented behaviour. The two pairs work the same way, so one class does both.
//
// Sprite list: 16 bytes (8 big-endian words) per sprite, 128 sprites on the
// K053245, 256 on the K053247:
//   word 0  bit 15 sprite on; bit 14 keep aspect (the X zoom follows the Y zoom);
//           bit 13 flip Y; bit 12 flip X; bits 10-11 height and bits 8-9 width
//           (1, 2, 4 or 8 tiles of 16x16); bits 0-7 drawing order (lower = in front)
//   word 1  tile number (multi-tile sprites take their tiles in Konami's interleaved order:
//           columns add 0, 1, 4, 5, 16, 17, 20, 21 and rows add 0, 2, 8, 10, 32, 34, 40, 42)
//   word 2  Y of the sprite's centre (10 bits, upwards); word 3 X of the centre (10 bits)
//   word 4  Y zoom, word 5 X zoom: 0x40 is normal size, smaller is bigger (size = 0x40 / zoom)
//   word 6  attributes. K053245: bits 0-4 colour, 5-6 priority, 7 shadow, 8 mirror X,
//           9 mirror Y. K053247: bits 0-11 colour / priority as the board wires them,
//           12 shadow, 14 mirror X, 15 mirror Y.
//           "Mirror" draws the second half of the sprite as a reflection of the first (K053245,
//           whole tiles). The K053247 mirrors pixel by pixel about the sprite's centre: the top
//           (left) half shows the bottom (right) half reflected, so a half-drawn shape such as a
//           shadow under a character's feet comes out whole.
//   word 7  unused
// The board's sprite callback turns the attribute word into a colour (palette entry =
// colour * 16 + pen) and a priority value for the K053251 mixer, and can change the shadow flag.
// Shadow sprites turn their pen 15 into a shadow; pen 0 is always see-through.
//
// Graphics: 16x16 tiles of 4 bits a pixel, 128 bytes each, in one of two layouts:
//   "planar" (K053245 boards): four 32-byte quarters (top left, top right, bottom left,
//            bottom right) of 8 rows x 4 bytes; byte n of a row is colour bit n of its 8
//            pixels, leftmost pixel in bit 7 (the same as the K052109's tiles).
//   "packed" (K053247 boards): 16 rows of 8 bytes; each pair of bytes is a little-endian
//            16-bit word whose four nibbles, from the top one down, are four pixels.
//
// Control chip registers:
//   K053244 (16): 0-1 X offset, 2-3 Y offset (high byte first), 5 bit 0 flip X, bit 1 flip Y,
//            bit 4 lets the CPU read the sprite ROMs; 6 (written) copies the sprite list into the
//            chip's own buffer, which is what gets drawn; 8-10 ROM read address; 12-15 read: ROM bytes.
//   K053246 (8): 0-1 X offset, 2-3 Y offset (high, low), 4, 6, 7 ROM read address, 5 bit 0 flip X,
//            bit 1 flip Y, bit 4 sprite DMA / interrupt enable (boards read k.dmaEnabled).
//            The Simpsons board copies its sprite RAM itself (see arcade_simpsons.js).
//
// Interface:
//   const k = new RPK053245({ type: "053245" | "053247", rom, layout: "planar" | "packed",
//                             spriteCallback(t), dx, dy })
//     t = { attr, color, pri, shadow, code }: set t.color / t.pri (/ t.shadow / t.code)
//     dx, dy: the board's fixed offsets from sprite coordinates to its picture (pixels)
//   k.ram                    the sprite list the CPU writes (Uint8Array, big-endian words)
//   k.buffer                 the copy that gets drawn (K053245: after a write to register 6;
//                            K053247 boards copy into it themselves)
//   k.readWord(i) / k.writeWord(i, v)   sprite RAM by word
//   k.regWrite(r, v) / k.regRead(r)     the K053244 / K053246 registers (ROM read on 053244 12-15)
//   k.readRom(off)           byte off of the sprite ROMs (the CPU's ROM test)
//   k.latch()                copy ram into buffer (what register 6 does)
//   k.render(pens, pri, shadow, w, h)   draw the buffer's sprites into a w x h picture:
//                            pens Uint16Array (colour * 16 + pen), pri Uint8Array, shadow Uint8Array
//                            (cleared to 0 by the caller). The picture's (0, 0) is the board's
//                            top-left visible pixel after dx / dy.
//   k.getState() / k.setState(o)
"use strict";

(function (global) {
  const XOFF = [0, 1, 4, 5, 16, 17, 20, 21];
  const YOFF = [0, 2, 8, 10, 32, 34, 40, 42];

  class RPK053245 {
    constructor(opts) {
      opts = opts || {};
      this.type = opts.type === "053247" ? "053247" : "053245";
      this.count = this.type === "053247" ? 256 : 128;
      this.ram = new Uint8Array(this.count * 16);
      this.buffer = new Uint8Array(this.count * 16);
      this.regs = new Uint8Array(16);
      this.rom = opts.rom || new Uint8Array(128);
      this.spriteCallback = opts.spriteCallback || null;
      this.dx = opts.dx || 0; this.dy = opts.dy || 0;
      this.pens = RPK053245.decode(this.rom, opts.layout || (this.type === "053247" ? "packed" : "planar"));
      this.tiles = this.pens.length >> 8;
      this.t = { attr: 0, color: 0, pri: 0, shadow: false, code: 0 };
      this.order = new Uint16Array(this.count);
    }

    static decode(rom, layout) {
      const n = rom.length >> 7, pens = new Uint8Array(Math.max(1, n) * 256);
      for (let t = 0; t < n; t++) {
        const base = t * 128, out = t * 256;
        if (layout === "packed") {
          for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
            const word = base + y * 8 + (x >> 2) * 2;
            const v = rom[word] | (rom[word + 1] << 8);
            pens[out + y * 16 + x] = (v >> (12 - (x & 3) * 4)) & 15;
          }
        } else {
          for (let q = 0; q < 4; q++) for (let y = 0; y < 8; y++) {
            const o = base + q * 32 + y * 4, b0 = rom[o], b1 = rom[o + 1], b2 = rom[o + 2], b3 = rom[o + 3];
            const py = y + (q & 2 ? 8 : 0), px0 = q & 1 ? 8 : 0;
            for (let x = 0; x < 8; x++) {
              const s = 7 - x;
              pens[out + py * 16 + px0 + x] = ((b0 >> s) & 1) | (((b1 >> s) & 1) << 1) | (((b2 >> s) & 1) << 2) | (((b3 >> s) & 1) << 3);
            }
          }
        }
      }
      return pens;
    }

    reset() { this.ram.fill(0); this.buffer.fill(0); this.regs.fill(0); }
    readWord(i) { i = (i % (this.count * 8)) * 2; return (this.ram[i] << 8) | this.ram[i + 1]; }
    writeWord(i, v) { i = (i % (this.count * 8)) * 2; this.ram[i] = (v >> 8) & 0xff; this.ram[i + 1] = v & 0xff; }
    latch() { this.buffer.set(this.ram); }

    get flipX() { return !!(this.regs[5] & 1); }
    get flipY() { return !!(this.regs[5] & 2); }
    get dmaEnabled() { return !!(this.regs[5] & 0x10); }
    get offsetX() {
      const v = (this.regs[0] << 8) | this.regs[1];
      return ((v & 0x3ff) << 22) >> 22;
    }
    get offsetY() {
      const v = (this.regs[2] << 8) | this.regs[3];
      return ((v & 0x3ff) << 22) >> 22;
    }

    regWrite(r, v) {
      r &= 15;
      if (this.type === "053247") r &= 7;
      this.regs[r] = v & 0xff;
      if (this.type === "053245" && r === 6) this.latch();
    }
    regRead(r) {
      r &= 15;
      if (this.type === "053245") {
        if (r >= 12 && (this.regs[5] & 0x10)) {
          const addr = ((this.regs[10] & 0x3f) << 18) | (this.regs[9] << 10) | (this.regs[8] << 2) | (r & 3);
          return this.readRom(addr);
        }
        return 0;
      }
      // K053246: two ROM bytes at the address in registers 4, 6, 7
      const addr = (this.regs[6] << 17) | (this.regs[7] << 9) | (this.regs[4] << 1) | (r & 1);
      return this.readRom(addr);
    }
    readRom(off) { const n = this.rom.length; return n ? this.rom[(off >>> 0) % n] : 0xff; }

    render(outPens, outPri, outShadow, w, h) {
      const b = this.buffer, count = this.count, t = this.t, cb = this.spriteCallback;
      const pens = this.pens, tiles = this.tiles, is247 = this.type === "053247";
      const pixMirror = is247;   // the K053247 mirrors pixel by pixel about the sprite's centre
      // drawing order: the highest order value first, so lower values end up in front
      let n = 0;
      for (let i = 0; i < count; i++) if (b[i * 16] & 0x80) this.order[n++] = i;
      const ord = Array.from(this.order.subarray(0, n));
      ord.sort((p, q) => (b[q * 16 + 1] - b[p * 16 + 1]) || (q - p));
      const flipScreenX = this.flipX, flipScreenY = this.flipY;
      const offX = this.offsetX, offY = this.offsetY;
      for (const i of ord) {
        const o = i * 16;
        const w0 = (b[o] << 8) | b[o + 1];
        const code = (b[o + 2] << 8) | b[o + 3];
        const y = (((b[o + 4] << 8) | b[o + 5]) << 22) >> 22;
        const x = (((b[o + 6] << 8) | b[o + 7]) << 22) >> 22;
        let zy = ((b[o + 8] << 8) | b[o + 9]) & 0x3ff;
        let zx = ((b[o + 10] << 8) | b[o + 11]) & 0x3ff;
        const attr = (b[o + 12] << 8) | b[o + 13];
        if (w0 & 0x4000) zx = zy;
        const tw = 1 << ((w0 >> 8) & 3), th = 1 << ((w0 >> 10) & 3);
        let flipX = !!(w0 & 0x1000), flipY = !!(w0 & 0x2000);
        let mirrorX, mirrorY;
        t.attr = attr; t.code = code; t.pri = 0;
        if (is247) { t.color = attr & 0xff; t.shadow = !!(attr & 0x1000); mirrorX = !!(attr & 0x4000); mirrorY = !!(attr & 0x8000); }
        else { t.color = attr & 0x1f; t.shadow = !!(attr & 0x80); mirrorX = !!(attr & 0x100); mirrorY = !!(attr & 0x200); }
        if (cb) cb(t);
        // size on screen: 0x40 / zoom of the normal size (zoom 0 counts as half a step)
        const sx = 64 / (zx || 0.5), sy = 64 / (zy || 0.5);
        if (zx > 0x2000 || zy > 0x2000) continue;
        const fullW = tw * 16, fullH = th * 16;
        const dw = Math.round(fullW * sx), dh = Math.round(fullH * sy);
        if (dw <= 0 || dh <= 0) continue;
        // centre position on the picture; Y counts upwards in sprite space
        let cx = ((x + offX + 1024 + 512) & 0x3ff) - 512;
        let cy = ((-(y + offY) + 1024 + 512) & 0x3ff) - 512;
        cx += this.dx; cy += this.dy;
        let left = Math.round(cx - dw / 2), top = Math.round(cy - dh / 2);
        if (flipScreenX) { left = w - left - dw; flipX = !flipX; }
        if (flipScreenY) { top = h - top - dh; flipY = !flipY; }
        if (left >= w || top >= h || left + dw <= 0 || top + dh <= 0) continue;
        const color = (t.color << 4) & 0xfff0, pri = t.pri, shadow = t.shadow, baseCode = t.code;
        const x0 = Math.max(0, left), x1 = Math.min(w, left + dw), y0 = Math.max(0, top), y1 = Math.min(h, top + dh);
        for (let py = y0; py < y1; py++) {
          let v = Math.floor((py - top) * fullH / dh);
          if (flipY) v = fullH - 1 - v;
          if (pixMirror && mirrorY && v * 2 < fullH) v = fullH - 1 - v;
          let ty = v >> 4, fy = v & 15;
          if (!pixMirror && mirrorY && ty * 2 >= th) { ty = th - 1 - ty; fy = 15 - fy; }
          const row = py * w;
          for (let px = x0; px < x1; px++) {
            let u = Math.floor((px - left) * fullW / dw);
            if (flipX) u = fullW - 1 - u;
            if (pixMirror && mirrorX && u * 2 < fullW) u = fullW - 1 - u;
            let tx = u >> 4, fx = u & 15;
            if (!pixMirror && mirrorX && tx * 2 >= tw) { tx = tw - 1 - tx; fx = 15 - fx; }
            const tile = (baseCode + XOFF[tx] + YOFF[ty]) % tiles;
            const pen = pens[tile * 256 + fy * 16 + fx];
            if (!pen) continue;
            const at = row + px;
            if (shadow && pen === 15) { outShadow[at] = 1; outPri[at] = pri; continue; }
            outPens[at] = color | pen; outPri[at] = pri; outShadow[at] = 0;
          }
        }
      }
    }

    getState() { return { ram: this.ram.slice(), buffer: this.buffer.slice(), regs: this.regs.slice() }; }
    setState(o) { this.ram.set(o.ram); this.buffer.set(o.buffer); this.regs.set(o.regs); }
  }

  global.RPK053245 = RPK053245;
})(typeof window !== "undefined" ? window : globalThis);
