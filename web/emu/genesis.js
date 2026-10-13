// RetroPixels — Sega Genesis / Mega Drive emulator. Original code.
//
// Machine: Motorola 68000 (RPM68K), Zilog Z80 sound CPU (RPZ80), Yamaha YM2612
// FM (RPYM2612), SN76489-compatible PSG, and the Sega VDP (planes A/B, window,
// sprites, shadow/highlight, DMA, interlace), written from the documented
// hardware behaviour. Cartridge battery saves, the >2 MB SRAM switch and the
// Super Street Fighter II bank mapper are supported; pads are 6-button.
"use strict";

(function (global) {
  const LINE = 3420;          // master clocks per scan line
  const ACTIVE = 2560;        // where the visible part of a line ends (render + H-interrupt point)
  const SLICE = 1140;         // CPUs are interleaved in slices this long
  const YM_CLKS = 7 * 144;    // master clocks per YM2612 sample
  const PSG_CLKS = 15 * 16;   // master clocks per PSG step

  // Measured output levels of the VDP's colour DAC (normal / shadow / highlight).
  const LV_N = [0, 52, 87, 116, 144, 172, 206, 255];
  const LV_S = [0, 29, 52, 70, 87, 101, 116, 130];
  const LV_H = [130, 144, 158, 172, 187, 206, 228, 255];

  // ===================================================================
  // PSG (SN76489 compatible)
  // ===================================================================
  class PSG {
    constructor() {
      this.vol = new Float32Array(16);
      for (let i = 0; i < 15; i++) this.vol[i] = Math.pow(10, -i * 2 / 20);
      this.reg = new Uint16Array(8); this.cnt = new Int32Array(4); this.pol = new Int8Array(4);
      this.reset();
    }
    reset() {
      this.reg.fill(0); for (let c = 0; c < 4; c++) this.reg[c * 2 + 1] = 15;
      this.cnt.fill(0); this.pol.fill(1); this.lfsr = 0x8000; this.latch = 0;
    }
    write(v) {
      if (v & 0x80) {
        this.latch = (v >> 4) & 7;
        const r = this.latch;
        if (r & 1) this.reg[r] = v & 15;
        else if (r === 6) { this.reg[6] = v & 7; this.lfsr = 0x8000; }
        else this.reg[r] = (this.reg[r] & 0x3f0) | (v & 15);
      } else {
        const r = this.latch;
        if (r & 1) this.reg[r] = v & 15;
        else if (r === 6) { this.reg[6] = v & 7; this.lfsr = 0x8000; }
        else this.reg[r] = (this.reg[r] & 15) | ((v & 0x3f) << 4);
      }
    }
    // Advance one step; returns the mixed output.
    step() {
      let out = 0;
      for (let c = 0; c < 3; c++) {
        const per = this.reg[c * 2];
        if (--this.cnt[c] <= 0) { this.cnt[c] = per; this.pol[c] = -this.pol[c]; }
        out += (per <= 1 ? 1 : this.pol[c]) * this.vol[this.reg[c * 2 + 1]];
      }
      const nr = this.reg[6] & 3, nper = nr === 3 ? this.reg[4] : 0x10 << nr;
      if (--this.cnt[3] <= 0) {
        this.cnt[3] = nper;
        this.pol[3] = -this.pol[3];
        if (this.pol[3] > 0) {
          const fb = (this.reg[6] & 4) ? ((this.lfsr ^ (this.lfsr >> 3)) & 1) : (this.lfsr & 1);
          this.lfsr = (this.lfsr >> 1) | (fb << 15);
        }
      }
      out += ((this.lfsr & 1) ? 1 : -1) * this.vol[this.reg[7]] * 0.5;
      return out;
    }
    saveState() { return { reg: this.reg.slice(), cnt: this.cnt.slice(), pol: this.pol.slice(), lfsr: this.lfsr, latch: this.latch }; }
    loadState(o) { this.reg.set(o.reg); this.cnt.set(o.cnt); this.pol.set(o.pol); this.lfsr = o.lfsr; this.latch = o.latch; }
  }

  // ===================================================================
  // VDP
  // ===================================================================
  class VDP {
    constructor(md) {
      this.md = md;
      this.vram = new Uint8Array(0x10000);
      this.cram = new Uint16Array(64);
      this.vsram = new Uint16Array(40);
      this.reg = new Uint8Array(24);
      this.sat = new Uint8Array(640); // the VDP's internal copy of sprite Y / size / link
      this.pal = new Uint32Array(64 * 3);
      this.bufA = new Uint8Array(336); this.bufB = new Uint8Array(336); this.bufS = new Uint8Array(336);
      this.sprList = new Int32Array(20);
      this.reset();
    }
    reset() {
      this.vram.fill(0); this.cram.fill(0); this.vsram.fill(0); this.reg.fill(0); this.sat.fill(0);
      this.addr = 0; this.code = 0; this.pending = false; this.fillPending = false;
      this.vintPending = false; this.hintPending = false; this.hcount = 0;
      this.sovr = false; this.scol = false; this.odd = 0; this.prevOverflow = false;
      this.readBuf = 0;
      for (let i = 0; i < 64; i++) this.updatePal(i);
    }
    get h40() { return (this.reg[12] & 0x81) !== 0; }
    get v30() { return (this.reg[1] & 8) !== 0; }
    get im2() { return (this.reg[12] & 6) === 6; }
    get display() { return (this.reg[1] & 0x40) !== 0; }
    get width() { return this.h40 ? 320 : 256; }
    get lines() { return this.v30 ? 240 : 224; }

    updatePal(i) {
      const w = this.cram[i], r = (w >> 1) & 7, g = (w >> 5) & 7, b = (w >> 9) & 7;
      const mk = (L) => 0xff000000 | (L[b] << 16) | (L[g] << 8) | L[r];
      this.pal[i] = mk(LV_S); this.pal[64 + i] = mk(LV_N); this.pal[128 + i] = mk(LV_H);
    }

    // ---- ports ----
    status() {
      const md = this.md;
      const pos = md.now68k() - md.lineStart;
      let s = 0x3600 | (this.vintPending ? 0x80 : 0) | (this.sovr ? 0x40 : 0) | (this.scol ? 0x20 : 0) |
        (this.odd && (this.reg[12] & 2) ? 0x10 : 0) | (md.vblank || !this.display ? 8 : 0) |
        (pos >= ACTIVE || pos < 40 ? 4 : 0) | (md.pal ? 1 : 0);
      this.pending = false; this.sovr = false; this.scol = false;
      return s;
    }
    hv() {
      const md = this.md;
      if ((this.reg[0] & 2) && md.hvLatch >= 0) return md.hvLatch;
      const pos = Math.max(0, Math.min(LINE - 1, md.now68k() - md.lineStart));
      let hc;
      if (this.h40) { const i = Math.floor(pos * 210 / LINE); hc = i < 183 ? i : i - 183 + 0xe4; }
      else { const i = Math.floor(pos * 171 / LINE); hc = i < 148 ? i : i - 148 + 0xe9; }
      let vc = md.line;
      if (md.pal) { if (vc > (this.v30 ? 0x10a : 0x102)) vc -= 57; }
      else if (!this.v30 && vc > 0xea) vc -= 6;
      vc &= 0xff;
      if (this.im2) vc = ((vc << 1) | (vc >> 7)) & 0xff;
      return (vc << 8) | (hc & 0xff);
    }
    writeControl(v) {
      if (this.pending) {
        this.pending = false;
        this.addr = (this.addr & 0x3fff) | ((v & 3) << 14);
        this.code = (this.code & 3) | ((v >> 2) & 0x3c);
        if ((this.code & 0x20) && (this.reg[1] & 0x10)) this.startDMA();
        return;
      }
      if ((v & 0xc000) === 0x8000) {
        const r = (v >> 8) & 0x1f;
        if (r < 24) this.writeReg(r, v & 0xff);
        this.code &= 0x3c; // a register write also clears the access code
        return;
      }
      this.pending = true;
      this.addr = (this.addr & 0xc000) | (v & 0x3fff);
      this.code = (this.code & 0x3c) | (v >> 14);
    }
    writeReg(r, v) {
      if (!(this.reg[1] & 4) && r > 10) return; // mode 4 (Master System) only has 11 registers
      const old = this.reg[r];
      this.reg[r] = v;
      if (r === 0 || r === 1) this.md.updateIRQ();
      if (r === 0 && (v & 2) && !(old & 2)) this.md.hvLatch = this.hv();
      if (r === 5 && false) this.reloadSat();
    }
    reloadSat() { const base = this.satBase(); for (let i = 0; i < 640; i++) this.sat[i] = this.vram[(base + i) & 0xffff]; }
    satBase() { return this.h40 ? (this.reg[5] & 0x7e) << 9 : (this.reg[5] & 0x7f) << 9; }

    vramWriteByte(a, b) {
      a &= 0xffff;
      this.vram[a] = b;
      const off = a - this.satBase();
      if (off >= 0 && off < 640 && (off & 7) < 4) this.sat[off] = b;
    }
    writeWord(v) {
      const a = this.addr;
      switch (this.code & 0xf) {
        case 1: {
          if (a & 1) v = ((v & 0xff) << 8) | (v >> 8);
          this.vramWriteByte(a & 0xfffe, v >> 8);
          this.vramWriteByte(a | 1, v & 0xff);
          break;
        }
        case 3: { const i = (a >> 1) & 0x3f; this.cram[i] = v & 0xeee; this.updatePal(i); break; }
        case 5: { const i = (a >> 1) & 0x3f; if (i < 40) this.vsram[i] = v & 0x7ff; break; }
      }
      this.addr = (this.addr + this.reg[15]) & 0xffff;
    }
    writeData(v) {
      this.pending = false;
      this.writeWord(v);
      if (this.fillPending) { this.fillPending = false; this.dmaFill(v); }
    }
    readData() {
      this.pending = false;
      const a = this.addr;
      let v;
      switch (this.code & 0xf) {
        case 0: v = (this.vram[a & 0xfffe] << 8) | this.vram[a | 1]; break;
        case 4: { const i = (a >> 1) & 0x3f; v = i < 40 ? this.vsram[i] : this.vsram[0]; v = (v & 0x7ff) | (this.readBuf & 0xf800); break; }
        case 8: v = (this.cram[(a >> 1) & 0x3f] & 0xeee) | (this.readBuf & 0xf111); break;
        case 0xc: v = (this.readBuf & 0xff00) | this.vram[a ^ 1]; break;
        default: v = this.readBuf;
      }
      this.readBuf = v;
      this.addr = (this.addr + this.reg[15]) & 0xffff;
      return v;
    }

    // ---- DMA ----
    dmaLength() { return (this.reg[19] | (this.reg[20] << 8)) || 0x10000; }
    startDMA() {
      const mode = this.reg[23] >> 6;
      if (mode === 2) { this.fillPending = true; return; }
      const len = this.dmaLength();
      if (mode === 3) {
        let src = this.reg[21] | (this.reg[22] << 8);
        for (let i = 0; i < len; i++) {
          this.vramWriteByte(this.addr, this.vram[src]);
          src = (src + 1) & 0xffff;
          this.addr = (this.addr + this.reg[15]) & 0xffff;
        }
        this.reg[21] = src & 0xff; this.reg[22] = src >> 8;
        this.md.dmaStall(len, true, false);
      } else {
        let src = ((this.reg[21] | (this.reg[22] << 8) | ((this.reg[23] & 0x7f) << 16)) << 1) >>> 0;
        const md = this.md;
        for (let i = 0; i < len; i++) {
          this.writeWord(md.dmaRead(src));
          src = (src & 0xfe0000) | ((src + 2) & 0x1ffff);
        }
        const s = src >>> 1;
        this.reg[21] = s & 0xff; this.reg[22] = (s >> 8) & 0xff; this.reg[23] = (this.reg[23] & 0x80) | ((s >> 16) & 0x7f);
        md.dmaStall(len, false, (this.code & 0xf) === 1);
      }
      this.reg[19] = 0; this.reg[20] = 0;
      this.code &= ~0x20;
    }
    dmaFill(v) {
      const len = this.dmaLength(), t = this.code & 0xf;
      for (let i = 0; i < len; i++) {
        if (t === 1) this.vramWriteByte(this.addr ^ 1, v >> 8);
        else if (t === 3) { const k = (this.addr >> 1) & 0x3f; this.cram[k] = v & 0xeee; this.updatePal(k); }
        else if (t === 5) { const k = (this.addr >> 1) & 0x3f; if (k < 40) this.vsram[k] = v & 0x7ff; }
        this.addr = (this.addr + this.reg[15]) & 0xffff;
      }
      this.reg[19] = 0; this.reg[20] = 0;
      this.code &= ~0x20;
      this.md.dmaStall(len, true, false);
    }

    // ---- rendering ----
    // Draw one background plane line into buf (value = prio<<7 | pal<<4 | index).
    drawPlane(buf, nt, hs, vsOff, y, W, isB) {
      const vram = this.vram, im2 = this.im2;
      const ws = this.reg[16] & 3, hsz = (this.reg[16] >> 4) & 3;
      const pw = ws === 3 ? 128 : ws === 1 ? 64 : 32;
      let ph = hsz === 3 ? 128 : hsz === 1 ? 64 : 32;
      if (pw === 128) ph = 32; else if (pw === 64 && ph === 128) ph = 64;
      const pxMask = pw * 8 - 1, cellH = im2 ? 16 : 8, pyMask = ph * cellH - 1;
      const colMode = this.reg[11] & 4;
      const fine = hs & 15;
      let lastCell = -1, lastCol = -2, py = 0, rowBytes = 0, attr = 0, flip = 0, pal = 0;
      for (let x = 0; x < W; x++) {
        if (colMode) {
          let col = ((x - fine + 16) >> 4) - 1; if (col < 0) col = 0;
          if (col !== lastCol) {
            lastCol = col;
            const vs = this.vsram[(col * 2 + vsOff) % 40] & (im2 ? 0x7ff : 0x3ff);
            py = (y + vs) & pyMask; lastCell = -1;
          }
        } else if (lastCol === -2) {
          lastCol = 0;
          py = (y + (this.vsram[vsOff] & (im2 ? 0x7ff : 0x3ff))) & pyMask;
        }
        const px = (x - hs) & pxMask, cell = px >> 3;
        if (cell !== lastCell) {
          lastCell = cell;
          const ea = (nt + (((py / cellH) | 0) * pw + cell) * 2) & 0xffff;
          attr = (vram[ea] << 8) | vram[ea + 1];
          let row = py & (cellH - 1);
          if (attr & 0x1000) row = cellH - 1 - row;
          const ta = im2 ? ((attr & 0x3ff) << 6) + row * 4 : ((attr & 0x7ff) << 5) + row * 4;
          rowBytes = (vram[ta & 0xffff] << 24) | (vram[(ta + 1) & 0xffff] << 16) | (vram[(ta + 2) & 0xffff] << 8) | vram[(ta + 3) & 0xffff];
          flip = attr & 0x800 ? 7 : 0;
          pal = ((attr >> 13) & 3) << 4 | ((attr >> 8) & 0x80);
        }
        const n = (px & 7) ^ flip;
        buf[x] = pal | ((rowBytes >>> (28 - n * 4)) & 15);
      }
    }
    drawWindow(buf, y, x0, x1) {
      const vram = this.vram, im2 = this.im2, cellH = im2 ? 16 : 8;
      const ww = this.h40 ? 64 : 32;
      const nt = this.h40 ? (this.reg[3] & 0x3c) << 10 : (this.reg[3] & 0x3e) << 10;
      const ry = (y / cellH) | 0;
      for (let x = x0; x < x1; x++) {
        const ea = (nt + (ry * ww + (x >> 3)) * 2) & 0xffff;
        const attr = (vram[ea] << 8) | vram[ea + 1];
        let row = y & (cellH - 1);
        if (attr & 0x1000) row = cellH - 1 - row;
        const col = (x & 7) ^ (attr & 0x800 ? 7 : 0);
        const ta = (im2 ? ((attr & 0x3ff) << 6) : ((attr & 0x7ff) << 5)) + row * 4 + (col >> 1);
        const b = vram[ta & 0xffff];
        buf[x] = (((attr >> 13) & 3) << 4) | ((attr >> 8) & 0x80) | ((col & 1) ? b & 15 : b >> 4);
      }
    }
    drawSprites(buf, y, W) {
      buf.fill(0, 0, W);
      const vram = this.vram, sat = this.sat, im2 = this.im2;
      const max = this.h40 ? 80 : 64, perLine = this.h40 ? 20 : 16, dotMax = W;
      const base = this.satBase(), cellH = im2 ? 16 : 8, yoff = im2 ? 256 : 128, ymask = im2 ? 0x3ff : 0x1ff;
      // find sprites on this line
      let n = 0, idx = 0, overflow = false;
      for (let k = 0; k < max; k++) {
        const o = idx * 8;
        const sy = (((sat[o] << 8) | sat[o + 1]) & ymask) - yoff;
        const size = sat[o + 2], h = ((size & 3) + 1) * cellH;
        if (y >= sy && y < sy + h) {
          if (n === perLine) { overflow = true; break; }
          this.sprList[n++] = idx;
        }
        idx = sat[o + 3] & 0x7f;
        if (!idx || idx >= max) break;
      }
      if (overflow) this.sovr = true;
      let dots = 0, masked = false, any = false;
      for (let k = 0; k < n && !masked; k++) {
        const i = this.sprList[k], o = i * 8, va = (base + o) & 0xffff;
        const sy = (((sat[o] << 8) | sat[o + 1]) & ymask) - yoff;
        const size = sat[o + 2], hc = (size & 3) + 1, wc = ((size >> 2) & 3) + 1;
        const attr = (vram[(va + 4) & 0xffff] << 8) | vram[(va + 5) & 0xffff];
        const sx = (((vram[(va + 6) & 0xffff] << 8) | vram[(va + 7) & 0xffff]) & 0x1ff) - 128;
        if (sx === -128 && (any || this.prevOverflow)) { masked = true; break; }
        any = true;
        let row = y - sy;
        if (attr & 0x1000) row = hc * cellH - 1 - row;
        const pal = (((attr >> 13) & 3) << 4) | ((attr >> 8) & 0x80);
        const hflip = attr & 0x800;
        for (let c = 0; c < wc; c++) {
          if (dots >= dotMax) { masked = true; break; }
          dots += 8;
          const cc = hflip ? wc - 1 - c : c;
          const tile = (attr & 0x7ff) + cc * hc + ((row / cellH) | 0);
          const ta = (im2 ? (tile & 0x3ff) << 6 : (tile & 0x7ff) << 5) + (row & (cellH - 1)) * 4;
          for (let p = 0; p < 8; p++) {
            const x = sx + c * 8 + p;
            if (x < 0 || x >= W) continue;
            const col = hflip ? 7 - p : p;
            const b = vram[(ta + (col >> 1)) & 0xffff];
            const v = (col & 1) ? b & 15 : b >> 4;
            if (!v) continue;
            if (buf[x] & 15) { this.scol = true; continue; }
            buf[x] = pal | v;
          }
        }
      }
      this.prevOverflow = overflow || dots >= dotMax;
    }

    renderLine(line, out) {
      const W = this.width, im2 = this.im2;
      const oy = im2 ? line * 2 + this.odd : line;
      const o = oy * W;
      const bg = this.reg[7] & 0x3f;
      if (!this.display) { const c = this.pal[64 + bg]; out.fill(c, o, o + W); this.prevOverflow = false; return; }
      const y = im2 ? line * 2 + this.odd : line;
      const vram = this.vram;
      // horizontal scroll
      const hb = (this.reg[13] & 0x3f) << 10;
      let ho;
      switch (this.reg[11] & 3) { case 0: ho = 0; break; case 1: ho = (line & 7) * 4; break; case 2: ho = (line & ~7) * 4; break; default: ho = line * 4; }
      const ha = (hb + ho) & 0xffff;
      const hsA = ((vram[ha] << 8) | vram[ha + 1]) & 0x3ff, hsB = ((vram[ha + 2] << 8) | vram[ha + 3]) & 0x3ff;
      const A = this.bufA, B = this.bufB, S = this.bufS;
      this.drawPlane(B, (this.reg[4] & 7) << 13, hsB, 1, y, W, true);
      // window region
      const wv = (this.reg[18] & 0x1f) * 8, wdown = this.reg[18] & 0x80;
      const wh = (this.reg[17] & 0x1f) * 16, wright = this.reg[17] & 0x80;
      let wx0 = 0, wx1 = 0;
      if (wdown ? line >= wv : line < wv) { wx0 = 0; wx1 = W; }
      else if (wright) { wx0 = Math.min(wh, W); wx1 = W; }
      else { wx0 = 0; wx1 = Math.min(wh, W); }
      if (wx0 > 0 || wx1 < W || wx1 === wx0) this.drawPlane(A, (this.reg[2] & 0x38) << 10, hsA, 0, y, W, false);
      if (wx1 > wx0) this.drawWindow(A, y, wx0, wx1);
      this.drawSprites(S, y, W);
      // compose
      const pal = this.pal, sh = this.reg[12] & 8;
      for (let x = 0; x < W; x++) {
        const a = A[x], b = B[x], s = S[x];
        let c, lv = 64;
        // layer order: S hi, A hi, B hi, S lo, A lo, B lo, backdrop
        let top, under;
        const so = s & 15, ao = a & 15, bo = b & 15;
        if (so && (s & 0x80)) top = 1;
        else if (ao && (a & 0x80)) top = 2;
        else if (bo && (b & 0x80)) top = 3;
        else if (so) top = 1;
        else if (ao) top = 2;
        else if (bo) top = 3;
        else top = 0;
        if (!sh) {
          c = top === 1 ? s & 0x3f : top === 2 ? a & 0x3f : top === 3 ? b & 0x3f : bg;
        } else {
          const planeHi = (a & 0x80) || (b & 0x80);
          lv = planeHi ? 64 : 0;
          if (top === 1) {
            const sc = s & 0x3f;
            if (sc === 0x3e || sc === 0x3f) {
              // shadow / highlight operator: show what's below the sprite
              if (ao && (a & 0x80)) under = a; else if (bo && (b & 0x80)) under = b; else if (ao) under = a; else if (bo) under = b; else under = -1;
              c = under < 0 ? bg : under & 0x3f;
              if (sc === 0x3e) lv = lv === 0 ? 64 : 128; else lv = 0;
            } else {
              c = sc;
              if ((s & 0x80) || (sc & 15) === 14) lv = 64;
            }
          } else c = top === 2 ? a & 0x3f : top === 3 ? b & 0x3f : bg;
        }
        out[o + x] = pal[lv + c];
      }
    }

    saveState() {
      return { vram: this.vram.slice(), cram: this.cram.slice(), vsram: this.vsram.slice(), reg: this.reg.slice(), sat: this.sat.slice(),
        addr: this.addr, code: this.code, pending: this.pending, fillPending: this.fillPending, vintPending: this.vintPending,
        hintPending: this.hintPending, hcount: this.hcount, sovr: this.sovr, scol: this.scol, odd: this.odd, prevOverflow: this.prevOverflow,
        readBuf: this.readBuf };
    }
    loadState(o) {
      this.vram.set(o.vram); this.cram.set(o.cram); this.vsram.set(o.vsram); this.reg.set(o.reg); this.sat.set(o.sat);
      for (const k of ["addr", "code", "pending", "fillPending", "vintPending", "hintPending", "hcount", "sovr", "scol", "odd", "prevOverflow", "readBuf"]) this[k] = o[k];
      for (let i = 0; i < 64; i++) this.updatePal(i);
    }
  }


  // ===================================================================
  // Serial (I2C) EEPROM used by a few cartridges for saves
  // ===================================================================
  class EEPROM {
    constructor(size, x24c01, page) {
      this.mem = new Uint8Array(size).fill(0xff);
      this.mask = size - 1; this.x24c01 = x24c01; this.pageMask = page - 1;
      this.scl = 1; this.sda = 1; this.out = 1; this.phase = 0; this.cnt = 0; this.shift = 0; this.addr = 0; this.hi = 0;
    }
    // phases: 0 idle, 1 address (24C01), 2 device, 3 word address, 4 write data, 5 read
    set(scl, sda) {
      if (scl && this.scl) {
        if (this.sda && !sda) { this.phase = this.x24c01 ? 1 : 2; this.cnt = 0; this.shift = 0; this.out = 1; }
        else if (!this.sda && sda) { this.phase = 0; this.out = 1; }
      } else if (!this.scl && scl) this.rise(sda);
      else if (this.scl && !scl) this.fall();
      this.scl = scl; this.sda = sda;
    }
    rise(sda) {
      const ph = this.phase;
      if (!ph) return;
      if (ph === 5) {
        if (this.cnt < 8) this.cnt++;
        else if (sda) this.phase = 0;
        else { this.addr = (this.addr + 1) & this.mask; this.cnt = 0; }
        return;
      }
      if (this.cnt < 8) { this.shift = ((this.shift << 1) | sda) & 0xff; this.cnt++; return; }
      this.cnt = 0;
      const b = this.shift; this.shift = 0;
      switch (ph) {
        case 1: this.addr = (b >> 1) & this.mask; this.phase = (b & 1) ? 5 : 4; break;
        case 2:
          if ((b & 0xf0) !== 0xa0) { this.phase = 0; break; }
          this.hi = ((b >> 1) & 7) << 8;
          if (b & 1) { this.addr = (this.hi | (this.addr & 0xff)) & this.mask; this.phase = 5; } else this.phase = 3;
          break;
        case 3: this.addr = (this.hi | b) & this.mask; this.phase = 4; break;
        case 4:
          this.mem[this.addr] = b; this.dirty = true;
          this.addr = (this.addr & ~this.pageMask) | ((this.addr + 1) & this.pageMask);
          break;
      }
    }
    fall() {
      const ph = this.phase;
      if (ph === 5) this.out = this.cnt < 8 ? (this.mem[this.addr] >> (7 - this.cnt)) & 1 : 1;
      else if (ph) this.out = this.cnt === 8 ? 0 : 1;
      else this.out = 1;
    }
    saveState() { return { mem: this.mem.slice(), r: [this.scl, this.sda, this.out, this.phase, this.cnt, this.shift, this.addr, this.hi] }; }
    loadState(o) { this.mem.set(o.mem); [this.scl, this.sda, this.out, this.phase, this.cnt, this.shift, this.addr, this.hi] = o.r; }
  }
  // Which cartridges use one, and how it's wired:
  //  sda / scl = [address, bit] the game writes; out = [address, bit] it reads back.
  const SEGA_WIRING = { sda: [0x200001, 0], scl: [0x200001, 1], out: [0x200001, 0] };
  const ACCLAIM32 = { sda: [0x200001, 0], scl: [0x200000, 0], out: [0x200001, 0] };
  const ACCLAIM16 = { sda: [0x200001, 0], scl: [0x200001, 1], out: [0x200001, 1] };
  const EEPROM_GAMES = {
    "T-81406": [ACCLAIM32, 512, false, 16],   // NBA Jam Tournament Edition
    "T-081326": [ACCLAIM16, 256, false, 4],   // NBA Jam
    "T-81033": [ACCLAIM16, 256, false, 4],    // NBA Jam (Japan)
    "T-081276": [ACCLAIM32, 256, false, 4],   // NFL Quarterback Club
    "T-81476": [ACCLAIM32, 2048, false, 16],  // Frank Thomas Big Hurt Baseball
    "T-81576": [ACCLAIM32, 2048, false, 16],  // College Slam
    "T-081586": [ACCLAIM32, 2048, false, 16], // NFL Quarterback Club 96
  };
  function eepromFor(rom) {
    const serial = String.fromCharCode(...rom.subarray(0x183, 0x18b)).trim().split(" ")[0];
    if (EEPROM_GAMES[serial]) return EEPROM_GAMES[serial];
    if (rom[0x1b0] === 0x52 && rom[0x1b1] === 0x41 && rom[0x1b2] === 0xe8 && rom[0x1b3] === 0x40) return [SEGA_WIRING, 128, true, 4];
    return null;
  }

  // ===================================================================
  // Cartridge helpers
  // ===================================================================
  function prepareRom(u8) {
    // .smd: 512-byte header + 16 KB blocks with odd / even bytes split.
    const hasSega = (r, o) => r.length > o + 0x104 && String.fromCharCode(r[o + 0x100], r[o + 0x101], r[o + 0x102], r[o + 0x103]) === "SEGA";
    if (!hasSega(u8, 0) && (u8.length % 16384) === 512 && u8[8] === 0xaa && u8[9] === 0xbb) {
      const body = u8.subarray(512), out = new Uint8Array(body.length);
      for (let b = 0; b < body.length; b += 16384) for (let i = 0; i < 8192; i++) {
        out[b + i * 2 + 1] = body[b + i]; out[b + i * 2] = body[b + 8192 + i];
      }
      u8 = out;
    }
    let size = 1; while (size < u8.length) size <<= 1;
    const rom = new Uint8Array(Math.max(size, 0x10000));
    rom.set(u8);
    if (u8.length < size) for (let i = u8.length; i < size; i++) rom[i] = rom[i - (size >> 1)]; // mirror an odd-sized tail
    return { rom, realSize: u8.length };
  }
  function headerRegion(rom) {
    const s = String.fromCharCode(rom[0x1f0], rom[0x1f1], rom[0x1f2]).toUpperCase();
    if (/^[0-9A-F]\s*$/.test(s.trim()) && !/[JUE]/.test(s)) {
      const n = parseInt(s.trim(), 16);
      if (n & 4) return "us"; if (n & 1) return "jp"; if (n & 8) return "eu";
      return "us";
    }
    if (s.includes("U")) return "us";
    if (s.includes("J")) return "jp";
    if (s.includes("E")) return "eu";
    return "us";
  }

  // ===================================================================
  // The machine
  // ===================================================================
  const BUTTONS = ["A", "B", "C", "Start", "X", "Y", "Z", "Mode", "Up", "Down", "Left", "Right"];
  const BIT = {}; BUTTONS.forEach((b, i) => { BIT[b] = 1 << i; });

  class Genesis {
    constructor(romIn, opts) {
      opts = opts || {};
      const { rom, realSize } = prepareRom(romIn);
      this.rom = rom; this.romSize = realSize;
      this.region = headerRegion(rom);
      this.pal = this.region === "eu";
      this.overseas = this.region !== "jp";
      this.sampleRate = opts.sampleRate || 48000;
      this.mclkRate = this.pal ? 53203424 : 53693175;
      this.linesPerFrame = this.pal ? 313 : 262;
      this.fps = this.mclkRate / (LINE * this.linesPerFrame);
      this.nativeRate = this.mclkRate / YM_CLKS;

      this.ram = new Uint8Array(0x10000);
      this.zram = new Uint8Array(0x2000);
      // cartridge save memory (0x200000-0x20FFFF)
      this.sram = new Uint8Array(0x10000).fill(0xff);
      const hasRA = rom[0x1b0] === 0x52 && rom[0x1b1] === 0x41;
      this.sramStart = 0x200000; this.sramEnd = 0x20ffff;
      if (hasRA) {
        const st = ((rom[0x1b4] << 24) | (rom[0x1b5] << 16) | (rom[0x1b6] << 8) | rom[0x1b7]) >>> 0;
        const en = ((rom[0x1b8] << 24) | (rom[0x1b9] << 16) | (rom[0x1ba] << 8) | rom[0x1bb]) >>> 0;
        if (st >= 0x200000 && st < 0x400000 && en >= st) { this.sramStart = st & ~1; this.sramEnd = Math.min(en | 1, this.sramStart + 0xffff); }
      }
      this.hasSaveHeader = hasRA;
      this.sramMapped = realSize <= 0x200000; // bigger games switch it in through 0xA130F1
      this.sramUsed = hasRA;
      if (opts.sram && opts.sram.length) { this.sram.set(opts.sram.subarray(0, 0x10000)); this.sramUsed = true; }
      this.ssf2 = realSize > 0x400000;
      const ee = eepromFor(rom);
      if (ee) {
        this.eeWire = ee[0]; this.eeprom = new EEPROM(ee[1], ee[2], ee[3]);
        this.sramMapped = false; this.sramUsed = true;
        if (opts.sram && opts.sram.length) this.eeprom.mem.set(opts.sram.subarray(0, ee[1]));
      }
      this.banks = new Uint8Array(8); for (let i = 0; i < 8; i++) this.banks[i] = i;

      this.vdp = new VDP(this);
      this.ym = new global.RPYM2612();
      this.psg = new PSG();
      const md = this;
      this.cpu = new global.RPM68K({
        read8: (a) => md.read8(a), read16: (a) => md.read16(a),
        write8: (a, v) => md.write8(a, v), write16: (a, v) => md.write16(a, v),
        ack: (lv) => md.ack(lv),
      });
      this.cpu.tasWrites = false;
      this.z80 = new global.RPZ80({
        read: (a) => md.zread(a), write: (a, v) => md.zwrite(a, v),
        in: () => 0xff, out: () => {}, vector: () => 0xff,
      });

      this.width = 320; this.height = 224;
      this.screen = new Uint32Array(320 * 480);
      this.input = [0, 0];
      this.audioL = new Float32Array(2048); this.audioR = new Float32Array(2048); this.audioN = 0;
      this.outBuf = new Float32Array(8192);
      this.reset(true);
    }

    reset(hard) {
      if (hard) { this.ram.fill(0); this.zram.fill(0); }
      this.vdp.reset(); this.ym.reset(); this.psg.reset(); this.z80.reset();
      this.ioData = new Uint8Array([0x7f, 0x7f, 0x7f]); this.ioCtrl = new Uint8Array(3);
      this.thCount = new Uint8Array(2); this.thTime = [0, 0];
      this.busReq = 0; this.zReset = 1; this.zBank = 0;
      this.mclk = 0; this.zc = 0; this.ymNext = YM_CLKS; this.psgAcc = 0;
      this.line = 0; this.lineStart = 0; this.vblank = false; this.hvLatch = -1;
      this.frame = 0; this.rsPos = 0; this.prevL = 0; this.prevR = 0; this.lpL = 0; this.lpR = 0;
      this.cpu.cycles = 0;
      this.cpu.reset();
      this.cpu.cycles = 0;
    }

    // ---- timing helpers ----
    now68k() { return this.cpu.cycles * 7; }
    dmaStall(len, internal, vramTarget) {
      if (internal) return; // fills / copies don't hold the 68000
      const blank = this.vblank || !this.vdp.display;
      const perLine = this.vdp.h40 ? (blank ? 205 : 18) : (blank ? 167 : 16);
      const words = vramTarget ? perLine / 2 : perLine;
      this.cpu.cycles += Math.ceil(len * LINE / words / 7);
    }
    dmaRead(a) {
      a &= 0xfffffe;
      if (a < 0x400000) return this.romWord(a);
      if (a >= 0xe00000) { a &= 0xffff; return (this.ram[a] << 8) | this.ram[a + 1]; }
      return 0;
    }
    updateIRQ() {
      const v = this.vdp;
      const lv = v.vintPending && (v.reg[1] & 0x20) ? 6 : v.hintPending && (v.reg[0] & 0x10) ? 4 : 0;
      if (lv !== this.cpu.irq) this.cpu.setIRQ(lv);
    }
    ack(lv) {
      if (lv === 6) this.vdp.vintPending = false;
      else if (lv === 4) this.vdp.hintPending = false;
      this.updateIRQ();
      return -1;
    }

    // ---- audio ----
    syncAudio(t) {
      const ym = this.ym, psg = this.psg;
      while (this.ymNext <= t) {
        ym.tick();
        this.psgAcc += YM_CLKS;
        let p = 0, n = 0;
        while (this.psgAcc >= PSG_CLKS) { this.psgAcc -= PSG_CLKS; p += psg.step(); n++; }
        const pv = n ? p / n * 0.13 : 0;
        if (this.audioN >= this.audioL.length) {
          const L = new Float32Array(this.audioL.length * 2); L.set(this.audioL); this.audioL = L;
          const R = new Float32Array(this.audioR.length * 2); R.set(this.audioR); this.audioR = R;
        }
        this.audioL[this.audioN] = ym.out[0] / 1400 + pv;
        this.audioR[this.audioN] = ym.out[1] / 1400 + pv;
        this.audioN++;
        this.ymNext += YM_CLKS;
      }
    }
    ymWrite(port, v, t) { this.syncAudio(t); this.ym.write(port, v); }
    ymRead(t) { this.syncAudio(t); return this.ym.readStatus(); }
    psgWrite(v, t) { this.syncAudio(t); this.psg.write(v); }

    // ---- cartridge ----
    romWord(a) {
      if (this.eeprom && a === (this.eeWire.out[0] & ~1)) return (this.romByte(a) << 8) | this.romByte(a + 1);
      if (a >= this.sramStart && a <= this.sramEnd && this.sramMapped) { const o = a - 0x200000; return (this.sram[o] << 8) | this.sram[o + 1]; }
      if (this.ssf2) a = (this.banks[a >> 19] << 19) | (a & 0x7ffff);
      a &= this.rom.length - 1;
      return (this.rom[a] << 8) | this.rom[a + 1];
    }
    romByte(a) {
      if (this.eeprom && (a & ~1) === (this.eeWire.out[0] & ~1)) {
        const w = this.eeWire.out;
        return a === w[0] ? (this.eeprom.out & this.eeprom.sda) << w[1] : 0;
      }
      if (a >= this.sramStart && a <= this.sramEnd && this.sramMapped) return this.sram[a - 0x200000];
      if (this.ssf2) a = (this.banks[a >> 19] << 19) | (a & 0x7ffff);
      return this.rom[a & (this.rom.length - 1)];
    }
    sramWrite(a, v) {
      if (this.eeprom) {
        const w = this.eeWire;
        if (a === w.sda[0] || a === w.scl[0]) {
          const e = this.eeprom;
          const sda = a === w.sda[0] ? (v >> w.sda[1]) & 1 : e.sda;
          const scl = a === w.scl[0] ? (v >> w.scl[1]) & 1 : e.scl;
          e.set(scl, sda);
        }
        return;
      }
      if (a >= this.sramStart && a <= this.sramEnd && this.sramMapped && !this.sramProtect) {
        const o = a - 0x200000;
        if (this.sram[o] !== v) { this.sram[o] = v; this.sramUsed = true; }
      }
    }

    // ---- I/O (pads) ----
    padRead(p) {
      const ctrl = this.ioCtrl[p], data = this.ioData[p];
      let v = 0x7f;
      if (p < 2) {
        const b = this.input[p] || 0;
        const th = (ctrl & 0x40) ? (data & 0x40) : 0x40;
        this.thTimeout(p);
        const c = this.thCount[p];
        const on = (n) => (b & BIT[n] ? 0 : 1);
        if (th) {
          if (c === 3) v = 0x40 | (on("C") << 5) | (on("B") << 4) | (on("Mode") << 3) | (on("X") << 2) | (on("Y") << 1) | on("Z");
          else v = 0x40 | (on("C") << 5) | (on("B") << 4) | (on("Right") << 3) | (on("Left") << 2) | (on("Down") << 1) | on("Up");
        } else {
          if (c === 3) v = (on("Start") << 5) | (on("A") << 4);
          else if (c === 4) v = (on("Start") << 5) | (on("A") << 4) | 0xf;
          else v = (on("Start") << 5) | (on("A") << 4) | (on("Down") << 1) | on("Up");
        }
      }
      return (data & 0x80) | ((data & ctrl) & 0x7f) | (v & ~ctrl & 0x7f);
    }
    thTimeout(p) {
      if (this.now68k() - this.thTime[p] > LINE * 24) this.thCount[p] = 0;
    }
    ioWrite(r, v) {
      if (r >= 1 && r <= 3) {
        const p = r - 1, oldTh = this.thLevel(p);
        this.ioData[p] = v;
        this.thEdge(p, oldTh);
      } else if (r >= 4 && r <= 6) {
        const p = r - 4, oldTh = this.thLevel(p);
        this.ioCtrl[p] = v;
        this.thEdge(p, oldTh);
      }
    }
    thLevel(p) { return (this.ioCtrl[p] & 0x40) ? (this.ioData[p] & 0x40) : 0x40; }
    thEdge(p, old) {
      if (p > 1) return;
      const now = this.thLevel(p);
      if (old && !now) { this.thTimeout(p); if (this.thCount[p] < 8) this.thCount[p]++; this.thTime[p] = this.now68k(); }
      else if (!old && now) this.thTime[p] = this.now68k();
    }
    ioRead(r) {
      switch (r) {
        case 0: return (this.overseas ? 0x80 : 0) | (this.pal ? 0x40 : 0) | 0x20;
        case 1: case 2: case 3: return this.padRead(r - 1);
        case 4: case 5: case 6: return this.ioCtrl[r - 4];
        default: return 0;
      }
    }

    // ---- Z80 side ----
    z80Busy() { return this.busReq || this.zReset; }
    zread(a) {
      if (a < 0x4000) return this.zram[a & 0x1fff];
      if (a < 0x6000) return this.ymRead(this.zc * 15);
      if (a < 0x8000) return 0xff;
      const ad = (this.zBank << 15) | (a & 0x7fff);
      if (ad >= 0xe00000) return 0xff;
      return this.read8(ad);
    }
    zwrite(a, v) {
      if (a < 0x4000) { this.zram[a & 0x1fff] = v; return; }
      if (a < 0x6000) { this.ymWrite(a & 3, v, this.zc * 15); return; }
      if (a < 0x6100) { this.zBank = ((this.zBank >> 1) | ((v & 1) << 8)) & 0x1ff; return; }
      if (a >= 0x7f00 && a < 0x7f20) { if ((a & 0x19) === 0x11) this.psgWrite(v, this.zc * 15); return; }
      if (a >= 0x8000) { const ad = (this.zBank << 15) | (a & 0x7fff); if (ad < 0xe00000) this.write8(ad, v); }
    }

    // ---- 68000 side ----
    read8(a) {
      a &= 0xffffff;
      if (a < 0x400000) return this.romByte(a);
      if (a >= 0xe00000) return this.ram[a & 0xffff];
      if (a >= 0xc00000 && a < 0xe00000) {
        if ((a & 0xe700e0) !== 0xc00000) return 0xff;
        const w = this.vdpRead(a & 0x1e);
        return (a & 1) ? w & 0xff : w >> 8;
      }
      if (a >= 0xa00000 && a < 0xa10000) {
        if (!this.busReq) return 0xff;
        const z = a & 0x7fff;
        if (z < 0x4000) return this.zram[z & 0x1fff];
        if (z < 0x6000) return this.ymRead(this.now68k());
        return 0xff;
      }
      if (a >= 0xa10000 && a < 0xa10020) return this.ioRead((a >> 1) & 0xf);
      if ((a & 0xfffffe) === 0xa11100) return (a & 1) ? 0 : (this.busReq && !this.zReset ? 0 : 1) | 0x80;
      if ((a & 0xfffffe) === 0xa11200) return 0xff;
      return 0xff;
    }
    read16(a) {
      a &= 0xfffffe;
      if (a < 0x400000) return this.romWord(a);
      if (a >= 0xe00000) { a &= 0xffff; return (this.ram[a] << 8) | this.ram[a + 1]; }
      if (a >= 0xc00000 && a < 0xe00000) { if ((a & 0xe700e0) !== 0xc00000) return 0xffff; return this.vdpRead(a & 0x1e); }
      if (a >= 0xa00000 && a < 0xa10000) { const b = this.read8(a); return (b << 8) | b; }
      if (a >= 0xa10000 && a < 0xa10020) { const b = this.ioRead((a >> 1) & 0xf); return (b << 8) | b; }
      if (a === 0xa11100) return ((this.busReq && !this.zReset ? 0 : 1) << 8) | 0x8000;
      return 0xffff;
    }
    write8(a, v) {
      a &= 0xffffff; v &= 0xff;
      if (a >= 0xe00000) { this.ram[a & 0xffff] = v; return; }
      if (a < 0x400000) { this.sramWrite(a, v); return; }
      if (a >= 0xc00000 && a < 0xe00000) {
        if ((a & 0xe700e0) !== 0xc00000) return;
        const r = a & 0x1f;
        if (r >= 0x10 && r < 0x18) { if (r & 1) this.psgWrite(v, this.now68k()); return; }
        this.vdpWrite(r & 0x1e, (v << 8) | v);
        return;
      }
      if (a >= 0xa00000 && a < 0xa10000) {
        if (!this.busReq) return;
        const z = a & 0x7fff;
        if (z < 0x4000) this.zram[z & 0x1fff] = v;
        else if (z < 0x6000) this.ymWrite(z & 3, v, this.now68k());
        else if (z < 0x6100) this.zBank = ((this.zBank >> 1) | ((v & 1) << 8)) & 0x1ff;
        else if (z >= 0x7f00 && (z & 0x19) === 0x11) this.psgWrite(v, this.now68k());
        return;
      }
      if (a >= 0xa10000 && a < 0xa10020) { if (a & 1) this.ioWrite((a >> 1) & 0xf, v); return; }
      if (a === 0xa11100) { this.setBusReq(v & 1); return; }
      if (a === 0xa11200) { this.setZReset(!(v & 1)); return; }
      if (a >= 0xa130f0 && a < 0xa13100) {
        if (a === 0xa130f1) { this.sramMapped = (v & 1) !== 0 || this.romSize <= 0x200000; this.sramProtect = (v & 2) !== 0; }
        else if (this.ssf2 && (a & 1)) this.banks[(a & 0xf) >> 1] = v & 0x3f;
      }
    }
    write16(a, v) {
      a &= 0xfffffe; v &= 0xffff;
      if (a >= 0xe00000) { a &= 0xffff; this.ram[a] = v >> 8; this.ram[a + 1] = v & 0xff; return; }
      if (a >= 0xc00000 && a < 0xe00000) {
        if ((a & 0xe700e0) !== 0xc00000) return;
        const r = a & 0x1f;
        if (r >= 0x10 && r < 0x18) { this.psgWrite(v & 0xff, this.now68k()); return; }
        this.vdpWrite(r & 0x1e, v);
        return;
      }
      if (a < 0x400000) {
        if (this.eeprom) { this.eeWrite16(a, v); return; }
        this.sramWrite(a, v >> 8); this.sramWrite(a + 1, v & 0xff); return;
      }
      if (a >= 0xa00000 && a < 0xa10000) { this.write8(a, v >> 8); return; }
      if (a >= 0xa10000 && a < 0xa10020) { this.ioWrite((a >> 1) & 0xf, v & 0xff); return; }
      if (a === 0xa11100) { this.setBusReq((v >> 8) & 1); return; }
      if (a === 0xa11200) { this.setZReset(!((v >> 8) & 1)); return; }
      this.write8(a, v >> 8); this.write8(a + 1, v & 0xff);
    }
    setBusReq(on) { this.busReq = on ? 1 : 0; }
    setZReset(on) {
      if (on && !this.zReset) { this.z80.reset(); this.syncAudio(this.now68k()); this.ym.reset(); }
      this.zReset = on ? 1 : 0;
    }
    vdpRead(r) {
      if (r < 4) return this.vdp.readData();
      if (r < 8) return this.vdp.status();
      if (r < 0x10) return this.vdp.hv();
      return 0xffff;
    }
    vdpWrite(r, v) {
      if (r < 4) this.vdp.writeData(v);
      else if (r < 8) this.vdp.writeControl(v);
    }

    // ---- running ----
    runTo(t) {
      const cpu = this.cpu, z80 = this.z80;
      while (this.mclk < t) {
        const end = Math.min(t, this.mclk + SLICE);
        if (this.z80Busy()) this.zc = Math.max(this.zc, Math.ceil(end / 15));
        else while (this.zc * 15 < end) this.zc += z80.step();
        cpu.run(Math.ceil(end / 7));
        this.mclk = end;
      }
    }
    runFrame() {
      const vdp = this.vdp;
      const H = vdp.lines, total = this.linesPerFrame;
      if (vdp.reg[12] & 2) vdp.odd ^= 1; else vdp.odd = 0;
      const im2 = vdp.im2;
      for (let line = 0; line < total; line++) {
        this.line = line;
        this.lineStart = this.mclk;
        if (line === 0) this.vblank = false;
        if (line === H) {
          this.vblank = true;
          this.runTo(this.lineStart + 80);
          vdp.vintPending = true;
          this.updateIRQ();
          this.z80.irq = true;
        } else if (line === H + 1) this.z80.irq = false;
        this.runTo(this.lineStart + ACTIVE);
        if (line < H) vdp.renderLine(line, this.screen);
        if (line <= H) {
          if (vdp.hcount === 0) { vdp.hcount = vdp.reg[10]; vdp.hintPending = true; this.updateIRQ(); }
          else vdp.hcount--;
        } else vdp.hcount = vdp.reg[10];
        this.runTo(this.lineStart + LINE);
      }
      this.syncAudio(this.mclk);
      this.width = vdp.width;
      this.height = im2 ? H * 2 : H;
      this.frame++;
    }

    setInput(p, bits) { this.input[p] = bits; }
    // Resample the native ~53 kHz output to the player's rate (interleaved stereo).
    takeAudio() {
      const n = this.audioN, ratio = this.nativeRate / this.sampleRate;
      const L = this.audioL, R = this.audioR;
      const maxOut = Math.ceil(n / ratio) + 2;
      if (this.outBuf.length < maxOut * 2) this.outBuf = new Float32Array(maxOut * 2);
      const out = this.outBuf;
      let k = 0, pos = this.rsPos;
      // one-pole low-pass at the native rate to soften aliasing
      const a = 0.55;
      for (let i = 0; i < n; i++) { this.lpL += (L[i] - this.lpL) * a; L[i] = this.lpL; this.lpR += (R[i] - this.lpR) * a; R[i] = this.lpR; }
      while (pos < n) {
        const i = Math.floor(pos), f = pos - i;
        const l0 = i === 0 ? this.prevL : L[i - 1], r0 = i === 0 ? this.prevR : R[i - 1];
        out[k++] = l0 + (L[i] - l0) * f;
        out[k++] = r0 + (R[i] - r0) * f;
        pos += ratio;
      }
      this.rsPos = pos - n;
      if (n) { this.prevL = L[n - 1]; this.prevR = R[n - 1]; }
      this.audioN = 0;
      return out.slice(0, k);
    }
    eeWrite16(a, v) {
      const w = this.eeWire, e = this.eeprom;
      let sda = e.sda, scl = e.scl, hit = false;
      for (const [ad, b] of [[a, v >> 8], [a + 1, v & 0xff]]) {
        if (ad === w.sda[0]) { sda = (b >> w.sda[1]) & 1; hit = true; }
        if (ad === w.scl[0]) { scl = (b >> w.scl[1]) & 1; hit = true; }
      }
      if (hit) e.set(scl, sda);
    }
    getSRAM() { if (this.eeprom) return this.eeprom.mem.slice(); return this.sramUsed ? this.sram.slice(0, Math.max(0x4000, this.sramEnd - 0x200000 + 1)) : null; }

    saveState() {
      return {
        v: 1, cpu: this.cpu.saveState(), z80: this.z80.getState(), vdp: this.vdp.saveState(), ym: this.ym.saveState(), psg: this.psg.saveState(),
        ram: this.ram.slice(), zram: this.zram.slice(), sram: this.sram.slice(), banks: this.banks.slice(),
        ioData: this.ioData.slice(), ioCtrl: this.ioCtrl.slice(), thCount: this.thCount.slice(),
        m: [this.busReq, this.zReset, this.zBank, this.mclk, this.zc, this.ymNext, this.psgAcc, this.sramMapped ? 1 : 0, this.sramProtect ? 1 : 0,
          this.hvLatch, this.frame, this.rsPos, this.lpL, this.lpR, this.prevL, this.prevR],
        thTime: this.thTime.slice(), ee: this.eeprom ? this.eeprom.saveState() : null,
      };
    }
    loadState(s) {
      this.cpu.loadState(s.cpu); this.z80.setState(s.z80); this.vdp.loadState(s.vdp); this.ym.loadState(s.ym); this.psg.loadState(s.psg);
      this.ram.set(s.ram); this.zram.set(s.zram); this.sram.set(s.sram); this.banks.set(s.banks);
      this.ioData.set(s.ioData); this.ioCtrl.set(s.ioCtrl); this.thCount.set(s.thCount); this.thTime = s.thTime.slice();
      [this.busReq, this.zReset, this.zBank, this.mclk, this.zc, this.ymNext, this.psgAcc] = s.m;
      this.sramMapped = !!s.m[7]; this.sramProtect = !!s.m[8]; this.hvLatch = s.m[9]; this.frame = s.m[10];
      [this.rsPos, this.lpL, this.lpR, this.prevL, this.prevR] = s.m.slice(11, 16);
      if (this.eeprom && s.ee) this.eeprom.loadState(s.ee);
      this.audioN = 0;
    }
  }

  Genesis.info = {
    system: "segaMD", name: "Sega Genesis", width: 320, height: 224, aspect: 4 / 3, fps: 59.92, stereo: true, players: 2,
    buttons: BUTTONS,
  };

  global.RPCores = global.RPCores || {};
  global.RPCores.segaMD = Genesis;
})(typeof window !== "undefined" ? window : globalThis);
