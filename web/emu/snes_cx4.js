// RetroPixels — SNES Cx4 cartridge chip (Mega Man X2 / X3). Original code.
//
// The Cx4 is a Hitachi HG51B processor running a fixed program. RetroPixels
// doesn't use that program: when the game starts one of the documented
// functions, the result is computed here directly (high-level emulation).
//
// Memory seen by the SNES at $00-$3F/$80-$BF:$6000-$7FFF:
//   $6000-$6BFF  3 KB work RAM (sprite lists, bitmaps, line tables ...)
//   $7F40-$7F47  copy unit: source (24-bit), length, destination; writing $7F47 copies
//   $7F4D        function page, $7F4F  start a function
//   $7F5E        status (never busy here)
//   $7F80-$7FAF  sixteen 24-bit registers used for parameters and results
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;
  const s16 = Kit.s16, s8 = Kit.s8;
  // 512-step circle, 1.15 fixed point
  const SIN = new Int16Array(512), COS = new Int16Array(512);
  for (let i = 0; i < 512; i++) { SIN[i] = Math.round(32767 * Math.sin(2 * Math.PI * i / 512)); COS[i] = Math.round(32767 * Math.cos(2 * Math.PI * i / 512)); }
  // The chip's built-in constant registers (as read back by its self-test function).
  const IMMEDIATE = [0x000000, 0xffffff, 0x00ff00, 0xff0000, 0x00ffff, 0xffff00, 0x800000, 0x7fffff, 0x008000, 0x007fff, 0xff7fff, 0xffff7f, 0x010000, 0xfeffff, 0x000100, 0x00feff];
  // Row offsets of a 5-tile-high column in a 32-tile-wide 2bpp picture.
  const COLUMN = []; for (let i = 0; i < 40; i++) COLUMN.push((i >> 3) * 0x200 + (i & 7) * 2);

  class Cx4 {
    constructor(snes) {
      this.snes = snes; this.cart = snes.cart;
      this.ram = new Uint8Array(0x2000);
      Kit.claim(snes, 0x00, 0x3f, 0x6000, 0x7fff, true);
      // shared scratch values of the wireframe maths
      this.w = { x: 0, y: 0, z: 0, x2: 0, y2: 0, dist: 0, scale: 0 };
    }
    reset() { this.ram.fill(0); }
    // ---- small helpers ----
    w16(a) { return this.ram[a] | (this.ram[a + 1] << 8); }
    s16at(a) { return s16(this.w16(a)); }
    w24(a) { return this.ram[a] | (this.ram[a + 1] << 8) | (this.ram[a + 2] << 16); }
    put16(a, v) { this.ram[a] = v & 0xff; this.ram[a + 1] = (v >> 8) & 0xff; }
    put24(a, v) { this.ram[a] = v & 0xff; this.ram[a + 1] = (v >> 8) & 0xff; this.ram[a + 2] = (v >> 16) & 0xff; }
    // a byte anywhere in the cartridge / work RAM by 24-bit SNES address
    bus(addr) {
      const bank = (addr >> 16) & 0xff, off = addr & 0xffff;
      if ((bank & 0x40) === 0 && off >= 0x6000 && off < 0x8000) return this.ram[off - 0x6000];
      if (bank === 0x7e || bank === 0x7f) return this.snes.wram[((bank & 1) << 16) | off];
      const v = this.cart.read(bank, off);
      return v < 0 ? 0 : v;
    }
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
      const r = this.ram;
      if (r[0x1f4d] === 0x0e && fn < 0x40 && (fn & 3) === 0) { r[0x1f80] = fn >> 2; return; }
      switch (fn) {
        case 0x00:
          switch (r[0x1f4d]) {
            case 0x00: this.buildOAM(); break;
            case 0x03: this.scaleRotate(0); break;
            case 0x05: this.transformLines(); break;
            case 0x07: this.scaleRotate(64); break;
            case 0x08: this.wireframe(); break;
            case 0x0b: this.disintegrate(); break;
            case 0x0c: this.wave(); break;
          }
          break;
        case 0x01: r.fill(0, 0x300, 0x300 + 0x900); this.wireframe(); break;
        case 0x05: { // propulsion
          let t = 0x10000;
          const d = this.w16(0x1f83);
          if (d) t = (Math.trunc(t / d) * this.w16(0x1f81)) >> 8;
          this.put16(0x1f80, t);
          break;
        }
        case 0x0d: { // stretch a vector to a given length
          const x = this.s16at(0x1f80), y = this.s16at(0x1f83), d = this.s16at(0x1f86);
          const len = Math.hypot(x, y);
          const k = len ? d / len : 0;
          this.put16(0x1f89, Math.trunc(x * k)); this.put16(0x1f8c, Math.trunc(y * k));
          break;
        }
        case 0x10: { // polar -> rectangular (Y slightly shortened, as the chip does)
          const a = this.w16(0x1f80) & 0x1ff, rad = this.s16at(0x1f83);
          this.put24(0x1f86, (rad * COS[a] * 2) >> 16);
          const y = (rad * SIN[a] * 2) >> 16;
          this.put24(0x1f89, y - (y >> 6));
          break;
        }
        case 0x13: { // polar -> rectangular, 8 more fraction bits
          const a = this.w16(0x1f80) & 0x1ff, rad = this.s16at(0x1f83);
          this.put24(0x1f86, (rad * COS[a] * 2) >> 8);
          this.put24(0x1f89, (rad * SIN[a] * 2) >> 8);
          break;
        }
        case 0x15: { // distance
          const x = this.s16at(0x1f80), y = this.s16at(0x1f83);
          this.put16(0x1f80, Math.trunc(Math.sqrt(x * x + y * y)));
          break;
        }
        case 0x1f: { // angle of a vector (512 steps)
          const x = this.s16at(0x1f80), y = this.s16at(0x1f83);
          let a;
          if (x === 0) a = y > 0 ? 0x080 : 0x180;
          else { a = Math.trunc(Math.atan(y / x) / (2 * Math.PI) * 512); if (x < 0) a += 0x100; a &= 0x1ff; }
          this.put16(0x1f86, a);
          break;
        }
        case 0x22: this.trapezoid(); break;
        case 0x25: { // 24 x 24 -> 48-bit multiply
          const a = BigInt(s8(r[0x1f82]) * 65536 + this.w16(0x1f80)), b = BigInt(s8(r[0x1f85]) * 65536 + this.w16(0x1f83));
          const p = BigInt.asUintN(48, a * b);
          this.put24(0x1f80, Number(p & 0xffffffn)); this.put24(0x1f83, Number(p >> 24n));
          break;
        }
        case 0x2d: { // rotate and scale one 3D point
          const w = this.w;
          w.x = this.s16at(0x1f81); w.y = this.s16at(0x1f84); w.z = this.s16at(0x1f87);
          w.x2 = r[0x1f89]; w.y2 = r[0x1f8a]; w.dist = r[0x1f8b]; w.scale = this.w16(0x1f90);
          this.rotate3D(false);
          this.put16(0x1f80, w.x); this.put16(0x1f83, w.y);
          break;
        }
        case 0x40: { let sum = 0; for (let i = 0; i < 0x800; i++) sum += r[i]; this.put16(0x1f80, sum); break; }
        case 0x54: { // square of a 24-bit number
          const a = BigInt(s8(r[0x1f82]) * 65536 + this.w16(0x1f80));
          const p = BigInt.asUintN(48, a * a);
          this.put24(0x1f83, Number(p & 0xffffffn)); this.put24(0x1f86, Number(p >> 24n));
          break;
        }
        case 0x5c: IMMEDIATE.forEach((v, i) => this.put24(i * 3, v)); break; // constant registers
        case 0x89: r[0x1f80] = 0x36; r[0x1f81] = 0x43; r[0x1f82] = 0x05; break; // ROM checksum value
      }
    }

    // ---- sprites: expand the game's object list into OAM at $6000 ----
    buildOAM() {
      const r = this.ram, first = r[0x626];
      let oam = first << 2;
      for (let i = 0x1fd; i > oam; i -= 4) r[i] = 0xe0; // park unused sprites below the screen
      const gx = this.w16(0x621), gy = this.w16(0x623);
      let hi = 0x200 + (first >> 2), shift = (first & 3) * 2, left = 128 - first;
      const put = (x, y, name, attr, big) => {
        r[oam] = x & 0xff; r[oam + 1] = y & 0xff; r[oam + 2] = name & 0xff; r[oam + 3] = attr & 0xff;
        r[hi] = (r[hi] & ~(3 << shift)) | ((x & 0x100 ? 1 : 0) << shift) | ((big ? 2 : 0) << shift);
        oam += 4; left--;
        shift = (shift + 2) & 6; if (!shift) hi++;
      };
      for (let n = r[0x620], src = 0x220; n > 0 && left > 0; n--, src += 16) {
        const sx = s16(this.w16(src) - gx), sy = s16(this.w16(src + 2) - gy);
        const name = r[src + 5], attr = r[src + 4] | r[src + 6];
        let p = r[src + 7] | (r[src + 8] << 8) | (r[src + 9] << 16);
        const pieces = this.bus(p++);
        if (!pieces) { put(sx, sy, name, attr, true); continue; }
        for (let k = pieces; k > 0 && left > 0; k--, p += 4) {
          const flags = this.bus(p), big = (flags & 0x20) !== 0, size = big ? 16 : 8;
          let x = s8(this.bus(p + 1));
          if (attr & 0x40) x = -x - size;
          x += sx;
          if (x < -16 || x > 272) continue;
          let y = s8(this.bus(p + 2));
          if (attr & 0x80) y = -y - size;
          y += sy;
          if (y < -16 || y > 224) continue;
          put(x, y, name + this.bus(p + 3), attr ^ (flags & 0xc0), big);
        }
      }
    }

    // ---- bitmap scale / rotate: 4bpp packed pixels at $6600 -> tiles at $6000 ----
    scaleRotate(pad) {
      const r = this.ram;
      let xs = this.w16(0x1f8f), ys = this.w16(0x1f92);
      if (xs & 0x8000) xs = 0x7fff;
      if (ys & 0x8000) ys = 0x7fff;
      const ang = this.w16(0x1f80);
      let A, B, C, D;
      if (ang === 0) { A = xs; B = 0; C = 0; D = ys; }
      else if (ang === 128) { A = 0; B = -ys; C = xs; D = 0; }
      else if (ang === 256) { A = -xs; B = 0; C = 0; D = -ys; }
      else if (ang === 384) { A = 0; B = ys; C = -xs; D = 0; }
      else {
        const a = ang & 0x1ff;
        A = s16((COS[a] * xs) >> 15); B = s16(-((SIN[a] * ys) >> 15)); C = s16((SIN[a] * xs) >> 15); D = s16((COS[a] * ys) >> 15);
      }
      A = s16(A); B = s16(B); C = s16(C); D = s16(D);
      const w = r[0x1f89] & ~7, h = r[0x1f8c] & ~7;
      r.fill(0, 0, Math.min(0x2000, ((w + pad / 4) * h) >> 1));
      const cx = this.s16at(0x1f83), cy = this.s16at(0x1f86);
      // 12 fraction bits; start where output (0,0) lands in the source
      let lx = ((cx << 12) - cx * A - cx * B) | 0, ly = ((cy << 12) - cy * C - cy * D) | 0;
      let out = 0, bit = 0x80;
      for (let y = 0; y < h; y++) {
        let X = lx >>> 0, Y = ly >>> 0;
        for (let x = 0; x < w; x++) {
          let px = 0;
          if ((X >>> 12) < w && (Y >>> 12) < h) {
            const i = (Y >>> 12) * w + (X >>> 12);
            px = r[0x600 + (i >> 1)];
            if (i & 1) px >>= 4;
          }
          if (out + 17 < 0x2000) {
            if (px & 1) r[out] |= bit;
            if (px & 2) r[out + 1] |= bit;
            if (px & 4) r[out + 16] |= bit;
            if (px & 8) r[out + 17] |= bit;
          }
          bit >>= 1;
          if (!bit) { bit = 0x80; out += 32; }
          X = (X + A) >>> 0; Y = (Y + C) >>> 0;
        }
        out += 2 + pad;
        if (out & 0x10) out &= ~0x10; else out -= w * 4 + pad;
        lx = (lx + B) | 0; ly = (ly + D) | 0;
      }
    }

    // ---- wireframe maths ----
    // rotate (x, y, z) by the three angles (1/128 turns); `persp` adds the camera
    rotate3D(persp) {
      const w = this.w;
      let x = w.x, y = w.y, z = persp ? w.z - 0x95 : w.z;
      let t = -w.x2 * Math.PI * 2 / 128;
      const y2 = y * Math.cos(t) - z * Math.sin(t), z2 = y * Math.sin(t) + z * Math.cos(t);
      t = -w.y2 * Math.PI * 2 / 128;
      const x2 = x * Math.cos(t) + z2 * Math.sin(t);
      z = x * -Math.sin(t) + z2 * Math.cos(t);
      t = -w.dist * Math.PI * 2 / 128;
      x = x2 * Math.cos(t) - y2 * Math.sin(t);
      y = x2 * Math.sin(t) + y2 * Math.cos(t);
      if (persp) { const k = w.scale / (0x90 * (z + 0x95)) * 0x95; w.x = s16(Math.trunc(x * k)); w.y = s16(Math.trunc(y * k)); }
      else { w.x = s16(Math.trunc(x * w.scale / 0x100)); w.y = s16(Math.trunc(y * w.scale / 0x100)); }
    }
    // step sizes (8.8) and length of the line (x,y) -> (x2,y2)
    lineSteps() {
      const w = this.w;
      let dx = s16(w.x2 - w.x), dy = s16(w.y2 - w.y);
      if (Math.abs(dx) > Math.abs(dy)) {
        w.dist = Math.abs(dx) + 1;
        dy = s16(Math.trunc(256 * dy / Math.abs(dx)));
        dx = dx < 0 ? -256 : 256;
      } else if (dy !== 0) {
        w.dist = Math.abs(dy) + 1;
        dx = s16(Math.trunc(256 * dx / Math.abs(dy)));
        dy = dy < 0 ? -256 : 256;
      } else w.dist = 0;
      w.x = dx; w.y = dy;
    }
    transformLines() {
      const r = this.ram, w = this.w;
      w.x2 = r[0x1f83]; w.y2 = r[0x1f86]; w.dist = r[0x1f89]; w.scale = r[0x1f8c];
      const ax = w.x2, ay = w.y2, az = w.dist, sc = w.scale;
      // transform the points (16 bytes each from $6000)
      for (let i = this.w16(0x1f80), p = 0; i > 0 && p + 10 < 0x2000; i--, p += 0x10) {
        w.x = this.s16at(p + 1); w.y = this.s16at(p + 5); w.z = this.s16at(p + 9);
        w.x2 = ax; w.y2 = ay; w.dist = az; w.scale = sc;
        this.rotate3D(true);
        this.put16(p + 1, w.x + 0x80); this.put16(p + 5, w.y + 0x50);
      }
      this.put16(0x600, 23); this.put16(0x602, 0x60); this.put16(0x605, 0x40);
      this.put16(0x608, 23); this.put16(0x60a, 0x60); this.put16(0x60d, 0x40);
      // line list at $6B00: pairs of point numbers -> step tables at $6600
      for (let i = this.w16(0xb00), p = 0xb02, o = 0; i > 0 && p < 0x1f00 && o < 0x1800; i--, p += 2, o += 8) {
        const a = r[p] << 4, b = r[p + 1] << 4;
        w.x = this.s16at(a + 1); w.y = this.s16at(a + 5); w.x2 = this.s16at(b + 1); w.y2 = this.s16at(b + 5);
        this.lineSteps();
        this.put16(o + 0x600, w.dist || 1); this.put16(o + 0x602, w.x); this.put16(o + 0x605, w.y);
      }
    }
    // draw the wireframe model whose line list is at the ROM address in $7F80
    wireframe() {
      const r = this.ram;
      let line = this.w24(0x1f80);
      const bank = r[0x1f82] << 16;
      const pt = (a) => { const p = bank | a; return [s16((this.bus(p) << 8) | this.bus(p + 1)), s16((this.bus(p + 2) << 8) | this.bus(p + 3)), s16((this.bus(p + 4) << 8) | this.bus(p + 5))]; };
      for (let n = r[0x295]; n > 0; n--, line += 5) {
        let a;
        if (this.bus(line) === 0xff && this.bus(line + 1) === 0xff) {
          // continue from the end of the previous line
          let t = line - 5;
          while (this.bus(t + 2) === 0xff && this.bus(t + 3) === 0xff && t > 0) t -= 5;
          a = (this.bus(t + 2) << 8) | this.bus(t + 3);
        } else a = (this.bus(line) << 8) | this.bus(line + 1);
        const b = (this.bus(line + 2) << 8) | this.bus(line + 3);
        this.drawLine(pt(a), pt(b), this.bus(line + 4));
      }
    }
    drawLine(p1, p2, color) {
      const r = this.ram, w = this.w;
      const place = (p) => {
        w.x = p[0]; w.y = p[1]; w.z = p[2];
        w.scale = r[0x1f90]; w.x2 = r[0x1f86]; w.y2 = r[0x1f87]; w.dist = r[0x1f88];
        this.rotate3D(false);
        return [(w.x + 48) << 8, (w.y + 48) << 8];
      };
      let [x1, y1] = place(p1);
      const [x2, y2] = place(p2);
      w.x = s16(x1 >> 8); w.y = s16(y1 >> 8); w.x2 = s16(x2 >> 8); w.y2 = s16(y2 >> 8);
      this.lineSteps();
      const dx = w.x, dy = w.y;
      // plot into a 12 x 12 tile 2bpp picture at $6300
      for (let i = w.dist || 1; i > 0; i--) {
        if (x1 > 0xff && y1 > 0xff && x1 < 0x6000 && y1 < 0x6000) {
          const X = x1 >> 8, Y = y1 >> 8;
          const a = 0x300 + (Y >> 3) * 0xc0 + (X >> 3) * 16 + (Y & 7) * 2, bit = 0x80 >> (X & 7);
          r[a] &= ~bit; r[a + 1] &= ~bit;
          if (color & 1) r[a] |= bit;
          if (color & 2) r[a + 1] |= bit;
        }
        x1 += dx; y1 += dy;
      }
    }

    // ---- left/right edges of a trapezoid for 225 lines -> $6800 / $6900 ----
    trapezoid() {
      const r = this.ram;
      const a1 = this.w16(0x1f8c) & 0x1ff, a2 = this.w16(0x1f8f) & 0x1ff;
      const tan = (a) => (COS[a] ? Math.trunc((SIN[a] * 65536) / COS[a]) | 0 : -0x80000000);
      const t1 = tan(a1), t2 = tan(a2);
      let y = s16(this.w16(0x1f83) - this.w16(0x1f89));
      const x0 = this.w16(0x1f80), x1 = this.w16(0x1f86), wid = this.w16(0x1f93);
      for (let j = 0; j < 225; j++) {
        let left, right;
        if (y >= 0) {
          left = s16(Math.floor(t1 * y / 65536) - x0 + x1);
          right = s16(Math.floor(t2 * y / 65536) - x0 + x1 + wid);
          if (left < 0 && right < 0) { left = 1; right = 0; } else if (left < 0) left = 0; else if (right < 0) right = 0;
          if (left > 255 && right > 255) { left = 255; right = 254; } else if (left > 255) left = 255; else if (right > 255) right = 255;
        } else { left = 1; right = 0; }
        r[0x800 + j] = left & 0xff; r[0x900 + j] = right & 0xff;
        y = s16(y + 1);
      }
    }

    // ---- wave effect over a 256 x 40 2bpp picture at $6000 ----
    wave() {
      const r = this.ram;
      let ptr = r[0x1f83], m1 = 0xc0c0, m2 = 0x3f3f;
      for (let tile = 0; tile < 32; tile++) {
        const dst = tile * 16;
        do {
          let h = -s8(r[0xb00 + ptr]) - 16;
          for (let i = 0; i < 40; i++) {
            const a = dst + COLUMN[i];
            let v = this.w16(a) & m2;
            if (h >= 0) v |= m1 & (h < 8 ? this.w16(0xa00 + h * 2) : 0xff00);
            this.put16(a, v);
            h++;
          }
          ptr = (ptr + 1) & 0x7f;
          m1 = ((m1 >> 2) | (m1 << 6)) & 0xffff;
          m2 = ((m2 >> 2) | (m2 << 6)) & 0xffff;
        } while (m1 !== 0xc0c0);
      }
    }

    // ---- break a 4bpp bitmap at $6600 apart (scaled about a centre) ----
    disintegrate() {
      const r = this.ram;
      const w = r[0x1f89], h = r[0x1f8c];
      const cx = this.s16at(0x1f80), cy = this.s16at(0x1f83), kx = this.s16at(0x1f86), ky = this.s16at(0x1f8f);
      const sx = -cx * kx + (cx << 8), sy = -cy * ky + (cy << 8);
      const src0 = 0x600;
      let src = src0;
      r.fill(0, 0, Math.min(0x2000, (w * h) >> 1));
      for (let i = 0, y = sy; i < h; i++, y += ky) {
        for (let j = 0, x = sx; j < w; j++, x += kx) {
          if ((x >> 8) >= 0 && (x >> 8) < w && (y >> 8) >= 0 && (y >> 8) < h && (y >> 8) * w + (x >> 8) < 0x2000 && src < 0x2000) {
            const px = j & 1 ? r[src] >> 4 : r[src];
            const idx = (y >> 11) * w * 4 + (x >> 11) * 32 + ((y >> 8) & 7) * 2, bit = 0x80 >> ((x >> 8) & 7);
            if (idx + 17 < 0x2000) {
              if (px & 1) r[idx] |= bit;
              if (px & 2) r[idx + 1] |= bit;
              if (px & 4) r[idx + 16] |= bit;
              if (px & 8) r[idx + 17] |= bit;
            }
          }
          if (j & 1) src++;
        }
      }
    }

    saveState() { return { ram: this.ram.slice(), w: Object.assign({}, this.w) }; }
    loadState(o) { this.ram.set(o.ram); Object.assign(this.w, o.w); }
  }

  global.RPSNESChips.push({ name: "Cx4", detect: (c) => c.chip === 0xf3, create: (snes) => new Cx4(snes) });
  global.RPSNESCx4 = Cx4;
})(typeof window !== "undefined" ? window : globalThis);
