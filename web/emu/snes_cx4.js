// RetroPixels — SNES Cx4 cartridge chip (Mega Man X2 / X3). Original code.
//
// The Cx4 is a Hitachi HG51B processor running a fixed program. RetroPixels
// doesn't use that program: when the game starts one of the program's
// functions, the result is computed here directly (high-level emulation).
//
// Memory seen by the SNES at $00-$3F/$80-$BF:$6000-$7FFF:
//   $6000-$6BFF  3 KB work RAM (sprite lists, bitmaps, line tables ...)
//   $7F40-$7F47  copy unit: source (24-bit), length, destination; writing $7F47 copies
//   $7F4D        function page, $7F4F  start a function
//   $7F5E        status (never busy here)
//   $7F80-$7FAF  sixteen 24-bit registers used for parameters and results
// Below, RAM offsets are relative to $6000 (so $7F80 is $1F80).
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;
  const s16 = Kit.s16, s8 = Kit.s8;
  // A circle in 512 steps, 1.15 fixed point.
  const SIN = new Int16Array(512), COS = new Int16Array(512);
  for (let i = 0; i < 512; i++) { SIN[i] = Math.round(32767 * Math.sin(2 * Math.PI * i / 512)); COS[i] = Math.round(32767 * Math.cos(2 * Math.PI * i / 512)); }
  // The processor's sixteen built-in constant registers, as its self-test
  // function copies them out (all 0s / all 1s and single-byte / sign-bit masks).
  const CONSTANTS = [0x000000, 0xffffff, 0x00ff00, 0xff0000, 0x00ffff, 0xffff00, 0x800000, 0x7fffff,
    0x008000, 0x007fff, 0xff7fff, 0xffff7f, 0x010000, 0xfeffff, 0x000100, 0x00feff];
  const PROGRAM_SUM = 0x054336; // what the checksum function reports for the chip's program
  const RAD = 2 * Math.PI / 128;  // wireframe angles are 1/128 turns

  // set the four bitplane bits of one pixel in a 4bpp SNES tile row at RAM `a`
  function plot4(r, a, bit, c) {
    if (c & 1) r[a] |= bit;
    if (c & 2) r[a + 1] |= bit;
    if (c & 4) r[a + 16] |= bit;
    if (c & 8) r[a + 17] |= bit;
  }

  class Cx4 {
    constructor(snes) {
      this.snes = snes; this.cart = snes.cart;
      this.ram = new Uint8Array(0x2000);
      Kit.claim(snes, 0x00, 0x3f, 0x6000, 0x7fff, true);
      // last point / step values of the wireframe functions (kept in save states)
      this.w = { x: 0, y: 0, z: 0, x2: 0, y2: 0, dist: 0, scale: 0 };
    }
    reset() { this.ram.fill(0); }
    // ---- small helpers ----
    w16(a) { return this.ram[a] | (this.ram[a + 1] << 8); }
    s16at(a) { return s16(this.w16(a)); }
    w24(a) { return this.ram[a] | (this.ram[a + 1] << 8) | (this.ram[a + 2] << 16); }
    put16(a, v) { this.ram[a] = v & 0xff; this.ram[a + 1] = (v >> 8) & 0xff; }
    put24(a, v) { this.put16(a, v); this.ram[a + 2] = (v >> 16) & 0xff; }
    reg24s(a) { return s8(this.ram[a + 2]) * 65536 + this.w16(a); } // signed 24-bit register
    // a byte anywhere in the cartridge / work RAM by 24-bit SNES address
    bus(addr) {
      const bank = (addr >> 16) & 0xff, off = addr & 0xffff;
      if ((bank & 0x40) === 0 && off >= 0x6000 && off < 0x8000) return this.ram[off - 0x6000];
      if (bank === 0x7e || bank === 0x7f) return this.snes.wram[((bank & 1) << 16) | off];
      const v = this.cart.read(bank, off);
      return v < 0 ? 0 : v;
    }
    busBE16(addr) { return (this.bus(addr) << 8) | this.bus(addr + 1); } // big-endian word in ROM
    // ---- bus side ----
    read(bank, off) {
      if (off < 0x6000 || off >= 0x8000) return -1;
      if (off === 0x7f5e) return 0; // not busy
      return this.ram[off - 0x6000];
    }
    write(bank, off, v) {
      if (off < 0x6000 || off >= 0x8000) return false;
      this.ram[off - 0x6000] = v;
      if (off === 0x7f47) this.copy();
      else if (off === 0x7f4f) this.run(v);
      return true;
    }
    copy() {
      const src = this.w24(0x1f40), len = this.w16(0x1f43), dst = this.w16(0x1f45);
      for (let i = 0; i < len; i++) {
        const d = (dst + i) & 0xffff;
        if (d >= 0x6000 && d < 0x8000) this.ram[d - 0x6000] = this.bus((src + i) & 0xffffff);
      }
    }
    run(fn) {
      const r = this.ram, page = r[0x1f4d];
      if (page === 0x0e && fn < 0x40 && (fn & 3) === 0) { r[0x1f80] = fn >> 2; return; } // page test
      if (fn === 0) {
        const job = { 0x00: () => this.buildOAM(), 0x03: () => this.scaleRotate(0), 0x05: () => this.transformLines(), 0x07: () => this.scaleRotate(64),
          0x08: () => this.wireframe(), 0x0b: () => this.disintegrate(), 0x0c: () => this.wave() }[page];
        if (job) job();
        return;
      }
      switch (fn) {
        case 0x01: r.fill(0, 0x300, 0xc00); this.wireframe(); break;
        case 0x05: { // propulsion: 256 x speed / distance
          const dist = this.w16(0x1f83);
          this.put16(0x1f80, dist ? (Math.floor(65536 / dist) * this.w16(0x1f81)) >> 8 : 65536);
          break;
        }
        case 0x0d: { // stretch a vector to a given length
          const x = this.s16at(0x1f80), y = this.s16at(0x1f83), want = this.s16at(0x1f86);
          const len = Math.hypot(x, y), k = len ? want / len : 0;
          this.put16(0x1f89, Math.trunc(x * k)); this.put16(0x1f8c, Math.trunc(y * k));
          break;
        }
        case 0x10: case 0x13: { // polar -> rectangular
          const ang = this.w16(0x1f80) & 0x1ff, radius = this.s16at(0x1f83);
          if (fn === 0x13) { // 16.8 results
            this.put24(0x1f86, (2 * radius * COS[ang]) >> 8);
            this.put24(0x1f89, (2 * radius * SIN[ang]) >> 8);
          } else { // whole numbers; the chip's Y comes out 1/64 short
            const y = (2 * radius * SIN[ang]) >> 16;
            this.put24(0x1f86, (2 * radius * COS[ang]) >> 16);
            this.put24(0x1f89, y - (y >> 6));
          }
          break;
        }
        case 0x15: { const x = this.s16at(0x1f80), y = this.s16at(0x1f83); this.put16(0x1f80, Math.trunc(Math.hypot(x, y))); break; } // distance
        case 0x1f: { // direction of a vector, 512 steps per turn
          const x = this.s16at(0x1f80), y = this.s16at(0x1f83);
          let a;
          if (x === 0) a = y > 0 ? 128 : 384;
          else a = (Math.trunc(Math.atan(y / x) * 256 / Math.PI) + (x < 0 ? 256 : 0)) & 0x1ff;
          this.put16(0x1f86, a);
          break;
        }
        case 0x22: this.trapezoid(); break;
        case 0x25: case 0x54: { // 24 x 24 -> 48-bit product (0x54 squares its one input)
          const a = BigInt(this.reg24s(0x1f80)), b = fn === 0x54 ? a : BigInt(this.reg24s(0x1f83));
          const p = BigInt.asUintN(48, a * b), at = fn === 0x54 ? 0x1f83 : 0x1f80;
          this.put24(at, Number(p & 0xffffffn)); this.put24(at + 3, Number(p >> 24n));
          break;
        }
        case 0x2d: { // turn and scale one 3D point
          const [x, y] = this.turn3D(this.s16at(0x1f81), this.s16at(0x1f84), this.s16at(0x1f87), r[0x1f89], r[0x1f8a], r[0x1f8b], this.w16(0x1f90), false);
          this.put16(0x1f80, x); this.put16(0x1f83, y);
          break;
        }
        case 0x40: { let sum = 0; for (let i = 0; i < 0x800; i++) sum += r[i]; this.put16(0x1f80, sum); break; } // RAM checksum
        case 0x5c: CONSTANTS.forEach((v, i) => this.put24(i * 3, v)); break;
        case 0x89: this.put24(0x1f80, PROGRAM_SUM); break;
      }
    }

    // ---- sprites: expand the game's object list into an OAM image at $6000 ----
    // Objects (16 bytes each from $6220, count at $6620): X, Y (16-bit), two
    // attribute bytes ORed together, first tile, and a ROM pointer to a list of
    // pieces (count, then 4 bytes each: flags, X offset, Y offset, tile offset).
    // Sprites are written from slot $6626 on, the screen position taken
    // relative to ($6621, $6623); unused slots are moved below the screen.
    buildOAM() {
      const r = this.ram;
      let slot = r[0x626];
      for (let s = slot; s < 128; s++) r[s * 4 + 1] = 0xe0;
      const camX = this.w16(0x621), camY = this.w16(0x623);
      const emit = (x, y, tile, attr, big) => {
        const o = slot * 4, hiByte = 0x200 + (slot >> 2), sh = (slot & 3) * 2;
        r[o] = x & 0xff; r[o + 1] = y & 0xff; r[o + 2] = tile & 0xff; r[o + 3] = attr & 0xff;
        r[hiByte] = (r[hiByte] & ~(3 << sh)) | ((((x >> 8) & 1) | (big ? 2 : 0)) << sh);
        slot++;
      };
      for (let n = 0; n < r[0x620] && slot < 128; n++) {
        const ob = 0x220 + n * 16;
        const ox = s16(this.w16(ob) - camX), oy = s16(this.w16(ob + 2) - camY);
        const tile = r[ob + 5], attr = r[ob + 4] | r[ob + 6];
        const list = r[ob + 7] | (r[ob + 8] << 8) | (r[ob + 9] << 16);
        const pieces = this.bus(list);
        if (!pieces) { emit(ox, oy, tile, attr, true); continue; } // a lone 16x16 sprite
        for (let k = 0; k < pieces && slot < 128; k++) {
          const pc = list + 1 + k * 4, flags = this.bus(pc), big = (flags & 0x20) !== 0, size = big ? 16 : 8;
          // offsets are mirrored with the object
          const dx = s8(this.bus(pc + 1)), dy = s8(this.bus(pc + 2));
          const x = ox + (attr & 0x40 ? -dx - size : dx), y = oy + (attr & 0x80 ? -dy - size : dy);
          if (x < -16 || x > 272 || y < -16 || y > 224) continue; // off screen
          emit(x, y, tile + this.bus(pc + 3), attr ^ (flags & 0xc0), big);
        }
      }
    }

    // ---- bitmap scale / rotate: 4bpp packed pixels at $6600 -> tiles at $6000 ----
    // Angle $7F80 (512 steps), scales $7F8F / $7F92 (1.0 = $1000), size
    // $7F89 x $7F8C (multiples of 8), centre ($7F83, $7F86). The output tile rows
    // are `pad` bytes apart beyond the picture's own width.
    scaleRotate(pad) {
      const r = this.ram;
      const xs = Math.min(this.w16(0x1f8f), 0x7fff), ys = Math.min(this.w16(0x1f92), 0x7fff);
      const ang = this.w16(0x1f80);
      // source steps per output pixel (A, C) and per output line (B, D), 4.12;
      // exact for quarter turns
      let A, B, C, D;
      switch (ang) {
        case 0: A = xs; B = 0; C = 0; D = ys; break;
        case 128: A = 0; B = -ys; C = xs; D = 0; break;
        case 256: A = -xs; B = 0; C = 0; D = -ys; break;
        case 384: A = 0; B = ys; C = -xs; D = 0; break;
        default: {
          const a = ang & 0x1ff;
          A = s16((COS[a] * xs) >> 15); B = s16(-((SIN[a] * ys) >> 15)); C = s16((SIN[a] * xs) >> 15); D = s16((COS[a] * ys) >> 15);
        }
      }
      const w = r[0x1f89] & ~7, h = r[0x1f8c] & ~7, rowBytes = w * 4 + pad;
      r.fill(0, 0, Math.min(0x2000, rowBytes * (h >> 3)));
      const cx = this.s16at(0x1f83), cy = this.s16at(0x1f86);
      // source position of output pixel (0, 0); positions wrap at 32 bits
      const u0 = (cx << 12) - cx * (A + B), v0 = (cy << 12) - cy * (C + D);
      for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
          const out = (y >> 3) * rowBytes + (y & 7) * 2 + (x >> 3) * 32;
          if (out + 17 >= 0x2000) continue;
          const sx = ((u0 + x * A + y * B) >>> 0) >>> 12, sy = ((v0 + x * C + y * D) >>> 0) >>> 12;
          if (sx >= w || sy >= h) continue;
          const i = sy * w + sx;
          plot4(r, out, 0x80 >> (x & 7), r[0x600 + (i >> 1)] >> ((i & 1) * 4));
        }
      }
    }

    // ---- wireframe maths ----
    // Turn (x, y, z) by angles ax, ay, az (1/128 turns, clockwise) about the X,
    // Y and Z axes in that order, then scale it: by scale / 256, or with
    // `persp` through a perspective camera $95 units away.
    turn3D(x, y, z, ax, ay, az, scale, persp) {
      const CAM = 0x95;
      if (persp) z -= CAM;
      const rot = (a, p, q) => { const c = Math.cos(-a * RAD), s = Math.sin(-a * RAD); return [p * c - q * s, p * s + q * c]; };
      [y, z] = rot(ax, y, z);
      [z, x] = rot(ay, z, x);
      [x, y] = rot(az, x, y);
      const k = persp ? scale / (0x90 * (z + CAM)) * CAM : scale / 0x100;
      return [s16(Math.trunc(x * k)), s16(Math.trunc(y * k))];
    }
    // Stepping a line from (x1, y1) to (x2, y2) one pixel at a time: the long
    // axis moves a whole pixel (256 in 8.8) per step, the other a fraction.
    // Returns [step x, step y, number of points].
    static lineSteps(x1, y1, x2, y2) {
      const dx = s16(x2 - x1), dy = s16(y2 - y1), ax = Math.abs(dx), ay = Math.abs(dy);
      if (ax > ay) return [Math.sign(dx) * 256, s16(Math.trunc(256 * dy / ax)), ax + 1];
      if (dy === 0) return [dx, dy, 0];
      return [s16(Math.trunc(256 * dx / ay)), Math.sign(dy) * 256, ay + 1];
    }
    // Turn the points at $6000 (16 bytes each, count in $7F80) to screen
    // positions, then make step tables at $6600 for the point pairs listed at $6B00.
    transformLines() {
      const r = this.ram, ax = r[0x1f83], ay = r[0x1f86], az = r[0x1f89], scale = r[0x1f8c];
      const points = this.w16(0x1f80), lines = this.w16(0xb00);
      for (let i = 0, p = 0; i < points && p + 10 < 0x2000; i++, p += 0x10) {
        const [x, y] = this.turn3D(this.s16at(p + 1), this.s16at(p + 5), this.s16at(p + 9), ax, ay, az, scale, true);
        this.put16(p + 1, x + 0x80); this.put16(p + 5, y + 0x50); // relative to the screen centre
      }
      // default step entries for the first two lines
      for (const o of [0x600, 0x608]) { this.put16(o, 23); this.put16(o + 2, 0x60); this.put16(o + 5, 0x40); }
      for (let i = 0, p = 0xb02, o = 0x600; i < lines && p < 0x1f00 && o < 0x1e00; i++, p += 2, o += 8) {
        const a = r[p] << 4, b = r[p + 1] << 4;
        const [sx, sy, n] = Cx4.lineSteps(this.s16at(a + 1), this.s16at(a + 5), this.s16at(b + 1), this.s16at(b + 5));
        this.put16(o, n || 1); this.put16(o + 2, sx); this.put16(o + 5, sy);
        Object.assign(this.w, { x: sx, y: sy, dist: n });
      }
    }
    // Draw the model whose line list is at the ROM address in $7F80: 5 bytes per
    // line (two point addresses in the same bank, colour); a first point of
    // $FFFF continues from where an earlier line ended. $6295 lines.
    wireframe() {
      const r = this.ram, bank = r[0x1f82] << 16;
      const point = (a) => [0, 2, 4].map((k) => s16(this.busBE16((bank | a) + k)));
      let line = this.w24(0x1f80);
      for (let n = r[0x295]; n > 0; n--, line += 5) {
        let from = this.busBE16(line);
        if (from === 0xffff) {
          let back = line - 5;
          while (back > 0 && this.busBE16(back + 2) === 0xffff) back -= 5;
          from = this.busBE16(back + 2);
        }
        this.drawLine(point(from), point(this.busBE16(line + 2)), this.bus(line + 4));
      }
    }
    // one line into the 96 x 96 pixel (12 x 12 tile) 2bpp picture at $6300
    drawLine(p1, p2, color) {
      const r = this.ram;
      const place = (p) => this.turn3D(p[0], p[1], p[2], r[0x1f86], r[0x1f87], r[0x1f88], r[0x1f90], false).map((v) => (v + 48) << 8);
      let [x, y] = place(p1);
      const [xe, ye] = place(p2);
      const [sx, sy, n] = Cx4.lineSteps(x >> 8, y >> 8, xe >> 8, ye >> 8);
      Object.assign(this.w, { x: sx, y: sy, dist: n });
      for (let i = Math.max(n, 1); i > 0; i--, x += sx, y += sy) {
        if (x < 0x100 || y < 0x100 || x >= 0x6000 || y >= 0x6000) continue;
        const px = x >> 8, py = y >> 8, a = 0x300 + (py >> 3) * 0xc0 + (px >> 3) * 16 + (py & 7) * 2, bit = 0x80 >> (px & 7);
        r[a] = color & 1 ? r[a] | bit : r[a] & ~bit;
        r[a + 1] = color & 2 ? r[a + 1] | bit : r[a + 1] & ~bit;
      }
    }

    // ---- left / right edges of a trapezoid for 225 screen lines -> $6800 / $6900 ----
    // Each edge leans by the tangent of its angle ($7F8C, $7F8F; 512 steps).
    trapezoid() {
      const r = this.ram;
      const slope = (a) => (COS[a] ? (Math.trunc(SIN[a] * 65536 / COS[a]) | 0) : -0x80000000); // 16.16
      const t1 = slope(this.w16(0x1f8c) & 0x1ff), t2 = slope(this.w16(0x1f8f) & 0x1ff);
      const shift = this.w16(0x1f86) - this.w16(0x1f80), width = this.w16(0x1f93);
      let y = s16(this.w16(0x1f83) - this.w16(0x1f89));
      for (let j = 0; j < 225; j++, y = s16(y + 1)) {
        let left = 1, right = 0; // an empty line
        if (y >= 0) {
          left = s16(Math.floor(t1 * y / 65536) + shift);
          right = s16(Math.floor(t2 * y / 65536) + shift + width);
          if (left < 0 && right < 0) { left = 1; right = 0; } else { left = Math.max(left, 0); right = Math.max(right, 0); }
          if (left > 255 && right > 255) { left = 255; right = 254; } else { left = Math.min(left, 255); right = Math.min(right, 255); }
        }
        r[0x800 + j] = left & 0xff; r[0x900 + j] = right & 0xff;
      }
    }

    // ---- wave effect on a 256 x 40 2bpp picture at $6000 (32 x 5 tiles) ----
    // Each 2-pixel-wide column is moved down by the signed byte at $6B00 +
    // (start $7F83 + column) & $7F, less 16 lines; the column is filled from the
    // top with the 8 rows at $6A00, then solid colour.
    wave() {
      const r = this.ram, start = r[0x1f83];
      for (let col = 0; col < 128; col++) {
        const tile = col >> 2, mask = 0xc0c0 >> ((col & 3) * 2);
        const shift = -s8(r[0xb00 + (col ? (start + col) & 0x7f : start)]) - 16;
        for (let row = 0; row < 40; row++) {
          const a = tile * 16 + (row >> 3) * 0x200 + (row & 7) * 2, src = row + shift;
          const fill = src < 0 ? 0 : src < 8 ? this.w16(0xa00 + src * 2) : 0xff00;
          this.put16(a, (this.w16(a) & ~mask) | (fill & mask));
        }
      }
    }

    // ---- scatter a 4bpp bitmap at $6600 (scaled about a centre) into tiles at $6000 ----
    // Size $7F89 x $7F8C, centre ($7F80, $7F83), scales $7F86 / $7F8F (8.8).
    disintegrate() {
      const r = this.ram;
      const w = r[0x1f89], h = r[0x1f8c];
      const cx = this.s16at(0x1f80), cy = this.s16at(0x1f83), kx = this.s16at(0x1f86), ky = this.s16at(0x1f8f);
      r.fill(0, 0, Math.min(0x2000, (w * h) >> 1));
      for (let i = 0; i < h; i++) {
        const ty = (cy << 8) + (i - cy) * ky, py = ty >> 8; // 8.8 target row
        for (let j = 0; j < w; j++) {
          const tx = (cx << 8) + (j - cx) * kx, px = tx >> 8;
          const src = 0x600 + i * (w >> 1) + (j >> 1); // source pixels, two per byte
          if (px < 0 || px >= w || py < 0 || py >= h || py * w + px >= 0x2000 || src >= 0x2000) continue;
          const out = (ty >> 11) * w * 4 + (tx >> 11) * 32 + (py & 7) * 2;
          if (out + 17 < 0x2000) plot4(r, out, 0x80 >> (px & 7), r[src] >> ((j & 1) * 4));
        }
      }
    }

    saveState() { return { ram: this.ram.slice(), w: Object.assign({}, this.w) }; }
    loadState(o) { this.ram.set(o.ram); Object.assign(this.w, o.w); }
  }

  global.RPSNESChips.push({ name: "Cx4", detect: (c) => c.chip === 0xf3, create: (snes) => new Cx4(snes) });
  global.RPSNESCx4 = Cx4;
})(typeof window !== "undefined" ? window : globalThis);
