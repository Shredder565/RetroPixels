// RetroPixels — SA-1 cartridge chip for the Super Nintendo emulator. Original code.
//
// The SA-1 is a second 65C816 inside the cartridge, running at 10.74 MHz (half
// the master clock), with its own 2 KB of fast "I-RAM", the battery-backed
// "BW-RAM" (also readable as a packed 2/4-bit-per-pixel bitmap), a ROM bank
// switcher (Super MMC), a multiply / divide / multiply-accumulate unit, a
// variable-length bit reader, a DMA unit that can turn bitmaps into SNES tiles
// ("character conversion"), and an H/V timer. Written from the documented
// behaviour of the chip; the second CPU reuses RetroPixels' own RP65816 core.
//
//   const sa1 = new RPSA1(snes);   // installs itself on snes.cart's read/write
//   sa1.catchUp(clock)             // run the SA-1 up to a master-clock time
"use strict";

(function (global) {
  const TWO40 = 1099511627776, TWO39 = 549755813888;

  class SA1 {
    constructor(snes) {
      this.snes = snes;
      const cart = snes.cart;
      this.cart = cart;
      // BW-RAM is the cartridge's save memory when the header says there is one
      if (cart.sramSize) { this.bw = cart.sram; this.ownBw = false; }
      else { this.bw = new Uint8Array(0x2000).fill(0xff); this.ownBw = true; }
      this.bwMask = this.bw.length - 1;
      this.iram = new Uint8Array(0x800);
      this.brf = new Uint8Array(16);
      const self = this;
      this.cpu = new global.RP65816({
        read: (a) => self.read(a),
        write: (a, v) => self.write(a, v),
        idle: () => { self.clock += 2; },
      });
      // The SNES side reaches the cartridge through cart.read / cart.write.
      cart.read = (bank, off) => self.snesRead(bank, off);
      cart.write = (bank, off, v) => self.snesWrite(bank, off, v);
      this.reset();
    }

    reset() {
      this.iram.fill(0); this.brf.fill(0);
      this.clock = this.snes.clock || 0;
      this.mdr = 0;
      // SNES-written registers
      this.ccnt = 0x20;            // SA-1 held in reset at power-on
      this.sie = 0;
      this.crv = 0; this.cnv = 0; this.civ = 0;
      this.mmc = [0, 1, 2, 3];      // CXB, DXB, EXB, FXB
      this.bmaps = 0; this.sbwe = 0; this.bwpa = 0x0f; this.siwp = 0;
      // SA-1-written registers
      this.scnt = 0; this.cie = 0; this.snv = 0; this.siv = 0;
      this.tmc = 0; this.hcnt = 0; this.vcnt = 0;
      this.bmap = 0; this.cbwe = 0; this.ciwp = 0;
      this.dcnt = 0; this.cdma = 0; this.sda = 0; this.dda = 0; this.dtc = 0;
      this.bbf = 0;
      this.mcnt = 0; this.ma = 0; this.mb = 0; this.mr = 0; this.of = 0;
      this.vbd = 0; this.vda = 0; this.vbit = 0;
      // flags
      this.sfrIrq = false; this.sfrDma = false;          // to the SNES CPU
      this.cfrIrq = false; this.cfrTimer = false; this.cfrDma = false; this.cfrNmi = false; // to the SA-1
      this.cc1 = false; this.cc2Line = 0;
      this.timerBase = this.clock; this.hLatch = 0; this.vLatch = 0;
      this.running = false;        // has left reset at least once
      this.snesIrq = false;
      this.cpu.nmiPending = false; this.cpu.irqLine = false;
    }

    // ---------------------------------------------------------------
    // ROM through the Super MMC
    // ---------------------------------------------------------------
    // banks 00-3F / 80-BF, $8000-$FFFF (LoROM-style 32 KB pages)
    romLo(bank, off) {
      const g = ((bank >> 6) & 2) | ((bank >> 5) & 1);
      const r = this.mmc[g];
      const block = (r & 0x80) ? r & 7 : g;
      return this.cart.romAt((block << 20) | ((bank & 0x1f) << 15) | (off & 0x7fff));
    }
    // banks C0-FF (HiROM-style 64 KB banks)
    romHi(bank, off) {
      const block = this.mmc[(bank >> 4) & 3] & 7;
      return this.cart.romAt((block << 20) | ((bank & 0x0f) << 16) | off);
    }
    // a ROM byte at a 24-bit SA-1 address (used by the bit reader and DMA)
    romByte(a) {
      const bank = (a >>> 16) & 0xff, off = a & 0xffff;
      if (bank >= 0xc0) return this.romHi(bank, off);
      if (!(bank & 0x40) && off >= 0x8000) return this.romLo(bank, off);
      return 0;
    }

    // ---------------------------------------------------------------
    // BW-RAM, write protection and the bitmap view
    // ---------------------------------------------------------------
    bwWritable(i) {
      return (this.sbwe & 0x80) || (this.cbwe & 0x80) || i >= (0x100 << (this.bwpa & 0x0f));
    }
    bwWrite(i, v) {
      i &= this.bwMask;
      if (!this.bwWritable(i)) return;
      if (this.bw[i] !== v) { this.bw[i] = v; this.cart.sramDirty = true; }
    }
    // bitmap address p (banks 60-6F, or the $6000 window in bitmap mode)
    bitmapRead(p) {
      if (this.bbf & 0x80) { const s = (p & 3) << 1; return (this.bw[(p >> 2) & this.bwMask] >> s) & 3; }
      const s = (p & 1) << 2; return (this.bw[(p >> 1) & this.bwMask] >> s) & 15;
    }
    bitmapWrite(p, v) {
      let i, m, s;
      if (this.bbf & 0x80) { i = (p >> 2) & this.bwMask; s = (p & 3) << 1; m = 3; }
      else { i = (p >> 1) & this.bwMask; s = (p & 1) << 2; m = 15; }
      this.bwWrite(i, (this.bw[i] & ~(m << s)) | ((v & m) << s));
    }

    // ---------------------------------------------------------------
    // The SA-1 CPU's bus
    // ---------------------------------------------------------------
    read(a) {
      const bank = a >>> 16, off = a & 0xffff;
      let v;
      if (!(bank & 0x40)) {
        if (off >= 0x8000) {
          this.clock += 2;
          // vector fetches by the SA-1 come from its own vector registers
          if (bank === 0 && off >= 0xffea) {
            switch (off) {
              case 0xfffc: v = this.crv & 0xff; break; case 0xfffd: v = this.crv >> 8; break;
              case 0xffea: case 0xfffa: v = this.cnv & 0xff; break; case 0xffeb: case 0xfffb: v = this.cnv >> 8; break;
              case 0xffee: case 0xfffe: v = this.civ & 0xff; break; case 0xffef: case 0xffff: v = this.civ >> 8; break;
            }
          }
          if (v === undefined) v = this.romLo(bank, off);
        } else if (off < 0x0800) { this.clock += 2; v = this.iram[off]; }
        else if (off >= 0x3000 && off < 0x3800) { this.clock += 2; v = this.iram[off & 0x7ff]; }
        else if (off >= 0x2200 && off < 0x2400) { this.clock += 2; v = this.ioRead(off, false); }
        else if (off >= 0x6000) {
          this.clock += 4;
          if (this.bmap & 0x80) v = this.bitmapRead(((this.bmap & 0x7f) << 13) | (off & 0x1fff));
          else v = this.bw[(((this.bmap & 0x1f) << 13) | (off & 0x1fff)) & this.bwMask];
        } else { this.clock += 2; v = this.mdr; }
      } else if (bank >= 0xc0) { this.clock += 2; v = this.romHi(bank, off); }
      else if (bank < 0x50) { this.clock += 4; v = this.bw[(((bank & 0x0f) << 16) | off) & this.bwMask]; }
      else if (bank >= 0x60 && bank < 0x70) { this.clock += 4; v = this.bitmapRead(((bank & 0x0f) << 16) | off); }
      else { this.clock += 2; v = this.mdr; }
      this.mdr = v;
      return v;
    }
    write(a, v) {
      const bank = a >>> 16, off = a & 0xffff;
      this.mdr = v;
      if (!(bank & 0x40)) {
        if (off >= 0x8000) { this.clock += 2; return; }
        if (off < 0x0800 || (off >= 0x3000 && off < 0x3800)) {
          this.clock += 2;
          if (this.ciwp & (1 << ((off >> 8) & 7))) this.iram[off & 0x7ff] = v;
          return;
        }
        if (off >= 0x2200 && off < 0x2400) { this.clock += 2; this.ioWrite(off, v, false); return; }
        if (off >= 0x6000) {
          this.clock += 4;
          if (this.bmap & 0x80) this.bitmapWrite(((this.bmap & 0x7f) << 13) | (off & 0x1fff), v);
          else this.bwWrite(((this.bmap & 0x1f) << 13) | (off & 0x1fff), v);
          return;
        }
        this.clock += 2; return;
      }
      if (bank >= 0x40 && bank < 0x50) { this.clock += 4; this.bwWrite(((bank & 0x0f) << 16) | off, v); return; }
      if (bank >= 0x60 && bank < 0x70) { this.clock += 4; this.bitmapWrite(((bank & 0x0f) << 16) | off, v); return; }
      this.clock += 2;
    }

    // ---------------------------------------------------------------
    // The SNES CPU's view of the cartridge (replaces Cart.read / write)
    // ---------------------------------------------------------------
    snesRead(bank, off) {
      if (!(bank & 0x40)) {
        if (off >= 0x8000) {
          // the SA-1 can swap in its own NMI / IRQ vectors for the SNES CPU
          if (bank === 0 && off >= 0xffea && off <= 0xffef) {
            if ((this.scnt & 0x10) && off === 0xffea) return this.snv & 0xff;
            if ((this.scnt & 0x10) && off === 0xffeb) return this.snv >> 8;
            if ((this.scnt & 0x40) && off === 0xffee) return this.siv & 0xff;
            if ((this.scnt & 0x40) && off === 0xffef) return this.siv >> 8;
          }
          return this.romLo(bank, off);
        }
        if (off >= 0x6000) { this.catchUp(this.snes.clock); return this.snesBw(((this.bmaps & 0x1f) << 13) | (off & 0x1fff)); }
        return -1;
      }
      if (bank >= 0xc0) return this.romHi(bank, off);
      if (bank < 0x50) { this.catchUp(this.snes.clock); return this.snesBw(((bank & 0x0f) << 16) | off); }
      return -1;
    }
    snesBw(i) {
      if (this.cc1) { const c = this.cc1Byte(i); if (c >= 0) return c; }
      return this.bw[i & this.bwMask];
    }
    snesWrite(bank, off, v) {
      if (!(bank & 0x40)) {
        if (off >= 0x6000 && off < 0x8000) { this.catchUp(this.snes.clock); this.bwWrite(((this.bmaps & 0x1f) << 13) | (off & 0x1fff), v); }
        return;
      }
      if (bank < 0x50) { this.catchUp(this.snes.clock); this.bwWrite(((bank & 0x0f) << 16) | off, v); }
    }
    // $2200-$3FFF in banks 00-3F / 80-BF on the SNES side: registers and I-RAM
    snesIO(off, write, v) {
      if (off >= 0x2400 && (off < 0x3000 || off >= 0x3800)) return write ? 0 : this.snes.mdr;
      this.catchUp(this.snes.clock);
      if (off >= 0x3000) {
        if (!write) return this.iram[off & 0x7ff];
        if (this.siwp & (1 << ((off >> 8) & 7))) this.iram[off & 0x7ff] = v;
        return 0;
      }
      if (write) { this.ioWrite(off, v, true); return 0; }
      return this.ioRead(off, true);
    }

    // ---------------------------------------------------------------
    // Interrupt lines
    // ---------------------------------------------------------------
    updateIrqs() {
      this.cpu.irqLine = (this.cfrIrq && (this.cie & 0x80) !== 0) || (this.cfrTimer && (this.cie & 0x40) !== 0) || (this.cfrDma && (this.cie & 0x20) !== 0);
      const s = (this.sfrIrq && (this.sie & 0x80) !== 0) || (this.sfrDma && (this.sie & 0x20) !== 0);
      if (s !== this.snesIrq) { this.snesIrq = s; this.snes.setIrqLine(); }
    }

    // ---------------------------------------------------------------
    // Registers
    // ---------------------------------------------------------------
    ioRead(off, fromSnes) {
      switch (off) {
        case 0x2300: // SFR: SNES CPU flags
          return (this.sfrIrq ? 0x80 : 0) | (this.scnt & 0x40) | (this.sfrDma ? 0x20 : 0) | (this.scnt & 0x10) | (this.scnt & 0x0f);
        case 0x2301: // CFR: SA-1 CPU flags
          return (this.cfrIrq ? 0x80 : 0) | (this.cfrTimer ? 0x40 : 0) | (this.cfrDma ? 0x20 : 0) | (this.cfrNmi ? 0x10 : 0) | (this.ccnt & 0x0f);
        case 0x2302: this.latchTimer(); return this.hLatch & 0xff;
        case 0x2303: return this.hLatch >> 8;
        case 0x2304: return this.vLatch & 0xff;
        case 0x2305: return this.vLatch >> 8;
        case 0x2306: case 0x2307: case 0x2308: case 0x2309: case 0x230a: {
          const u = this.mr < 0 ? this.mr + TWO40 : this.mr;
          return Math.floor(u / Math.pow(2, (off - 0x2306) * 8)) & 0xff;
        }
        case 0x230b: return this.of ? 0x80 : 0;
        case 0x230c: return this.vdp() & 0xff;
        case 0x230d: {
          const v = this.vdp() >> 8;
          if (this.vbd & 0x80) this.vAdvance(); // auto-increment mode
          return v;
        }
        case 0x230e: return 0x23; // version code
      }
      return fromSnes ? this.snes.mdr : this.mdr;
    }
    ioWrite(off, v, fromSnes) {
      switch (off) {
        // ---- written by the SNES CPU ----
        case 0x2200: { // CCNT
          const was = this.ccnt;
          this.ccnt = v;
          if (v & 0x80) this.cfrIrq = true;
          if (v & 0x10) { this.cfrNmi = true; if (this.cie & 0x10) this.cpu.nmiPending = true; }
          if ((was & 0x20) && !(v & 0x20)) { this.clock = Math.max(this.clock, this.snes.clock); this.running = true; this.cpu.reset(); this.timerBase = this.clock; }
          this.updateIrqs();
          return;
        }
        case 0x2201: this.sie = v; this.updateIrqs(); return;
        case 0x2202: // SIC
          if (v & 0x80) this.sfrIrq = false;
          if (v & 0x20) this.sfrDma = false;
          this.updateIrqs();
          return;
        case 0x2203: this.crv = (this.crv & 0xff00) | v; return;
        case 0x2204: this.crv = (this.crv & 0xff) | (v << 8); return;
        case 0x2205: this.cnv = (this.cnv & 0xff00) | v; return;
        case 0x2206: this.cnv = (this.cnv & 0xff) | (v << 8); return;
        case 0x2207: this.civ = (this.civ & 0xff00) | v; return;
        case 0x2208: this.civ = (this.civ & 0xff) | (v << 8); return;
        case 0x2220: case 0x2221: case 0x2222: case 0x2223: this.mmc[off - 0x2220] = v; return;
        case 0x2224: this.bmaps = v; return;
        case 0x2226: this.sbwe = v; return;
        case 0x2228: this.bwpa = v; return;
        case 0x2229: this.siwp = v; return;
        // ---- written by the SA-1 CPU ----
        case 0x2209: // SCNT
          this.scnt = v;
          if (v & 0x80) this.sfrIrq = true;
          this.updateIrqs();
          return;
        case 0x220a: {
          const was = this.cie;
          this.cie = v;
          if (!(was & 0x10) && (v & 0x10) && this.cfrNmi) this.cpu.nmiPending = true;
          this.updateIrqs();
          return;
        }
        case 0x220b: // CIC
          if (v & 0x80) this.cfrIrq = false;
          if (v & 0x40) this.cfrTimer = false;
          if (v & 0x20) this.cfrDma = false;
          if (v & 0x10) this.cfrNmi = false;
          this.updateIrqs();
          return;
        case 0x220c: this.snv = (this.snv & 0xff00) | v; return;
        case 0x220d: this.snv = (this.snv & 0xff) | (v << 8); return;
        case 0x220e: this.siv = (this.siv & 0xff00) | v; return;
        case 0x220f: this.siv = (this.siv & 0xff) | (v << 8); return;
        case 0x2210: this.tmc = v; return;
        case 0x2211: this.timerBase = this.clock; return;
        case 0x2212: this.hcnt = (this.hcnt & 0x100) | v; return;
        case 0x2213: this.hcnt = (this.hcnt & 0xff) | ((v & 1) << 8); return;
        case 0x2214: this.vcnt = (this.vcnt & 0x100) | v; return;
        case 0x2215: this.vcnt = (this.vcnt & 0xff) | ((v & 1) << 8); return;
        case 0x2225: this.bmap = v; return;
        case 0x2227: this.cbwe = v; return;
        case 0x222a: this.ciwp = v; return;
        case 0x2230: this.dcnt = v; this.cc2Line = 0; if (!(v & 0x80)) this.cc1 = false; return;
        case 0x2231: // CDMA (both CPUs); bit 7 ends character conversion 1
          this.cdma = v;
          if (v & 0x80) this.cc1 = false;
          return;
        case 0x2232: this.sda = (this.sda & 0xffff00) | v; return;
        case 0x2233: this.sda = (this.sda & 0xff00ff) | (v << 8); return;
        case 0x2234: this.sda = (this.sda & 0x00ffff) | (v << 16); return;
        case 0x2235: this.dda = (this.dda & 0xffff00) | v; this.cc2Line = 0; return;
        case 0x2236:
          this.dda = (this.dda & 0xff00ff) | (v << 8);
          if (this.dcnt & 0x80) {
            if (this.dcnt & 0x20) { if (this.dcnt & 0x10) this.startCc1(); }
            else if (!(this.dcnt & 0x04)) this.normalDma();
          }
          return;
        case 0x2237:
          this.dda = (this.dda & 0x00ffff) | (v << 16);
          if ((this.dcnt & 0xa0) === 0x80 && (this.dcnt & 0x04)) this.normalDma();
          return;
        case 0x2238: this.dtc = (this.dtc & 0xff00) | v; return;
        case 0x2239: this.dtc = (this.dtc & 0xff) | (v << 8); return;
        case 0x223f: this.bbf = v; return;
        case 0x2250: // MCNT
          this.mcnt = v;
          if (v & 2) { this.mr = 0; this.of = 0; }
          return;
        case 0x2251: this.ma = (this.ma & 0xff00) | v; return;
        case 0x2252: this.ma = (this.ma & 0xff) | (v << 8); return;
        case 0x2253: this.mb = (this.mb & 0xff00) | v; return;
        case 0x2254: this.mb = (this.mb & 0xff) | (v << 8); this.arith(); return;
        case 0x2258: // VBD
          this.vbd = v;
          if (!(v & 0x80)) this.vAdvance(); // fixed mode: each write steps the reader
          return;
        case 0x2259: this.vda = (this.vda & 0xffff00) | v; return;
        case 0x225a: this.vda = (this.vda & 0xff00ff) | (v << 8); return;
        case 0x225b: this.vda = (this.vda & 0x00ffff) | (v << 16); this.vbit = 0; return;
      }
      if (off >= 0x2240 && off <= 0x224f) { this.brf[off & 15] = v; if ((off & 7) === 7 && (this.dcnt & 0xb0) === 0xa0) this.cc2Row(off & 8); }
      void fromSnes;
    }

    // ---------------------------------------------------------------
    // Arithmetic unit
    // ---------------------------------------------------------------
    arith() {
      const a = (this.ma << 16) >> 16;
      if (this.mcnt & 2) { // multiply and add to the 40-bit sum
        const b = (this.mb << 16) >> 16;
        let s = this.mr + a * b;
        if (s >= TWO39 || s < -TWO39) { this.of = 1; s = s >= TWO39 ? s - TWO40 : s + TWO40; }
        this.mr = s;
        this.mb = 0;
      } else if (this.mcnt & 1) { // signed 16 / unsigned 16 -> quotient, remainder
        const b = this.mb;
        let q = 0, r = 0;
        if (b) { q = Math.floor(a / b); r = a - q * b; }
        this.mr = (q & 0xffff) + (r & 0xffff) * 65536;
        this.ma = 0; this.mb = 0;
      } else { // signed 16 x signed 16
        const b = (this.mb << 16) >> 16;
        this.mr = a * b;
        this.mb = 0;
      }
    }

    // ---------------------------------------------------------------
    // Variable-length bit reader
    // ---------------------------------------------------------------
    vdp() {
      const a = this.vda;
      const d = this.romByte(a) | (this.romByte((a + 1) & 0xffffff) << 8) | (this.romByte((a + 2) & 0xffffff) << 16);
      return (d >> this.vbit) & 0xffff;
    }
    vAdvance() {
      const n = (this.vbd & 15) || 16;
      this.vbit += n;
      this.vda = (this.vda + (this.vbit >> 3)) & 0xffffff;
      this.vbit &= 7;
    }

    // ---------------------------------------------------------------
    // DMA
    // ---------------------------------------------------------------
    normalDma() {
      const src = this.dcnt & 3, toBw = (this.dcnt & 4) !== 0;
      let s = this.sda, d = this.dda;
      const n = this.dtc;
      for (let i = 0; i < n; i++) {
        let v;
        if (src === 0) v = this.romByte(s);
        else if (src === 1) v = this.bw[s & this.bwMask];
        else v = this.iram[s & 0x7ff];
        if (toBw) this.bwWrite(d & 0x3ffff, v); else this.iram[d & 0x7ff] = v;
        s = (s + 1) & 0xffffff; d = (d + 1) & 0xffffff;
      }
      // the CPU shares the bus with the DMA: about one cycle per byte (two for BW-RAM)
      this.clock += n * ((src === 1 || toBw) ? 4 : 2);
      this.cfrDma = true;
      this.updateIrqs();
    }
    tileInfo() {
      const depth = this.cdma & 3;                 // 0 = 8bpp, 1 = 4bpp, 2 = 2bpp
      const bpp = depth === 0 ? 8 : depth === 1 ? 4 : 2;
      const w = 1 << Math.min(5, (this.cdma >> 2) & 7); // characters across
      return { bpp, w, tileBytes: bpp * 8 };
    }
    // Character conversion 1: the SNES DMA reads BW-RAM from SDA and gets tiles
    startCc1() {
      this.cc1 = true;
      this.sfrDma = true; // tells the SNES CPU to start its DMA
      this.updateIrqs();
    }
    cc1Byte(i) {
      const base = this.sda & 0x3ffff;
      const rel = (i & this.bwMask) - (base & this.bwMask);
      if (rel < 0) return -1;
      const { bpp, w, tileBytes } = this.tileInfo();
      const t = Math.floor(rel / tileBytes), b = rel % tileBytes;
      const tx = t % w, ty = Math.floor(t / w);
      const row = (b >> 1) & 7, plane = ((b >> 4) << 1) | (b & 1);
      const lineAddr = base + (ty * 8 + row) * (w * bpp) + tx * bpp;
      let out = 0;
      for (let x = 0; x < 8; x++) {
        const bitPos = x * bpp;
        const byte = this.bw[(lineAddr + (bitPos >> 3)) & this.bwMask];
        const px = (byte >> (bitPos & 7)) & ((1 << bpp) - 1);
        out |= ((px >> plane) & 1) << (7 - x);
      }
      // keep the I-RAM buffer in step (two characters, used in turn)
      this.iram[((this.dda & 0x7ff) + (t & 1) * tileBytes + b) & 0x7ff] = out;
      return out;
    }
    // Character conversion 2: the SA-1 writes pixels to BRF, rows land in I-RAM
    cc2Row(half) {
      const { bpp, tileBytes } = this.tileInfo();
      const line = this.cc2Line & 15;
      const base = (this.dda & 0x7ff) + (line >> 3) * tileBytes + (line & 7) * 2;
      const mask = (1 << bpp) - 1;
      for (let p = 0; p < bpp; p++) {
        let byte = 0;
        for (let x = 0; x < 8; x++) byte |= (((this.brf[half + x] & mask) >> p) & 1) << (7 - x);
        this.iram[(base + (p >> 1) * 16 + (p & 1)) & 0x7ff] = byte;
      }
      this.cc2Line = (this.cc2Line + 1) & 15;
      this.clock += 2 * bpp;
    }

    // ---------------------------------------------------------------
    // H/V timer
    // ---------------------------------------------------------------
    timerGeom() {
      const linear = (this.tmc & 0x80) !== 0;
      const L = linear ? 512 : 341;
      return { L, F: linear ? 512 * 512 : 341 * this.snes.lines };
    }
    // next master-clock time (after `from`) that the timer fires, or Infinity
    timerTarget() {
      const en = this.tmc & 3;
      if (!en) return { t: 0, P: 0 };
      const { L, F } = this.timerGeom();
      if (en === 1) return { t: this.hcnt, P: L };
      if (en === 2) return { t: this.vcnt * L, P: F };
      return { t: this.vcnt * L + this.hcnt, P: F };
    }
    latchTimer() {
      const { L, F } = this.timerGeom();
      const d = Math.floor((this.clock - this.timerBase) / 4) % F;
      this.hLatch = d % L; this.vLatch = Math.floor(d / L);
    }
    checkTimer(c0, c1) {
      const tg = this.timerTarget();
      if (!tg.P) return;
      const d0 = Math.floor((c0 - this.timerBase) / 4), d1 = Math.floor((c1 - this.timerBase) / 4);
      if (Math.floor((d1 - tg.t) / tg.P) > Math.floor((d0 - tg.t) / tg.P)) { this.cfrTimer = true; this.updateIrqs(); }
    }
    nextTimerClock(from) {
      const tg = this.timerTarget();
      if (!tg.P) return Infinity;
      const d = Math.floor((from - this.timerBase) / 4);
      const k = Math.floor((d - tg.t) / tg.P) + 1;
      return this.timerBase + (tg.t + k * tg.P) * 4;
    }

    // ---------------------------------------------------------------
    // Running
    // ---------------------------------------------------------------
    catchUp(target) {
      if (this.clock >= target) return;
      if (this.ccnt & 0x60) { this.checkTimer(this.clock, target); this.clock = target; return; } // held in reset or waiting
      const cpu = this.cpu, timer = (this.tmc & 3) !== 0;
      while (this.clock < target) {
        if (cpu.isWaiting() && !cpu.irqLine && !cpu.nmiPending) {
          // asleep (WAI): skip ahead to the next thing that could wake it
          const next = Math.min(target, timer ? this.nextTimerClock(this.clock) : Infinity);
          const c0 = this.clock;
          this.clock = Math.max(next, c0 + 2);
          if (timer) this.checkTimer(c0, this.clock);
          continue;
        }
        const c0 = this.clock;
        cpu.step();
        if (timer) this.checkTimer(c0, this.clock);
      }
    }

    // ---------------------------------------------------------------
    // Save states
    // ---------------------------------------------------------------
    saveState() {
      return {
        cpu: this.cpu.getState(), iram: this.iram.slice(), brf: this.brf.slice(), bw: this.ownBw ? this.bw.slice() : null, mmc: this.mmc.slice(),
        m: [this.clock, this.mdr, this.ccnt, this.sie, this.crv, this.cnv, this.civ, this.bmaps, this.sbwe, this.bwpa, this.siwp,
          this.scnt, this.cie, this.snv, this.siv, this.tmc, this.hcnt, this.vcnt, this.bmap, this.cbwe, this.ciwp,
          this.dcnt, this.cdma, this.sda, this.dda, this.dtc, this.bbf, this.mcnt, this.ma, this.mb, this.mr, this.of,
          this.vbd, this.vda, this.vbit, this.sfrIrq ? 1 : 0, this.sfrDma ? 1 : 0, this.cfrIrq ? 1 : 0, this.cfrTimer ? 1 : 0,
          this.cfrDma ? 1 : 0, this.cfrNmi ? 1 : 0, this.cc1 ? 1 : 0, this.cc2Line, this.timerBase, this.hLatch, this.vLatch, this.running ? 1 : 0],
      };
    }
    loadState(s) {
      this.cpu.setState(s.cpu); this.iram.set(s.iram); this.brf.set(s.brf); if (s.bw && this.ownBw) this.bw.set(s.bw); this.mmc = s.mmc.slice();
      const m = s.m;
      [this.clock, this.mdr, this.ccnt, this.sie, this.crv, this.cnv, this.civ, this.bmaps, this.sbwe, this.bwpa, this.siwp,
        this.scnt, this.cie, this.snv, this.siv, this.tmc, this.hcnt, this.vcnt, this.bmap, this.cbwe, this.ciwp,
        this.dcnt, this.cdma, this.sda, this.dda, this.dtc, this.bbf, this.mcnt, this.ma, this.mb, this.mr, this.of,
        this.vbd, this.vda, this.vbit] = m;
      this.sfrIrq = !!m[35]; this.sfrDma = !!m[36]; this.cfrIrq = !!m[37]; this.cfrTimer = !!m[38];
      this.cfrDma = !!m[39]; this.cfrNmi = !!m[40]; this.cc1 = !!m[41];
      [this.cc2Line, this.timerBase, this.hLatch, this.vLatch] = m.slice(42, 46); this.running = !!m[46];
      this.snesIrq = (this.sfrIrq && (this.sie & 0x80) !== 0) || (this.sfrDma && (this.sie & 0x20) !== 0);
    }
  }

  global.RPSA1 = SA1;
})(typeof window !== "undefined" ? window : globalThis);
