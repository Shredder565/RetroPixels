// RetroPixels — Sega Master System / Mark III emulator. Original code.
//
// Machine: Zilog Z80 (RPZ80, shared with the Genesis), the SMS VDP in its
// "mode 4" (tiles, 64 sprites, line interrupts, 192- and 224-line screens),
// the SN76489 PSG (RPSN76489), two pads plus the console's Pause and Reset
// buttons, and the Sega cartridge mapper with battery-backed cartridge RAM
// (also the Codemasters mapper). The BIOS isn't needed: the cartridge starts
// directly. Written from the documented hardware behaviour.
"use strict";

(function (global) {
  const CPU_HZ = 3579545;
  const LINE = 228;              // processor cycles per scan line
  const ACTIVE = 171;            // ...of which the picture takes this many
  const AUDIO_DIV = 64;          // processor cycles per native audio sample (PSG ticks every 16)

  // ===================================================================
  // VDP (mode 4)
  // ===================================================================
  // TMS9918 colours (RGB), each channel rounded to the nearest of 0/85/170/255.
  const TMS_RGB = [0x000000, 0x000000, 0x21c842, 0x5edc78, 0x5455ed, 0x7d76fc, 0xd4524d, 0x42ebf5,
    0xfc5554, 0xff7978, 0xd4c154, 0xe6ce80, 0x21b03b, 0xc95bba, 0xcccccc, 0xffffff];
  const TMS_PAL = new Uint32Array(TMS_RGB.map((v) => {
    const q = (c) => Math.round(c / 85) * 85;
    return (0xff000000 | (q(v & 255) << 16) | (q((v >> 8) & 255) << 8) | q(v >> 16)) >>> 0;
  }));

  class VDP {
    constructor(sms) {
      this.sms = sms;
      this.vram = new Uint8Array(0x4000);
      this.cram = new Uint8Array(32);
      this.reg = new Uint8Array(16);
      this.pal = new Uint32Array(32);
      this.bgCol = new Uint8Array(256); this.bgPri = new Uint8Array(256); this.spr = new Uint8Array(256);
      this.reset();
    }
    reset() {
      this.vram.fill(0); this.cram.fill(0); this.reg.fill(0);
      // the BIOS leaves these set up
      this.reg[0] = 0x36; this.reg[1] = 0x80; this.reg[2] = 0xff; this.reg[3] = 0xff; this.reg[4] = 0xff; this.reg[5] = 0xff;
      this.reg[6] = 0xfb; this.reg[10] = 0xff;
      this.addr = 0; this.code = 0; this.latch = false; this.buf = 0;
      this.status = 0; this.lineIrq = false; this.lineCnt = 0xff; this.vscroll = 0;
      for (let i = 0; i < 32; i++) this.updatePal(i);
    }
    updatePal(i) {
      const c = this.cram[i], L = (v) => v * 85;
      this.pal[i] = 0xff000000 | (L((c >> 4) & 3) << 16) | (L((c >> 2) & 3) << 8) | L(c & 3);
    }
    get lines() { return (this.reg[0] & 6) === 6 && (this.reg[1] & 0x18) === 0x10 ? 224 : 192; }
    get displayOn() { return (this.reg[1] & 0x40) !== 0; }
    irqLine() { return ((this.status & 0x80) && (this.reg[1] & 0x20)) || (this.lineIrq && (this.reg[0] & 0x10)); }

    // ---- ports ----
    writeControl(v) {
      if (!this.latch) { this.latch = true; this.addr = (this.addr & 0x3f00) | v; return; }
      this.latch = false;
      this.addr = ((v & 0x3f) << 8) | (this.addr & 0xff);
      this.code = v >> 6;
      if (this.code === 0) { this.buf = this.vram[this.addr]; this.addr = (this.addr + 1) & 0x3fff; }
      else if (this.code === 2) {
        const r = v & 15;
        if (r < 11) { this.reg[r] = this.addr & 0xff; this.sms.updateIrq(); }
      }
    }
    writeData(v) {
      this.latch = false;
      if (this.code === 3) { const i = this.addr & 31; this.cram[i] = v & 0x3f; this.updatePal(i); }
      else this.vram[this.addr] = v;
      this.buf = v;
      this.addr = (this.addr + 1) & 0x3fff;
    }
    readData() {
      this.latch = false;
      const v = this.buf;
      this.buf = this.vram[this.addr];
      this.addr = (this.addr + 1) & 0x3fff;
      return v;
    }
    readStatus() {
      const v = this.status | 0x1f;
      this.status = 0; this.lineIrq = false; this.latch = false;
      this.sms.updateIrq();
      return v;
    }

    // ---- per-line counters ----
    // Called at the end of each line: the line counter runs on lines 0..active (inclusive).
    lineDone(line) {
      if (line <= this.lines) {
        if (this.lineCnt === 0) { this.lineCnt = this.reg[10]; this.lineIrq = true; }
        else this.lineCnt = (this.lineCnt - 1) & 0xff;
      } else this.lineCnt = this.reg[10];
    }

    // ---- drawing ----
    renderLine(y, out, W) {
      const o = y * W, backdrop = this.pal[16 + (this.reg[7] & 15)];
      if (!this.displayOn) { out.fill(backdrop, o, o + W); return; }
      if (!(this.reg[0] & 4)) { this.renderTMS(y, out, W); return; }
      const vram = this.vram, r0 = this.reg[0];
      const tall = this.lines === 224;
      const nameBase = tall ? ((this.reg[2] & 0x0c) << 10) | 0x0700 : (this.reg[2] & 0x0e) << 10;
      const sx = (r0 & 0x40) && y < 16 ? 0 : this.reg[8];
      const col = this.bgCol, pri = this.bgPri;
      // background
      let lastCell = -1, b0 = 0, b1 = 0, b2 = 0, b3 = 0, palOff = 0, prio = 0, hf = 0;
      for (let x = 0; x < 256; x++) {
        const vs = (r0 & 0x80) && x >= 192 ? 0 : this.vscroll;
        let row = y + vs;
        row = tall ? row & 255 : row % 224;
        const bx = (x - sx) & 255, cell = ((row >> 3) << 5) | (bx >> 3);
        if (cell !== lastCell) {
          lastCell = cell;
          const ea = (nameBase + cell * 2) & 0x3fff;
          const e = vram[ea] | (vram[ea + 1] << 8);
          let ty = row & 7; if (e & 0x400) ty = 7 - ty;
          const pa = ((e & 0x1ff) << 5) + ty * 4;
          b0 = vram[pa & 0x3fff]; b1 = vram[(pa + 1) & 0x3fff]; b2 = vram[(pa + 2) & 0x3fff]; b3 = vram[(pa + 3) & 0x3fff];
          hf = e & 0x200; palOff = (e & 0x800) ? 16 : 0; prio = (e >> 12) & 1;
        }
        const bit = hf ? bx & 7 : 7 - (bx & 7);
        const c = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1) | (((b2 >> bit) & 1) << 2) | (((b3 >> bit) & 1) << 3);
        col[x] = c | palOff; pri[x] = prio && c ? 1 : 0;
      }
      // sprites
      const spr = this.spr; spr.fill(0);
      const satBase = (this.reg[5] & 0x7e) << 7, tallSpr = this.reg[1] & 2, zoom = this.reg[1] & 1;
      const h = (tallSpr ? 16 : 8) << (zoom ? 1 : 0);
      const patHi = (this.reg[6] & 4) ? 256 : 0, shift = (r0 & 8) ? 8 : 0;
      let n = 0;
      for (let i = 0; i < 64; i++) {
        const sy0 = vram[satBase + i];
        if (sy0 === 0xd0 && !tall) break;
        let sy = sy0 + 1; if (sy > 240) sy -= 256;
        if (y < sy || y >= sy + h) continue;
        if (n === 8) { this.status |= 0x40; break; }
        n++;
        const sxp = vram[satBase + 128 + i * 2] - shift;
        let pat = vram[satBase + 129 + i * 2] | patHi;
        if (tallSpr) pat &= ~1;
        let r = (y - sy) >> (zoom ? 1 : 0);
        const pa = (pat << 5) + r * 4;
        const p0 = vram[pa & 0x3fff], p1 = vram[(pa + 1) & 0x3fff], p2 = vram[(pa + 2) & 0x3fff], p3 = vram[(pa + 3) & 0x3fff];
        for (let px = 0; px < 8; px++) {
          const bit = 7 - px;
          const c = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
          if (!c) continue;
          for (let z = 0; z <= (zoom ? 1 : 0); z++) {
            const x = sxp + (zoom ? px * 2 + z : px);
            if (x < 0 || x >= 256) continue;
            if (spr[x]) { this.status |= 0x20; continue; }
            spr[x] = 16 | c;
          }
        }
      }
      // compose
      const pal = this.pal;
      for (let x = 0; x < W; x++) {
        let c;
        if (spr[x] && !pri[x]) c = pal[spr[x]];
        else c = pal[col[x]];
        out[o + x] = c;
      }
      if (r0 & 0x20) out.fill(backdrop, o, o + 8); // left column hidden
    }

    // ---- the older TMS9918 modes (Graphics I / II, Text, Multicolor) ----
    // Used by SG-1000 era games and some homebrew. Fixed 16-colour palette, rounded
    // to the 2-bit-per-channel colours this VDP can make.
    renderTMS(y, out, W) {
      const vram = this.vram, reg = this.reg, o = y * W;
      const tc = (i) => TMS_PAL[i || (reg[7] & 15)];
      const backdrop = tc(reg[7] & 15);
      if (!this.displayOn) { out.fill(backdrop, o, o + W); return; }
      const m1 = reg[1] & 0x10, m2 = reg[1] & 8, m3 = reg[0] & 2;
      const nameBase = (reg[2] & 0x0f) << 10, row = y >> 3, line = y & 7;
      const col = this.bgCol; // TMS colour index per pixel, 0 = transparent
      if (m1) { // Text: 40 columns of 6 pixels, no sprites
        const fg = reg[7] >> 4, bg = reg[7] & 15, patBase = (reg[4] & 7) << 11;
        col.fill(bg);
        for (let cx = 0; cx < 40; cx++) {
          const p = vram[(patBase + (vram[(nameBase + row * 40 + cx) & 0x3fff] << 3) + line) & 0x3fff];
          for (let b = 0; b < 6; b++) col[8 + cx * 6 + b] = (p & (0x80 >> b)) ? fg : bg;
        }
        for (let x = 0; x < W; x++) out[o + x] = tc(col[x]);
        return;
      }
      for (let cx = 0; cx < 32; cx++) {
        const name = vram[(nameBase + row * 32 + cx) & 0x3fff];
        let p, c;
        if (m2) { // Multicolor: 4x4 blocks
          const a = ((reg[4] & 7) << 11) + (name << 3) + ((row & 3) << 1) + (line >> 2);
          const v = vram[a & 0x3fff];
          for (let b = 0; b < 8; b++) col[cx * 8 + b] = b < 4 ? v >> 4 : v & 15;
          continue;
        }
        if (m3) { // Graphics II: the screen in three thirds, a colour byte per pattern line
          const idx = ((row >> 3) << 8) | name;
          p = vram[(((reg[4] & 4) << 11) + ((idx & (((reg[4] & 3) << 8) | 0xff)) << 3) + line) & 0x3fff];
          c = vram[(((reg[3] & 0x80) << 6) + ((idx & (((reg[3] & 0x7f) << 3) | 7)) << 3) + line) & 0x3fff];
        } else { // Graphics I: a colour byte per 8 patterns
          p = vram[(((reg[4] & 7) << 11) + (name << 3) + line) & 0x3fff];
          c = vram[((reg[3] << 6) + (name >> 3)) & 0x3fff];
        }
        for (let b = 0; b < 8; b++) col[cx * 8 + b] = (p & (0x80 >> b)) ? c >> 4 : c & 15;
      }
      // sprites: 32 of them, 4 per line, one colour each
      const spr = this.spr; spr.fill(0);
      const sat = (reg[5] & 0x7f) << 7, pg = (reg[6] & 7) << 11;
      const big = reg[1] & 2, mag = reg[1] & 1, size = (big ? 16 : 8) << (mag ? 1 : 0);
      let n = 0;
      for (let i = 0; i < 32; i++) {
        const a = sat + i * 4, sy0 = vram[a];
        if (sy0 === 0xd0) break;
        let sy = sy0 + 1; if (sy > 0xe0) sy -= 256;
        if (y < sy || y >= sy + size) continue;
        if (n === 4) { if (!(this.status & 0x40)) this.status = (this.status & 0xe0) | 0x40 | i; break; }
        n++;
        const attr = vram[a + 3], c = attr & 15;
        let sx = vram[a + 1] - ((attr & 0x80) ? 32 : 0);
        const r = (y - sy) >> (mag ? 1 : 0), pat = big ? vram[a + 2] & 0xfc : vram[a + 2];
        for (let half = 0; half < (big ? 2 : 1); half++) {
          const bits = vram[(pg + (pat << 3) + r + half * 16) & 0x3fff];
          for (let b = 0; b < 8; b++) {
            if (!(bits & (0x80 >> b))) continue;
            for (let z = 0; z <= (mag ? 1 : 0); z++) {
              const x = sx + ((half * 8 + b) << (mag ? 1 : 0)) + z;
              if (x < 0 || x >= 256) continue;
              if (spr[x]) { this.status |= 0x20; continue; }
              spr[x] = 16 | c;
            }
          }
        }
      }
      for (let x = 0; x < W; x++) {
        const s = spr[x] & 15;
        out[o + x] = tc(s ? s : col[x]);
      }
    }

    saveState() {
      return { vram: this.vram.slice(), cram: this.cram.slice(), reg: this.reg.slice(),
        r: [this.addr, this.code, this.latch ? 1 : 0, this.buf, this.status, this.lineIrq ? 1 : 0, this.lineCnt, this.vscroll] };
    }
    loadState(o) {
      this.vram.set(o.vram); this.cram.set(o.cram); this.reg.set(o.reg);
      [this.addr, this.code, this.latch, this.buf, this.status, this.lineIrq, this.lineCnt, this.vscroll] = o.r;
      this.latch = !!this.latch; this.lineIrq = !!this.lineIrq;
      for (let i = 0; i < 32; i++) this.updatePal(i);
    }
  }

  // ===================================================================
  // Cartridge
  // ===================================================================
  // Codemasters carts carry their own header at $7FE0 with a checksum and its complement.
  function isCodemasters(rom) {
    if (rom.length < 0x8000) return false;
    const sum = rom[0x7fe6] | (rom[0x7fe7] << 8), inv = rom[0x7fe8] | (rom[0x7fe9] << 8);
    return sum !== 0 && ((sum + inv) & 0xffff) === 0;
  }

  // ===================================================================
  // The console
  // ===================================================================
  const BUTTONS = ["Up", "Down", "Left", "Right", "1", "2", "Pause", "Reset"];
  const B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_1 = 16, B_2 = 32, B_PAUSE = 64, B_RESET = 128;

  class SMS {
    constructor(romIn, opts) {
      opts = opts || {};
      // Some dumps carry a 512-byte copier header.
      let data = romIn;
      if (data.length % 0x4000 === 512) data = data.subarray(512);
      let size = 0x4000; while (size < data.length) size <<= 1;
      this.rom = new Uint8Array(size); this.rom.set(data);
      for (let i = data.length; i < size; i++) this.rom[i] = this.rom[i % Math.max(1, data.length)];
      this.nBanks = size >> 14;
      this.codemasters = isCodemasters(this.rom);
      this.sampleRate = opts.sampleRate || 48000;
      this.fps = CPU_HZ / (LINE * 262);
      this.ram = new Uint8Array(0x2000);
      this.cartRam = new Uint8Array(0x8000);
      this.cartRamUsed = false;
      if (opts.sram && opts.sram.length) { this.cartRam.set(opts.sram.subarray(0, 0x8000)); this.cartRamUsed = true; }
      this.vdp = new VDP(this);
      this.psg = new global.RPSN76489();
      const self = this;
      this.cpu = new global.RPZ80({
        read: (a) => self.read(a), write: (a, v) => self.write(a, v),
        in: (p) => self.portIn(p & 0xff), out: (p, v) => self.portOut(p & 0xff, v), vector: () => 0xff,
      });
      this.width = 256; this.height = 192;
      this.screen = new Uint32Array(256 * 240);
      this.input = [0, 0];
      this.audio = new Float32Array(4096); this.audioN = 0; this.outBuf = new Float32Array(4096);
      this.reset();
    }
    reset() {
      this.ram.fill(0);
      this.vdp.reset(); this.psg.reset(); this.cpu.reset();
      this.ctrl = 0; this.slot = [0, 1, 2]; if (this.codemasters) this.slot = [0, 1, 0];
      this.port3F = 0xff; this.hLatch = 0;
      this.line = 0; this.cyc = 0; this.frameCyc = 0; this.psgCyc = 0; this.sampleAcc = 0; this.sampleN = 0;
      this.pauseWas = false; this.rsPos = 0; this.prev = 0; this.lp = 0; this.hp = 0; this.hpPrev = 0;
      // As the BIOS leaves it: the stack near the top of RAM.
      this.cpu.setState(Object.assign(this.cpu.getState().slice(), { 10: 0xdff0 }));
    }

    // ---- memory ----
    read(a) {
      if (a >= 0xc000) return this.ram[a & 0x1fff];
      if (a < 0x400 && !this.codemasters) return this.rom[a];
      if (a >= 0x8000 && (this.ctrl & 8)) return this.cartRam[((this.ctrl & 4) << 12) | (a & 0x3fff)];
      if (this.codemasters && a >= 0xa000 && a < 0xc000 && this.cmRam) return this.cartRam[a & 0x1fff];
      return this.rom[(this.slot[a >> 14] % this.nBanks) * 0x4000 + (a & 0x3fff)];
    }
    write(a, v) {
      if (a >= 0xc000) {
        this.ram[a & 0x1fff] = v;
        if (a >= 0xfffc && !this.codemasters) {
          if (a === 0xfffc) this.ctrl = v; else this.slot[a - 0xfffd] = v;
        }
        return;
      }
      if (this.codemasters) {
        if (a === 0x0000) this.slot[0] = v;
        else if (a === 0x4000) { this.slot[1] = v & 0x7f; this.cmRam = !!(v & 0x80); }
        else if (a === 0x8000) this.slot[2] = v;
        else if (this.cmRam && a >= 0xa000 && a < 0xc000) { this.cartRam[a & 0x1fff] = v; this.cartRamUsed = true; }
        return;
      }
      if (a >= 0x8000 && (this.ctrl & 8)) { this.cartRam[((this.ctrl & 4) << 12) | (a & 0x3fff)] = v; this.cartRamUsed = true; }
    }

    // ---- I/O ports ----
    portIn(p) {
      switch (p & 0xc1) {
        case 0x00: case 0x01: return 0xff;
        case 0x40: return this.vCounter();
        case 0x41: return this.hLatch;
        case 0x80: return this.vdp.readData();
        case 0x81: return this.vdp.readStatus();
        case 0xc0: { // pad 1, and up / down of pad 2
          const a = this.input[0], b = this.input[1];
          let v = 0xff;
          if (a & B_UP) v &= ~1; if (a & B_DOWN) v &= ~2; if (a & B_LEFT) v &= ~4; if (a & B_RIGHT) v &= ~8;
          if (a & B_1) v &= ~0x10; if (a & B_2) v &= ~0x20; if (b & B_UP) v &= ~0x40; if (b & B_DOWN) v &= ~0x80;
          return v;
        }
        case 0xc1: { // the rest of pad 2, the Reset button and the TH lines
          const b = this.input[1];
          let v = 0x3f;
          if (b & B_LEFT) v &= ~1; if (b & B_RIGHT) v &= ~2; if (b & B_1) v &= ~4; if (b & B_2) v &= ~8;
          if (this.input[0] & B_RESET) v &= ~0x10;
          // TH pins: what the game drives on them when they're outputs (an export console)
          const f = this.port3F;
          v |= ((f & 2) ? 0x40 : (f & 0x20) << 1) | ((f & 8) ? 0x80 : f & 0x80);
          return v;
        }
      }
      return 0xff;
    }
    portOut(p, v) {
      switch (p & 0xc1) {
        case 0x01: { // I/O control: a TH line going high latches the H counter
          const old = this.port3F;
          this.port3F = v;
          const th = (x) => ((x & 2) ? 1 : (x >> 5) & 1) | (((x & 8) ? 1 : (x >> 7) & 1) << 1);
          if ((~th(old) & th(v)) !== 0) this.hLatch = this.hCounter();
          return;
        }
        case 0x40: case 0x41: this.syncAudio(); this.psg.write(v); return;
        case 0x80: this.vdp.writeData(v); return;
        case 0x81: this.vdp.writeControl(v); return;
      }
    }
    vCounter() {
      const l = this.line, tall = this.vdp.lines === 224;
      if (tall) return (l > 0xea ? l - 6 : l) & 0xff;
      return (l > 0xda ? l - 6 : l) & 0xff;
    }
    // The H counter only changes the value games can read when a TH line latches it.
    hCounter() { const h = Math.floor((this.cyc - this.lineStart) * 171 / LINE); return (h < 0x94 ? h : h + 0x55) & 0xff; }
    updateIrq() { this.cpu.irq = !!this.vdp.irqLine(); }

    // ---- sound ----
    syncAudio() {
      while (this.psgCyc + 16 <= this.cyc) {
        this.psgCyc += 16;
        this.sampleAcc += this.psg.step(); this.sampleN++;
        if (this.sampleN === AUDIO_DIV / 16) {
          if (this.audioN >= this.audio.length) { const n = new Float32Array(this.audio.length * 2); n.set(this.audio); this.audio = n; }
          this.audio[this.audioN++] = this.sampleAcc / this.sampleN * 0.25;
          this.sampleAcc = 0; this.sampleN = 0;
        }
      }
    }
    takeAudio() {
      const n = this.audioN, ratio = CPU_HZ / AUDIO_DIV / this.sampleRate, A = this.audio;
      const maxOut = Math.ceil(n / ratio) + 2;
      if (this.outBuf.length < maxOut) this.outBuf = new Float32Array(maxOut);
      const out = this.outBuf;
      for (let i = 0; i < n; i++) { // soften aliasing, then remove the DC offset
        this.lp += (A[i] - this.lp) * 0.5;
        this.hp = 0.999 * (this.hp + this.lp - this.hpPrev); this.hpPrev = this.lp; A[i] = this.hp;
      }
      let k = 0, pos = this.rsPos;
      while (pos < n) {
        const i = Math.floor(pos), f = pos - i, a = i === 0 ? this.prev : A[i - 1];
        out[k++] = a + (A[i] - a) * f;
        pos += ratio;
      }
      this.rsPos = pos - n;
      if (n) this.prev = A[n - 1];
      this.audioN = 0;
      return out.slice(0, k);
    }

    // ---- running ----
    runFrame() {
      const vdp = this.vdp, cpu = this.cpu;
      // Pause is wired to the processor's NMI: pressing it interrupts once.
      const pause = !!(this.input[0] & B_PAUSE);
      if (pause && !this.pauseWas) cpu.nmi();
      this.pauseWas = pause;
      const H = vdp.lines;
      this.height = H;
      for (let line = 0; line < 262; line++) {
        this.line = line; this.lineStart = this.cyc;
        if (line === 0) vdp.vscroll = vdp.reg[9]; // vertical scroll is taken once per frame
        if (line < H) vdp.renderLine(line, this.screen, 256);
        // the visible part of the line; the line and frame interrupts come at its end,
        // so a handler running in the horizontal blank changes the next line
        let end = this.lineStart + ACTIVE;
        while (this.cyc < end) this.cyc += cpu.step();
        vdp.lineDone(line);
        if (line === H) vdp.status |= 0x80; // frame interrupt at the end of the picture
        this.updateIrq();
        end = this.lineStart + LINE;
        while (this.cyc < end) this.cyc += cpu.step();
      }
      this.syncAudio();
      // keep the counters small
      this.psgCyc -= this.cyc; this.cyc = 0;
    }
    setInput(p, bits) { this.input[p] = bits; }
    getSRAM() { return this.cartRamUsed ? this.cartRam.slice() : null; }
    saveState() {
      return { v: 1, cpu: this.cpu.getState(), vdp: this.vdp.saveState(), psg: this.psg.saveState(), ram: this.ram.slice(), cartRam: this.cartRam.slice(),
        m: [this.ctrl, this.slot[0], this.slot[1], this.slot[2], this.port3F, this.hLatch, this.cmRam ? 1 : 0, this.cartRamUsed ? 1 : 0, this.pauseWas ? 1 : 0] };
    }
    loadState(s) {
      this.cpu.setState(s.cpu); this.vdp.loadState(s.vdp); this.psg.loadState(s.psg); this.ram.set(s.ram); this.cartRam.set(s.cartRam);
      [this.ctrl, this.slot[0], this.slot[1], this.slot[2], this.port3F, this.hLatch] = s.m;
      this.cmRam = !!s.m[6]; this.cartRamUsed = !!s.m[7]; this.pauseWas = !!s.m[8];
      this.cyc = 0; this.psgCyc = 0; this.audioN = 0;
      this.updateIrq();
    }
  }

  SMS.info = { system: "sms", name: "Master System", width: 256, height: 192, aspect: 4 / 3, fps: CPU_HZ / (LINE * 262), players: 2, buttons: BUTTONS };

  global.RPCores = global.RPCores || {};
  global.RPCores.sms = SMS;
})(typeof window !== "undefined" ? window : globalThis);
