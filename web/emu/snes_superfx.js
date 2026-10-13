// RetroPixels — Super FX (GSU-1 / GSU-2, "MARIO chip") cartridge coprocessor
// for the SNES emulator. Original code.
//
// The Super FX is a 16-bit RISC CPU inside the cartridge (Star Fox, Stunt Race FX,
// Yoshi's Island, Doom, Vortex, Winter Gold, Dirt Trax FX). It has sixteen 16-bit
// registers, one-byte opcodes with ALT1/ALT2/ALT3 and FROM/TO/WITH prefixes, a
// two-stage pipeline (so branches and jumps run the following byte first), a
// 512-byte instruction cache, a ROM read buffer, a RAM write buffer, and a pixel
// plotter (PLOT / RPIX) that writes into SNES bitplane tile layout through two
// eight-pixel caches. The SNES reaches it through registers at $3000-$34FF.
// Written from the documented hardware behaviour.
"use strict";

(function (global) {
  // Flags in SFR
  const F_Z = 0x02, F_CY = 0x04, F_S = 0x08, F_OV = 0x10, F_GO = 0x20, F_R = 0x40, F_ALT1 = 0x100, F_ALT2 = 0x200, F_B = 0x1000, F_IRQ = 0x8000;
  // What the SNES sees in place of ROM while the Super FX owns it: the interrupt
  // vectors point into WRAM ($0100 / $0104 / $0108 / $010C), everything else is filler.
  const ROM_STANDIN = [0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08, 0x01, 0x00, 0x01, 0x0c, 0x01];

  class SuperFX {
    // cart: the SNES Cart (ROM + header info); snes: the console (for its clock and IRQ line)
    constructor(cart, snes, savedRam) {
      this.snes = snes;
      // ROM, padded to a power of two with the usual mirroring
      let rs = 1; while (rs < cart.size) rs <<= 1;
      rs = Math.max(rs, 0x8000);
      this.rom = new Uint8Array(rs);
      for (let i = 0; i < rs; i++) this.rom[i] = i < cart.size ? cart.rom[i] : cart.romAt(i);
      this.romMask = rs - 1;
      // Game pak RAM: from the extended header when there is one, else 32 KB (Star Fox)
      const h = cart.headerAt, r = cart.rom;
      let ram = 0x8000;
      if (r[h + 0x1a] === 0x33 && r[h - 3] > 0 && r[h - 3] <= 8) ram = 1024 << r[h - 3];
      ram = Math.min(0x20000, Math.max(0x8000, ram));
      this.ram = new Uint8Array(ram);
      this.ramMask = ram - 1;
      // battery: chip types $x5 / $xA keep RAM over power-off (Yoshi's Island, Stunt Race FX)
      const lowNib = cart.chip & 15;
      this.battery = lowNib === 5 || lowNib === 6 || lowNib === 0xa;
      this.batterySize = this.battery ? Math.min(ram, cart.sramSize || ram) : 0;
      if (this.battery && savedRam && savedRam.length) this.ram.set(savedRam.subarray(0, Math.min(savedRam.length, ram)));
      this.version = cart.chip === 0x13 ? 1 : 4; // MARIO chip vs GSU-2
      this.r = new Uint16Array(16);
      this.cache = new Uint8Array(512);
      this.valid = new Uint8Array(32);
      // pixel caches: [0] primary, [1] secondary; colours per pixel + which pixels are set
      this.pcColor = [new Uint8Array(8), new Uint8Array(8)];
      this.pcX = new Int32Array(2); this.pcY = new Int32Array(2); this.pcMask = new Int32Array(2);
      this.reset();
    }
    reset() {
      this.r.fill(0); this.cache.fill(0); this.valid.fill(0);
      this.z = false; this.cy = false; this.s = false; this.ov = false;
      this.go = false; this.irq = false;
      this.alt = 0; this.bflag = false; this.sreg = 0; this.dreg = 0;
      this.pbr = 0; this.rombr = 0; this.rambr = 0; this.cbr = 0;
      this.scbr = 0; this.scmr = 0; this.cfgr = 0; this.clsr = 0; this.bramr = 0;
      this.colr = 0; this.por = 0;
      this.pipe = 0x01; this.r15w = false;
      this.romBuf = 0; this.romReady = 0; this.ramReady = 0; this.ramAddr = 0;
      this.pcMask[0] = 0; this.pcMask[1] = 0;
      this.clock = this.snes ? this.snes.clock || 0 : 0;
      this.ramDirty = false;
    }

    // ---- timing: everything is counted in SNES master clocks ----
    get spd() { return (this.clsr & 1) ? 1 : 2; }          // one GSU cycle
    get mem() { return (this.clsr & 1) ? 5 : 6; }          // one ROM / RAM byte (5 cycles at 21 MHz, 3 at 10.7 MHz)

    // ---- the Super FX's own view of memory ----
    busRead(bank, a) {
      if (bank < 0x40) return this.rom[(((bank & 0x3f) << 15) | (a & 0x7fff)) & this.romMask];
      if (bank < 0x60) return this.rom[(((bank & 0x1f) << 16) | a) & this.romMask];
      if (bank >= 0x70 && bank < 0x74) return this.ram[(((bank & 3) << 16) | a) & this.ramMask];
      return 0;
    }
    ramRead(a) { return this.ram[((this.rambr << 16) | a) & this.ramMask]; }
    ramWrite(a, v) { const i = ((this.rambr << 16) | a) & this.ramMask; if (this.ram[i] !== v) { this.ram[i] = v; this.ramDirty = true; } }
    ramWait() { if (this.clock < this.ramReady) this.clock = this.ramReady; }
    loadWord(a) {
      this.ramWait(); this.ramAddr = a; this.clock += this.mem + this.spd;
      return this.ramRead(a) | (this.ramRead(a ^ 1) << 8);
    }
    loadByte(a) { this.ramWait(); this.ramAddr = a; this.clock += this.mem; return this.ramRead(a); }
    storeWord(a, v) {
      this.ramWait(); this.ramAddr = a;
      this.ramWrite(a, v & 0xff); this.ramWrite(a ^ 1, v >> 8);
      this.ramReady = this.clock + this.mem + this.spd; // the write buffer finishes in the background
    }
    storeByte(a, v) { this.ramWait(); this.ramAddr = a; this.ramWrite(a, v & 0xff); this.ramReady = this.clock + this.mem; }

    // Fetch one program byte, through the instruction cache when it's in the cache window.
    fetch(a) {
      const off = (a - this.cbr) & 0xffff;
      if (off < 512) {
        const line = off >> 4;
        if (!this.valid[line]) {
          const base = (this.cbr + (line << 4)) & 0xffff;
          for (let i = 0; i < 16; i++) this.cache[(line << 4) | i] = this.busRead(this.pbr, (base + i) & 0xffff);
          this.valid[line] = 1;
          this.clock += this.mem * (16 - (off & 15));
        }
        this.clock += this.spd;
        return this.cache[off];
      }
      this.clock += this.mem;
      return this.busRead(this.pbr, a);
    }
    imm() { const v = this.pipe; this.r[15] = (this.r[15] + 1) & 0xffff; this.pipe = this.fetch(this.r[15]); return v; }
    flushCache() { this.valid.fill(0); }

    // Writing R14 starts a ROM buffer read; writing R15 is a jump (after the next byte).
    setReg(n, v) {
      v &= 0xffff;
      this.r[n] = v;
      if (n === 14) this.romLoad();
      else if (n === 15) this.r15w = true;
    }
    romLoad() {
      this.romBuf = this.busRead(this.rombr, this.r[14]);
      this.romReady = this.clock + this.mem;
    }
    romWait() { if (this.clock < this.romReady) this.clock = this.romReady; return this.romBuf; }
    setSZ(v) { this.s = (v & 0x8000) !== 0; this.z = (v & 0xffff) === 0; }

    // ---- plotting ----
    bpp() { const m = this.scmr & 3; return m === 0 ? 2 : m === 3 ? 8 : 4; }
    // RAM index of the first bitplane byte for pixel row (x, y)
    pixelAddr(x, y, bpp) {
      let ht = ((this.scmr >> 2) & 1) | ((this.scmr >> 4) & 2);
      if (this.por & 0x10) ht = 3;
      let tile;
      const cx = x >> 3, cy = y >> 3;
      switch (ht) {
        case 0: tile = cx * 16 + cy; break;
        case 1: tile = cx * 20 + cy; break;
        case 2: tile = cx * 24 + cy; break;
        default: tile = ((y & 0x80) << 2) | ((x & 0x80) << 1) | ((y & 0x78) << 1) | ((x & 0x78) >> 3); break;
      }
      return (this.scbr << 10) + tile * (bpp << 3) + ((y & 7) << 1);
    }
    // write one eight-pixel cache row into RAM
    writePixelRow(k) {
      const mask = this.pcMask[k];
      if (!mask) return;
      const bpp = this.bpp(), base = this.pixelAddr(this.pcX[k] << 3, this.pcY[k], bpp), col = this.pcColor[k];
      this.ramWait();
      for (let p = 0; p < bpp; p++) {
        const a = (base + ((p >> 1) << 4) + (p & 1)) & this.ramMask;
        let bits = 0;
        for (let i = 0; i < 8; i++) if ((col[i] >> p) & 1) bits |= 0x80 >> i;
        const v = mask === 0xff ? bits : (this.ram[a] & ~mask) | (bits & mask);
        if (this.ram[a] !== v) { this.ram[a] = v; this.ramDirty = true; }
      }
      this.ramReady = this.clock + this.mem * bpp * (mask === 0xff ? 1 : 2);
      this.pcMask[k] = 0;
    }
    // move the primary cache into the secondary one (writing out what was there)
    pushPrimary() {
      if (!this.pcMask[0]) return;
      this.writePixelRow(1);
      this.pcColor[1].set(this.pcColor[0]);
      this.pcX[1] = this.pcX[0]; this.pcY[1] = this.pcY[0]; this.pcMask[1] = this.pcMask[0];
      this.pcMask[0] = 0;
    }
    plot() {
      const x = this.r[1] & 0xff, y = this.r[2] & 0xff, bpp = this.bpp();
      let c = this.colr;
      if (bpp !== 8 && (this.por & 2) && ((x ^ y) & 1)) c >>= 4;
      c &= bpp === 2 ? 3 : bpp === 4 ? 15 : 0xff;
      let draw = true;
      if (!(this.por & 1)) draw = (bpp === 8 && (this.por & 8)) ? (c & 15) !== 0 : c !== 0;
      if (draw) {
        if (this.pcMask[0] && (this.pcX[0] !== (x >> 3) || this.pcY[0] !== y)) this.pushPrimary();
        this.pcX[0] = x >> 3; this.pcY[0] = y;
        this.pcColor[0][x & 7] = c;
        this.pcMask[0] |= 0x80 >> (x & 7);
        if (this.pcMask[0] === 0xff) this.pushPrimary();
      }
      this.r[1] = (this.r[1] + 1) & 0xffff;
    }
    rpix() {
      this.pushPrimary(); this.writePixelRow(1);
      const x = this.r[1] & 0xff, y = this.r[2] & 0xff, bpp = this.bpp();
      const base = this.pixelAddr(x, y, bpp), sh = 7 - (x & 7);
      this.ramWait();
      let c = 0;
      for (let p = 0; p < bpp; p++) c |= ((this.ram[(base + ((p >> 1) << 4) + (p & 1)) & this.ramMask] >> sh) & 1) << p;
      this.clock += this.mem * bpp;
      return c;
    }
    setColor(v) {
      v &= 0xff;
      if (this.por & 4) v = (v & 0xf0) | (v >> 4);
      if (this.por & 8) v = (this.colr & 0xf0) | (v & 0x0f);
      this.colr = v;
    }

    // ---- running ----
    start() {
      this.go = true;
      this.pipe = 0x01; this.r15w = true; // the pipeline refills from R15
      if (this.clock < this.snes.clock) this.clock = this.snes.clock;
    }
    stop() {
      this.go = false;
      this.irq = true;
      this.snes.updateIrq();
    }
    // Run until the master clock reaches `target`.
    run(target) {
      if (!this.go) { if (this.clock < target) this.clock = target; return; }
      while (this.go && this.clock < target) this.step();
      if (!this.go && this.clock < target) this.clock = target;
    }
    sync() { this.run(this.snes.clock); }
    step() {
      const op = this.pipe;
      if (this.r15w) this.r15w = false; else this.r[15] = (this.r[15] + 1) & 0xffff;
      this.pipe = this.fetch(this.r[15]);
      this.exec(op);
    }
    done() { this.alt = 0; this.bflag = false; this.sreg = 0; this.dreg = 0; }
    branch(cond) {
      const e = (this.imm() << 24) >> 24;
      if (cond) this.setReg(15, this.r[15] + e);
      this.done();
    }
    exec(op) {
      const r = this.r, n = op & 15, alt = this.alt;
      const S = r[this.sreg], D = this.dreg;
      switch (op >> 4) {
        case 0x0:
          switch (op) {
            case 0x00: this.done(); this.stop(); return;                     // STOP
            case 0x01: this.done(); return;                                   // NOP
            case 0x02: {                                                      // CACHE
              const b = r[15] & 0xfff0;
              if (b !== this.cbr) { this.cbr = b; this.flushCache(); }
              this.done(); return;
            }
            case 0x03: { const v = S >> 1; this.cy = (S & 1) !== 0; this.setSZ(v); this.setReg(D, v); this.done(); return; } // LSR
            case 0x04: { const v = ((S << 1) | (this.cy ? 1 : 0)) & 0xffff; this.cy = (S & 0x8000) !== 0; this.setSZ(v); this.setReg(D, v); this.done(); return; } // ROL
            case 0x05: this.branch(true); return;                                 // BRA
            case 0x06: this.branch(this.s === this.ov); return;                   // BGE
            case 0x07: this.branch(this.s !== this.ov); return;                   // BLT
            case 0x08: this.branch(!this.z); return;                              // BNE
            case 0x09: this.branch(this.z); return;                               // BEQ
            case 0x0a: this.branch(!this.s); return;                              // BPL
            case 0x0b: this.branch(this.s); return;                               // BMI
            case 0x0c: this.branch(!this.cy); return;                             // BCC
            case 0x0d: this.branch(this.cy); return;                              // BCS
            case 0x0e: this.branch(!this.ov); return;                             // BVC
            case 0x0f: this.branch(this.ov); return;                              // BVS
          }
          return;
        case 0x1: // TO / MOVE
          if (this.bflag) { this.setReg(n, S); this.done(); } else this.dreg = n;
          return;
        case 0x2: // WITH
          this.sreg = n; this.dreg = n; this.bflag = true; return;
        case 0x3:
          if (n < 12) { // STW / STB (Rn)
            if (alt & 1) this.storeByte(r[n], S); else this.storeWord(r[n], S);
            this.done(); return;
          }
          if (n === 12) { // LOOP
            const v = (r[12] - 1) & 0xffff; r[12] = v; this.setSZ(v);
            if (v) this.setReg(15, r[13]);
            this.done(); return;
          }
          if (n === 13) { this.alt = 1; return; }  // ALT1
          if (n === 14) { this.alt = 2; return; }  // ALT2
          this.alt = 3; return;                    // ALT3
        case 0x4:
          if (n < 12) { // LDW / LDB (Rn)
            const v = (alt & 1) ? this.loadByte(r[n]) : this.loadWord(r[n]);
            this.setReg(D, v); this.done(); return;
          }
          switch (n) {
            case 12: // PLOT / RPIX
              if (alt & 1) { const v = this.rpix(); this.setSZ(v); this.setReg(D, v); }
              else this.plot();
              this.done(); return;
            case 13: { const v = ((S >> 8) | (S << 8)) & 0xffff; this.setSZ(v); this.setReg(D, v); this.done(); return; } // SWAP
            case 14: // COLOR / CMODE
              if (alt & 1) this.por = S & 0x1f; else this.setColor(S);
              this.done(); return;
            default: { const v = (~S) & 0xffff; this.setSZ(v); this.setReg(D, v); this.done(); return; } // NOT
          }
        case 0x5: { // ADD / ADC / ADD # / ADC #
          const o = (alt & 2) ? n : r[n];
          const sum = S + o + ((alt & 1) && this.cy ? 1 : 0);
          const v = sum & 0xffff;
          this.ov = ((~(S ^ o)) & (o ^ v) & 0x8000) !== 0;
          this.cy = sum > 0xffff; this.setSZ(v);
          this.setReg(D, v); this.done(); return;
        }
        case 0x6: { // SUB / SBC / SUB # / CMP
          const o = alt === 2 ? n : r[n];
          const dif = S - o - (alt === 1 && !this.cy ? 1 : 0);
          const v = dif & 0xffff;
          this.ov = ((S ^ o) & (S ^ v) & 0x8000) !== 0;
          this.cy = dif >= 0; this.setSZ(v);
          if (alt !== 3) this.setReg(D, v);
          this.done(); return;
        }
        case 0x7: {
          if (n === 0) { // MERGE
            const v = (r[7] & 0xff00) | (r[8] >> 8);
            this.s = (v & 0x8080) !== 0; this.ov = (v & 0xc0c0) !== 0; this.cy = (v & 0xe0e0) !== 0; this.z = (v & 0xf0f0) !== 0; // (hardware sets Z when any of these bits are set)
            this.setReg(D, v); this.done(); return;
          }
          const o = (alt & 2) ? n : r[n];
          const v = (alt & 1) ? S & ~o & 0xffff : S & o; // AND / BIC
          this.setSZ(v); this.setReg(D, v); this.done(); return;
        }
        case 0x8: { // MULT / UMULT (8 x 8 -> 16)
          const o = (alt & 2) ? n : r[n];
          const v = (alt & 1) ? ((S & 0xff) * (o & 0xff)) & 0xffff : (((S << 24) >> 24) * ((o << 24) >> 24)) & 0xffff;
          if (!(this.cfgr & 0x20)) this.clock += this.spd;
          this.setSZ(v); this.setReg(D, v); this.done(); return;
        }
        case 0x9:
          switch (n) {
            case 0: this.storeWord(this.ramAddr, S); this.done(); return; // SBK
            case 1: case 2: case 3: case 4: this.setReg(11, r[15] + n); this.done(); return; // LINK #n
            case 5: { const v = ((S << 24) >> 24) & 0xffff; this.setSZ(v); this.setReg(D, v); this.done(); return; } // SEX
            case 6: { // ASR / DIV2
              let v = ((S << 16) >> 17) & 0xffff;
              if ((alt & 1) && S === 0xffff) v = 0;
              this.cy = (S & 1) !== 0; this.setSZ(v); this.setReg(D, v); this.done(); return;
            }
            case 7: { const v = (S >> 1) | (this.cy ? 0x8000 : 0); this.cy = (S & 1) !== 0; this.setSZ(v); this.setReg(D, v); this.done(); return; } // ROR
            case 14: { const v = S & 0xff; this.s = (v & 0x80) !== 0; this.z = v === 0; this.setReg(D, v); this.done(); return; } // LOB
            case 15: { // FMULT / LMULT
              const p = ((S << 16) >> 16) * ((r[6] << 16) >> 16);
              const hi = (p >> 16) & 0xffff;
              this.clock += this.spd * ((this.cfgr & 0x20) ? 3 : 7);
              if (alt & 1) r[4] = p & 0xffff;
              this.cy = ((p >> 15) & 1) !== 0; this.setSZ(hi); this.setReg(D, hi); this.done(); return;
            }
            default: // 8..13: JMP / LJMP
              if (alt & 1) {
                this.pbr = r[n] & 0x7f;
                this.setReg(15, S);
                this.cbr = S & 0xfff0; this.flushCache();
              } else this.setReg(15, r[n]);
              this.done(); return;
          }
        case 0xa: { // IBT / LMS / SMS
          const k = this.imm();
          if (alt === 0) this.setReg(n, (k << 24) >> 24);
          else if (alt === 2) this.storeWord(k << 1, r[n]);
          else this.setReg(n, this.loadWord(k << 1));
          this.done(); return;
        }
        case 0xb: // FROM / MOVES
          if (this.bflag) {
            const v = r[n];
            this.ov = (v & 0x80) !== 0; this.setSZ(v);
            this.setReg(D, v); this.done();
          } else this.sreg = n;
          return;
        case 0xc: {
          if (n === 0) { const v = S >> 8; this.s = (v & 0x80) !== 0; this.z = v === 0; this.setReg(D, v); this.done(); return; } // HIB
          const o = (alt & 2) ? n : r[n];
          const v = (alt & 1) ? S ^ o : S | o; // OR / XOR
          this.setSZ(v); this.setReg(D, v); this.done(); return;
        }
        case 0xd:
          if (n < 15) { const v = (r[n] + 1) & 0xffff; this.setSZ(v); this.setReg(n, v); this.done(); return; } // INC
          if (alt === 3) { this.romWait(); this.rombr = S & 0x7f; }       // ROMB
          else if (alt === 2) { this.ramWait(); this.rambr = S & 1; }     // RAMB
          else this.setColor(this.romWait());                              // GETC
          this.done(); return;
        case 0xe:
          if (n < 15) { const v = (r[n] - 1) & 0xffff; this.setSZ(v); this.setReg(n, v); this.done(); return; } // DEC
          { // GETB / GETBH / GETBL / GETBS
            const b = this.romWait();
            let v;
            if (alt === 0) v = b;
            else if (alt === 1) v = (S & 0xff) | (b << 8);
            else if (alt === 2) v = (S & 0xff00) | b;
            else v = ((b << 24) >> 24) & 0xffff;
            this.setReg(D, v); this.done(); return;
          }
        default: { // IWT / LM / SM
          const lo = this.imm(), hi = this.imm(), k = lo | (hi << 8);
          if (alt === 0) this.setReg(n, k);
          else if (alt === 2) this.storeWord(k, r[n]);
          else this.setReg(n, this.loadWord(k));
          this.done(); return;
        }
      }
    }

    // ---- SNES side ----
    sfr() {
      return (this.z ? F_Z : 0) | (this.cy ? F_CY : 0) | (this.s ? F_S : 0) | (this.ov ? F_OV : 0) | (this.go ? F_GO : 0) |
        (this.clock < this.romReady ? F_R : 0) | ((this.alt & 1) ? F_ALT1 : 0) | ((this.alt & 2) ? F_ALT2 : 0) | (this.bflag ? F_B : 0) | (this.irq ? F_IRQ : 0);
    }
    // the SNES CPU's interrupt request from the chip
    irqOut() { return this.irq && !(this.cfgr & 0x80); }
    regRead(off) {
      this.sync();
      if (off >= 0x3100 && off < 0x3300) return this.cache[off - 0x3100];
      const a = off;
      if (a < 0x3020) { const v = this.r[(a >> 1) & 15]; return (a & 1) ? v >> 8 : v & 0xff; }
      switch (a) {
        case 0x3030: return this.sfr() & 0xff;
        case 0x3031: { const v = this.sfr() >> 8; if (this.irq) { this.irq = false; this.snes.updateIrq(); } return v; }
        case 0x3033: return this.bramr;
        case 0x3034: return this.pbr;
        case 0x3036: return this.rombr;
        case 0x3037: return this.cfgr;
        case 0x3038: return this.scbr;
        case 0x3039: return this.clsr;
        case 0x303a: return this.scmr;
        case 0x303b: return this.version;
        case 0x303c: return this.rambr;
        case 0x303e: return this.cbr & 0xff;
        case 0x303f: return this.cbr >> 8;
      }
      return 0;
    }
    regWrite(off, v) {
      this.sync();
      if (off >= 0x3100 && off < 0x3300) {
        const i = off - 0x3100;
        this.cache[i] = v;
        if ((i & 15) === 15) this.valid[i >> 4] = 1;
        return;
      }
      const a = off;
      if (a < 0x3020) {
        const k = (a >> 1) & 15;
        this.r[k] = (a & 1) ? (this.r[k] & 0xff) | (v << 8) : (this.r[k] & 0xff00) | v;
        if (a & 1) {
          if (k === 14) this.romLoad();
          if (k === 15) this.start();
        }
        return;
      }
      switch (a) {
        case 0x3030: {
          const wasGo = this.go;
          this.z = (v & F_Z) !== 0; this.cy = (v & F_CY) !== 0; this.s = (v & F_S) !== 0; this.ov = (v & F_OV) !== 0;
          if (!(v & F_GO)) { this.go = false; if (wasGo || this.cbr) { this.cbr = 0; this.flushCache(); } }
          else if (!wasGo) this.start();
          return;
        }
        case 0x3031:
          this.alt = ((v & 1) ? 1 : 0) | ((v & 2) ? 2 : 0); this.bflag = (v & 0x10) !== 0;
          this.irq = (v & 0x80) !== 0; this.snes.updateIrq();
          return;
        case 0x3033: this.bramr = v & 1; return;
        case 0x3034: this.pbr = v & 0x7f; return;
        case 0x3037: this.cfgr = v; this.snes.updateIrq(); return;
        case 0x3038: this.scbr = v; return;
        case 0x3039: this.clsr = v & 1; return;
        case 0x303a: this.scmr = v; return;
      }
    }
    romOwned() { return this.go && (this.scmr & 0x10) !== 0; }
    ramOwned() { return this.go && (this.scmr & 0x08) !== 0; }
    // SNES bus outside $3000-$34FF; returns -1 for open bus
    snesRead(bank, off) {
      const b = bank & 0x7f;
      if (b < 0x40) {
        if (off >= 0x8000) {
          if (this.romOwned()) { this.sync(); if (this.romOwned()) return ROM_STANDIN[off & 15]; }
          return this.rom[((b << 15) | (off & 0x7fff)) & this.romMask];
        }
        if (off >= 0x6000) { if (this.ramOwned()) return -1; return this.ram[(off - 0x6000) & this.ramMask]; }
        return -1;
      }
      if (b < 0x60) {
        if (this.romOwned()) { this.sync(); if (this.romOwned()) return ROM_STANDIN[off & 15]; }
        return this.rom[(((b & 0x1f) << 16) | off) & this.romMask];
      }
      if (b >= 0x70 && b < 0x74) { if (this.ramOwned()) return -1; return this.ram[(((b & 3) << 16) | off) & this.ramMask]; }
      return -1;
    }
    snesWrite(bank, off, v) {
      const b = bank & 0x7f;
      let i = -1;
      if (b < 0x40) { if (off >= 0x6000 && off < 0x8000) i = (off - 0x6000) & this.ramMask; }
      else if (b >= 0x70 && b < 0x74) i = (((b & 3) << 16) | off) & this.ramMask;
      if (i < 0 || this.ramOwned()) return;
      if (this.ram[i] !== v) { this.ram[i] = v; this.ramDirty = true; }
    }
    getSRAM() { return this.battery ? this.ram.slice(0, this.batterySize) : null; }

    saveState() {
      return {
        r: this.r.slice(), cache: this.cache.slice(), valid: this.valid.slice(), ram: this.ram.slice(),
        pc0: this.pcColor[0].slice(), pc1: this.pcColor[1].slice(), pcx: this.pcX.slice(), pcy: this.pcY.slice(), pcm: this.pcMask.slice(),
        m: [this.z, this.cy, this.s, this.ov, this.go, this.irq, this.bflag, this.r15w].map((b) => (b ? 1 : 0)).concat([
          this.alt, this.sreg, this.dreg, this.pbr, this.rombr, this.rambr, this.cbr, this.scbr, this.scmr, this.cfgr, this.clsr, this.bramr,
          this.colr, this.por, this.pipe, this.romBuf, this.romReady, this.ramReady, this.ramAddr, this.clock]),
      };
    }
    loadState(o) {
      this.r.set(o.r); this.cache.set(o.cache); this.valid.set(o.valid); this.ram.set(o.ram);
      this.pcColor[0].set(o.pc0); this.pcColor[1].set(o.pc1); this.pcX.set(o.pcx); this.pcY.set(o.pcy); this.pcMask.set(o.pcm);
      const m = o.m;
      [this.z, this.cy, this.s, this.ov, this.go, this.irq, this.bflag, this.r15w] = m.slice(0, 8).map((x) => !!x);
      [this.alt, this.sreg, this.dreg, this.pbr, this.rombr, this.rambr, this.cbr, this.scbr, this.scmr, this.cfgr, this.clsr, this.bramr,
        this.colr, this.por, this.pipe, this.romBuf, this.romReady, this.ramReady, this.ramAddr, this.clock] = m.slice(8);
    }
  }

  global.RPSuperFX = SuperFX;
})(typeof window !== "undefined" ? window : globalThis);
