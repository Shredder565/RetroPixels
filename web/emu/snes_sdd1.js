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

  // Probability estimator: 33 states, each naming the run-length generator it
  // uses (0-7, longer runs for more predictable bits) and the state that follows
  // a run of the likely bit (MPS) or a less likely bit (LPS).
  //   state 0         start: always moves to 25
  //   states 25-32    warm-up, one per generator; an LPS drops into the main chain
  //   states 1-24     main chain: generators 0-3 four states each, 4-7 two each;
  //                   an MPS run moves one state up, an LPS one state down
  // Each entry: [generator, next after MPS, next after LPS].
  const EVOLUTION = (() => {
    const t = [[0, 25, 25]];
    const perGen = [4, 4, 4, 4, 2, 2, 2, 2];
    perGen.forEach((n, g) => { for (let i = 0; i < n; i++) t.push([g]); });
    for (let st = 1; st <= 24; st++) t[st].push(Math.min(24, st + 1), Math.max(1, st - 1));
    const warmDrop = [1, 2, 4, 8, 12, 16, 18, 22];
    for (let g = 0; g < 8; g++) t.push([g, g === 7 ? 24 : 26 + g, warmDrop[g]]);
    return t;
  })();
  // Neighbour bits that pick the context, per header context mode (bits 4-5):
  // [mask of bits 5+ of the plane's history, mask of its lowest bits]
  const NEIGHBOURS = [[0x0e, 1], [0x0c, 1], [0x06, 1], [0x0c, 3]];

  class Decompressor {
    // read(addr) gives a ROM byte by SNES address
    constructor(read) {
      this.readByte = read;
      this.mps = new Uint8Array(32); this.state = new Uint8Array(32);   // per context
      this.runMPS = new Int32Array(8); this.runLPS = new Uint8Array(8); // per run-length generator
      this.prev = new Uint16Array(8);                                   // bits seen so far, per bitplane
    }
    start(addr) {
      this.addr = addr;
      const head = this.readByte(addr);
      this.planes = head & 0xc0; this.ctxBits = head & 0x30;
      this.cur = head; this.bitPos = 4; // the stream starts after the 4 header bits
      this.mps.fill(0); this.state.fill(0); this.runMPS.fill(0); this.runLPS.fill(0); this.prev.fill(0);
      this.bitNum = 0; this.plane = 0;
      this.held = -1; // second byte of a bitplane pair waiting to go out
    }
    inBit() {
      const b = (this.cur >> (7 - this.bitPos)) & 1;
      if (++this.bitPos === 8) { this.bitPos = 0; this.addr = (this.addr + 1) & 0xffffff; this.cur = this.readByte(this.addr); }
      return b;
    }
    // Next bit from run-length generator n. Its code words: "0" means 2^n likely
    // bits; "1" and n more bits (lowest first) give a shorter run of likely bits
    // ended by one unlikely bit. Sets endOfRun when this bit finished a code word.
    genBit(n) {
      const M = this.runMPS, L = this.runLPS;
      if (M[n] === 0 && L[n] === 0) {
        if (this.inBit() === 0) M[n] = 1 << n;
        else {
          let len = 0;
          for (let i = 0; i < n; i++) len |= this.inBit() << i;
          M[n] = (len ^ ((1 << n) - 1)); L[n] = 1;
        }
      }
      const bit = M[n] ? 0 : 1;
      if (bit) L[n] = 0; else M[n]--;
      this.endOfRun = M[n] === 0 && L[n] === 0;
      return bit;
    }
    // Next bit in context ctx: the generator says "likely" (0) or "unlikely" (1);
    // the context's state only moves when a code word ends.
    probBit(ctx) {
      const st = this.state[ctx], mps = this.mps[ctx], e = EVOLUTION[st];
      const lps = this.genBit(e[0]);
      if (this.endOfRun) {
        this.state[ctx] = lps ? e[2] : e[1];
        if (lps && st < 2) this.mps[ctx] = mps ^ 1; // in the first states an unlikely bit swaps which bit is likely
      }
      return lps ^ mps;
    }
    // Bitplane that bit number n belongs to, per header mode (bits 6-7): 2bpp
    // alternates two planes; 4bpp and 8bpp alternate a pair of planes and move to
    // the next pair every 128 bits; Mode 7 cycles all eight.
    planeOf(n) {
      switch (this.planes) {
        case 0x00: return 6 | (n & 1);
        case 0x40: return ((n >> 6) & 6) | (n & 1);
        case 0x80: return 4 | ((n >> 6) & 2) | (n & 1);
        default: return n & 7;
      }
    }
    modelBit() {
      const plane = this.plane = this.planeOf(this.bitNum);
      const h = this.prev[plane], nb = NEIGHBOURS[this.ctxBits >> 4];
      const ctx = ((plane & 1) << 4) | ((h >> 5) & nb[0]) | (h & nb[1]);
      const bit = this.probBit(ctx);
      this.prev[plane] = (h << 1) | bit;
      this.bitNum++;
      return bit;
    }
    next() {
      if (this.planes === 0xc0) { // Mode 7: 8 bits of one pixel, low bit first
        let v = 0;
        for (let k = 0; k < 8; k++) v |= this.modelBit() << k;
        return v;
      }
      if (this.held >= 0) { const v = this.held; this.held = -1; return v; }
      // two bitplane bytes, their bits interleaved, highest first
      let a = 0, b = 0;
      for (let k = 7; k >= 0; k--) { a |= this.modelBit() << k; b |= this.modelBit() << k; }
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
