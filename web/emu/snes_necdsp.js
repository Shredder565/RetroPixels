// RetroPixels — NEC uPD77C25 (DSP-3, DSP-4) and uPD96050 (ST010, ST011)
// signal processors, emulated instruction by instruction. Original code,
// written from the processors' documented instruction set.
//
// These chips run a program stored inside the chip. That program is Nintendo /
// Seta code, so RetroPixels doesn't ship it: the player supplies their own dump
// once (the page keeps it), and it arrives here as opts.firmware["DSP-3"] etc.
// A dump appended to the end of the game ROM works too.
//
// Firmware file layout (the common "dsp3.rom" / "st010.rom" convention):
// the program ROM's 24-bit words, 3 bytes each, low byte first, then the data
// ROM's 16-bit words, 2 bytes each, low byte first. Dumps saved high byte first
// are recognised too (the order that makes more of the program's jump
// instructions valid wins).
//   uPD77C25: 2048 program words + 1024 data words = 8192 bytes
//   uPD96050: 16384 program words + 2048 data words = 53248 bytes
//
// The processor in brief
//   Accumulators A and B (16-bit) with flags S1 S0 C Z OV1 OV0 each; a 16 x 16
//   multiplier (K x L -> M:N, 31-bit product shifted left once); TR and TRB
//   temporaries; DP (data RAM pointer), RP (data ROM pointer); a small return
//   stack; DR (data register) and SR (status register) shared with the SNES.
//   Instructions (24-bit) come in four kinds, chosen by the top two bits:
//     00 OP  ALU operation + one register move + DP/RP adjustments
//     01 RT  the same, then return from subroutine
//     10 JP  conditional jump / call
//     11 LD  load a 16-bit immediate into a register
//
// Talking to the SNES: DR is read and written a byte at a time (8-bit mode,
// SR.DRC = 1) or as two bytes low then high (16-bit mode, SR.DRS says which
// byte is next). RQM ("request for master") is raised when the chip's program
// puts data in DR or takes data out of DR, and drops when the SNES has moved
// the byte(s). The SNES sees the top half of SR.
//
// Timing: the chip catches up with the SNES clock whenever the SNES touches
// it, at 7.6 million instructions a second for the uPD77C25 and 11 / 15 million
// for the uPD96050 (ST010 / ST011). When the program sits in a loop jumping to
// itself (waiting for the SNES) the rest of the time slice is skipped.
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;

  // Flag bits (one set per accumulator)
  const OV0 = 1, OV1 = 2, Z = 4, C = 8, S0 = 16, S1 = 32;
  // SR bits
  const RQM = 0x8000, DRS = 0x1000, DRC = 0x0400, SR_WRITABLE = 0x6f83;

  const MODELS = {
    upd7725: { prog: 2048, data: 1024, ram: 256, stack: 4, dpMask: 0xff, rpMask: 0x3ff, klm: 0x40, pcMask: 0x7ff, big: false },
    upd96050: { prog: 16384, data: 2048, ram: 2048, stack: 16, dpMask: 0x7ff, rpMask: 0x7ff, klm: 0x400, pcMask: 0x3fff, big: true },
  };
  const fwSize = (m) => MODELS[m].prog * 3 + MODELS[m].data * 2;

  // Jump conditions that exist on the chip (9-bit codes); used to tell the
  // firmware's byte order.
  const VALID_BRANCH = new Set([0x100, 0x140]);
  for (let c = 0x080; c <= 0x0be; c += 2) VALID_BRANCH.add(c);
  [0x0b1, 0x0b3].forEach((c) => VALID_BRANCH.add(c));

  // Split a firmware dump into program and data words.
  function parseFirmware(bytes, model) {
    const M = MODELS[model];
    if (!bytes || bytes.length !== fwSize(model)) return null;
    const read = (bigEndian) => {
      const prog = new Uint32Array(M.prog), data = new Uint16Array(M.data);
      for (let i = 0; i < M.prog; i++) {
        const a = bytes[i * 3], b = bytes[i * 3 + 1], c = bytes[i * 3 + 2];
        prog[i] = bigEndian ? (a << 16) | (b << 8) | c : (c << 16) | (b << 8) | a;
      }
      const d0 = M.prog * 3;
      for (let i = 0; i < M.data; i++) {
        const a = bytes[d0 + i * 2], b = bytes[d0 + i * 2 + 1];
        data[i] = bigEndian ? (a << 8) | b : (b << 8) | a;
      }
      return { prog, data };
    };
    const score = (prog) => {
      let s = 0;
      for (let i = 0; i < prog.length; i++) {
        const w = prog[i];
        if (w >>> 22 !== 2) continue;
        const brch = (w >> 13) & 0x1ff;
        if (VALID_BRANCH.has(brch) && (M.big || !(w & 3))) s++; else s--;
      }
      return s;
    };
    const le = read(false), be = read(true);
    return score(be.prog) > score(le.prog) ? be : le;
  }

  // ===================================================================
  // The processor
  // ===================================================================
  class NECDSP {
    constructor(model, fw) {
      const M = this.M = MODELS[model];
      this.model = model;
      this.prog = fw ? fw.prog : new Uint32Array(M.prog);
      this.drom = fw ? fw.data : new Uint16Array(M.data);
      this.ram = new Uint16Array(M.ram);
      this.stack = new Uint16Array(M.stack);
      this.reset();
    }
    reset() {
      this.pc = 0; this.sp = 0; this.dp = 0; this.rp = 0;
      this.a = 0; this.b = 0; this.fa = 0; this.fb = 0;
      this.tr = 0; this.trb = 0; this.k = 0; this.l = 0; this.m = 0; this.n = 0;
      this.dr = 0; this.sr = 0; this.si = 0; this.so = 0;
      this.budget = 0; this.stack.fill(0);
    }

    // ---------- the SNES side of DR / SR ----------
    hostReadSR() { return this.sr >> 8; }
    hostReadDR() {
      if (this.sr & DRC) { this.sr &= ~RQM; return this.dr & 0xff; }
      if (!(this.sr & DRS)) { this.sr |= DRS; return this.dr & 0xff; }
      this.sr &= ~(DRS | RQM); return this.dr >> 8;
    }
    hostWriteDR(v) {
      if (this.sr & DRC) { this.dr = (this.dr & 0xff00) | v; this.sr &= ~RQM; return; }
      if (!(this.sr & DRS)) { this.dr = (this.dr & 0xff00) | v; this.sr |= DRS; return; }
      this.dr = (this.dr & 0x00ff) | (v << 8); this.sr &= ~(DRS | RQM);
    }

    // ---------- running ----------
    run(n) {
      this.budget += n;
      while (this.budget > 0) { this.budget--; this.step(); }
    }
    step() {
      const at = this.pc, op = this.prog[at];
      this.pc = (at + 1) & this.M.pcMask;
      switch (op >>> 22) {
        case 0: this.opALU(op); break;
        case 1: this.opALU(op); this.sp = (this.sp - 1) & (this.M.stack - 1); this.pc = this.stack[this.sp]; break;
        case 2: this.opJump(op, at); break;
        default: this.moveTo((op >> 6) & 0xffff, op & 15); this.multiply(); break;
      }
    }
    multiply() {
      const p = ((this.k << 16) >> 16) * ((this.l << 16) >> 16) * 2;
      this.m = (p >> 16) & 0xffff; this.n = p & 0xffff;
    }

    // value put on the internal bus by a source code
    source(s) {
      switch (s) {
        case 0: return this.trb;
        case 1: return this.a;
        case 2: return this.b;
        case 3: return this.tr;
        case 4: return this.dp;
        case 5: return this.rp;
        case 6: return this.drom[this.rp];
        case 7: return this.fa & S1 ? 0x8000 : 0x7fff; // SGN: the value to saturate to
        case 8: this.sr |= RQM; return this.dr; // DR, asking the SNES for the next value
        case 9: return this.dr; // DRNF: DR without the request
        case 10: return this.sr;
        case 11: case 12: return this.si; // serial input (nothing connected on the SNES)
        case 13: return this.k;
        case 14: return this.l;
        default: return this.ram[this.dp];
      }
    }
    // move a bus value to a destination code
    moveTo(v, d) {
      switch (d) {
        case 0: break;
        case 1: this.a = v; break;
        case 2: this.b = v; break;
        case 3: this.tr = v; break;
        case 4: this.dp = v & this.M.dpMask; break;
        case 5: this.rp = v & this.M.rpMask; break;
        case 6: this.dr = v; this.sr |= RQM; break; // DR, asking the SNES to read it
        case 7: this.sr = (this.sr & ~SR_WRITABLE) | (v & SR_WRITABLE); break;
        case 8: case 9: this.so = v; break; // serial output (LSB / MSB first)
        case 10: this.k = v; break;
        case 11: this.k = v; this.l = this.drom[this.rp]; break; // KLR
        case 12: this.l = v; this.k = this.ram[this.dp | this.M.klm]; break; // KLM
        case 13: this.l = v; break;
        case 14: this.trb = v; break;
        default: this.ram[this.dp] = v; break;
      }
    }

    opALU(op) {
      const pselect = (op >> 20) & 3, fn = (op >> 16) & 15, asl = (op >> 15) & 1;
      const idb = this.source((op >> 4) & 15);
      if (fn) {
        const p = pselect === 0 ? this.ram[this.dp] : pselect === 1 ? idb : pselect === 2 ? this.m : this.n;
        this.alu(fn, p, asl);
      }
      this.moveTo(idb, op & 15);
      // DP low nibble: none / +1 / -1 / clear (it wraps inside the nibble)
      const dpl = (op >> 13) & 3;
      let dp = this.dp;
      if (dpl === 1) dp = (dp & ~15) | ((dp + 1) & 15);
      else if (dpl === 2) dp = (dp & ~15) | ((dp - 1) & 15);
      else if (dpl === 3) dp &= ~15;
      dp ^= ((op >> 9) & 15) << 4; // DP high bits flipped by DPHM
      this.dp = dp & this.M.dpMask;
      if (op & 0x100) this.rp = (this.rp - 1) & this.M.rpMask;
      this.multiply();
    }

    alu(fn, p, asl) {
      const q = asl ? this.b : this.a;
      const otherC = (asl ? this.fa : this.fb) & C ? 1 : 0; // ADC / SBB / SHL1 use the other accumulator's carry
      let r, carry = 0, ov = 0, arith = false;
      switch (fn) {
        case 1: r = q | p; break;
        case 2: r = q & p; break;
        case 3: r = q ^ p; break;
        case 4: case 6: case 8: { // SUB, SBB, DEC
          const s = fn === 8 ? 1 : p, full = q - s - (fn === 6 ? otherC : 0);
          r = full & 0xffff; carry = full < 0 ? 1 : 0; ov = (q ^ s) & (q ^ r) & 0x8000; arith = true; break;
        }
        case 5: case 7: case 9: { // ADD, ADC, INC
          const s = fn === 9 ? 1 : p, full = q + s + (fn === 7 ? otherC : 0);
          r = full & 0xffff; carry = full >> 16; ov = (q ^ r) & (s ^ r) & 0x8000; arith = true; break;
        }
        case 10: r = ~q & 0xffff; break; // CMP: one's complement
        case 11: r = (q >> 1) | (q & 0x8000); carry = q & 1; break; // SHR1 (sign kept)
        case 12: r = ((q << 1) | otherC) & 0xffff; carry = q >> 15; break; // SHL1
        case 13: r = ((q << 2) | 3) & 0xffff; break; // SHL2 (ones shifted in)
        default: r = fn === 14 ? ((q << 4) | 15) & 0xffff : ((q >> 8) | (q << 8)) & 0xffff; break; // SHL4, XCHG
      }
      let f = asl ? this.fb : this.fa;
      const s0 = r & 0x8000 ? S0 : 0;
      if (arith) {
        // OV1 remembers an overflow until a later one in the other direction
        // cancels it; S1 is then the true sign of the running result.
        let ov1 = f & OV1;
        if (ov) ov1 ^= OV1;
        f = s0 | (r ? 0 : Z) | (carry ? C : 0) | (ov ? OV0 : 0) | ov1 | ((ov1 ? !s0 : s0) ? S1 : 0);
      } else {
        f = s0 | (r ? 0 : Z) | (carry ? C : 0) | (s0 ? S1 : 0);
      }
      if (asl) { this.b = r; this.fb = f; } else { this.a = r; this.fa = f; }
    }

    opJump(op, at) {
      const brch = (op >> 13) & 0x1ff, M = this.M;
      const target = M.big ? (at & 0x2000) | ((op & 3) << 11) | ((op >> 2) & 0x7ff) : (op >> 2) & 0x7ff;
      let take;
      if (brch === 0x100) take = true;
      else if (brch === 0x140) { this.stack[this.sp] = this.pc; this.sp = (this.sp + 1) & (M.stack - 1); this.pc = target; return; }
      else if (M.big && brch === 0x000) { this.pc = this.so & M.pcMask; return; } // JMPSO
      else if (M.big && brch === 0x101) { this.pc = target | 0x2000; return; } // jump into the upper program half
      else if (M.big && brch === 0x141) { this.stack[this.sp] = this.pc; this.sp = (this.sp + 1) & (M.stack - 1); this.pc = target | 0x2000; return; }
      else {
        const fa = this.fa, fb = this.fb, dpl = this.dp & 15;
        switch (brch) {
          case 0x080: take = !(fa & C); break; case 0x082: take = !!(fa & C); break;
          case 0x084: take = !(fb & C); break; case 0x086: take = !!(fb & C); break;
          case 0x088: take = !(fa & Z); break; case 0x08a: take = !!(fa & Z); break;
          case 0x08c: take = !(fb & Z); break; case 0x08e: take = !!(fb & Z); break;
          case 0x090: take = !(fa & OV0); break; case 0x092: take = !!(fa & OV0); break;
          case 0x094: take = !(fb & OV0); break; case 0x096: take = !!(fb & OV0); break;
          case 0x098: take = !(fa & OV1); break; case 0x09a: take = !!(fa & OV1); break;
          case 0x09c: take = !(fb & OV1); break; case 0x09e: take = !!(fb & OV1); break;
          case 0x0a0: take = !(fa & S0); break; case 0x0a2: take = !!(fa & S0); break;
          case 0x0a4: take = !(fb & S0); break; case 0x0a6: take = !!(fb & S0); break;
          case 0x0a8: take = !(fa & S1); break; case 0x0aa: take = !!(fa & S1); break;
          case 0x0ac: take = !(fb & S1); break; case 0x0ae: take = !!(fb & S1); break;
          case 0x0b0: take = dpl === 0; break; case 0x0b1: take = dpl !== 0; break;
          case 0x0b2: take = dpl === 15; break; case 0x0b3: take = dpl !== 15; break;
          case 0x0b4: take = true; break; case 0x0b6: take = false; break; // serial in never acknowledged
          case 0x0b8: take = true; break; case 0x0ba: take = false; break; // serial out never acknowledged
          case 0x0bc: take = !(this.sr & RQM); break; case 0x0be: take = !!(this.sr & RQM); break;
          default: take = false;
        }
      }
      if (!take) return;
      this.pc = target;
      // A jump to itself can only end when the SNES does something: skip ahead.
      if (target === at) this.budget = 0;
    }

    saveState() {
      return {
        ram: this.ram.slice(), stack: this.stack.slice(),
        r: [this.pc, this.sp, this.dp, this.rp, this.a, this.b, this.fa, this.fb, this.tr, this.trb, this.k, this.l, this.m, this.n, this.dr, this.sr, this.si, this.so, this.budget],
      };
    }
    loadState(o) {
      this.ram.set(o.ram); this.stack.set(o.stack);
      [this.pc, this.sp, this.dp, this.rp, this.a, this.b, this.fa, this.fb, this.tr, this.trb, this.k, this.l, this.m, this.n, this.dr, this.sr, this.si, this.so, this.budget] = o.r;
    }
  }

  // ===================================================================
  // Finding the firmware
  // ===================================================================
  const FILES = {
    "DSP-1": { file: "dsp1.rom", model: "upd7725" },
    "DSP-2": { file: "dsp2.rom", model: "upd7725" },
    "DSP-3": { file: "dsp3.rom", model: "upd7725" },
    "DSP-4": { file: "dsp4.rom", model: "upd7725" },
    "ST010": { file: "st010.rom", model: "upd96050" },
    "ST011": { file: "st011.rom", model: "upd96050" },
  };
  // Returns the parsed program for `chip`, or null when the player hasn't given one.
  function findFirmware(snes, opts, chip) {
    const spec = FILES[chip], size = fwSize(spec.model), cart = snes.cart;
    const given = opts && opts.firmware && opts.firmware[chip];
    if (given && given.length) {
      const fw = parseFirmware(given, spec.model);
      if (!fw) {
        const e = needFirmware(chip);
        e.message = `The ${chip} chip's program (${spec.file}) must be ${size / 1024} KB (${size} bytes), but the file given is ${given.length} bytes. Choose the right file.`;
        throw e;
      }
      return fw;
    }
    // A dump appended to the game ROM: the file is bigger than its header says by exactly the program's size.
    const romSize = 0x400 << (cart.rom[cart.headerAt + 0x17] & 15);
    if (cart.size === romSize + size) {
      const fw = parseFirmware(cart.rom.subarray(romSize), spec.model);
      cart.rom = cart.rom.slice(0, romSize); cart.size = romSize;
      return fw;
    }
    return null;
  }
  function needFirmware(chip) {
    const spec = FILES[chip], size = fwSize(spec.model);
    const e = new Error(`This game needs the ${chip} chip's program (${spec.file}, ${size / 1024} KB). Choose the file to continue.`);
    e.needsFirmware = { chip, file: spec.file, size, alt: [] };
    return e;
  }

  // ===================================================================
  // Cartridge glue
  // ===================================================================
  // Keeps a processor in step with the SNES clock.
  class Clocked {
    constructor(snes, dsp, hz) {
      this.snes = snes; this.dsp = dsp;
      // (made while the SNES is still being built, so its clock rate comes from the cartridge)
      const P = global.RPSNESParts, master = snes.cart.pal ? P.PAL_HZ : P.NTSC_HZ;
      this.rate = hz / master;
      this.maxGap = master / 15; // never catch up more than a few frames at once
      this.last = 0; this.frac = 0;
    }
    sync() {
      const now = this.snes.clock;
      let d = now - this.last;
      this.last = now;
      if (d <= 0) return;
      if (d > this.maxGap) d = this.maxGap;
      this.frac += d * this.rate;
      const n = Math.floor(this.frac);
      this.frac -= n;
      if (n > 0) this.dsp.run(n);
    }
    reset() { this.dsp.reset(); this.last = this.snes.clock; this.frac = 0; }
    readDR() { this.sync(); return this.dsp.hostReadDR(); }
    readSR() { this.sync(); return this.dsp.hostReadSR(); }
    writeDR(v) { this.sync(); this.dsp.hostWriteDR(v); }
    saveState() { return { dsp: this.dsp.saveState(), last: this.last, frac: this.frac }; }
    loadState(o) { this.dsp.loadState(o.dsp); this.last = o.last; this.frac = o.frac; }
  }

  // DSP-3 / DSP-4: DR at $8000-$BFFF and SR at $C000-$FFFF of banks b0..b1 (and $80+ mirrors).
  class DSPn {
    constructor(snes, opts, chip, b0, b1) {
      const fw = findFirmware(snes, opts, chip);
      if (!fw) throw needFirmware(chip);
      this.snes = snes; this.b0 = b0; this.b1 = b1;
      this.port = new Clocked(snes, new NECDSP("upd7725", fw), 7600000);
      Kit.claim(snes, b0, b1, 0x8000, 0xffff, true);
    }
    hit(bank) { const b = bank & 0x7f; return b >= this.b0 && b <= this.b1; }
    read(bank, off) {
      if (!this.hit(bank) || off < 0x8000) return -1;
      return off < 0xc000 ? this.port.readDR() : this.port.readSR();
    }
    write(bank, off, v) {
      if (!this.hit(bank) || off < 0x8000) return false;
      if (off < 0xc000) this.port.writeDR(v);
      return true;
    }
    reset() { this.port.reset(); }
    saveState() { return this.port.saveState(); }
    loadState(o) { this.port.loadState(o); }
  }

  // ST010 / ST011: DR at $60-$67:xxx0 and SR at $60-$67:xxx1 (offsets $0000-$0FFF),
  // the chip's 4 KB data RAM at $68-$6F:0000-0FFF; banks $E0-$EF mirror them.
  // ST010's data RAM is kept by a battery.
  class ST01x {
    constructor(snes, opts, chip) {
      const fw = findFirmware(snes, opts, chip);
      if (!fw) throw needFirmware(chip);
      this.snes = snes; this.chip = chip;
      this.dsp = new NECDSP("upd96050", fw);
      this.port = new Clocked(snes, this.dsp, chip === "ST010" ? 11000000 : 15000000);
      this.battery = chip === "ST010";
      if (this.battery && opts && opts.sram && opts.sram.length >= 0x1000) {
        const s = opts.sram, r = this.dsp.ram;
        for (let i = 0; i < 0x800; i++) r[i] = s[i * 2] | (s[i * 2 + 1] << 8);
      }
      Kit.claim(snes, 0x60, 0x6f, 0x0000, 0x0fff, true);
    }
    read(bank, off) {
      const b = bank & 0x7f;
      if (b < 0x60 || b > 0x6f || off > 0x0fff) return -1;
      if (b < 0x68) return off & 1 ? this.port.readSR() : this.port.readDR();
      this.port.sync();
      const w = this.dsp.ram[off >> 1];
      return off & 1 ? w >> 8 : w & 0xff;
    }
    write(bank, off, v) {
      const b = bank & 0x7f;
      if (b < 0x60 || b > 0x6f || off > 0x0fff) return false;
      if (b < 0x68) { if (!(off & 1)) this.port.writeDR(v); return true; }
      this.port.sync();
      const r = this.dsp.ram, i = off >> 1;
      r[i] = off & 1 ? (r[i] & 0x00ff) | (v << 8) : (r[i] & 0xff00) | v;
      return true;
    }
    reset() { this.port.reset(); }
    getSRAM() {
      if (!this.battery) return null;
      const r = this.dsp.ram, o = new Uint8Array(0x1000);
      for (let i = 0; i < 0x800; i++) { o[i * 2] = r[i] & 0xff; o[i * 2 + 1] = r[i] >> 8; }
      return o;
    }
    saveState() { return this.port.saveState(); }
    loadState(o) { this.port.loadState(o); }
  }

  // Listed ahead of the "not supported yet" entries in snes_chips.js.
  const list = global.RPSNESChips;
  list.unshift(
    { name: "DSP-3", detect: (c) => Kit.dspKind(c) === 3, create: (snes, opts) => new DSPn(snes, opts, "DSP-3", 0x20, 0x3f) },
    { name: "DSP-4", detect: (c) => Kit.dspKind(c) === 4, create: (snes, opts) => new DSPn(snes, opts, "DSP-4", 0x30, 0x3f) },
    { name: "ST010", detect: (c) => c.chip === 0xf6 && Kit.titleHas(c, "F1 ROC"), create: (snes, opts) => new ST01x(snes, opts, "ST010") },
    { name: "ST011", detect: (c) => c.chip === 0xf6, create: (snes, opts) => new ST01x(snes, opts, "ST011") },
  );
  // Used by snes_dsp.js (a supplied DSP-1 / DSP-2 program) and RetroPixels' own tests.
  global.RPNECDSP = { NECDSP, Clocked, parseFirmware, findFirmware, needFirmware, fwSize, MODELS };
})(typeof window !== "undefined" ? window : globalThis);
