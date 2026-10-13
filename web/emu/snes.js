// RetroPixels — Super Nintendo emulator. Original code.
//
// 65C816 CPU (RP65816), SPC700 + S-DSP sound (RPSPC700, RPSDSP), the PPU
// (modes 0-7, sprites, windows, colour math, mosaic, offset-per-tile, hires,
// Mode 7), DMA / HDMA, timers, multiply / divide unit and joypads, written from
// the documented hardware behaviour. The sound CPU's start-up program (normally
// a small Nintendo ROM) is replaced by RetroPixels' own implementation of the
// same upload protocol, so no Nintendo code is used.
"use strict";

(function (global) {
  const LINE = 1364;            // master clocks per scan line
  const HBLANK = 1096;          // render point / HDMA
  const NTSC_HZ = 21477272, PAL_HZ = 21281370;

  // ===================================================================
  // Cartridge
  // ===================================================================
  class Cart {
    constructor(romIn, sram) {
      let rom = romIn;
      if ((rom.length & 0x3ff) === 0x200) rom = rom.subarray(0x200); // copier header
      this.rom = new Uint8Array(rom);
      this.size = this.rom.length;
      const score = (base) => {
        if (base + 0x40 > this.size) return -100;
        const r = this.rom;
        let s = 0;
        const chk = r[base + 0x1e] | (r[base + 0x1f] << 8), cmp = r[base + 0x1c] | (r[base + 0x1d] << 8);
        if ((chk ^ cmp) === 0xffff) s += 4;
        const mode = r[base + 0x15];
        const reset = r[base + 0x3c] | (r[base + 0x3d] << 8);
        if (reset >= 0x8000) s += 3; else s -= 4;
        if (base === 0x7fc0 && (mode & 0xef) === 0x20) s += 2;
        if (base === 0xffc0 && (mode & 0xef) === 0x21) s += 2;
        if (base === 0x40ffc0 && (mode & 0xef) === 0x25) s += 3;
        if (base === 0x7fc0 && mode === 0x23) s += 2; // SA-1 cartridges
        let printable = 0; for (let i = 0; i < 21; i++) { const c = r[base + i]; if (c >= 0x20 && c < 0x7f) printable++; }
        if (printable > 16) s += 1;
        const op = r[(base & ~0x7fff) + (reset & 0x7fff)];
        if ([0x78, 0x18, 0x38, 0x9c, 0x4c, 0x5c].includes(op)) s += 2;
        return s;
      };
      const cands = [[0x7fc0, "lo"], [0xffc0, "hi"], [0x40ffc0, "exhi"]].map(([b, t]) => [score(b), b, t]);
      cands.sort((a, b) => b[0] - a[0]);
      this.headerAt = cands[0][1]; this.map = cands[0][2];
      const h = this.headerAt, r = this.rom;
      this.title = String.fromCharCode(...r.subarray(h, h + 21)).replace(/[^\x20-\x7e]/g, " ").trim();
      this.chip = r[h + 0x16];
      const sramK = r[h + 0x18];
      this.sramSize = sramK ? Math.min(0x20000, 0x400 << sramK) : 0;
      this.pal = r[h + 0x19] >= 2 && r[h + 0x19] <= 12;
      this.sram = new Uint8Array(Math.max(this.sramSize, 1)).fill(0xff);
      if (sram && sram.length && this.sramSize) this.sram.set(sram.subarray(0, this.sramSize));
      this.sramMask = this.sramSize ? this.sramSize - 1 : 0;
      this.sramDirty = false;
      // power-of-two mirroring of odd-sized ROMs
      this.mirror = new Int32Array(0);
    }
    special() {
      const c = this.chip;
      // [chips] the snes_*.js files name the cartridge chips they recognise;
      // ones they can run return null here (no error), the rest return their name
      const known = global.RPSNESChips ? global.RPSNESChips.find((e) => e.detect(this)) : null;
      if (known) return known.create ? null : known.name;
      if (c === 0x03 || c === 0x05) return "DSP-1";
      if (c >= 0x13 && c <= 0x1a) return "Super FX";
      if (c === 0x34 || c === 0x35) return "SA-1";
      if (c === 0x43 || c === 0x45) return "S-DD1";
      if (c === 0xf5 || c === 0xf9) return "SPC7110";
      if (c === 0xf3) return "Cx4";
      return null;
    }
    romAt(off) {
      const n = this.size;
      if (off < n) return this.rom[off];
      // mirror: drop the highest bits until it fits
      let base = 0, size = n, a = off & 0xffffff, mask = 0x800000;
      while (a >= size) {
        while (!(a & mask)) mask >>= 1;
        a -= mask;
        if (size > mask) { size -= mask; base += mask; }
        mask >>= 1;
      }
      return this.rom[base + a] | 0;
    }
    // Returns -1 when the address isn't cartridge ROM/SRAM.
    read(bank, off) {
      const lo = this.map === "lo";
      if (lo) {
        if ((bank & 0x7f) >= 0x70 && (bank & 0x7f) < 0x7e && off < 0x8000 && this.sramSize) return this.sram[(((bank & 0xf) << 15) | off) & this.sramMask];
        if (off >= 0x8000) return this.romAt(((bank & 0x7f) << 15) | (off & 0x7fff));
        if ((bank & 0x7f) >= 0x40) return this.romAt(((bank & 0x7f) << 15) | (off & 0x7fff));
        return -1;
      }
      const b = bank & 0x7f;
      if (b < 0x40 && off >= 0x6000 && off < 0x8000) return this.sramSize ? this.sram[(((bank & 0x1f) << 13) | (off - 0x6000)) & this.sramMask] : -1;
      if (this.map === "exhi") {
        if (bank >= 0xc0) return this.romAt(((bank & 0x3f) << 16) | off);
        if (bank >= 0x40 && bank < 0x7e) return this.romAt(0x400000 | ((bank & 0x3f) << 16) | off);
        if (off >= 0x8000) return this.romAt((bank < 0x80 ? 0x400000 : 0) | ((bank & 0x3f) << 16) | off);
        return -1;
      }
      if (b >= 0x40) return this.romAt(((bank & 0x3f) << 16) | off);
      if (off >= 0x8000) return this.romAt(((bank & 0x3f) << 16) | off);
      return -1;
    }
    write(bank, off, v) {
      if (!this.sramSize) return;
      let i = -1;
      if (this.map === "lo") { if ((bank & 0x7f) >= 0x70 && (bank & 0x7f) < 0x7e && off < 0x8000) i = (((bank & 0xf) << 15) | off) & this.sramMask; }
      else if ((bank & 0x7f) < 0x40 && off >= 0x6000 && off < 0x8000) i = (((bank & 0x1f) << 13) | (off - 0x6000)) & this.sramMask;
      if (i >= 0 && this.sram[i] !== v) { this.sram[i] = v; this.sramDirty = true; }
    }
  }

  // ===================================================================
  // Sound unit: SPC700 + DSP + timers + RetroPixels' upload program
  // ===================================================================
  class APU {
    constructor() {
      this.ram = new Uint8Array(0x10000);
      this.dsp = new global.RPSDSP(this.ram);
      const self = this;
      this.spc = new global.RPSPC700({ read: (a) => self.read(a), write: (a, v) => self.write(a, v) });
      this.portIn = new Uint8Array(4); this.portOut = new Uint8Array(4);
      this.tTarget = new Uint16Array(3); this.tStage = new Uint16Array(3); this.tOut = new Uint8Array(3); this.tDiv = new Int32Array(3);
      this.samples = new Int16Array(4096); this.nSamples = 0;
      this.reset();
    }
    reset() {
      this.ram.fill(0); this.dsp.reset(); this.spc.reset();
      this.portIn.fill(0); this.portOut.fill(0);
      this.tTarget.fill(0); this.tStage.fill(0); this.tOut.fill(0); this.tDiv.fill(0);
      this.control = 0x80; this.dspAddr = 0;
      this.cycles = 0; this.sampleDiv = 0;
      this.enterIpl();
    }
    // ---- RetroPixels' start-up / upload program (runs in place of the IPL ROM) ----
    enterIpl() {
      this.ipl = true; this.iplState = 0; this.iplAddr = 0; this.iplIdx = 0; this.iplSeen = -1; this.iplWait = 0;
    }
    iplStep() {
      const pi = this.portIn, po = this.portOut;
      // the real start-up program takes a few cycles to notice a new value and read
      // the other ports, so a 16-bit write from the main CPU always lands complete
      if (pi[0] !== this.iplSeen) { this.iplSeen = pi[0]; this.iplWait = 3; return; }
      if (this.iplWait > 0) { this.iplWait--; return; }
      switch (this.iplState) {
        case 0: // set up and announce we're ready
          this.ram.fill(0, 0, 0xf0);
          po[0] = 0xaa; po[1] = 0xbb;
          this.iplState = 1;
          return;
        case 1: if (pi[0] === 0xcc) this.iplCommand(); return;
        case 2: if (pi[0] !== 0) return; this.iplIdx = 0; this.iplState = 3; // falls into data
        // eslint-disable-next-line no-fallthrough
        case 3: {
          const p = pi[0];
          if (p === (this.iplIdx & 0xff)) {
            this.ram[(this.iplAddr + this.iplIdx) & 0xffff] = pi[1];
            po[0] = p;
            this.iplIdx++;
          } else if (((this.iplIdx - p) & 0x80) !== 0) this.iplCommand();
          return;
        }
      }
    }
    iplCommand() {
      const pi = this.portIn;
      this.iplAddr = pi[2] | (pi[3] << 8);
      this.portOut[0] = pi[0];
      if (pi[1]) this.iplState = 2;
      else { // run the uploaded program
        this.ipl = false;
        this.spc.setRegs({ pc: this.iplAddr, sp: 0xef, a: 0, x: 0, y: 0, psw: 0x02 });
      }
    }
    // ---- SPC700 memory ----
    read(a) {
      if (a >= 0xf0 && a < 0x100) {
        switch (a) {
          case 0xf2: return this.dspAddr;
          case 0xf3: return this.dsp.read(this.dspAddr & 0x7f);
          case 0xf4: case 0xf5: case 0xf6: case 0xf7: return this.portIn[a - 0xf4];
          case 0xfd: case 0xfe: case 0xff: { const v = this.tOut[a - 0xfd]; this.tOut[a - 0xfd] = 0; return v; }
          case 0xf0: case 0xf1: case 0xfa: case 0xfb: case 0xfc: return 0;
        }
      }
      return this.ram[a];
    }
    write(a, v) {
      if (a >= 0xf0 && a < 0x100) {
        switch (a) {
          case 0xf1:
            for (let t = 0; t < 3; t++) if ((v & (1 << t)) && !(this.control & (1 << t))) { this.tStage[t] = 0; this.tOut[t] = 0; }
            if (v & 0x10) { this.portIn[0] = 0; this.portIn[1] = 0; }
            if (v & 0x20) { this.portIn[2] = 0; this.portIn[3] = 0; }
            this.control = v;
            return;
          case 0xf2: this.dspAddr = v; return;
          case 0xf3: if (this.dspAddr < 0x80) this.dsp.write(this.dspAddr, v); return;
          case 0xf4: case 0xf5: case 0xf6: case 0xf7: this.portOut[a - 0xf4] = v; return;
          case 0xfa: case 0xfb: case 0xfc: this.tTarget[a - 0xfa] = v || 256; return;
        }
      }
      this.ram[a] = v;
    }
    // Run the sound unit to SPC cycle `target`.
    run(target) {
      const spc = this.spc;
      while (this.cycles < target) {
        let n;
        if (this.ipl) { this.iplStep(); n = 4; }
        else {
          if ((this.control & 0x80) && spc.getPC() >= 0xffc0) { this.enterIpl(); continue; }
          n = spc.step();
        }
        this.cycles += n;
        // timers (T0/T1 every 128 cycles, T2 every 16)
        for (let t = 0; t < 3; t++) {
          this.tDiv[t] += n;
          const per = t === 2 ? 16 : 128;
          while (this.tDiv[t] >= per) {
            this.tDiv[t] -= per;
            if (this.control & (1 << t)) {
              if (++this.tStage[t] >= (this.tTarget[t] || 256)) { this.tStage[t] = 0; this.tOut[t] = (this.tOut[t] + 1) & 15; }
            }
          }
        }
        this.sampleDiv += n;
        while (this.sampleDiv >= 32) {
          this.sampleDiv -= 32;
          this.dsp.sample();
          if (this.nSamples < this.samples.length - 2) { this.samples[this.nSamples++] = this.dsp.out[0]; this.samples[this.nSamples++] = this.dsp.out[1]; }
        }
      }
    }
    saveState() {
      return { ram: this.ram.slice(), dsp: this.dsp.saveState(), spc: this.spc.getState(), pin: this.portIn.slice(), pout: this.portOut.slice(),
        tt: this.tTarget.slice(), ts: this.tStage.slice(), to: this.tOut.slice(), td: this.tDiv.slice(),
        m: [this.control, this.dspAddr, this.cycles, this.sampleDiv, this.ipl ? 1 : 0, this.iplState, this.iplAddr, this.iplIdx, this.iplSeen, this.iplWait] };
    }
    loadState(o) {
      this.ram.set(o.ram); this.dsp.loadState(o.dsp); this.spc.setState(o.spc); this.portIn.set(o.pin); this.portOut.set(o.pout);
      this.tTarget.set(o.tt); this.tStage.set(o.ts); this.tOut.set(o.to); this.tDiv.set(o.td);
      [this.control, this.dspAddr, this.cycles, this.sampleDiv] = o.m; this.ipl = !!o.m[4]; [this.iplState, this.iplAddr, this.iplIdx, this.iplSeen, this.iplWait] = o.m.slice(5);
      this.nSamples = 0;
    }
  }

  global.RPSNESParts = { Cart, APU, LINE, HBLANK, NTSC_HZ, PAL_HZ };
})(typeof window !== "undefined" ? window : globalThis);

// ---------------------------------------------------------------------
// Video (PPU)
// ---------------------------------------------------------------------
(function (global) {
  const P = global.RPSNESParts;
  // sprite sizes per OBSEL setting: [small w, small h, large w, large h]
  const OBJ_SIZES = [[8, 8, 16, 16], [8, 8, 32, 32], [8, 8, 64, 64], [16, 16, 32, 32], [16, 16, 64, 64], [32, 32, 64, 64], [16, 32, 32, 64], [16, 32, 32, 32]];
  // BG bits per pixel per mode (0 = no layer)
  const BPP = [[2, 2, 2, 2], [4, 4, 2, 0], [4, 4, 0, 0], [8, 4, 0, 0], [8, 2, 0, 0], [4, 2, 0, 0], [4, 0, 0, 0], [8, 0, 0, 0]];
  // Front-to-back layer order per mode; entries are "Bn,prio" or "S,prio". Higher index = further back.
  const ORDER = {
    0: ["S3", "B0h", "B1h", "S2", "B0l", "B1l", "S1", "B2h", "B3h", "S0", "B2l", "B3l"],
    1: ["S3", "B0h", "B1h", "S2", "B0l", "B1l", "S1", "B2h", "S0", "B2l"],
    "1x": ["B2h", "S3", "B0h", "B1h", "S2", "B0l", "B1l", "S1", "S0", "B2l"],
    2: ["S3", "B0h", "S2", "B1h", "S1", "B0l", "S0", "B1l"],
    7: ["S3", "S2", "S1", "B0h", "B0l", "S0"],
    "7x": ["S3", "S2", "B1h", "S1", "B0h", "B0l", "S0", "B1l"],
  };
  // z tables: zBG[mode key][layer*2+prio], zOBJ[prio]
  function zTable(key) {
    const list = ORDER[key], n = list.length;
    const zb = new Int8Array(8).fill(0), zo = new Int8Array(4);
    list.forEach((e, i) => {
      const z = n - i;
      if (e[0] === "S") zo[+e[1]] = z;
      else zb[(+e[1]) * 2 + (e[2] === "h" ? 1 : 0)] = z;
    });
    return { zb, zo };
  }
  const Z = {};
  for (const k of Object.keys(ORDER)) Z[k] = zTable(k);

  // 5-bit -> 8-bit with brightness
  const BRIGHT = [];
  for (let b = 0; b < 16; b++) { const t = new Uint8Array(32); for (let c = 0; c < 32; c++) t[c] = Math.round(((c << 3) | (c >> 2)) * (b + 1) / 16); BRIGHT.push(t); }

  class PPU {
    constructor(snes) {
      this.snes = snes;
      this.vram = new Uint16Array(0x8000);
      this.cgram = new Uint16Array(256);
      this.oam = new Uint8Array(544);
      this.frame512 = new Uint32Array(512 * 240);
      this.frame256 = new Uint32Array(256 * 240);
      // per-line layer buffers (512 wide for hires): colour | 0x10000 opaque | prio << 17
      this.layer = [0, 1, 2, 3].map(() => new Uint32Array(512));
      this.objBuf = new Uint32Array(256); // colour | 0x10000 | prio << 17 | palHigh << 19
      this.mainC = new Uint16Array(512); this.subC = new Uint16Array(512);
      this.reset();
    }
    reset() {
      this.vram.fill(0); this.cgram.fill(0); this.oam.fill(0);
      const r = this;
      r.inidisp = 0x80; r.obsel = 0; r.oamAddr = 0; r.oamReload = 0; r.oamLatch = 0; r.oamPri = false;
      r.bgmode = 0; r.mosaic = 0; r.bgsc = [0, 0, 0, 0]; r.bgnba = [0, 0, 0, 0];
      r.hofs = [0, 0, 0, 0]; r.vofs = [0, 0, 0, 0]; r.ofsPrev = 0; r.m7Prev = 0;
      r.vmain = 0x80; r.vaddr = 0; r.vlatch = 0;
      r.m7sel = 0; r.m7 = [0, 0, 0, 0, 0, 0]; r.m7hofs = 0; r.m7vofs = 0;
      r.cgaddr = 0; r.cgLatch = -1; r.cgReadHigh = false;
      r.wsel = [0, 0, 0]; r.wh = [0, 0, 0, 0]; r.wbglog = 0; r.wobjlog = 0;
      r.tm = 0; r.ts = 0; r.tmw = 0; r.tsw = 0; r.cgwsel = 0; r.cgadsub = 0; r.fixed = 0; r.setini = 0;
      r.mpy = 0; r.hLatch = 0; r.vLatch = 0; r.hFlip = false; r.vFlip = false; r.latched = false;
      r.timeOver = false; r.rangeOver = false; r.field = 0;
      r.hiresUsed = false;
    }
    // ---------- registers ----------
    vramAddrMapped() {
      const a = this.vaddr;
      switch ((this.vmain >> 2) & 3) {
        case 0: return a & 0x7fff;
        case 1: return ((a & 0xff00) | ((a & 0x1f) << 3) | ((a >> 5) & 7)) & 0x7fff;
        case 2: return ((a & 0xfe00) | ((a & 0x3f) << 3) | ((a >> 6) & 7)) & 0x7fff;
        default: return ((a & 0xfc00) | ((a & 0x7f) << 3) | ((a >> 7) & 7)) & 0x7fff;
      }
    }
    vinc() { const n = [1, 32, 128, 128][this.vmain & 3]; this.vaddr = (this.vaddr + n) & 0xffff; }
    forcedBlankOrVblank() { return (this.inidisp & 0x80) || this.snes.vblank; }
    write(r, v) {
      switch (r) {
        case 0x00: this.inidisp = v; return;
        case 0x01: this.obsel = v; return;
        case 0x02: this.oamReload = (this.oamReload & 0x100) | v; this.oamAddr = (this.oamReload << 1) & 0x3ff; this.oamPri = (this.oamReload & 0x8000) !== 0; return;
        case 0x03: this.oamReload = (this.oamReload & 0xff) | ((v & 1) << 8) | ((v & 0x80) << 8); this.oamAddr = ((this.oamReload & 0x1ff) << 1); this.oamPri = (v & 0x80) !== 0; return;
        case 0x04: {
          const a = this.oamAddr;
          if (a < 0x200) {
            if (!(a & 1)) this.oamLatch = v;
            else { this.oam[a - 1] = this.oamLatch; this.oam[a] = v; }
          } else this.oam[0x200 | (a & 0x1f)] = v;
          this.oamAddr = (a + 1) & 0x3ff;
          return;
        }
        case 0x05: this.bgmode = v; return;
        case 0x06: this.mosaic = v; return;
        case 0x07: case 0x08: case 0x09: case 0x0a: this.bgsc[r - 7] = v; return;
        case 0x0b: this.bgnba[0] = v & 15; this.bgnba[1] = v >> 4; return;
        case 0x0c: this.bgnba[2] = v & 15; this.bgnba[3] = v >> 4; return;
        case 0x0d: case 0x0f: case 0x11: case 0x13: {
          const k = (r - 0x0d) >> 1;
          this.hofs[k] = ((v << 8) | (this.ofsPrev & ~7) | ((this.hofs[k] >> 8) & 7)) & 0x3ff;
          this.ofsPrev = v;
          if (k === 0) { this.m7hofs = ((v << 8) | this.m7Prev) & 0x1fff; this.m7Prev = v; }
          return;
        }
        case 0x0e: case 0x10: case 0x12: case 0x14: {
          const k = (r - 0x0e) >> 1;
          this.vofs[k] = ((v << 8) | this.ofsPrev) & 0x3ff;
          this.ofsPrev = v;
          if (k === 0) { this.m7vofs = ((v << 8) | this.m7Prev) & 0x1fff; this.m7Prev = v; }
          return;
        }
        case 0x15: this.vmain = v; return;
        case 0x16: this.vaddr = (this.vaddr & 0xff00) | v; this.vlatch = this.vram[this.vramAddrMapped()]; return;
        case 0x17: this.vaddr = (this.vaddr & 0xff) | (v << 8); this.vlatch = this.vram[this.vramAddrMapped()]; return;
        case 0x18: { const a = this.vramAddrMapped(); this.vram[a] = (this.vram[a] & 0xff00) | v; if (!(this.vmain & 0x80)) this.vinc(); return; }
        case 0x19: { const a = this.vramAddrMapped(); this.vram[a] = (this.vram[a] & 0xff) | (v << 8); if (this.vmain & 0x80) this.vinc(); return; }
        case 0x1a: this.m7sel = v; return;
        case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20: {
          const k = r - 0x1b;
          this.m7[k] = ((v << 8) | this.m7Prev) & 0xffff;
          this.m7Prev = v;
          if (k <= 1) { const a16 = (this.m7[0] << 16) >> 16, b8 = (this.m7[1] << 16) >> 24; this.mpy = (a16 * b8) & 0xffffff; }
          return;
        }
        case 0x21: this.cgaddr = v; this.cgLatch = -1; this.cgReadHigh = false; return;
        case 0x22:
          if (this.cgLatch < 0) this.cgLatch = v;
          else { this.cgram[this.cgaddr] = (this.cgLatch | (v << 8)) & 0x7fff; this.cgaddr = (this.cgaddr + 1) & 0xff; this.cgLatch = -1; }
          return;
        case 0x23: this.wsel[0] = v; return;
        case 0x24: this.wsel[1] = v; return;
        case 0x25: this.wsel[2] = v; return;
        case 0x26: case 0x27: case 0x28: case 0x29: this.wh[r - 0x26] = v; return;
        case 0x2a: this.wbglog = v; return;
        case 0x2b: this.wobjlog = v; return;
        case 0x2c: this.tm = v; return;
        case 0x2d: this.ts = v; return;
        case 0x2e: this.tmw = v; return;
        case 0x2f: this.tsw = v; return;
        case 0x30: this.cgwsel = v; return;
        case 0x31: this.cgadsub = v; return;
        case 0x32: {
          const c = v & 31;
          if (v & 0x20) this.fixed = (this.fixed & ~0x1f) | c;
          if (v & 0x40) this.fixed = (this.fixed & ~0x3e0) | (c << 5);
          if (v & 0x80) this.fixed = (this.fixed & ~0x7c00) | (c << 10);
          return;
        }
        case 0x33: this.setini = v; return;
      }
    }
    read(r) {
      switch (r) {
        case 0x34: return this.mpy & 0xff;
        case 0x35: return (this.mpy >> 8) & 0xff;
        case 0x36: return (this.mpy >> 16) & 0xff;
        case 0x37: this.latchCounters(); return this.snes.mdr;
        case 0x38: {
          const a = this.oamAddr;
          const v = a < 0x200 ? this.oam[a] : this.oam[0x200 | (a & 0x1f)];
          this.oamAddr = (a + 1) & 0x3ff;
          return v;
        }
        case 0x39: { const v = this.vlatch & 0xff; if (!(this.vmain & 0x80)) { this.vlatch = this.vram[this.vramAddrMapped()]; this.vinc(); } return v; }
        case 0x3a: { const v = this.vlatch >> 8; if (this.vmain & 0x80) { this.vlatch = this.vram[this.vramAddrMapped()]; this.vinc(); } return v; }
        case 0x3b: {
          const c = this.cgram[this.cgaddr];
          if (!this.cgReadHigh) { this.cgReadHigh = true; return c & 0xff; }
          this.cgReadHigh = false; this.cgaddr = (this.cgaddr + 1) & 0xff; return (c >> 8) & 0x7f;
        }
        case 0x3c: { const v = this.hFlip ? (this.hLatch >> 8) & 1 : this.hLatch & 0xff; this.hFlip = !this.hFlip; return v; }
        case 0x3d: { const v = this.vFlip ? (this.vLatch >> 8) & 1 : this.vLatch & 0xff; this.vFlip = !this.vFlip; return v; }
        case 0x3e: return (this.timeOver ? 0x80 : 0) | (this.rangeOver ? 0x40 : 0) | 1;
        case 0x3f: {
          const v = (this.field << 7) | (this.latched ? 0x40 : 0) | (this.snes.pal ? 0x10 : 0) | 3;
          this.hFlip = false; this.vFlip = false; this.latched = false;
          return v;
        }
      }
      return this.snes.mdr;
    }
    latchCounters() {
      const s = this.snes;
      this.hLatch = Math.min(339, Math.floor((s.clock - s.lineStart) / 4));
      this.vLatch = s.line;
      this.latched = true;
    }
    vblankStart() {
      if (!(this.inidisp & 0x80)) { this.oamAddr = (this.oamReload << 1) & 0x3ff; }
    }

    // ---------- windows ----------
    winMask(sel, logic, out) {
      // sel: 4 bits (w1 invert, w1 enable, w2 invert, w2 enable); fills out[0..255] with 1 where the window applies
      const e1 = sel & 2, e2 = sel & 8;
      if (!e1 && !e2) { out.fill(0, 0, 256); return false; }
      const l1 = this.wh[0], r1 = this.wh[1], l2 = this.wh[2], r2 = this.wh[3];
      const i1 = sel & 1, i2 = sel & 4;
      for (let x = 0; x < 256; x++) {
        let a = e1 ? ((x >= l1 && x <= r1) ? 1 : 0) ^ (i1 ? 1 : 0) : -1;
        let b = e2 ? ((x >= l2 && x <= r2) ? 1 : 0) ^ (i2 ? 1 : 0) : -1;
        let w;
        if (a < 0) w = b; else if (b < 0) w = a;
        else switch (logic) { case 0: w = a | b; break; case 1: w = a & b; break; case 2: w = a ^ b; break; default: w = (a ^ b) ^ 1; }
        out[x] = w;
      }
      return true;
    }

    // ---------- background layers ----------
    renderBG(k, bpp, y, hires, buf, opt) {
      const vram = this.vram, cg = this.cgram;
      const big = (this.bgmode >> (4 + k)) & 1;
      const tw = hires ? 16 : big ? 16 : 8, th = big ? 16 : 8;
      const sc = this.bgsc[k], mapBase = (sc & 0xfc) << 8, wide = sc & 1, tall = sc & 2;
      const chrBase = this.bgnba[k] << 12;
      const words = bpp * 4; // words per 8x8 tile
      const W = hires ? 512 : 256;
      const mos = (this.mosaic >> k) & 1 ? (this.mosaic >> 4) + 1 : 1;
      let ly = y;
      if (mos > 1) ly = y - ((y - 1) % mos);
      const palBase = this.bgmode === 0 ? k * 32 : 0;
      const direct = bpp === 8 && (this.cgwsel & 1);
      const hofsBase = this.hofs[k], vofsBase = this.vofs[k];
      const mapWpx = (wide ? 64 : 32) * tw, mapHpx = (tall ? 64 : 32) * th;
      let lastCell = -1, rowBits = null, attr = 0, plane = 0;
      const pix = new Uint8Array(16);
      for (let sx = 0; sx < W; sx++) {
        const x = mos > 1 ? sx - (sx % (hires ? mos * 2 : mos)) : sx;
        let hofs = hofsBase, vofs = vofsBase;
        if (opt) { const o = opt(k, hires ? x >> 1 : x); if (o) { if (o.h !== undefined) hofs = o.h; if (o.v !== undefined) vofs = o.v; } }
        const px = ((hires ? hofs * 2 : hofs) + x) & (mapWpx - 1);
        const py = (vofs + ly) & (mapHpx - 1);
        const cy = Math.floor(py / th);
        const tx = hires ? (px >> 4) : Math.floor(px / tw);
        const cellKey = (tx << 8) | cy | (vofs << 20);
        if (cellKey !== lastCell) {
          lastCell = cellKey;
          let mx = tx, my = cy;
          let ma = mapBase + ((my & 31) << 5) + (mx & 31);
          if (mx & 32 && wide) ma += 0x400;
          if (my & 32 && tall) ma += wide ? 0x800 : 0x400;
          attr = vram[ma & 0x7fff];
          // decode the (up to 16 pixel wide) row of this tile
          const vf = attr & 0x8000, hf = attr & 0x4000;
          let row = py % th; if (vf) row = th - 1 - row;
          let tile = attr & 0x3ff;
          if (th === 16 && row >= 8) tile += 16;
          const tileW = hires || big ? 16 : 8;
          for (let half = 0; half < tileW / 8; half++) {
            let t = tile + (tileW === 16 ? (hf ? 1 - half : half) : 0);
            const base = (chrBase + (t & 0x3ff) * words + (row & 7)) & 0x7fff;
            const p01 = vram[base], p23 = bpp >= 4 ? vram[(base + 8) & 0x7fff] : 0;
            const p45 = bpp === 8 ? vram[(base + 16) & 0x7fff] : 0, p67 = bpp === 8 ? vram[(base + 24) & 0x7fff] : 0;
            for (let c = 0; c < 8; c++) {
              const bit = hf ? c : 7 - c;
              let v = ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1);
              if (bpp >= 4) v |= (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3);
              if (bpp === 8) v |= (((p45 >> bit) & 1) << 4) | (((p45 >> (bit + 8)) & 1) << 5) | (((p67 >> bit) & 1) << 6) | (((p67 >> (bit + 8)) & 1) << 7);
              pix[half * 8 + c] = v;
            }
          }
          rowBits = pix; plane = tileW;
        }
        const col = (hires ? px : px) % (plane === 16 ? 16 : 8);
        const v = rowBits[hires ? (px & 15) : (tw === 16 ? px & 15 : px & 7)];
        void col;
        if (!v) { buf[sx] = 0; continue; }
        const pal = (attr >> 10) & 7, prio = (attr >> 13) & 1;
        let color;
        if (direct) color = ((v & 7) << 2) | ((pal & 1) << 1) | ((((v >> 3) & 7) << 2 | (pal & 2)) << 5) | ((((v >> 6) & 3) << 3 | (pal & 4)) << 10);
        else color = cg[(palBase + (bpp === 2 ? pal * 4 : bpp === 4 ? pal * 16 : 0) + v) & 0xff];
        buf[sx] = color | 0x10000 | (prio << 17);
      }
    }
    // offset-per-tile lookups for modes 2, 4 and 6
    makeOpt() {
      const mode = this.bgmode & 7;
      if (mode !== 2 && mode !== 4 && mode !== 6) return null;
      const vram = this.vram, sc = this.bgsc[2], mapBase = (sc & 0xfc) << 8;
      const h3 = this.hofs[2], v3 = this.vofs[2];
      const self = this;
      return (k, x) => {
        const col = ((x + (self.hofs[k] & 7)) >> 3) - 1;
        if (col < 0) return null;
        const tx = ((h3 >> 3) + col) & 63, ty = (v3 >> 3) & 63;
        const addr = (row) => { let a = mapBase + (((ty + row) & 31) << 5) + (tx & 31); if ((tx & 32) && (sc & 1)) a += 0x400; return a & 0x7fff; };
        const bit = k === 0 ? 0x2000 : 0x4000;
        const out = {};
        if (mode === 4) {
          const e = vram[addr(0)];
          if (e & bit) { if (e & 0x8000) out.v = e & 0x3ff; else out.h = (e & 0x3f8) | (self.hofs[k] & 7); }
        } else {
          const eh = vram[addr(0)], ev = vram[addr(1)];
          if (eh & bit) out.h = (eh & 0x3f8) | (self.hofs[k] & 7);
          if (ev & bit) out.v = ev & 0x3ff;
        }
        return out;
      };
    }
    renderMode7(y, buf, ext) {
      const vram = this.vram, cg = this.cgram;
      const sel = this.m7sel;
      const s13 = (v) => (v << 19) >> 19;
      const A = (this.m7[0] << 16) >> 16, B = (this.m7[1] << 16) >> 16, C = (this.m7[2] << 16) >> 16, D = (this.m7[3] << 16) >> 16;
      const X0 = s13(this.m7[4]), Y0 = s13(this.m7[5]);
      const H = s13(this.m7hofs), V = s13(this.m7vofs);
      const clip = (v) => (v & 0x2000 ? v | ~0x3ff : v & 0x3ff);
      const mos = (this.mosaic & 1) ? (this.mosaic >> 4) + 1 : 1;
      let ly = mos > 1 ? y - ((y - 1) % mos) : y;
      const sy = sel & 2 ? 255 - ly : ly;
      const hx = clip(H - X0), vy = clip(V - Y0);
      let bx = ((A * hx) & ~63) + ((B * vy) & ~63) + ((B * sy) & ~63) + (X0 << 8);
      let by = ((C * hx) & ~63) + ((D * vy) & ~63) + ((D * sy) & ~63) + (Y0 << 8);
      const direct = this.cgwsel & 1;
      for (let sx = 0; sx < 256; sx++) {
        const x0 = mos > 1 ? sx - (sx % mos) : sx;
        const x = sel & 1 ? 255 - x0 : x0;
        let px = (bx + A * x) >> 8, py = (by + C * x) >> 8;
        let tile;
        const out = px < 0 || py < 0 || px > 1023 || py > 1023;
        if (out && (sel & 0x80)) {
          if (sel & 0x40) tile = 0; else { buf[sx] = 0; continue; }
          px &= 7; py &= 7;
        } else { px &= 1023; py &= 1023; tile = vram[((py >> 3) << 7) + (px >> 3)] & 0xff; }
        const v = vram[(tile << 6) + ((py & 7) << 3) + (px & 7)] >> 8;
        if (ext) {
          if (!(v & 0x7f)) { buf[sx] = 0; continue; }
          buf[sx] = cg[v & 0x7f] | 0x10000 | ((v >> 7) << 17);
        } else {
          if (!v) { buf[sx] = 0; continue; }
          const color = direct ? ((v & 7) << 2) | ((((v >> 3) & 7) << 2) << 5) | ((((v >> 6) & 3) << 3) << 10) : cg[v];
          buf[sx] = color | 0x10000;
        }
      }
    }
    // ---------- sprites ----------
    renderOBJ(y) {
      const oam = this.oam, vram = this.vram, cg = this.cgram, buf = this.objBuf;
      buf.fill(0);
      const sz = OBJ_SIZES[this.obsel >> 5];
      const base = (this.obsel & 7) << 13, gap = (((this.obsel >> 3) & 3) + 1) << 12;
      const first = this.oamPri ? (this.oamAddr >> 2) & 0x7f : 0;
      const list = [];
      for (let n = 0; n < 128; n++) {
        const i = (first + n) & 127;
        const hi = oam[0x200 + (i >> 2)] >> ((i & 3) * 2);
        const large = hi & 2;
        const w = large ? sz[2] : sz[0], h = large ? sz[3] : sz[1];
        let x = oam[i * 4] | ((hi & 1) << 8); if (x >= 256) x -= 512;
        const sy = oam[i * 4 + 1];
        const row = (y - sy) & 0xff;
        if (row >= h) continue;
        if (x <= -w || x >= 256) continue;
        if (list.length === 32) { this.rangeOver = true; break; }
        list.push(i);
      }
      let tiles = 0;
      for (let li = list.length - 1; li >= 0; li--) {
        const i = list[li];
        const hi = oam[0x200 + (i >> 2)] >> ((i & 3) * 2);
        const large = hi & 2;
        const w = large ? sz[2] : sz[0], h = large ? sz[3] : sz[1];
        let x = oam[i * 4] | ((hi & 1) << 8); if (x >= 256) x -= 512;
        const attr = oam[i * 4 + 3], t = oam[i * 4 + 2] | ((attr & 1) << 8);
        let row = (y - oam[i * 4 + 1]) & 0xff;
        if (attr & 0x80) row = h - 1 - row;
        const pal = (attr >> 1) & 7, prio = (attr >> 4) & 3, hf = attr & 0x40;
        const cy = row >> 3;
        for (let c = 0; c < w / 8; c++) {
          const sx = x + c * 8;
          if (sx <= -8 || sx >= 256) continue;
          if (++tiles > 34) { this.timeOver = true; return; }
          const cx = hf ? w / 8 - 1 - c : c;
          const tn = ((((t >> 4) + cy) & 15) << 4) | ((t + cx) & 15);
          const addr = (base + ((t & 0x100) ? gap : 0) + tn * 16 + (row & 7)) & 0x7fff;
          const p01 = vram[addr], p23 = vram[(addr + 8) & 0x7fff];
          for (let p = 0; p < 8; p++) {
            const xx = sx + p;
            if (xx < 0 || xx >= 256) continue;
            const bit = hf ? p : 7 - p;
            const v = ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1) | (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3);
            if (!v) continue;
            buf[xx] = cg[128 + pal * 16 + v] | 0x10000 | (prio << 17) | ((pal >= 4 ? 1 : 0) << 19);
          }
        }
      }
    }

    // ---------- compose one line ----------
    renderLine(y) {
      const outY = y - 1;
      if (outY < 0 || outY >= 240) return;
      const f512 = this.frame512, f256 = this.frame256;
      if (this.inidisp & 0x80) {
        f512.fill(0xff000000, outY * 512, outY * 512 + 512); f256.fill(0xff000000, outY * 256, outY * 256 + 256);
        return;
      }
      const mode = this.bgmode & 7;
      const hires = mode === 5 || mode === 6 || (this.setini & 8);
      const W = mode === 5 || mode === 6 ? 512 : 256;
      if (hires) this.hiresUsed = true;
      const bpp = BPP[mode];
      const layers = this.tm | this.ts;
      const opt = this.makeOpt();
      const L = this.layer;
      for (let k = 0; k < 4; k++) {
        if (!(layers & (1 << k))) continue;
        if (mode === 7) {
          if (k === 0) this.renderMode7(y, L[0], false);
          else if (k === 1 && (this.setini & 0x40)) this.renderMode7(y, L[1], true);
          continue;
        }
        if (!bpp[k]) continue;
        this.renderBG(k, bpp[k], y, W === 512, L[k], k < 2 ? opt : null);
      }
      if (layers & 16) this.renderOBJ(y); else this.objBuf.fill(0);
      // z order
      const key = mode === 1 ? ((this.bgmode & 8) ? "1x" : 1) : mode === 7 ? ((this.setini & 0x40) ? "7x" : 7) : mode === 0 ? 0 : 2;
      const zt = Z[key], zb = zt.zb, zo = zt.zo;
      const valid = mode === 7 ? ((this.setini & 0x40) ? 3 : 1) : bpp.reduce((m, b, i) => m | (b ? 1 << i : 0), 0);
      // windows
      const wins = this._wins || (this._wins = [0, 1, 2, 3, 4, 5].map(() => new Uint8Array(256)));
      const sel = [this.wsel[0] & 15, this.wsel[0] >> 4, this.wsel[1] & 15, this.wsel[1] >> 4, this.wsel[2] & 15, this.wsel[2] >> 4];
      const logic = [this.wbglog & 3, (this.wbglog >> 2) & 3, (this.wbglog >> 4) & 3, (this.wbglog >> 6) & 3, this.wobjlog & 3, (this.wobjlog >> 2) & 3];
      for (let k = 0; k < 6; k++) this.winMask(sel[k], logic[k], wins[k]);
      const cw = wins[5];
      const back = this.cgram[0];
      const obj = this.objBuf;
      const pickScreen = (enable, winEn, x, xs, out) => {
        // returns layer id (0-3 BG, 4 OBJ, 5 backdrop) and writes colour to out
        let bestZ = 0, bestL = 5, bestC = back, bestPH = 0;
        for (let k = 0; k < 4; k++) {
          if (!(enable & (1 << k)) || !(valid & (1 << k))) continue;
          if ((winEn & (1 << k)) && wins[k][xs]) continue;
          const v = L[k][x];
          if (!(v & 0x10000)) continue;
          const z = zb[k * 2 + ((v >> 17) & 1)];
          if (z > bestZ) { bestZ = z; bestL = k; bestC = v & 0x7fff; }
        }
        if (enable & 16) {
          const v = obj[xs];
          if ((v & 0x10000) && !((winEn & 16) && wins[4][xs])) {
            const z = zo[(v >> 17) & 3];
            if (z > bestZ) { bestZ = z; bestL = 4; bestC = v & 0x7fff; bestPH = (v >> 19) & 1; }
          }
        }
        out.c = bestC; out.ph = bestPH;
        return bestL;
      };
      const m = { c: 0, ph: 0 }, s = { c: 0, ph: 0 };
      const cgw = this.cgwsel, cga = this.cgadsub;
      const clipMode = (cgw >> 6) & 3, mathMode = (cgw >> 4) & 3, useSub = cgw & 2;
      const sub = cga & 0x80, half = cga & 0x40;
      const bright = BRIGHT[this.inidisp & 15];
      const o512 = outY * 512, o256 = outY * 256;
      for (let sx = 0; sx < W; sx++) {
        const xs = W === 512 ? sx >> 1 : sx;
        const ml = pickScreen(this.tm, this.tmw, sx, xs, m);
        let mc = m.c;
        const inCw = cw[xs];
        const black = clipMode === 3 || (clipMode === 1 && !inCw) || (clipMode === 2 && inCw);
        if (black) mc = 0;
        const mathOk = mathMode === 0 || (mathMode === 1 && inCw) || (mathMode === 2 && !inCw);
        const layerMath = ml === 4 ? ((cga & 16) && m.ph) : (cga & (1 << ml));
        let c = mc;
        if (mathOk && layerMath) {
          let oc, halfOk = half;
          if (useSub) {
            const sl = pickScreen(this.ts, this.tsw, sx, xs, s);
            if (sl === 5) { oc = this.fixed; halfOk = 0; } else oc = s.c;
          } else oc = this.fixed;
          let r = mc & 31, g = (mc >> 5) & 31, b = (mc >> 10) & 31;
          const r2 = oc & 31, g2 = (oc >> 5) & 31, b2 = (oc >> 10) & 31;
          if (sub) { r -= r2; g -= g2; b -= b2; if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0; }
          else { r += r2; g += g2; b += b2; }
          if (halfOk && !black) { r >>= 1; g >>= 1; b >>= 1; }
          if (r > 31) r = 31; if (g > 31) g = 31; if (b > 31) b = 31;
          c = r | (g << 5) | (b << 10);
        }
        const px = 0xff000000 | (bright[(c >> 10) & 31] << 16) | (bright[(c >> 5) & 31] << 8) | bright[c & 31];
        if (W === 512) { f512[o512 + sx] = px; if (!(sx & 1)) f256[o256 + (sx >> 1)] = px; }
        else { f512[o512 + sx * 2] = px; f512[o512 + sx * 2 + 1] = px; f256[o256 + sx] = px; }
      }
    }
    saveState() {
      const o = { vram: this.vram.slice(), cgram: this.cgram.slice(), oam: this.oam.slice() };
      for (const k of ["inidisp", "obsel", "oamAddr", "oamReload", "oamLatch", "oamPri", "bgmode", "mosaic", "ofsPrev", "m7Prev", "vmain", "vaddr", "vlatch", "m7sel",
        "m7hofs", "m7vofs", "cgaddr", "cgLatch", "cgReadHigh", "wbglog", "wobjlog", "tm", "ts", "tmw", "tsw", "cgwsel", "cgadsub", "fixed", "setini", "mpy",
        "hLatch", "vLatch", "hFlip", "vFlip", "latched", "timeOver", "rangeOver", "field"]) o[k] = this[k];
      for (const k of ["bgsc", "bgnba", "hofs", "vofs", "m7", "wsel", "wh"]) o[k] = this[k].slice();
      return o;
    }
    loadState(o) {
      this.vram.set(o.vram); this.cgram.set(o.cgram); this.oam.set(o.oam);
      for (const k in o) if (!ArrayBuffer.isView(o[k])) this[k] = Array.isArray(o[k]) ? o[k].slice() : o[k];
    }
  }
  P.PPU = PPU;
})(typeof window !== "undefined" ? window : globalThis);

// ---------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------
(function (global) {
  const P = global.RPSNESParts;
  const { Cart, APU, PPU, LINE, HBLANK, NTSC_HZ, PAL_HZ } = P;
  const PATTERN = [[0], [0, 1], [0, 0], [0, 0, 1, 1], [0, 1, 2, 3], [0, 1, 0, 1], [0, 0], [0, 0, 1, 1]];
  const BUTTONS = ["R", "L", "X", "A", "Right", "Left", "Down", "Up", "Start", "Select", "Y", "B"];

  class SNES {
    constructor(romIn, opts) {
      opts = opts || {};
      this.cart = new Cart(romIn, opts.sram);
      const chip = this.cart.special();
      // [chips] cartridge coprocessor from the snes_*.js files, mapped over the
      // 4 KB pages it claims in chipPages (bank << 4 | address >> 12)
      this.chipPages = new Uint8Array(4096);
      const known = chip || !global.RPSNESChips ? null : global.RPSNESChips.find((e) => e.detect(this.cart));
      this.coproc = known && known.create ? known.create(this, opts) : null;
      if (chip && chip !== "SA-1" && !(chip === "Super FX" && global.RPSuperFX)) throw new Error(`This game uses the ${chip} chip inside the cartridge, which RetroPixels' SNES emulator doesn't support yet.`);
      this.chip = chip;
      this.gsu = null; // [Super FX] see snes_superfx.js
      this.pal = this.cart.pal;
      this.masterHz = this.pal ? PAL_HZ : NTSC_HZ;
      this.lines = this.pal ? 312 : 262;
      this.fps = this.masterHz / (LINE * this.lines);
      this.spcRatio = 1024000 / this.masterHz;
      this.sampleRate = opts.sampleRate || 48000;
      this.wram = new Uint8Array(0x20000);
      this.apu = new APU();
      this.ppu = new PPU(this);
      const self = this;
      this.cpu = new global.RP65816({ read: (a) => self.read(a), write: (a, v) => self.write(a, v), idle: () => { self.clock += 6; } });
      this.clock = 0;
      // SA-1 chip (snes_sa1.js): takes over the cartridge bus and runs alongside the CPU
      this.sa1 = chip === "SA-1" ? new global.RPSA1(this) : null;
      this.dma = new Uint8Array(0x80);
      this.hdma = [0, 1, 2, 3, 4, 5, 6, 7].map(() => ({ active: false, doTransfer: false, line: 0, addr: 0, ind: 0 }));
      this.input = [0, 0];
      this.width = 256; this.height = 224;
      this.screen = this.ppu.frame256;
      this.out = new Float32Array(8192);
      this.clock = 0;
      if (chip === "Super FX") this.gsu = new global.RPSuperFX(this.cart, this, opts.sram); // [Super FX]
      this.reset();
    }
    reset() {
      this.wram.fill(0x55);
      this.apu.reset(); this.ppu.reset();
      this.dma.fill(0xff);
      this.clock = 0; this.line = 0; this.lineStart = 0; this.frameStart = 0;
      this.vblank = false; this.hblank = false; this.mdr = 0;
      this.nmitimen = 0; this.wrio = 0xff; this.wrmpya = 0xff; this.wrdiv = 0xffff; this.htime = 0x1ff; this.vtime = 0x1ff;
      this.memsel = 0; this.hdmaen = 0; this.nmiFlag = false; this.irqFlag = false; this.autoJoyUntil = 0;
      this.rddiv = 0; this.rdmpy = 0; this.joy = new Uint16Array(4); this.ser = [0, 0]; this.strobe = 0;
      this.wmadd = 0;
      this.rsPos = 0; this.prevL = 0; this.prevR = 0;
      if (this.sa1) this.sa1.reset(); // SA-1 chip
      if (this.coproc) this.coproc.reset(); // [chips]
      this.cpu.reset();
      if (this.gsu) this.gsu.reset(); // [Super FX]
    }
    updateIrq() { this.setIrqLine(); }

    // ---------- bus ----------
    decode(addr, write, v) {
      // returns value (read) ; sets this.cost
      const bank = addr >>> 16, off = addr & 0xffff;
      if (this.chipPages[addr >>> 12]) { // [chips] coprocessor registers / memory
        const r = write ? (this.coproc.write(bank, off, v) ? 0 : -1) : this.coproc.read(bank, off);
        if (r >= 0) { this.cost = (bank & 0x80) && (off & 0x8000 || bank >= 0xc0) && (this.memsel & 1) ? 6 : 8; return r; }
      }
      if (!(bank & 0x40)) {
        if (off < 0x2000) { this.cost = 8; if (write) { this.wram[off] = v; return 0; } return this.wram[off]; }
        if (off < 0x4000) {
          this.cost = 6;
          if (this.gsu && off >= 0x3000 && off < 0x3500) { if (write) { this.gsu.regWrite(off, v); return 0; } return this.gsu.regRead(off); } // [Super FX]
          if (off >= 0x2100 && off < 0x2200) { if (write) { this.writeB(off & 0xff, v); return 0; } return this.readB(off & 0xff); }
          if (this.sa1 && off >= 0x2200) return this.sa1.snesIO(off, write, v); // SA-1 chip registers and I-RAM
          return write ? 0 : this.mdr;
        }
        if (off < 0x4200) { this.cost = 12; if (write) { if (off === 0x4016) this.writeStrobe(v); return 0; } return this.readSerial(off); }
        if (off < 0x6000) { this.cost = 6; if (write) { this.writeIO(off, v); return 0; } return this.readIO(off); }
        this.cost = off >= 0x8000 && (bank & 0x80) && (this.memsel & 1) ? 6 : 8;
      } else if (bank === 0x7e || bank === 0x7f) {
        this.cost = 8;
        const a = ((bank & 1) << 16) | off;
        if (write) { this.wram[a] = v; return 0; }
        return this.wram[a];
      } else this.cost = bank >= 0xc0 && (this.memsel & 1) ? 6 : 8;
      if (this.gsu) { // [Super FX] cartridge ROM / RAM through the chip
        if (write) { this.gsu.snesWrite(bank, off, v); return 0; }
        const g = this.gsu.snesRead(bank, off);
        return g < 0 ? this.mdr : g;
      }
      if (write) { this.cart.write(bank, off, v); return 0; }
      const r = this.cart.read(bank, off);
      return r < 0 ? this.mdr : r;
    }
    read(addr) { const v = this.decode(addr, false, 0); this.clock += this.cost; this.mdr = v; return v; }
    write(addr, v) { this.clock += 0; this.decode(addr, true, v); this.clock += this.cost; this.mdr = v; }
    // A-bus access used by DMA (timing is counted by the DMA itself)
    peekA(addr) { const bank = addr >>> 16, off = addr & 0xffff; if (!(bank & 0x40) && ((off >= 0x2100 && off < 0x2200) || (off >= 0x4000 && off < 0x4400))) return this.mdr; return this.decode(addr, false, 0); }
    pokeA(addr, v) { const bank = addr >>> 16, off = addr & 0xffff; if (!(bank & 0x40) && ((off >= 0x2100 && off < 0x2200) || (off >= 0x4000 && off < 0x4400))) return; this.decode(addr, true, v); }

    syncApu() { this.apu.run(Math.floor(this.clock * this.spcRatio)); }
    readB(r) {
      if (r >= 0x34 && r < 0x40) return this.ppu.read(r);
      if (r >= 0x40 && r < 0x80) { this.syncApu(); return this.apu.portOut[r & 3]; }
      if (r === 0x80) { const v = this.wram[this.wmadd]; this.wmadd = (this.wmadd + 1) & 0x1ffff; return v; }
      return this.mdr;
    }
    writeB(r, v) {
      if (r < 0x34) { this.ppu.write(r, v); return; }
      if (r >= 0x40 && r < 0x80) { this.syncApu(); this.apu.portIn[r & 3] = v; return; }
      switch (r) {
        case 0x80: this.wram[this.wmadd] = v; this.wmadd = (this.wmadd + 1) & 0x1ffff; return;
        case 0x81: this.wmadd = (this.wmadd & 0x1ff00) | v; return;
        case 0x82: this.wmadd = (this.wmadd & 0x100ff) | (v << 8); return;
        case 0x83: this.wmadd = (this.wmadd & 0xffff) | ((v & 1) << 16); return;
      }
    }
    // ---------- joypads ----------
    writeStrobe(v) {
      const s = v & 1;
      if (this.strobe && !s) { this.ser[0] = this.joyBits(0); this.ser[1] = this.joyBits(1); }
      this.strobe = s;
      if (s) { this.ser[0] = this.joyBits(0); this.ser[1] = this.joyBits(1); }
    }
    readSerial(off) {
      if (off !== 0x4016 && off !== 0x4017) return this.mdr;
      const p = off - 0x4016;
      const bit = (this.ser[p] >> 15) & 1;
      if (!this.strobe) this.ser[p] = ((this.ser[p] << 1) | 1) & 0xffff;
      return (this.mdr & 0xfc) | bit | (p ? 0x1c : 0);
    }
    joyBits(p) { return (this.input[p] << 4) & 0xfff0; }

    // ---------- CPU I/O registers ----------
    readIO(off) {
      if (off >= 0x4300 && off < 0x4380) return this.dma[off - 0x4300];
      switch (off) {
        case 0x4210: { const v = (this.nmiFlag ? 0x80 : 0) | 0x02 | (this.mdr & 0x70); this.nmiFlag = false; return v; }
        case 0x4211: { const v = (this.irqFlag ? 0x80 : 0) | (this.mdr & 0x7f); this.irqFlag = false; this.setIrqLine(); return v; }
        case 0x4212: {
          const pos = this.clock - this.lineStart;
          return (this.vblank ? 0x80 : 0) | (pos >= HBLANK || pos < 4 ? 0x40 : 0) | (this.clock < this.autoJoyUntil ? 1 : 0) | (this.mdr & 0x3e);
        }
        case 0x4213: return this.wrio;
        case 0x4214: return this.rddiv & 0xff;
        case 0x4215: return this.rddiv >> 8;
        case 0x4216: return this.rdmpy & 0xff;
        case 0x4217: return this.rdmpy >> 8;
        case 0x4218: case 0x4219: case 0x421a: case 0x421b: case 0x421c: case 0x421d: case 0x421e: case 0x421f: {
          const k = (off - 0x4218) >> 1;
          return (off & 1) ? this.joy[k] >> 8 : this.joy[k] & 0xff;
        }
      }
      return this.mdr;
    }
    writeIO(off, v) {
      if (off >= 0x4300 && off < 0x4380) { this.dma[off - 0x4300] = v; return; }
      switch (off) {
        case 0x4200: {
          const was = this.nmitimen;
          this.nmitimen = v;
          if (!(was & 0x80) && (v & 0x80) && this.nmiFlag && this.vblank) this.cpu.nmiPending = true;
          if (!(v & 0x30)) { this.irqFlag = false; this.setIrqLine(); }
          return;
        }
        case 0x4201: if ((this.wrio & 0x80) && !(v & 0x80)) this.ppu.latchCounters(); this.wrio = v; return;
        case 0x4202: this.wrmpya = v; return;
        case 0x4203: this.rdmpy = this.wrmpya * v; this.rddiv = v; return;
        case 0x4204: this.wrdiv = (this.wrdiv & 0xff00) | v; return;
        case 0x4205: this.wrdiv = (this.wrdiv & 0xff) | (v << 8); return;
        case 0x4206:
          if (!v) { this.rddiv = 0xffff; this.rdmpy = this.wrdiv; }
          else { this.rddiv = Math.floor(this.wrdiv / v); this.rdmpy = this.wrdiv % v; }
          return;
        case 0x4207: this.htime = (this.htime & 0x100) | v; return;
        case 0x4208: this.htime = (this.htime & 0xff) | ((v & 1) << 8); return;
        case 0x4209: this.vtime = (this.vtime & 0x100) | v; return;
        case 0x420a: this.vtime = (this.vtime & 0xff) | ((v & 1) << 8); return;
        case 0x420b: if (v) this.runDma(v); return;
        case 0x420c: this.hdmaen = v; return;
        case 0x420d: this.memsel = v; return;
      }
    }

    // ---------- DMA ----------
    runDma(mask) {
      this.clock += 8;
      const d = this.dma;
      for (let ch = 0; ch < 8; ch++) {
        if (!(mask & (1 << ch))) continue;
        const o = ch * 16;
        const ctrl = d[o], bbad = d[o + 1];
        let a1t = d[o + 2] | (d[o + 3] << 8);
        const a1b = d[o + 4];
        let count = (d[o + 5] | (d[o + 6] << 8)) || 0x10000;
        const pat = PATTERN[ctrl & 7];
        const step = (ctrl & 8) ? 0 : (ctrl & 0x10) ? -1 : 1;
        this.clock += 8;
        // [chips] a coprocessor may supply the data itself (S-DD1 decompression)
        const feed = this.coproc && this.coproc.dmaFeed && !(ctrl & 0x80) ? this.coproc.dmaFeed(ch, (a1b << 16) | a1t, count) : null;
        let i = 0;
        while (count > 0) {
          const b = (bbad + pat[i % pat.length]) & 0xff;
          const a = (a1b << 16) | a1t;
          if (ctrl & 0x80) this.pokeA(a, this.readB(b));
          else this.writeB(b, feed ? feed() : this.peekA(a));
          this.clock += 8;
          a1t = (a1t + step) & 0xffff;
          count--; i++;
        }
        d[o + 2] = a1t & 0xff; d[o + 3] = a1t >> 8;
        d[o + 5] = 0; d[o + 6] = 0;
      }
    }
    hdmaInit() {
      const d = this.dma;
      for (let ch = 0; ch < 8; ch++) {
        const h = this.hdma[ch];
        h.active = false;
        if (!(this.hdmaen & (1 << ch))) continue;
        const o = ch * 16;
        h.addr = d[o + 2] | (d[o + 3] << 8);
        this.hdmaLoad(ch);
        this.clock += 8;
      }
    }
    hdmaLoad(ch) {
      const d = this.dma, o = ch * 16, h = this.hdma[ch];
      const bank = d[o + 4] << 16;
      h.line = this.peekA(bank | h.addr); h.addr = (h.addr + 1) & 0xffff;
      if (d[o] & 0x40) { // indirect: next word is the data address
        const lo = this.peekA(bank | h.addr), hi = this.peekA(bank | ((h.addr + 1) & 0xffff));
        h.addr = (h.addr + 2) & 0xffff;
        d[o + 5] = lo; d[o + 6] = hi;
        this.clock += 16;
      }
      h.active = h.line !== 0;
      h.doTransfer = true;
      d[o + 8] = h.addr & 0xff; d[o + 9] = h.addr >> 8; d[o + 10] = h.line;
    }
    hdmaLine() {
      const d = this.dma;
      let any = false;
      for (let ch = 0; ch < 8; ch++) {
        const h = this.hdma[ch];
        if (!(this.hdmaen & (1 << ch)) || !h.active) continue;
        any = true;
        const o = ch * 16, ctrl = d[o], bbad = d[o + 1];
        if (h.doTransfer) {
          const pat = PATTERN[ctrl & 7];
          for (let i = 0; i < pat.length; i++) {
            let a;
            if (ctrl & 0x40) { const ia = d[o + 5] | (d[o + 6] << 8); a = (d[o + 7] << 16) | ia; const n = (ia + 1) & 0xffff; d[o + 5] = n & 0xff; d[o + 6] = n >> 8; }
            else { a = (d[o + 4] << 16) | h.addr; h.addr = (h.addr + 1) & 0xffff; }
            const b = (bbad + pat[i]) & 0xff;
            if (ctrl & 0x80) this.pokeA(a, this.readB(b));
            else this.writeB(b, this.peekA(a));
            this.clock += 8;
          }
        }
        h.line = (h.line - 1) & 0xff;
        h.doTransfer = (h.line & 0x80) !== 0;
        if (!(h.line & 0x7f)) this.hdmaLoad(ch);
        d[o + 8] = h.addr & 0xff; d[o + 9] = h.addr >> 8; d[o + 10] = h.line;
      }
      if (any) this.clock += 18;
    }

    // ---------- running ----------
    runTo(t) {
      const cpu = this.cpu;
      const sa1 = this.sa1;
      if (sa1) { while (this.clock < t) { cpu.step(); sa1.catchUp(this.clock); } return; } // SA-1 chip runs in step
      if (this.gsu) { // [Super FX] run the chip alongside the CPU in short slices
        while (this.clock < t) { const end = Math.min(t, this.clock + 256); while (this.clock < end) cpu.step(); this.gsu.run(this.clock); }
        return;
      }
      while (this.clock < t) cpu.step();
    }
    fireIrq() { this.irqFlag = true; this.setIrqLine(); }
    // the CPU's IRQ input: the timer IRQ, or a cartridge chip (SA-1, Super FX) asking for attention
    setIrqLine() { this.cpu.irqLine = this.irqFlag || (this.sa1 ? this.sa1.snesIrq : false) || !!(this.gsu && this.gsu.irqOut()); }
    runFrame() {
      const ppu = this.ppu;
      const visible = (ppu.setini & 4) ? 239 : 224;
      const vbl = visible + 1;
      ppu.hiresUsed = false;
      for (let line = 0; line < this.lines; line++) {
        this.line = line;
        this.lineStart = this.frameStart + line * LINE;
        if (this.clock < this.lineStart) this.clock = this.lineStart; // CPU was waiting
        if (line === 0) {
          this.vblank = false; this.nmiFlag = false;
          ppu.timeOver = false; ppu.rangeOver = false;
          ppu.field ^= 1;
          this.hdmaInit();
        }
        if (line === vbl) {
          this.vblank = true; this.nmiFlag = true;
          ppu.vblankStart();
          if (this.nmitimen & 0x80) this.cpu.nmiPending = true;
          if (this.nmitimen & 1) {
            for (let p = 0; p < 2; p++) this.joy[p] = this.joyBits(p);
            this.autoJoyUntil = this.clock + 4224;
          }
        }
        this.clock += 40; // memory refresh
        // timer interrupt position in this line
        const mode = (this.nmitimen >> 4) & 3;
        let irqAt = -1;
        if (mode === 1) irqAt = this.htime * 4 + 14;
        else if (mode === 2 && line === this.vtime) irqAt = 10;
        else if (mode === 3 && line === this.vtime) irqAt = this.htime * 4 + 14;
        if (irqAt >= 0 && irqAt < HBLANK) { this.runTo(this.lineStart + irqAt); this.fireIrq(); }
        this.runTo(this.lineStart + HBLANK);
        this.hblank = true;
        if (line >= 1 && line <= visible) ppu.renderLine(line);
        if (line < vbl) this.hdmaLine();
        if (irqAt >= HBLANK) { this.runTo(this.lineStart + irqAt); this.fireIrq(); }
        this.runTo(this.lineStart + LINE);
        this.hblank = false;
      }
      this.frameStart += LINE * this.lines;
      this.syncApu();
      this.height = visible;
      if (ppu.hiresUsed) { this.width = 512; this.screen = ppu.frame512; } else { this.width = 256; this.screen = ppu.frame256; }
    }
    setInput(p, bits) { if (p < 2) this.input[p] = bits & 0xfff; }
    takeAudio() {
      const a = this.apu, n = a.nSamples >> 1, src = a.samples;
      const ratio = 32040 / this.sampleRate;
      const maxOut = Math.ceil(n / ratio) + 2;
      if (this.out.length < maxOut * 2) this.out = new Float32Array(maxOut * 2);
      const out = this.out;
      let k = 0, pos = this.rsPos;
      while (pos < n) {
        const i = Math.floor(pos), f = pos - i;
        const l0 = i === 0 ? this.prevL : src[(i - 1) * 2], r0 = i === 0 ? this.prevR : src[(i - 1) * 2 + 1];
        out[k++] = (l0 + (src[i * 2] - l0) * f) / 32768;
        out[k++] = (r0 + (src[i * 2 + 1] - r0) * f) / 32768;
        pos += ratio;
      }
      this.rsPos = pos - n;
      if (n) { this.prevL = src[(n - 1) * 2]; this.prevR = src[(n - 1) * 2 + 1]; }
      a.nSamples = 0;
      return out.slice(0, k);
    }
    getSRAM() {
      if (this.gsu) return this.gsu.getSRAM(); // [Super FX]
      if (this.coproc && this.coproc.getSRAM) return this.coproc.getSRAM(); // [chips]
      return this.cart.sramSize ? this.cart.sram.slice(0, this.cart.sramSize) : null;
    }
    saveState() {
      return {
        v: 1, cpu: this.cpu.getState(), wram: this.wram.slice(), apu: this.apu.saveState(), ppu: this.ppu.saveState(), sram: this.cart.sram.slice(),
        dma: this.dma.slice(), hdma: this.hdma.map((h) => Object.assign({}, h)), joy: this.joy.slice(),
        gsu: this.gsu ? this.gsu.saveState() : undefined, // [Super FX]
        chip: this.coproc ? this.coproc.saveState() : null, // [chips]
        m: [this.clock, this.frameStart, this.mdr, this.nmitimen, this.wrio, this.wrmpya, this.wrdiv, this.htime, this.vtime, this.memsel, this.hdmaen,
          this.nmiFlag ? 1 : 0, this.irqFlag ? 1 : 0, this.autoJoyUntil, this.rddiv, this.rdmpy, this.ser[0], this.ser[1], this.strobe, this.wmadd, this.rsPos, this.prevL, this.prevR],
        sa1: this.sa1 ? this.sa1.saveState() : null,
      };
    }
    loadState(s) {
      this.cpu.setState(s.cpu); this.wram.set(s.wram); this.apu.loadState(s.apu); this.ppu.loadState(s.ppu); this.cart.sram.set(s.sram);
      this.dma.set(s.dma); s.hdma.forEach((h, i) => Object.assign(this.hdma[i], h)); this.joy.set(s.joy);
      if (this.coproc && s.chip) this.coproc.loadState(s.chip); // [chips]
      const m = s.m;
      [this.clock, this.frameStart, this.mdr, this.nmitimen, this.wrio, this.wrmpya, this.wrdiv, this.htime, this.vtime, this.memsel, this.hdmaen] = m;
      this.nmiFlag = !!m[11]; this.irqFlag = !!m[12];
      if (this.gsu && s.gsu) this.gsu.loadState(s.gsu); // [Super FX]
      [this.autoJoyUntil, this.rddiv, this.rdmpy] = m.slice(13, 16); this.ser = [m[16], m[17]]; [this.strobe, this.wmadd, this.rsPos, this.prevL, this.prevR] = m.slice(18);
      if (this.sa1 && s.sa1) { this.sa1.loadState(s.sa1); this.setIrqLine(); }
    }
  }
  SNES.info = { system: "snes", name: "Super Nintendo", width: 256, height: 224, aspect: 4 / 3, fps: 60.0988, stereo: true, players: 2, buttons: BUTTONS };
  global.RPCores = global.RPCores || {};
  global.RPCores.snes = SNES;
})(typeof window !== "undefined" ? window : globalThis);
