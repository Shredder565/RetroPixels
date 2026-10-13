// RetroPixels — SNES ST018 chip (Hayazashi Nidan Morita Shougi 2). Original code,
// written from the documented behaviour of the chip.
//
// The ST018 is a small ARM computer (ARMv3, about 21 MHz) on the cartridge with
// 128 KB program ROM, 32 KB data ROM and 16 KB RAM. It talks to the SNES through
// a few registers at $00-$3F/$80-$BF:3800-38FF. Its program is Seta's code and
// can't ship with RetroPixels, so the player supplies their own copy
// (st018.rom, 163840 bytes: program ROM then data ROM, both as stored in the
// chip, 32-bit words little-endian). The ARM runs on RetroPixels' ARM core
// (arm7.js) — the ST018's ARMv3 instructions are a subset of what it runs.
//
// ARM memory map:
//   $00000000  program ROM (128 KB, mirrored)
//   $40000000  ports to the SNES:  +$00 write: byte to the SNES
//                                  +$10 read: byte from the SNES, write: raise the "signal" flag
//                                  +$20 read: status (same bits as the SNES sees)
//   $A0000000  data ROM (32 KB, mirrored)
//   $E0000000  RAM (16 KB, mirrored)
// SNES side:
//   $3800 read   byte from the ARM (clears status bit 0)
//   $3802 read   acknowledge the ARM's signal (clears status bit 2)
//   $3802 write  byte to the ARM (sets status bit 3 until the ARM reads it)
//   $3804 read   status: bit 0 byte from the ARM waiting, bit 2 ARM signal,
//                bit 3 byte to the ARM not read yet, bit 7 ARM running
//   $3804 write  bit 0: 1 = hold the ARM in reset, 0 = let it run
//
// Timing: the ARM is clocked by the SNES master clock, so one ARM cycle is
// counted as one master clock; each memory access costs one cycle (approximate).
// The ARM runs lazily: it catches up to the SNES whenever the SNES touches its
// registers (nothing else can see it), and a polling loop that can't change
// anything until the SNES acts is skipped ahead in one go.
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;
  const PROG = 0x20000, DATA = 0x8000, RAM = 0x4000, FW = PROG + DATA;

  function needFirmware(msg) {
    const e = new Error(msg);
    e.needsFirmware = { chip: "ST018", file: "st018.rom", size: FW, alt: [] };
    return e;
  }

  // Find the chip's program: from the player's firmware files, or appended to the game ROM.
  function findFirmware(cart, opts) {
    const fw = opts && opts.firmware && opts.firmware.ST018;
    if (fw && fw.length) {
      if (fw.length !== FW) throw needFirmware(`The ST018 chip program should be 160 KB (${FW} bytes), but the chosen file is ${fw.length} bytes. Choose the right file to continue.`);
      return new Uint8Array(fw);
    }
    const declared = 0x400 << cart.rom[cart.headerAt + 0x17];
    if (cart.rom.length === declared + FW) {
      const tail = cart.rom.slice(declared);
      cart.rom = cart.rom.subarray(0, declared); cart.size = declared; // the game itself
      return tail;
    }
    throw needFirmware("This game needs the ST018 chip's program (st018.rom, 160 KB). Choose the file to continue.");
  }

  const words = (bytes, off, n) => { const w = new Int32Array(n); for (let i = 0; i < n; i++) { const k = off + i * 4; w[i] = bytes[k] | (bytes[k + 1] << 8) | (bytes[k + 2] << 16) | (bytes[k + 3] << 24); } return w; };

  class ST018 {
    constructor(snes, opts) {
      if (!global.RPARM7) throw new Error("RetroPixels' ARM processor (arm7.js) isn't loaded.");
      this.snes = snes;
      const fw = findFirmware(snes.cart, opts);
      this.prog = words(fw, 0, PROG >> 2);
      this.data = words(fw, PROG, DATA >> 2);
      this.ram = new Uint8Array(RAM);
      this.arm = new global.RPARM7(this.makeBus());
      Kit.claim(snes, 0x00, 0x3f, 0x3000, 0x3fff, true);
      this.held = false;
      this.reset();
    }

    // ---- the SNES <-> ARM ports ----
    status() { return this.toSnesFull | (this.signal << 2) | (this.toArmFull << 3) | (this.held ? 0 : 0x80); }
    resetArm() {
      this.ram.fill(0);
      this.toSnes = 0; this.toSnesFull = 0; this.toArm = 0; this.toArmFull = 0; this.signal = 0;
      this.arm.reset(); this.arm.cycles = 0;
      this.changed();
    }
    reset() {
      this.held = false;
      this.clock = this.snes.clock;
      this.resetArm();
    }
    changed() { this.ver = (this.ver + 1) | 0; this.pollPC = -1; }

    // ---- the ARM's bus ----
    makeBus() {
      const self = this;
      const rd32 = (a) => {
        self.arm.cycles++;
        switch (a >>> 29) {
          case 0: return self.prog[(a >>> 2) & 0x7fff];
          case 2: return self.io(a);
          case 5: return self.data[(a >>> 2) & 0x1fff];
          case 7: { const r = self.ram, k = a & 0x3ffc; return r[k] | (r[k + 1] << 8) | (r[k + 2] << 16) | (r[k + 3] << 24); }
          default: return 0;
        }
      };
      const rd8 = (a) => {
        if ((a >>> 29) === 7) { self.arm.cycles++; return self.ram[a & 0x3fff]; }
        if ((a >>> 29) === 2) { self.arm.cycles++; return self.io(a); }
        return (rd32(a) >>> ((a & 3) * 8)) & 0xff;
      };
      const wr = (a, v, size) => {
        self.arm.cycles++;
        const top = a >>> 29;
        if (top === 7) {
          const r = self.ram;
          if (size === 1) r[a & 0x3fff] = v;
          else if (size === 2) { const k = a & 0x3ffe; r[k] = v; r[k + 1] = v >> 8; }
          else { const k = a & 0x3ffc; r[k] = v; r[k + 1] = v >> 8; r[k + 2] = v >> 16; r[k + 3] = v >> 24; }
          self.changed();
        } else if (top === 2) self.ioWrite(a, v & 0xff);
      };
      return {
        fetch32: rd32, read32: rd32, read8: rd8,
        read16: (a) => (rd32(a) >>> ((a & 2) * 8)) & 0xffff, fetch16: (a) => (rd32(a) >>> ((a & 2) * 8)) & 0xffff,
        write8: (a, v) => wr(a, v, 1), write16: (a, v) => wr(a, v, 2), write32: (a, v) => wr(a, v, 4),
      };
    }
    io(a) {
      switch (a & 0x3c) {
        case 0x10: if (this.toArmFull) { this.toArmFull = 0; this.changed(); } return this.toArm;
        case 0x20: this.pollCheck(); return this.status();
        default: return 0;
      }
    }
    ioWrite(a, v) {
      switch (a & 0x3c) {
        case 0x00: this.toSnes = v; this.toSnesFull = 1; break;
        case 0x10: this.signal = 1; break;
        default: return;
      }
      this.changed();
    }
    // The ARM reads its status port. If its whole state is the same as at the last
    // status read here and nothing has changed since, it is in a waiting loop that
    // will spin until the SNES acts: skip ahead to the SNES's time.
    pollCheck() {
      const arm = this.arm, r = arm.r, s = this.pollRegs || (this.pollRegs = new Int32Array(17));
      const cpsr = arm.getCPSR();
      if (this.pollPC === arm.pc && this.pollVer === this.ver && s[16] === cpsr) {
        let same = true;
        for (let k = 0; k < 16; k++) if (s[k] !== r[k]) { same = false; break; }
        if (same) { this.idle = true; return; }
      }
      this.pollPC = arm.pc; this.pollVer = this.ver; s.set(r); s[16] = cpsr;
    }

    // Run the ARM up to the SNES's current time.
    sync() {
      const target = this.snes.clock;
      if (this.held) { this.clock = target; return; }
      const arm = this.arm;
      this.idle = false;
      while (this.clock < target) {
        arm.cycles = 0;
        arm.step();
        this.clock += arm.cycles || 1;
        if (this.idle) { this.idle = false; if (this.clock < target) this.clock = target; }
      }
    }

    // ---- SNES side ----
    read(bank, off) {
      if (off < 0x3800 || off >= 0x3900) return -1;
      this.sync();
      switch (off) {
        case 0x3800: if (this.toSnesFull) { this.toSnesFull = 0; this.changed(); } return this.toSnes;
        case 0x3802: if (this.signal) { this.signal = 0; this.changed(); } return 0;
        case 0x3804: return this.status();
        default: return -1;
      }
    }
    write(bank, off, v) {
      if (off < 0x3800 || off >= 0x3900) return false;
      this.sync();
      if (off === 0x3802) { this.toArm = v; this.toArmFull = 1; this.changed(); }
      else if (off === 0x3804) {
        const hold = (v & 1) === 1;
        if (hold && !this.held) this.resetArm();
        this.held = hold;
        this.changed();
      }
      return true;
    }

    saveState() {
      return { arm: this.arm.saveState(), ram: this.ram.slice(), clock: this.clock, held: this.held ? 1 : 0,
        ports: [this.toSnes, this.toSnesFull, this.toArm, this.toArmFull, this.signal] };
    }
    loadState(o) {
      this.arm.loadState(o.arm); this.ram.set(o.ram); this.clock = o.clock; this.held = !!o.held;
      [this.toSnes, this.toSnesFull, this.toArm, this.toArmFull, this.signal] = o.ports;
      this.changed();
    }
  }

  // ST018 cartridges: chip byte $F5 on a LoROM board ($F5 with map mode $3A is SPC7110)
  const lorom = (c) => (Kit.mapMode(c) & 0x0f) === 0 || (Kit.mapMode(c) & 0x0f) === 2 || c.map === "lo";
  global.RPSNESChips.push({ name: "ST018", detect: (c) => c.chip === 0xf5 && lorom(c) && Kit.mapMode(c) !== 0x3a, create: (snes, opts) => new ST018(snes, opts) });
  global.RPSNESST018 = ST018;
})(typeof window !== "undefined" ? window : globalThis);
