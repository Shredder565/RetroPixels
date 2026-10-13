// RetroPixels — Atari 2600 DPC+ cartridges (Harmony / Melody). Original code.
//
// DPC+ is the successor of Pitfall II's DPC chip, run in software by the ARM
// processor on a Harmony or Melody cartridge. The 32K file holds the ARM
// driver (3K), six 4K banks for the 6507, 4K of display data and a 1K table
// of note frequencies. The driver's work for the 6507 (data fetchers, random
// numbers, music, copy / fill functions) is done here directly; the game's
// own ARM code (CALLFUNCTION 254 / 255) runs on RetroPixels' ARM7TDMI core
// (arm7.js), in the same memory layout the cartridge has:
//   0x00000000  the cartridge's 32K flash (the game file)
//   0x40000000  8K of RAM: driver copy, display data at 0x40000C00,
//               frequency table at 0x40001C00 (the custom code's variables and
//               stack live in the top part of it)
"use strict";

(function (global) {
  const Carts = global.RPAtariCarts;
  const CPU_HZ = 3579545 / 3;
  const RANDOM_INIT = 0x2b435044;  // "DPC+"
  const RANDOM_TAPS = 0x10adab1e;
  const RETURN_ADDR = 0x0fffff00;   // where the ARM code returns to when it's done
  const ARM_LIMIT = 20000000;       // instructions; a runaway routine is stopped after this many

  class DPCPlusCart extends Carts.Cart {
    constructor(rom, type) {
      // Some files are missing the padding at the end; make room for all 32K.
      if (rom.length < 32768) { const r = new Uint8Array(32768); r.set(rom); rom = r; }
      super(rom, type);
      this.flash = rom.subarray(0, 32768);
      this.ram8k = new Uint8Array(8192);
      this.ram8k.set(this.flash.subarray(0, 0x0c00), 0);
      this.ram8k.set(this.flash.subarray(0x6c00, 0x7c00), 0x0c00);
      this.ram8k.set(this.flash.subarray(0x7c00, 0x8000), 0x1c00);
      this.bank = 5;
      this.ptr = new Uint16Array(8);          // data fetcher pointers (12 bits)
      this.fptr = new Uint16Array(8);         // fractional fetcher pointers
      this.ffrac = new Uint8Array(8); this.finc = new Uint8Array(8);
      this.tops = new Uint8Array(8); this.bots = new Uint8Array(8);
      this.random = RANDOM_INIT;
      this.fast = false; this.ldaNext = false;
      this.params = new Uint8Array(8); this.nParams = 0;
      this.wave = new Uint8Array(3); this.freq = new Uint32Array(3); this.phase = new Uint32Array(3);
      this.musicCycles = 0;
    }
    fixedKeys() { return super.fixedKeys().concat(["arm"]); }

    // ---- 6507 side ----
    read(a, m) {
      const off = a & 0xfff;
      let v;
      if (off < 0x28) v = this.reg(off, m);
      else {
        if (off >= 0xff6 && off <= 0xffb) this.bank = off - 0xff6;
        v = this.flash[0x0c00 + this.bank * 4096 + off];
      }
      // Fast fetch: the operand of LDA #imm (if below $28) reads that register instead.
      if (this.ldaNext) {
        this.ldaNext = false;
        if (v < 0x28 && off >= 0x28) v = this.reg(v, m);
      } else if (this.fast && m && m.isOp && v === 0xa9) this.ldaNext = true;
      return v;
    }
    write(a, v, m) {
      const off = a & 0xfff;
      if (off >= 0x28 && off < 0x80) this.regWrite(off, v, m);
      else if (off >= 0xff6 && off <= 0xffb) this.bank = off - 0xff6;
    }
    disp(i) { return this.ram8k[0x0c00 + (i & 0xfff)]; }
    inWindow(i) { const p = this.ptr[i] & 0xff; return ((p - this.tops[i] - 1) & 0xff) < ((this.bots[i] - this.tops[i] - 1) & 0xff); }
    reg(r, m) {
      const i = r & 7;
      switch (r >> 3) {
        case 0:
          switch (r) {
            case 0: this.clockRandom(); return this.random & 0xff;
            case 1: this.unclockRandom(); return this.random & 0xff;
            case 2: return (this.random >>> 8) & 0xff;
            case 3: return (this.random >>> 16) & 0xff;
            case 4: return (this.random >>> 24) & 0xff;
            case 5: return this.amplitude(m);
            default: return 0;
          }
        case 1: { const v = this.disp(this.ptr[i]); this.ptr[i] = (this.ptr[i] + 1) & 0xfff; return v; }
        case 2: { const v = this.inWindow(i) ? this.disp(this.ptr[i]) : 0; this.ptr[i] = (this.ptr[i] + 1) & 0xfff; return v; }
        case 3: {
          const v = this.disp(this.fptr[i]);
          const f = this.ffrac[i] + this.finc[i];
          this.ffrac[i] = f & 0xff;
          if (f > 0xff) this.fptr[i] = (this.fptr[i] + 1) & 0xfff;
          return v;
        }
        case 4: return i < 4 ? (this.inWindow(i) ? 0xff : 0) : 0;
      }
      return 0;
    }
    regWrite(r, v, m) {
      const i = r & 7;
      switch (r >> 3) {
        case 5: this.fptr[i] = (this.fptr[i] & 0xf00) | v; this.ffrac[i] = 0; return;      // $28 DFxFRACLOW
        case 6: this.fptr[i] = (this.fptr[i] & 0x0ff) | ((v & 15) << 8); this.ffrac[i] = 0; return; // $30 DFxFRACHI
        case 7: this.finc[i] = v; this.ffrac[i] = 0; return;                                // $38 DFxFRACINC
        case 8: this.tops[i] = v; return;                                                   // $40 DFxTOP
        case 9: this.bots[i] = v; return;                                                   // $48 DFxBOT
        case 10: this.ptr[i] = (this.ptr[i] & 0xf00) | v; return;                           // $50 DFxLOW
        case 11:
          switch (r) {
            case 0x58: this.fast = v === 0; return;                                         // FASTFETCH
            case 0x59: if (this.nParams < 8) this.params[this.nParams++] = v; return;       // PARAMETER
            case 0x5a: this.callFunction(v, m); return;                                     // CALLFUNCTION
            case 0x5d: case 0x5e: case 0x5f: this.syncMusic(m); this.wave[r - 0x5d] = v & 0x7f; return; // WAVEFORMx
          }
          return;
        case 12: this.ptr[i] = (this.ptr[i] - 1) & 0xfff; this.ram8k[0x0c00 + this.ptr[i]] = v; return; // $60 DFxPUSH
        case 13: this.ptr[i] = (this.ptr[i] & 0x0ff) | ((v & 15) << 8); return;             // $68 DFxHI
        case 14:
          if (r === 0x70) this.random = RANDOM_INIT;                                         // RRESET
          else if (r <= 0x74) { const sh = (r - 0x71) * 8; this.random = ((this.random & ~(0xff << sh)) | (v << sh)) >>> 0; } // RWRITEx
          else if (r <= 0x77) { this.syncMusic(m); this.freq[r - 0x75] = this.ram32(0x1c00 + v * 4); } // NOTEx
          return;
        case 15: this.ram8k[0x0c00 + this.ptr[i]] = v; this.ptr[i] = (this.ptr[i] + 1) & 0xfff; return; // $78 DFxWRITE
      }
    }
    ram32(o) { const r = this.ram8k; o &= 0x1ffc; return (r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | (r[o + 3] << 24)) >>> 0; }

    // 32-bit random number generator, steppable both ways.
    clockRandom() {
      const r = this.random;
      this.random = ((r >>> 11) | (r << 21)) ^ ((r & 0x400) ? RANDOM_TAPS : 0);
      this.random >>>= 0;
    }
    unclockRandom() {
      let r = this.random;
      if (r & 0x80000000) r ^= RANDOM_TAPS;
      this.random = ((r << 11) | (r >>> 21)) >>> 0;
    }

    // ---- music: three 32-step waveforms in display data, stepped at 20 kHz ----
    syncMusic(m) {
      if (!m) return;
      const steps = Math.floor((m.cycles - this.musicCycles) * 20000 / CPU_HZ);
      if (steps <= 0) return;
      this.musicCycles += steps * CPU_HZ / 20000;
      for (let v = 0; v < 3; v++) this.phase[v] = (this.phase[v] + Math.imul(this.freq[v], steps)) >>> 0;
    }
    amplitude(m) {
      this.syncMusic(m);
      let s = 0;
      for (let v = 0; v < 3; v++) s += this.ram8k[0x0c00 + ((this.wave[v] << 5) + (this.phase[v] >>> 27) & 0xfff)];
      return s & 0xff;
    }

    // ---- functions ----
    callFunction(fn, m) {
      const p = this.params;
      switch (fn) {
        case 0: break;
        case 1: { // copy from the 6507 banks into display data at fetcher p[2]'s pointer
          let src = 0x0c00 + ((p[1] << 8) | p[0]), dst = this.ptr[p[2] & 7];
          for (let k = 0; k < p[3]; k++) this.ram8k[0x0c00 + ((dst + k) & 0xfff)] = this.flash[(src + k) & 0x7fff];
          break;
        }
        case 2: { // fill display data at fetcher p[2]'s pointer with p[0]
          const dst = this.ptr[p[2] & 7];
          for (let k = 0; k < p[3]; k++) this.ram8k[0x0c00 + ((dst + k) & 0xfff)] = p[0];
          break;
        }
        case 254: case 255: this.runARM(); break;
      }
      this.nParams = 0;
    }

    // ---- the game's own ARM code ----
    armBus() {
      const flash = this.flash, ram = this.ram8k;
      const rd8 = (a) => { a >>>= 0; if (a < 0x8000) return flash[a]; if (a >= 0x40000000 && a < 0x40002000) return ram[a - 0x40000000]; return 0; };
      const wr8 = (a, v) => { a >>>= 0; if (a >= 0x40000000 && a < 0x40002000) ram[a - 0x40000000] = v; };
      const rd16 = (a) => { a &= ~1; return rd8(a) | (rd8(a + 1) << 8); };
      const rd32 = (a) => { a &= ~3; return (rd8(a) | (rd8(a + 1) << 8) | (rd8(a + 2) << 16) | (rd8(a + 3) << 24)) | 0; };
      return {
        read8: rd8, read16: rd16, read32: rd32, fetch16: rd16, fetch32: rd32,
        write8: (a, v) => wr8(a, v & 0xff),
        write16: (a, v) => { a &= ~1; wr8(a, v & 0xff); wr8(a + 1, (v >> 8) & 0xff); },
        write32: (a, v) => { a &= ~3; wr8(a, v & 0xff); wr8(a + 1, (v >> 8) & 0xff); wr8(a + 2, (v >> 16) & 0xff); wr8(a + 3, (v >>> 24) & 0xff); },
      };
    }
    runARM() {
      if (!global.RPARM7) return;
      if (!this.arm) this.arm = new global.RPARM7(this.armBus());
      const c = this.arm;
      c.reset();
      c.r[13] = 0x40001fdc;            // the driver's stack for the custom code
      c.r[14] = RETURN_ADDR | 1;       // returning lands here (in Thumb state)
      c.i = 1; c.f = 1;
      c.branch(0x0c00);                // custom code entry (ARM state)
      for (let n = 0; n < ARM_LIMIT; n++) {
        if ((c.pc & ~1) === RETURN_ADDR) return;
        c.step();
      }
      throw new Error("This game's ARM code didn't finish (DPC+).");
    }
  }

  Carts.register("DPC+", DPCPlusCart);
})(typeof window !== "undefined" ? window : globalThis);
