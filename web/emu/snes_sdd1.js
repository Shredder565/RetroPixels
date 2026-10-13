// RetroPixels — SNES S-DD1 cartridge chip (Star Ocean, Street Fighter Alpha 2). Original code.
//
// The S-DD1 maps 1 MB pieces of a large ROM into banks $C0-$FF and, when the
// game asks for it, decompresses graphics on the fly while the SNES copies them
// with DMA. The decompressor below follows the documented algorithm: a
// Golomb-coded bit stream feeds eight run-length "bit generators"; a probability
// estimator with 32 contexts picks a generator per bit; a context model mixes
// in the bits already decoded on the same bitplane; the output stage packs the
// bits into 2, 4 or 8 bitplane tiles or Mode 7 pixels.
//
// Registers ($00-$3F/$80-$BF:$4800-$4807):
//   $4800  channels that decompress     $4801  channels armed for their next DMA
//   $4804-$4807  1 MB ROM piece shown in banks $C0-$CF, $D0-$DF, $E0-$EF, $F0-$FF
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;

  // Probability estimator states: [generator (code length), next on MPS, next on LPS]
  const EVOLUTION = [
    [0, 25, 25], [0, 2, 1], [0, 3, 1], [0, 4, 2], [0, 5, 3], [1, 6, 4], [1, 7, 5], [1, 8, 6], [1, 9, 7], [2, 10, 8], [2, 11, 9], [2, 12, 10], [2, 13, 11],
    [3, 14, 12], [3, 15, 13], [3, 16, 14], [3, 17, 15], [4, 18, 16], [4, 19, 17], [5, 20, 18], [5, 21, 19], [6, 22, 20], [6, 23, 21], [7, 24, 22], [7, 24, 23],
    [0, 26, 1], [1, 27, 2], [2, 28, 4], [3, 29, 8], [4, 30, 12], [5, 31, 16], [6, 32, 18], [7, 24, 22],
  ];

  class Decompressor {
    // read(addr) gives a ROM byte by SNES address
    constructor(read) { this.readByte = read; this.mps = new Uint8Array(32); this.state = new Uint8Array(32); this.runMPS = new Int32Array(8); this.runLPS = new Uint8Array(8); this.prev = new Uint16Array(8); }
    start(addr) {
      this.addr = addr;
      const head = this.readByte(addr);
      this.planes = head & 0xc0; this.ctxBits = head & 0x30;
      this.cur = head; this.bitPos = 4; // the stream starts after the 4 header bits
      this.mps.fill(0); this.state.fill(0); this.runMPS.fill(0); this.runLPS.fill(0); this.prev.fill(0);
      this.bitNum = 0; this.plane = 7;
      this.held = -1; // second byte of a bitplane pair waiting to go out
    }
    inBit() {
      const b = (this.cur >> (7 - this.bitPos)) & 1;
      if (++this.bitPos === 8) { this.bitPos = 0; this.addr = (this.addr + 1) & 0xffffff; this.cur = this.readByte(this.addr); }
      return b;
    }
    // one bit from run-length generator n (codes: "0" = 2^n MPS; "1"+n bits = run then an LPS)
    genBit(n) {
      if (!this.runMPS[n] && !this.runLPS[n]) {
        if (!this.inBit()) this.runMPS[n] = 1 << n;
        else {
          let v = 0;
          for (let i = 0; i < n; i++) v |= this.inBit() << i; // bits arrive in reverse order
          this.runMPS[n] = ~v & ((1 << n) - 1);
          this.runLPS[n] = 1;
        }
      }
      let bit;
      if (this.runMPS[n]) { bit = 0; this.runMPS[n]--; } else { bit = 1; this.runLPS[n] = 0; }
      this.endOfRun = !this.runMPS[n] && !this.runLPS[n];
      return bit;
    }
    probBit(ctx) {
      const st = this.state[ctx], mps = this.mps[ctx];
      const bit = this.genBit(EVOLUTION[st][0]);
      if (this.endOfRun) {
        if (bit) { if (st < 2) this.mps[ctx] ^= 1; this.state[ctx] = EVOLUTION[st][2]; }
        else this.state[ctx] = EVOLUTION[st][1];
      }
      return bit ^ mps;
    }
    modelBit() {
      switch (this.planes) {
        case 0x00: this.plane ^= 1; break;
        case 0x40: this.plane ^= 1; if (!(this.bitNum & 0x7f)) this.plane = (this.plane + 2) & 7; break;
        case 0x80: this.plane ^= 1; if (!(this.bitNum & 0x7f)) this.plane ^= 2; break;
        default: this.plane = this.bitNum & 7;
      }
      const h = this.prev[this.plane];
      let ctx = (this.plane & 1) << 4;
      switch (this.ctxBits) {
        case 0x00: ctx |= ((h & 0x1c0) >> 5) | (h & 1); break;
        case 0x10: ctx |= ((h & 0x180) >> 5) | (h & 1); break;
        case 0x20: ctx |= ((h & 0x0c0) >> 5) | (h & 1); break;
        default: ctx |= ((h & 0x180) >> 5) | (h & 3);
      }
      const bit = this.probBit(ctx);
      this.prev[this.plane] = (h << 1) | bit;
      this.bitNum++;
      return bit;
    }
    next() {
      if (this.planes === 0xc0) { // Mode 7: 8 bits of one pixel, low bit first
        let v = 0;
        for (let m = 1; m < 0x100; m <<= 1) if (this.modelBit()) v |= m;
        return v;
      }
      if (this.held >= 0) { const v = this.held; this.held = -1; return v; }
      let a = 0, b = 0;
      for (let m = 0x80; m; m >>= 1) { if (this.modelBit()) a |= m; if (this.modelBit()) b |= m; }
      this.held = b;
      return a;
    }
    saveState() {
      return { addr: this.addr, cur: this.cur, bitPos: this.bitPos, planes: this.planes, ctxBits: this.ctxBits, bitNum: this.bitNum, plane: this.plane, held: this.held,
        mps: this.mps.slice(), state: this.state.slice(), runMPS: this.runMPS.slice(), runLPS: this.runLPS.slice(), prev: this.prev.slice() };
    }
    loadState(o) {
      Object.assign(this, { addr: o.addr, cur: o.cur, bitPos: o.bitPos, planes: o.planes, ctxBits: o.ctxBits, bitNum: o.bitNum, plane: o.plane, held: o.held });
      this.mps.set(o.mps); this.state.set(o.state); this.runMPS.set(o.runMPS); this.runLPS.set(o.runLPS); this.prev.set(o.prev);
    }
  }

  class SDD1 {
    constructor(snes) {
      this.snes = snes; this.cart = snes.cart;
      this.regs = new Uint8Array(8);
      this.dec = new Decompressor((a) => this.romByte(a));
      Kit.claim(snes, 0x00, 0x3f, 0x4000, 0x4fff, true); // registers
      Kit.claim(snes, 0x00, 0x3f, 0x8000, 0xffff, true); // fixed ROM (LoROM layout, first 2 MB)
      Kit.claim(snes, 0xc0, 0xff, 0x0000, 0xffff, false); // switchable ROM
      this.reset();
    }
    reset() { this.regs.fill(0); this.regs[4] = 0; this.regs[5] = 1; this.regs[6] = 2; this.regs[7] = 3; }
    // ROM byte by SNES address, through the S-DD1's banking
    romByte(addr) {
      const bank = (addr >> 16) & 0xff, off = addr & 0xffff;
      if (bank >= 0xc0) return this.cart.romAt(((this.regs[4 + ((bank >> 4) & 3)] & 7) << 20) | ((bank & 0x0f) << 16) | off);
      if (off >= 0x8000) return this.cart.romAt(((bank & 0x3f) << 15) | (off & 0x7fff));
      const v = this.cart.read(bank, off);
      return v < 0 ? 0 : v;
    }
    read(bank, off) {
      if (bank >= 0xc0 || ((bank & 0x40) === 0 && off >= 0x8000)) return this.romByte((bank << 16) | off);
      if (off >= 0x4800 && off < 0x4808) return this.regs[off & 7];
      return -1;
    }
    write(bank, off, v) {
      if (bank >= 0xc0 || ((bank & 0x40) === 0 && off >= 0x8000)) return true; // ROM
      if (off >= 0x4800 && off < 0x4808) { this.regs[off & 7] = v; return true; }
      return false;
    }
    // A DMA is starting: an armed channel reads decompressed data instead of ROM.
    dmaFeed(ch, addr) {
      const bit = 1 << ch;
      if (!(this.regs[0] & this.regs[1] & bit)) return null;
      this.regs[1] &= ~bit;
      this.dec.start(addr);
      return () => this.dec.next();
    }
    saveState() { return { regs: this.regs.slice(), dec: this.dec.addr !== undefined ? this.dec.saveState() : null }; }
    loadState(o) { this.regs.set(o.regs); if (o.dec) this.dec.loadState(o.dec); }
  }

  global.RPSNESChips.push({ name: "S-DD1", detect: (c) => c.chip === 0x43 || c.chip === 0x45, create: (snes) => new SDD1(snes) });
  global.RPSNESSDD1 = { SDD1, Decompressor, EVOLUTION };
})(typeof window !== "undefined" ? window : globalThis);
