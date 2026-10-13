// RetroPixels — NES / Famicom emulator. Original code.
//
// Machine = 6502 CPU (shared RPCPU6502) + PPU (graphics, emulated dot by dot)
// + APU (sound) + cartridge mapper. Every CPU memory access advances the PPU
// three dots and the APU one cycle, so timing-based effects behave as on hardware.
"use strict";

(function (global) {
  const CPU_HZ = 1789773;           // NTSC CPU clock
  const FPS = 60.0988;

  // ===================================================================
  // Palette: computed from the NES's composite video levels (NTSC model).
  // ===================================================================
  function buildPalette() {
    // Signal levels per luma row, low and high halves of the waveform (black = 0, white = 1).
    const lo = [-0.117, 0.000, 0.308, 0.715], hi = [0.397, 0.681, 1.000, 1.000];
    const pal = new Uint32Array(512);
    for (let emph = 0; emph < 8; emph++) {
      for (let c = 0; c < 64; c++) {
        const hue = c & 15, lum = (c >> 4) & 3;
        let y = 0, i = 0, q = 0;
        for (let p = 0; p < 12; p++) {
          // 12 phases per colour cycle; the colour's square wave is "high" for 6 of them.
          let level;
          if (hue === 0) level = hi[lum];
          else if (hue >= 13) level = hue === 13 ? lo[lum] : lo[1];
          else level = ((p + hue + 12 - 1) % 12) < 6 ? hi[lum] : lo[lum];
          // Emphasis bits darken the phases tied to red, green and blue.
          if (hue < 14 && emph) {
            const r = (emph & 1) && ((p + 12 - 0) % 12) < 6;
            const g = (emph & 2) && ((p + 12 - 4) % 12) < 6;
            const b = (emph & 4) && ((p + 12 - 8) % 12) < 6;
            if (r || g || b) level = level * 0.746 - 0.0912 * 0.746 * 0;
          }
          const ang = Math.PI * (p + 3) / 6; // colour-burst phase reference
          y += level; i += level * Math.cos(ang); q += level * Math.sin(ang);
        }
        y /= 12; i /= 12; q /= 12;
        const sat = 1.6;
        const r = y + sat * (0.956 * i + 0.621 * q);
        const g = y + sat * (-0.272 * i - 0.647 * q);
        const b = y + sat * (-1.106 * i + 1.703 * q);
        const to8 = (v) => Math.max(0, Math.min(255, Math.round(Math.pow(Math.max(0, v), 1 / 1.1) * 255)));
        pal[emph * 64 + c] = 0xff000000 | (to8(b) << 16) | (to8(g) << 8) | to8(r);
      }
    }
    return pal;
  }
  const PALETTE = buildPalette();

  // ===================================================================
  // Cartridges (mappers)
  // ===================================================================
  const MIRROR = { H: 0, V: 1, ONE_LO: 2, ONE_HI: 3, FOUR: 4, CUSTOM: 5 };

  // Mapper state for save states: every plain field of the mapper (numbers, flags,
  // typed arrays and nested chip objects) is saved automatically.
  const NO_SAVE = new Set(["cart", "prg", "chr", "chrRam", "prgRam", "prgMap", "chrMap", "mirror", "irq", "ciram", "nes", "mapper", "audio"]);
  function packFields(o) {
    const r = {};
    for (const k of Object.keys(o)) {
      if (NO_SAVE.has(k)) continue;
      const v = o[k];
      if (v === null || typeof v === "number" || typeof v === "boolean" || typeof v === "string") r[k] = v;
      else if (ArrayBuffer.isView(v)) r[k] = v.slice();
      else if (Array.isArray(v)) r[k] = v.map(x => (x && typeof x === "object" && !ArrayBuffer.isView(x)) ? packFields(x) : (ArrayBuffer.isView(x) ? x.slice() : x));
      else if (typeof v === "object") r[k] = packFields(v);
    }
    return r;
  }
  function unpackFields(o, r) {
    for (const k of Object.keys(r)) {
      if (NO_SAVE.has(k)) continue;
      const v = r[k], cur = o[k];
      if (ArrayBuffer.isView(cur) && v && v.length !== undefined) cur.set(v.length > cur.length ? v.subarray(0, cur.length) : v);
      else if (Array.isArray(cur) && Array.isArray(v)) {
        for (let i = 0; i < v.length; i++) {
          if (cur[i] && typeof cur[i] === "object" && !ArrayBuffer.isView(cur[i])) unpackFields(cur[i], v[i]);
          else if (ArrayBuffer.isView(cur[i])) cur[i].set(v[i]); else cur[i] = v[i];
        }
      } else if (cur && typeof cur === "object" && v && typeof v === "object") unpackFields(cur, v);
      else if (typeof cur !== "function") o[k] = v;
    }
  }

  class Mapper {
    constructor(cart) {
      this.cart = cart;
      this.prg = cart.prg; this.chr = cart.chr; this.chrRam = cart.chrRam;
      this.prgRam = new Uint8Array(0x2000);
      this.mirror = cart.mirror;
      this.irq = false;
      this.prgMap = new Int32Array(4);  // 4 × 8 KB windows at $8000
      this.chrMap = new Int32Array(8);  // 8 × 1 KB windows
      this.setPrg32(0);
      this.setChr8(0);
    }
    prgBanks8() { return this.prg.length >> 13; }
    chrBanks1() { return this.chr.length >> 10; }
    setPrg8(slot, bank) { const n = this.prgBanks8(); this.prgMap[slot] = ((bank % n) + n) % n * 0x2000; }
    setPrg16(slot, bank) { this.setPrg8(slot * 2, bank * 2); this.setPrg8(slot * 2 + 1, bank * 2 + 1); }
    setPrg32(bank) { for (let i = 0; i < 4; i++) this.setPrg8(i, bank * 4 + i); }
    setChr1(slot, bank) { const n = this.chrBanks1(); this.chrMap[slot] = ((bank % n) + n) % n * 0x400; }
    setChr2(slot, bank) { this.setChr1(slot * 2, bank * 2); this.setChr1(slot * 2 + 1, bank * 2 + 1); }
    setChr4(slot, bank) { for (let i = 0; i < 4; i++) this.setChr1(slot * 4 + i, bank * 4 + i); }
    setChr8(bank) { for (let i = 0; i < 8; i++) this.setChr1(i, bank * 8 + i); }
    // Map each of the four nametables to a 1 KB page of nametable RAM.
    setNt(a, b, c, d) { this.mirror = MIRROR.CUSTOM; this.ntPages = [a, b, c, d]; }
    cpuRead(a) {
      if (a >= 0x8000) return this.prg[this.prgMap[(a >> 13) & 3] + (a & 0x1fff)];
      if (a >= 0x6000) return this.prgRam[a & 0x1fff];
      return -1; // open bus
    }
    cpuWrite(a, v) { if (a >= 0x6000 && a < 0x8000) this.prgRam[a & 0x1fff] = v; }
    ppuRead(a) { return this.chr[this.chrMap[a >> 10] + (a & 0x3ff)]; }
    ppuWrite(a, v) { if (this.chrRam) this.chr[this.chrMap[a >> 10] + (a & 0x3ff)] = v; }
    ntIndex(a) { // nametable address ($2000-$2FFF) -> index into 4 KB VRAM
      const t = (a >> 10) & 3, off = a & 0x3ff;
      switch (this.mirror) {
        case MIRROR.H: return ((t >> 1) << 10) | off;
        case MIRROR.V: return ((t & 1) << 10) | off;
        case MIRROR.ONE_LO: return off;
        case MIRROR.ONE_HI: return 0x400 | off;
        case MIRROR.CUSTOM: return (this.ntPages[t] << 10) | off;
        default: return (t << 10) | off;
      }
    }
    cpuClock() {}
    scanline() {}
    saveState() { return { prgRam: this.prgRam, chr: this.chrRam ? this.chr : null, mirror: this.mirror, irq: this.irq,
      prgMap: this.prgMap, chrMap: this.chrMap, regs: this.regs ? this.regs() : null, fields: packFields(this),
      audio: this.audio ? packFields(this.audio) : null }; }
    loadState(o) {
      this.prgRam.set(o.prgRam); if (o.chr && this.chrRam) this.chr.set(o.chr);
      if (o.fields) unpackFields(this, o.fields);
      if (o.audio && this.audio) unpackFields(this.audio, o.audio);
      this.mirror = o.mirror; this.irq = o.irq; this.prgMap.set(o.prgMap); this.chrMap.set(o.chrMap);
      if (o.regs && this.setRegs) this.setRegs(o.regs);
      if (this.afterLoad) this.afterLoad();
    }
  }

  // 0 — NROM
  class M0 extends Mapper {}

  // 1 — MMC1 (Zelda, Metroid, Mega Man 2…)
  class M1 extends Mapper {
    constructor(cart) {
      super(cart);
      this.shift = 0x10; this.ctrl = 0x0c; this.chr0 = 0; this.chr1 = 0; this.prgReg = 0; this.lastWrite = -10;
      // SOROM / SXROM boards have 16-32 KB of RAM, banked by CHR register bits.
      const ram = (cart.prgRamSize || 0) + (cart.prgNvramSize || 0);
      if (ram > 0x2000) this.prgRam = new Uint8Array(ram);
      this.ramBank = 0;
      this.canDisableRam = cart.mapper === 1; // the MMC1A (mapper 155) can't switch its RAM off
      this.noRam = cart.nes2 && ram === 0;
      this.update();
    }
    cpuRead(a) {
      if (a >= 0x6000 && a < 0x8000) return !this.noRam && (this.ramEnabled || !this.canDisableRam) ? this.prgRam[this.ramBank + (a & 0x1fff)] : -1;
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a < 0x8000) { if (a >= 0x6000 && (this.ramEnabled || !this.canDisableRam)) this.prgRam[this.ramBank + (a & 0x1fff)] = v; return; }
      // Writes on back-to-back cycles (from RMW instructions) are ignored by the chip.
      const now = this.cart.cpuCycles(); const consecutive = now - this.lastWrite < 2; this.lastWrite = now;
      if (v & 0x80) { this.shift = 0x10; this.ctrl |= 0x0c; this.update(); return; }
      if (consecutive) return;
      const done = this.shift & 1;
      this.shift = (this.shift >> 1) | ((v & 1) << 4);
      if (done) {
        const r = this.shift; this.shift = 0x10;
        switch ((a >> 13) & 3) {
          case 0: this.ctrl = r; break;
          case 1: this.chr0 = r; break;
          case 2: this.chr1 = r; break;
          case 3: this.prgReg = r; break;
        }
        this.update();
      }
    }
    update() {
      this.mirror = [MIRROR.ONE_LO, MIRROR.ONE_HI, MIRROR.V, MIRROR.H][this.ctrl & 3];
      // Large boards (SUROM, 512 KB) use a CHR bit to pick the 256 KB PRG half.
      const outer = this.prg.length > 0x40000 ? (this.chr0 & 0x10) : 0;
      const p = (this.prgReg & 0x0f) | outer;
      switch ((this.ctrl >> 2) & 3) {
        case 0: case 1: this.setPrg32(p >> 1); break;
        case 2: this.setPrg16(0, outer); this.setPrg16(1, p); break;
        case 3: this.setPrg16(0, p); this.setPrg16(1, outer | 0x0f); break;
      }
      if (this.ctrl & 0x10) { this.setChr4(0, this.chr0); this.setChr4(1, this.chr1); }
      else this.setChr8(this.chr0 >> 1);
      this.ramEnabled = !(this.prgReg & 0x10);
      const banks = this.prgRam.length >> 13;
      this.ramBank = banks > 1 ? (((this.chr0 >> 2) & 3) % banks) << 13 : 0;
    }
    regs() { return [this.shift, this.ctrl, this.chr0, this.chr1, this.prgReg]; }
    setRegs(r) { [this.shift, this.ctrl, this.chr0, this.chr1, this.prgReg] = r; this.update(); }
  }

  // 2 — UxROM (Mega Man, Castlevania, Contra…)
  class M2 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(1, -1); }
    cpuWrite(a, v) { if (a >= 0x8000) this.setPrg16(0, v & (this.cart.busConflict ? this.cpuRead(a) : 0xff)); else super.cpuWrite(a, v); }
  }

  // 3 — CNROM
  class M3 extends Mapper {
    cpuWrite(a, v) { if (a >= 0x8000) this.setChr8(v & this.cpuRead(a)); else super.cpuWrite(a, v); }
  }

  // 4 — MMC3 (SMB3, Kirby, TMNT 2 & 3…)
  class M4 extends Mapper {
    constructor(cart) {
      super(cart);
      this.bankSel = 0; this.r = new Uint8Array(8); this.r.set([0, 2, 4, 5, 6, 7, 0, 1]);
      this.irqLatch = 0; this.irqCounter = 0; this.irqReload = false; this.irqEnable = false;
      this.lastA12 = 0; this.a12LowSince = 0;
      this.fourScreen = cart.mirror === MIRROR.FOUR;
      // $A001 RAM enable/write-protect. Old (iNES) headers can't tell MMC3 from MMC6 games,
      // whose RAM control works differently, so it's only honoured for NES 2.0 MMC3 headers.
      this.ramCtl = 0x80; this.useRamCtl = (cart.mapper === 4 || cart.mapper === 118 || cart.mapper === 119) && cart.nes2 && cart.submapper !== 1;
      this.revA = cart.mapper === 4 && cart.submapper === 4; // MMC3A: no IRQ on a natural reload to 0
      this.update();
    }
    cpuRead(a) {
      if (a >= 0x6000 && a < 0x8000 && this.useRamCtl && !(this.ramCtl & 0x80)) return -1;
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a < 0x8000) { if (!this.useRamCtl || (this.ramCtl & 0xc0) === 0x80) super.cpuWrite(a, v); return; }
      const even = !(a & 1);
      switch (a & 0xe000) {
        case 0x8000: if (even) this.bankSel = v; else this.r[this.bankSel & 7] = v; this.update(); break;
        case 0xa000: if (even) { if (!this.fourScreen) this.mirror = v & 1 ? MIRROR.H : MIRROR.V; } else this.ramCtl = v; break;
        case 0xc000: if (even) this.irqLatch = v; else { this.irqCounter = 0; this.irqReload = true; } break;
        case 0xe000: if (even) { this.irqEnable = false; this.irq = false; } else this.irqEnable = true; break;
      }
    }
    update() {
      const r = this.r;
      if (this.bankSel & 0x40) { this.setPrg8(0, -2); this.setPrg8(2, r[6]); }
      else { this.setPrg8(0, r[6]); this.setPrg8(2, -2); }
      this.setPrg8(1, r[7]); this.setPrg8(3, -1);
      const inv = (this.bankSel & 0x80) ? 4 : 0;
      this.setChr1(0 ^ inv, r[0] & 0xfe); this.setChr1(1 ^ inv, r[0] | 1);
      this.setChr1(2 ^ inv, r[1] & 0xfe); this.setChr1(3 ^ inv, r[1] | 1);
      this.setChr1(4 ^ inv, r[2]); this.setChr1(5 ^ inv, r[3]); this.setChr1(6 ^ inv, r[4]); this.setChr1(7 ^ inv, r[5]);
    }
    // The scanline counter is clocked by rising edges of PPU address line A12
    // (after it has been low for a little while).
    ppuAddr(a) {
      const a12 = (a >> 12) & 1;
      if (a12 && !this.lastA12) {
        if (this.cart.cpuCycles() - this.a12LowSince >= 3) this.clockIrq();
      } else if (!a12 && this.lastA12) {
        this.a12LowSince = this.cart.cpuCycles();
      }
      this.lastA12 = a12;
    }
    ppuRead(a) { this.ppuAddr(a); return super.ppuRead(a); }
    clockIrq() {
      const was = this.irqCounter, forced = this.irqReload;
      if (this.irqCounter === 0 || this.irqReload) { this.irqCounter = this.irqLatch; this.irqReload = false; }
      else this.irqCounter--;
      if (this.irqCounter === 0 && this.irqEnable && (!this.revA || was !== 0 || forced)) this.irq = true;
    }
    regs() { return [this.bankSel, ...this.r, this.irqLatch, this.irqCounter, this.irqReload ? 1 : 0, this.irqEnable ? 1 : 0, this.lastA12, this.a12LowSince]; }
    setRegs(x) {
      this.bankSel = x[0]; this.r.set(x.slice(1, 9)); this.irqLatch = x[9]; this.irqCounter = x[10];
      this.irqReload = !!x[11]; this.irqEnable = !!x[12]; this.lastA12 = x[13]; this.a12LowSince = x[14]; this.update();
    }
  }

  // 7 — AxROM (Battletoads, Wizards & Warriors…)
  class M7 extends Mapper {
    constructor(cart) { super(cart); this.mirror = MIRROR.ONE_LO; }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      this.setPrg32(v & 0x0f); this.mirror = v & 0x10 ? MIRROR.ONE_HI : MIRROR.ONE_LO;
    }
  }

  // 9 / 10 — MMC2 / MMC4 (Punch-Out!!, Fire Emblem): CHR latches flip on special tiles.
  class M9 extends Mapper {
    constructor(cart, mmc4) {
      super(cart); this.mmc4 = mmc4;
      this.cr = new Uint8Array(4); this.latch = [1, 1];
      if (mmc4) { this.setPrg16(1, -1); } else { this.setPrg8(1, -3); this.setPrg8(2, -2); this.setPrg8(3, -1); }
      this.updateChr();
    }
    cpuWrite(a, v) {
      if (a < 0xa000) return super.cpuWrite(a, v);
      switch (a & 0xf000) {
        case 0xa000: if (this.mmc4) this.setPrg16(0, v & 0x0f); else this.setPrg8(0, v & 0x0f); break;
        case 0xb000: this.cr[0] = v & 0x1f; break;
        case 0xc000: this.cr[1] = v & 0x1f; break;
        case 0xd000: this.cr[2] = v & 0x1f; break;
        case 0xe000: this.cr[3] = v & 0x1f; break;
        case 0xf000: this.mirror = v & 1 ? MIRROR.H : MIRROR.V; break;
      }
      this.updateChr();
    }
    updateChr() { this.setChr4(0, this.cr[this.latch[0] ? 1 : 0]); this.setChr4(1, this.cr[this.latch[1] ? 3 : 2]); }
    ppuRead(a) {
      const v = super.ppuRead(a);
      if (a === 0x0fd8 || (this.mmc4 && (a & 0x1ff0) === 0x0fd0)) { this.latch[0] = 0; this.updateChr(); }
      else if (a === 0x0fe8 || (this.mmc4 && (a & 0x1ff0) === 0x0fe0)) { this.latch[0] = 1; this.updateChr(); }
      else if ((a & 0x1ff8) === 0x1fd8) { this.latch[1] = 0; this.updateChr(); }
      else if ((a & 0x1ff8) === 0x1fe8) { this.latch[1] = 1; this.updateChr(); }
      return v;
    }
    regs() { return [...this.cr, ...this.latch]; }
    setRegs(r) { this.cr.set(r.slice(0, 4)); this.latch = [r[4], r[5]]; this.updateChr(); }
  }

  // 11 — Color Dreams; 66 — GxROM; 140 — Jaleco
  class M11 extends Mapper { cpuWrite(a, v) { if (a >= 0x8000) { this.setPrg32(v & 3); this.setChr8(v >> 4); } else super.cpuWrite(a, v); } }
  class M66 extends Mapper { cpuWrite(a, v) { if (a >= 0x8000) { this.setPrg32((v >> 4) & 3); this.setChr8(v & 3); } else super.cpuWrite(a, v); } }
  class M140 extends Mapper { cpuWrite(a, v) { if (a >= 0x6000 && a < 0x8000) { this.setPrg32((v >> 4) & 3); this.setChr8(v & 15); } } }

  // 13 — CPROM (Videomation): switchable upper 4 KB of CHR RAM
  class M13 extends Mapper {
    constructor(cart) { super(cart); this.setChr4(0, 0); this.setChr4(1, 0); }
    cpuWrite(a, v) { if (a >= 0x8000) this.setChr4(1, v & 3); else super.cpuWrite(a, v); }
  }

  // 34 — BNROM / NINA-001 (NINA-001 boards have CHR ROM; BNROM has CHR RAM)
  class M34 extends Mapper {
    constructor(cart) { super(cart); this.nina = cart.submapper === 1 || (cart.submapper !== 2 && !cart.chrRam); }
    cpuWrite(a, v) {
      if (a >= 0x8000) { if (!this.nina) this.setPrg32(v & this.cpuRead(a)); return; }
      if (this.nina) {
        if (a === 0x7ffd) this.setPrg32(v & 1);
        else if (a === 0x7ffe) this.setChr4(0, v & 15);
        else if (a === 0x7fff) this.setChr4(1, v & 15);
      }
      super.cpuWrite(a, v);
    }
  }

  // 69 — Sunsoft FME-7 (Batman: Return of the Joker, Gimmick!) — banking and IRQ
  class M69 extends Mapper {
    constructor(cart) {
      super(cart); this.cmd = 0; this.ramMode = 0; this.ramBank = 0; this.irqEnable = 0; this.irqCount = 0;
      this.setPrg8(3, -1);
      if (global.RPNesAudio) this.audio = new global.RPNesAudio.S5B(); // Sunsoft 5B sound (Gimmick!)
    }
    cpuRead(a) {
      if (a >= 0x6000 && a < 0x8000) {
        if (this.ramMode & 0x40) return this.ramMode & 0x80 ? this.prgRam[a & 0x1fff] : -1;
        return this.prg[((this.ramBank % this.prgBanks8()) * 0x2000) + (a & 0x1fff)];
      }
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x8000) { if ((this.ramMode & 0xc0) === 0xc0) this.prgRam[a & 0x1fff] = v; return; }
      if (a >= 0x8000 && a < 0xa000) { this.cmd = v & 15; return; }
      if (a >= 0xc000) { if (this.audio) { if (a < 0xe000) this.audio.writeAddr(v); else this.audio.writeData(v); } return; }
      if (a >= 0xa000 && a < 0xc000) {
        const c = this.cmd;
        if (c < 8) this.setChr1(c, v);
        else if (c === 8) { this.ramMode = v; this.ramBank = v & 0x3f; }
        else if (c < 12) this.setPrg8(c - 9, v & 0x3f);
        else if (c === 12) this.mirror = [MIRROR.V, MIRROR.H, MIRROR.ONE_LO, MIRROR.ONE_HI][v & 3];
        else if (c === 13) { this.irqEnable = v; this.irq = false; }
        else if (c === 14) this.irqCount = (this.irqCount & 0xff00) | v;
        else this.irqCount = (this.irqCount & 0xff) | (v << 8);
      }
    }
    cpuClock() {
      if (this.audio) this.audio.clock();
      if (this.irqEnable & 0x80) {
        this.irqCount = (this.irqCount - 1) & 0xffff;
        if (this.irqCount === 0xffff && (this.irqEnable & 1)) this.irq = true;
      }
    }
    regs() { return [this.cmd, this.ramMode, this.ramBank, this.irqEnable, this.irqCount]; }
    setRegs(r) { [this.cmd, this.ramMode, this.ramBank, this.irqEnable, this.irqCount] = r; }
  }

  // 71 — Camerica (Micro Machines, Fire Hawk); 180 — UNROM with fixed first bank (Crazy Climber)
  class M71 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(1, -1); }
    cpuWrite(a, v) {
      if (a >= 0xc000) this.setPrg16(0, v & 0x0f);
      else if (a >= 0x9000 && a < 0xa000) this.mirror = v & 0x10 ? MIRROR.ONE_HI : MIRROR.ONE_LO;
      else super.cpuWrite(a, v);
    }
  }
  class M180 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(0, 0); }
    cpuWrite(a, v) { if (a >= 0x8000) this.setPrg16(1, v & 7); else super.cpuWrite(a, v); }
  }

  // 206 — Namco 108 (MMC3 without IRQs or mirroring control)
  class M206 extends M4 {
    cpuWrite(a, v) { if (a >= 0x8000 && a < 0xa000) { if (!(a & 1)) this.bankSel = v & 7; else this.r[this.bankSel & 7] = v & 0x3f; this.update(); } else Mapper.prototype.cpuWrite.call(this, a, v); }
    ppuRead(a) { return Mapper.prototype.ppuRead.call(this, a); }
  }

  // 5 — MMC5 (Castlevania III, Just Breed, many romhacks)
  class M5 extends Mapper {
    constructor(cart) {
      super(cart);
      this.prgRam = new Uint8Array(0x10000);
      this.exram = new Uint8Array(0x400);
      this.prgMode = 3; this.chrMode = 3; this.ramProt1 = 0; this.ramProt2 = 0; this.exMode = 0; this.ntMap = 0;
      this.fillTile = 0; this.fillAttr = 0; this.prgRegs = new Uint8Array([0, 0, 0, 0, 0xff]); // $5113-$5117
      this.chrA = new Uint16Array(8); this.chrB = new Uint16Array(4); this.chrHi = 0; this.lastSet = 0;
      this.irqCompare = 0; this.irqEnable = false; this.irqPending = false; this.inFrame = false; this.lineCount = 0;
      this.mulA = 0xff; this.mulB = 0xff; this.exAttr = 0;
      this.splitCtl = 0; this.splitScroll = 0; this.splitBank = 0; this.inSplit = false; this.splitFineY = 0; this.splitAttr = 0;
      this.ciram = cart.vram; // the console's 2 KB of nametable RAM
      if (global.RPNesAudio) this.audio = new global.RPNesAudio.MMC5Audio();
      this.win = [null, null, null, null];
      this.updatePrg();
    }
    get irq() { return this.irqPending && this.irqEnable; }
    set irq(v) {}
    updatePrg() {
      // Each of the four 8 KB windows at $8000 resolves to ROM or RAM.
      const set = (slot, reg, bankSize) => {
        const v = this.prgRegs[reg], rom = reg === 4 || (v & 0x80);
        let bank8 = v & 0x7f;
        if (bankSize === 32) bank8 = (bank8 & ~3) | slot;
        else if (bankSize === 16) bank8 = (bank8 & ~1) | (slot & 1);
        this.win[slot] = rom ? { rom: true, off: (bank8 % this.prgBanks8()) * 0x2000 } : { rom: false, off: (bank8 & 7) * 0x2000 };
      };
      switch (this.prgMode) {
        case 0: for (let i = 0; i < 4; i++) set(i, 4, 32); break;
        case 1: set(0, 2, 16); set(1, 2, 16); set(2, 4, 16); set(3, 4, 16); break;
        case 2: set(0, 2, 16); set(1, 2, 16); set(2, 3, 8); set(3, 4, 8); break;
        default: set(0, 1, 8); set(1, 2, 8); set(2, 3, 8); set(3, 4, 8); break;
      }
    }
    cpuRead(a) {
      if (a >= 0x8000) { const w = this.win[(a >> 13) & 3]; return w.rom ? this.prg[w.off + (a & 0x1fff)] : this.prgRam[w.off + (a & 0x1fff)]; }
      if (a >= 0x6000) return this.prgRam[(this.prgRegs[0] & 7) * 0x2000 + (a & 0x1fff)];
      if (a >= 0x5c00) return this.exMode >= 2 ? this.exram[a & 0x3ff] : -1;
      if (a === 0x5204) {
        const v = (this.irqPending ? 0x80 : 0) | (this.inFrame ? 0x40 : 0);
        this.irqPending = false; return v;
      }
      if (a === 0x5015 && this.audio) return this.audio.status();
      if (a === 0x5205) return (this.mulA * this.mulB) & 0xff;
      if (a === 0x5206) return (this.mulA * this.mulB) >> 8;
      return -1;
    }
    ramWritable() { return this.ramProt1 === 2 && this.ramProt2 === 1; }
    cpuWrite(a, v) {
      if (a >= 0x8000) { const w = this.win[(a >> 13) & 3]; if (!w.rom && this.ramWritable()) this.prgRam[w.off + (a & 0x1fff)] = v; return; }
      if (a >= 0x6000) { if (this.ramWritable()) this.prgRam[(this.prgRegs[0] & 7) * 0x2000 + (a & 0x1fff)] = v; return; }
      if (a >= 0x5c00) { if (this.exMode !== 3) this.exram[a & 0x3ff] = v; return; }
      switch (a) {
        case 0x5100: this.prgMode = v & 3; this.updatePrg(); return;
        case 0x5101: this.chrMode = v & 3; return;
        case 0x5102: this.ramProt1 = v & 3; return;
        case 0x5103: this.ramProt2 = v & 3; return;
        case 0x5104: this.exMode = v & 3; return;
        case 0x5105: this.ntMap = v; return;
        case 0x5106: this.fillTile = v; return;
        case 0x5107: this.fillAttr = v & 3; return;
        case 0x5130: this.chrHi = v & 3; return;
        case 0x5200: this.splitCtl = v; return;
        case 0x5201: this.splitScroll = v; return;
        case 0x5202: this.splitBank = v; return;
        case 0x5203: this.irqCompare = v; return;
        case 0x5204: this.irqEnable = !!(v & 0x80); return;
        case 0x5205: this.mulA = v; return;
        case 0x5206: this.mulB = v; return;
      }
      if (a >= 0x5000 && a <= 0x5015) { if (this.audio) this.audio.write(a, v); return; }
      if (a >= 0x5113 && a <= 0x5117) { this.prgRegs[a - 0x5113] = v; this.updatePrg(); return; }
      if (a >= 0x5120 && a <= 0x5127) { this.chrA[a - 0x5120] = v | (this.chrHi << 8); this.lastSet = 0; return; }
      if (a >= 0x5128 && a <= 0x512b) { this.chrB[a - 0x5128] = v | (this.chrHi << 8); this.lastSet = 1; return; }
    }
    // Which CHR register set applies: with 8x16 sprites, sprites use set A and the
    // background set B; with 8x8 sprites, whichever set was written last.
    chrAddr(a) {
      const ppu = this.cart.ppu(), tall = !!(ppu.ctrl & 0x20);
      const useB = tall ? ppu.fetch === 1 : this.lastSet === 1;
      let bank, size;
      if (useB) {
        switch (this.chrMode) {
          case 0: bank = this.chrB[3]; size = 8; break;
          case 1: bank = this.chrB[3]; size = 4; break;
          case 2: bank = this.chrB[(a >> 11) & 1 ? 3 : 1]; size = 2; break;
          default: bank = this.chrB[(a >> 10) & 3]; size = 1; break;
        }
      } else {
        switch (this.chrMode) {
          case 0: bank = this.chrA[7]; size = 8; break;
          case 1: bank = this.chrA[(a >> 12) ? 7 : 3]; size = 4; break;
          case 2: bank = this.chrA[((a >> 11) << 1) | 1]; size = 2; break;
          default: bank = this.chrA[a >> 10]; size = 1; break;
        }
      }
      const span = size * 0x400;
      return ((bank * span) + (a & (span - 1))) % this.chr.length;
    }
    ppuRead(a) {
      const fetch = this.cart.ppu().fetch;
      // Split-screen tiles come from their own 4 KB CHR bank, at the split's own fine scroll.
      if (fetch === 1 && this.inSplit) return this.chr[((this.splitBank * 0x1000) + ((a & 0xff8) | this.splitFineY)) % this.chr.length];
      // Extended attribute mode: each background tile picks its own 4 KB CHR bank.
      if (this.exMode === 1 && fetch === 1) {
        const bank = (this.exAttr & 0x3f) | (this.chrHi << 6);
        return this.chr[((bank * 0x1000) + (a & 0xfff)) % this.chr.length];
      }
      return this.chr[this.chrAddr(a)];
    }
    ppuWrite(a, v) { if (this.chrRam) this.chr[this.chrAddr(a)] = v; }
    ntRead(a) {
      const ppu = this.cart.ppu(), off = a & 0x3ff, src = (this.ntMap >> (((a >> 10) & 3) * 2)) & 3;
      if (ppu.fetch === 1 && (this.splitCtl & 0x80) && this.exMode <= 1) {
        // Vertical split: tiles on one side of a column come from ExRAM with their own vertical scroll.
        if (off < 0x3c0) {
          const dot = ppu.dot, col = dot >= 321 ? (dot - 321) >> 3 : ((dot - 1) >> 3) + 2;
          const t = this.splitCtl & 0x1f;
          this.inSplit = this.splitCtl & 0x40 ? col >= t : col < t;
          if (this.inSplit) {
            const line = dot >= 321 ? (ppu.line === 261 ? 0 : ppu.line + 1) : ppu.line;
            let y = this.splitScroll + line; if (y >= 240) y -= 240; y %= 240;
            const row = y >> 3, c = col & 31;
            this.splitFineY = y & 7;
            const at = this.exram[0x3c0 + ((row >> 2) << 3) + (c >> 2)];
            this.splitAttr = (at >> (((row & 2) << 1) | (c & 2))) & 3;
            return this.exram[(row << 5) + c];
          }
        } else if (this.inSplit) return this.splitAttr * 0x55;
      }
      if (this.exMode === 1 && ppu.fetch === 1) {
        if (off < 0x3c0) this.exAttr = this.exram[off];
        else { const p = (this.exAttr >> 6) & 3; return p | (p << 2) | (p << 4) | (p << 6); }
      }
      switch (src) {
        case 0: return this.ciram[off];
        case 1: return this.ciram[0x400 | off];
        case 2: return this.exMode <= 1 ? this.exram[off] : 0;
        default: return off < 0x3c0 ? this.fillTile : this.fillAttr * 0x55;
      }
    }
    ntWrite(a, v) {
      const off = a & 0x3ff, src = (this.ntMap >> (((a >> 10) & 3) * 2)) & 3;
      if (src === 0) this.ciram[off] = v; else if (src === 1) this.ciram[0x400 | off] = v;
      else if (src === 2 && this.exMode <= 1) this.exram[off] = v;
    }
    scanline(line) {
      if (line >= 240) return;
      if (!this.inFrame) { this.inFrame = true; this.lineCount = 0; this.irqPending = false; }
      else { this.lineCount++; if (this.lineCount === this.irqCompare) this.irqPending = true; }
    }
    vblank() { this.inFrame = false; this.inSplit = false; }
    cpuClock() { if (this.audio) this.audio.clock(); }
    saveState() {
      const o = super.saveState();
      o.exram = this.exram;
      o.m5 = [this.prgMode, this.chrMode, this.ramProt1, this.ramProt2, this.exMode, this.ntMap, this.fillTile, this.fillAttr, ...this.prgRegs,
        ...this.chrA, ...this.chrB, this.chrHi, this.lastSet, this.irqCompare, this.irqEnable ? 1 : 0, this.irqPending ? 1 : 0, this.inFrame ? 1 : 0,
        this.lineCount, this.mulA, this.mulB, this.exAttr];
      return o;
    }
    loadState(o) {
      super.loadState(o); this.exram.set(o.exram);
      const r = o.m5; let i = 8;
      [this.prgMode, this.chrMode, this.ramProt1, this.ramProt2, this.exMode, this.ntMap, this.fillTile, this.fillAttr] = r.slice(0, 8);
      this.prgRegs.set(r.slice(i, i + 5)); i += 5; this.chrA.set(r.slice(i, i + 8)); i += 8; this.chrB.set(r.slice(i, i + 4)); i += 4;
      [this.chrHi, this.lastSet, this.irqCompare] = r.slice(i, i + 3); i += 3;
      this.irqEnable = !!r[i++]; this.irqPending = !!r[i++]; this.inFrame = !!r[i++];
      [this.lineCount, this.mulA, this.mulB, this.exAttr] = r.slice(i, i + 4);
      this.updatePrg();
    }
  }

  const MAPPERS = { 0: M0, 1: M1, 2: M2, 3: M3, 4: M4, 5: M5, 7: M7, 9: (c) => new M9(c, false), 10: (c) => new M9(c, true),
    11: M11, 13: M13, 34: M34, 66: M66, 69: M69, 71: M71, 140: M140, 180: M180, 206: M206 };

  // UNIF files name the circuit board instead of giving a mapper number.
  const UNIF_BOARDS = {
    NROM: 0, "NROM-128": 0, "NROM-256": 0, RROM: 0, "RROM-128": 0,
    SAROM: 1, SBROM: 1, SCROM: 1, SEROM: 1, SFROM: 1, SGROM: 1, SHROM: 1, SJROM: 1, SKROM: 1, SLROM: 1, SL1ROM: 1, SNROM: 1, SOROM: 1, SUROM: 1, SXROM: 1,
    UNROM: 2, UOROM: 2, CNROM: 3, TBROM: 4, TEROM: 4, TFROM: 4, TGROM: 4, TKROM: 4, TLROM: 4, TNROM: 4, TR1ROM: 4, TSROM: 4, TVROM: 4, B4: 4, HKROM: 4,
    EKROM: 5, ELROM: 5, ETROM: 5, EWROM: 5, AMROM: 7, ANROM: 7, AN1ROM: 7, AOROM: 7, PEEOROM: 9, PNROM: 9, FJROM: 10, FKROM: 10,
    "UNROM-512-8": 30, "UNROM-512-16": 30, "UNROM-512-32": 30, CPROM: 13, BNROM: 34, NINA001: 34, GNROM: 66, MHROM: 66, CDREAMS: 11,
    TLSROM: 118, TKSROM: 118, TQROM: 119, "SA-72007": 145, "SA-72008": 133, "SA-0036": 149, "SA-0037": 148, "SA-016-1M": 79, "TC-U01-1.5M": 147,
    "SACHEN-8259A": 141, "SACHEN-8259B": 138, "SACHEN-8259C": 139, "SACHEN-8259D": 137, "SA-NROM": 143, "22211": 132, "NTBROM": 68,
    KS7058: 171, 
  };
  function parseUNIF(data) {
    const chunks = {}; const prgs = [], chrs = [];
    for (let p = 32; p + 8 <= data.length;) {
      const id = String.fromCharCode(data[p], data[p + 1], data[p + 2], data[p + 3]);
      const len = data[p + 4] | (data[p + 5] << 8) | (data[p + 6] << 16) | (data[p + 7] << 24);
      const body = data.subarray(p + 8, p + 8 + len);
      if (/^PRG[0-9A-F]$/.test(id)) prgs[parseInt(id[3], 16)] = body;
      else if (/^CHR[0-9A-F]$/.test(id)) chrs[parseInt(id[3], 16)] = body;
      else chunks[id] = body;
      p += 8 + len;
    }
    let name = chunks.MAPR ? String.fromCharCode(...chunks.MAPR).replace(/\0.*$/s, "").trim().toUpperCase() : "";
    const board = name.replace(/^(NES|HVC|UNL|BMC|BTL|IREM|KONAMI|TENGEN|TAITO|NAMCOT|SUNSOFT|JALECO|BANDAI|AVE|TXC)-/, "");
    let mapper = UNIF_BOARDS[board];
    if (mapper === undefined && /^S.ROM$/.test(board)) mapper = 1;
    if (mapper === undefined && /^T.ROM$/.test(board)) mapper = 4;
    if (mapper === undefined) throw new Error(`This NES game uses a circuit board RetroPixels doesn't support yet (${name || "unknown board"}).`);
    const cat = (list) => { const parts = list.filter(Boolean); const n = parts.reduce((t, x) => t + x.length, 0); const o = new Uint8Array(n); let i = 0; for (const x of parts) { o.set(x, i); i += x.length; } return o; };
    const prg = cat(prgs); let chr = cat(chrs);
    const chrRam = chr.length === 0;
    if (chrRam) chr = new Uint8Array(0x2000);
    const m = chunks.MIRR ? chunks.MIRR[0] : 5;
    const mirror = m === 0 ? MIRROR.H : m === 1 ? MIRROR.V : m === 2 ? MIRROR.ONE_LO : m === 3 ? MIRROR.ONE_HI : m === 4 ? MIRROR.FOUR : MIRROR.H;
    return { prg, chr, chrRam, mapper, submapper: 0, mirror, battery: !!chunks.BATR, nes2: false, prgRamSize: 0x2000, prgNvramSize: 0, chrRamSize: chrRam ? 0x2000 : 0, flags6: m === 1 ? 1 : 0, board };
  }

  function parseROM(data) {
    if ((data[0] === 0x46 && data[1] === 0x44 && data[2] === 0x53 && data[3] === 0x1a) ||
        (data[0] === 0x01 && String.fromCharCode(...data.subarray(1, 15)) === "*NINTENDO-HVC*"))
      throw new Error("This is a Famicom Disk System game. The Disk System needs Nintendo's own start-up program (its BIOS), " +
        "which RetroPixels can't include, so disk games can't be played. Cartridge (.nes) games work.");
    if (data[0] === 0x55 && data[1] === 0x4e && data[2] === 0x49 && data[3] === 0x46) return parseUNIF(data);
    if (data[0] !== 0x4e || data[1] !== 0x45 || data[2] !== 0x53 || data[3] !== 0x1a) throw new Error("This isn't an NES game file (.nes)");
    const nes2 = (data[7] & 0x0c) === 0x08;
    let prgSize = data[4] * 0x4000, chrSize = data[5] * 0x2000;
    let mapper = (data[6] >> 4) | (data[7] & 0xf0), submapper = 0;
    let prgRamSize = 0x2000, prgNvramSize = 0, chrRamSize = 0x2000;
    if (nes2) {
      mapper |= (data[8] & 0x0f) << 8; submapper = data[8] >> 4;
      // Sizes: either a bank count, or (when the top nibble is $F) an exponent-multiplier.
      const big = (lo, hi, unit) => hi === 0x0f ? (1 << (lo >> 2)) * ((lo & 3) * 2 + 1) : ((hi << 8) | lo) * unit;
      prgSize = big(data[4], data[9] & 0x0f, 0x4000);
      chrSize = big(data[5], data[9] >> 4, 0x2000);
      const shift = (n) => n ? 64 << n : 0;
      prgRamSize = shift(data[10] & 0x0f); prgNvramSize = shift(data[10] >> 4);
      chrRamSize = shift(data[11] & 0x0f) + shift(data[11] >> 4);
    }
    // Old dumps sometimes have junk in bytes 7-15; ignore the high nibble then.
    if (!nes2 && (data[12] || data[13] || data[14] || data[15])) mapper &= 0x0f;
    const trainer = data[6] & 4 ? 512 : 0;
    let off = 16 + trainer;
    let prg = data.slice(off, off + prgSize); off += prgSize;
    if (prg.length < 0x2000) { const p = new Uint8Array(0x2000); for (let i = 0; i < p.length; i++) p[i] = prg[i % Math.max(1, prg.length)]; prg = p; }
    let chr = data.slice(off, off + chrSize);
    const chrRam = chr.length === 0;
    if (chrRam) chr = new Uint8Array(Math.max(0x2000, chrRamSize));
    const mirror = data[6] & 8 ? MIRROR.FOUR : (data[6] & 1 ? MIRROR.V : MIRROR.H);
    return { prg, chr, chrRam, mapper, submapper, mirror, battery: !!(data[6] & 2), nes2, prgRamSize, prgNvramSize, chrRamSize: chrRam ? chr.length : chrRamSize,
      flags6: data[6], trainer: trainer ? data.slice(16, 16 + 512) : null };
  }

  // ===================================================================
  // PPU
  // ===================================================================
  class PPU {
    constructor(nes) {
      this.nes = nes;
      this.vram = new Uint8Array(0x1000);
      this.palRam = new Uint8Array(32);
      this.oam = new Uint8Array(256);
      this.screen = new Uint32Array(256 * 240);
      this.ctrl = 0; this.mask = 0; this.status = 0; this.oamAddr = 0;
      this.v = 0; this.t = 0; this.x = 0; this.w = 0; this.buffer = 0; this.openBus = 0;
      this.line = 0; this.dot = 0; this.odd = false; this.frame = 0; this.nmiOut = false;
      // background pipeline
      this.ntByte = 0; this.atByte = 0; this.ptLo = 0; this.ptHi = 0;
      this.bgLo = 0; this.bgHi = 0; this.atLo = 0; this.atHi = 0; this.atLatchLo = 0; this.atLatchHi = 0;
      // sprites for the current line
      this.spCount = 0; this.spX = new Uint8Array(8); this.spAttr = new Uint8Array(8);
      this.spLo = new Uint8Array(8); this.spHi = new Uint8Array(8); this.spZero = false;
      this.nextSp = new Uint8Array(32); this.nextCount = 0; this.nextZero = false;
      this.frameDone = false;
    }
    get rendering() { return (this.mask & 0x18) !== 0; }

    // this.fetch tells the cartridge what the PPU is reading: 0 = CPU access,
    // 1 = background, 2 = sprites (the MMC5 needs to know).
    read(a) {
      const m = this.nes.mapper;
      a &= 0x3fff;
      if (a < 0x2000) return m.ppuRead(a);
      if (a < 0x3f00) return m.ntRead ? m.ntRead(a & 0xfff) : this.vram[m.ntIndex(a)];
      return this.palRam[this.palIndex(a)];
    }
    write(a, v) {
      const m = this.nes.mapper;
      a &= 0x3fff;
      if (a < 0x2000) m.ppuWrite(a, v);
      else if (a < 0x3f00) { if (m.ntWrite) m.ntWrite(a & 0xfff, v); else this.vram[m.ntIndex(a)] = v; }
      else this.palRam[this.palIndex(a)] = v & 0x3f;
    }
    palIndex(a) { a &= 0x1f; if ((a & 0x13) === 0x10) a &= 0x0f; return a; }

    // CPU-visible registers $2000-$2007
    cpuRead(r) {
      // The PPU's data bus holds the last value for a while, then fades to 0.
      if (this.frame - (this.busFrame || 0) > 36) { this.openBus = 0; this.busFrame = this.frame; }
      switch (r) {
        case 2: {
          let v = (this.status & 0xe0) | (this.openBus & 0x1f);
          if (this.line === 241) {
            // One dot before vblank starts: reads clear and the flag/NMI never happen this frame.
            if (this.dot === 1) { v &= 0x7f; this.suppressNmi = true; }
            // On the same dot or just after: reads set, but the NMI is cancelled.
            else if (this.dot === 2 || this.dot === 3) this.nes.cpu.nmiPending = false;
          }
          this.status &= 0x7f; this.w = 0; this.updateNmi();
          this.openBus = v; return v;
        }
        case 4: this.openBus = this.oam[this.oamAddr]; return this.openBus;
        case 7: {
          let v;
          if ((this.v & 0x3fff) >= 0x3f00) { v = (this.read(this.v) & 0x3f) | (this.openBus & 0xc0); this.buffer = this.read(this.v - 0x1000); }
          else { v = this.buffer; this.buffer = this.read(this.v); }
          this.incV(); this.openBus = v; return v;
        }
      }
      return this.openBus;
    }
    cpuWrite(r, v) {
      this.openBus = v; this.busFrame = this.frame;
      switch (r) {
        case 0: this.ctrl = v; this.t = (this.t & 0xf3ff) | ((v & 3) << 10); this.updateNmi(); break;
        case 1: this.mask = v; break;
        case 3: this.oamAddr = v; break;
        case 4: this.oam[this.oamAddr] = (this.oamAddr & 3) === 2 ? v & 0xe3 : v; this.oamAddr = (this.oamAddr + 1) & 0xff; break;
        case 5:
          if (!this.w) { this.t = (this.t & 0xffe0) | (v >> 3); this.x = v & 7; }
          else this.t = (this.t & 0x8c1f) | ((v & 7) << 12) | ((v & 0xf8) << 2);
          this.w ^= 1; break;
        case 6:
          if (!this.w) this.t = (this.t & 0x00ff) | ((v & 0x3f) << 8);
          else { this.t = (this.t & 0xff00) | v; this.v = this.t; this.nes.mapper.ppuAddr && this.nes.mapper.ppuAddr(this.v); }
          this.w ^= 1; break;
        case 7: this.write(this.v, v); this.incV(); break;
      }
    }
    incV() {
      this.v = (this.v + (this.ctrl & 4 ? 32 : 1)) & 0x7fff;
      if (this.nes.mapper.ppuAddr) this.nes.mapper.ppuAddr(this.v);
    }
    updateNmi() {
      const out = !!(this.ctrl & 0x80) && !!(this.status & 0x80);
      if (out && !this.nmiOut) this.nes.cpu.nmiPending = true;
      // Turning NMIs off right as vblank starts cancels that NMI.
      if (!out && this.nmiOut && this.line === 241 && this.dot <= 3) this.nes.cpu.nmiPending = false;
      this.nmiOut = out;
    }

    // scroll helpers (the "loopy" v/t registers)
    incX() { if ((this.v & 0x1f) === 31) { this.v &= ~0x1f; this.v ^= 0x400; } else this.v++; }
    incY() {
      if ((this.v & 0x7000) !== 0x7000) { this.v += 0x1000; return; }
      this.v &= ~0x7000;
      let y = (this.v & 0x3e0) >> 5;
      if (y === 29) { y = 0; this.v ^= 0x800; } else if (y === 31) y = 0; else y++;
      this.v = (this.v & ~0x3e0) | (y << 5);
    }

    bgFetch() {
      this.fetch = 1;
      switch (this.dot & 7) {
        case 1: this.loadShifters(); this.ntByte = this.read(0x2000 | (this.v & 0xfff)); break;
        case 3: {
          const v = this.v;
          const at = this.read(0x23c0 | (v & 0xc00) | ((v >> 4) & 0x38) | ((v >> 2) & 7));
          this.atByte = (at >> (((v >> 4) & 4) | (v & 2))) & 3;
          break;
        }
        case 5: this.ptLo = this.read(((this.ctrl & 0x10) << 8) + this.ntByte * 16 + ((this.v >> 12) & 7)); break;
        case 7: this.ptHi = this.read(((this.ctrl & 0x10) << 8) + this.ntByte * 16 + ((this.v >> 12) & 7) + 8); break;
        case 0: this.incX(); break;
      }
    }
    loadShifters() {
      this.bgLo = (this.bgLo & 0xff00) | this.ptLo;
      this.bgHi = (this.bgHi & 0xff00) | this.ptHi;
      this.atLatchLo = this.atByte & 1; this.atLatchHi = (this.atByte >> 1) & 1;
    }
    shift() {
      this.bgLo = (this.bgLo << 1) & 0xffff; this.bgHi = (this.bgHi << 1) & 0xffff;
      this.atLo = ((this.atLo << 1) | this.atLatchLo) & 0xff; this.atHi = ((this.atHi << 1) | this.atLatchHi) & 0xff;
    }

    // Find sprites on the next line (done in one go at the end of the line).
    evalSprites() {
      const h = this.ctrl & 0x20 ? 16 : 8, next = this.line === 261 ? 0 : this.line + 1;
      let n = 0; this.nextZero = false;
      for (let i = 0; i < 64; i++) {
        const y = this.oam[i * 4];
        const row = next - y - 1;
        if (row >= 0 && row < h) {
          if (n < 8) { this.nextSp.set(this.oam.subarray(i * 4, i * 4 + 4), n * 4); if (i === 0) this.nextZero = true; n++; }
          else { this.status |= 0x20; break; }
        }
      }
      this.nextCount = n;
    }
    // Pattern fetches for the next line's sprites (dots 257-320; done per slot so
    // cartridge chips watching the PPU address bus see the right sequence).
    spriteFetch(slot) {
      this.fetch = 2;
      const h = this.ctrl & 0x20 ? 16 : 8, next = this.line === 261 ? 0 : this.line + 1;
      let addr;
      if (slot < this.nextCount) {
        const y = this.nextSp[slot * 4], tile = this.nextSp[slot * 4 + 1], attr = this.nextSp[slot * 4 + 2];
        let row = next - y - 1; if (attr & 0x80) row = h - 1 - row;
        if (h === 16) addr = ((tile & 1) << 12) | ((tile & 0xfe) << 4) | (row & 8 ? 16 : 0) | (row & 7);
        else addr = ((this.ctrl & 8) << 9) | (tile << 4) | row;
        let lo = this.read(addr), hi = this.read(addr + 8);
        if (attr & 0x40) { lo = REV[lo]; hi = REV[hi]; }
        this.spLoN[slot] = lo; this.spHiN[slot] = hi;
      } else {
        addr = h === 16 ? 0x1ff0 : (((this.ctrl & 8) << 9) | 0xff0);
        this.read(addr); this.read(addr + 8);
      }
    }

    tick() {
      const line = this.line, dot = this.dot, render = this.rendering;
      if (line < 240 || line === 261) {
        if (render) {
          if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) this.shift();
          if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336)) this.bgFetch();
          if (dot === 256) this.incY();
          else if (dot === 257) {
            this.v = (this.v & ~0x41f) | (this.t & 0x41f);
            this.evalSprites();
            this.spLoN = this.spLoN || new Uint8Array(8); this.spHiN = this.spHiN || new Uint8Array(8);
          }
          if (dot >= 257 && dot <= 320 && ((dot - 257) & 7) === 3) this.spriteFetch((dot - 257) >> 3);
          if (line === 261 && dot >= 280 && dot <= 304) this.v = (this.v & ~0x7be0) | (this.t & 0x7be0);
          if (dot === 337 || dot === 339) this.read(0x2000 | (this.v & 0xfff)); // garbage nametable fetches
          if (dot === 320) {
            this.spCount = this.nextCount; this.spZero = this.nextZero;
            for (let i = 0; i < 8; i++) {
              this.spX[i] = this.nextSp[i * 4 + 3]; this.spAttr[i] = this.nextSp[i * 4 + 2];
              this.spLo[i] = this.spLoN[i]; this.spHi[i] = this.spHiN[i];
            }
          }
        }
        this.fetch = 0;
        if (render && dot === 4 && this.nes.mapper.scanline) this.nes.mapper.scanline(line);
        if (line < 240 && dot >= 1 && dot <= 256) this.pixel(dot - 1, line);
        if (line === 261 && dot === 1) { this.status &= 0x1f; this.updateNmi(); }
      } else if (line === 241 && dot === 1) {
        if (this.nes.mapper.vblank) this.nes.mapper.vblank();
        if (!this.suppressNmi) this.status |= 0x80;
        this.suppressNmi = false;
        this.updateNmi();
        this.frameDone = true;
      }
      // advance
      if (line === 261 && dot === 338) this.renderAtSkip = render;
      this.dot++;
      if (line === 261 && this.dot === 340 && this.odd && this.renderAtSkip) this.dot = 341; // skipped dot on odd frames
      if (this.dot > 340) {
        this.dot = 0; this.line++;
        if (this.line > 261) { this.line = 0; this.odd = !this.odd; this.frame++; }
      }
    }

    pixel(x, y) {
      let bg = 0, pal = 0;
      const mask = this.mask;
      if ((mask & 8) && (x >= 8 || (mask & 2))) {
        const bit = 15 - this.x;
        bg = ((this.bgLo >> bit) & 1) | (((this.bgHi >> bit) & 1) << 1);
        if (bg) pal = (((this.atLo >> (7 - this.x)) & 1) | (((this.atHi >> (7 - this.x)) & 1) << 1)) << 2 | bg;
      }
      let color = bg ? pal : 0;
      if ((mask & 0x10) && (x >= 8 || (mask & 4))) {
        for (let i = 0; i < this.spCount; i++) {
          const off = x - this.spX[i];
          if (off < 0 || off > 7) continue;
          const sp = ((this.spLo[i] >> (7 - off)) & 1) | (((this.spHi[i] >> (7 - off)) & 1) << 1);
          if (!sp) continue;
          if (i === 0 && this.spZero && bg && x !== 255) this.status |= 0x40;
          if (!bg || !(this.spAttr[i] & 0x20)) color = 0x10 | ((this.spAttr[i] & 3) << 2) | sp;
          break;
        }
      }
      let c = this.palRam[this.palIndex(0x3f00 | color)];
      if (mask & 1) c &= 0x30; // greyscale
      this.screen[y * 256 + x] = PALETTE[((mask >> 5) & 7) * 64 + c];
    }

    saveState() {
      const o = {};
      for (const k of ["ctrl", "mask", "status", "oamAddr", "v", "t", "x", "w", "buffer", "openBus", "line", "dot", "odd", "frame",
        "nmiOut", "ntByte", "atByte", "ptLo", "ptHi", "bgLo", "bgHi", "atLo", "atHi", "atLatchLo", "atLatchHi", "spCount", "spZero",
        "nextCount", "nextZero", "suppressNmi"]) o[k] = this[k];
      o.vram = this.vram; o.palRam = this.palRam; o.oam = this.oam; o.spX = this.spX; o.spAttr = this.spAttr;
      o.spLo = this.spLo; o.spHi = this.spHi; o.nextSp = this.nextSp;
      return o;
    }
    loadState(o) {
      for (const k in o) {
        if (ArrayBuffer.isView(this[k])) this[k].set(o[k]); else this[k] = o[k];
      }
    }
  }
  const REV = new Uint8Array(256);
  for (let i = 0; i < 256; i++) { let r = 0; for (let b = 0; b < 8; b++) if (i & (1 << b)) r |= 0x80 >> b; REV[i] = r; }

  // ===================================================================
  // APU (sound)
  // ===================================================================
  const LENGTHS = [10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30];
  const DUTY = [[0, 1, 0, 0, 0, 0, 0, 0], [0, 1, 1, 0, 0, 0, 0, 0], [0, 1, 1, 1, 1, 0, 0, 0], [1, 0, 0, 1, 1, 1, 1, 1]];
  const TRI = []; for (let i = 15; i >= 0; i--) TRI.push(i); for (let i = 0; i < 16; i++) TRI.push(i);
  const NOISE_PERIOD = [4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068];
  const DMC_RATE = [428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54];

  class Envelope {
    constructor() { this.start = false; this.divider = 0; this.decay = 0; this.loop = false; this.constant = false; this.vol = 0; }
    clock() {
      if (this.start) { this.start = false; this.decay = 15; this.divider = this.vol; return; }
      if (this.divider > 0) { this.divider--; return; }
      this.divider = this.vol;
      if (this.decay > 0) this.decay--; else if (this.loop) this.decay = 15;
    }
    out() { return this.constant ? this.vol : this.decay; }
  }

  class Pulse {
    constructor(second) { this.second = second; this.env = new Envelope(); this.enabled = false; this.len = 0; this.duty = 0; this.step = 0;
      this.period = 0; this.timer = 0; this.sweepOn = false; this.sweepPeriod = 0; this.sweepNeg = false; this.sweepShift = 0;
      this.sweepDiv = 0; this.sweepReload = false; }
    write(r, v) {
      switch (r) {
        case 0: this.duty = v >> 6; this.env.loop = !!(v & 0x20); this.env.constant = !!(v & 0x10); this.env.vol = v & 15; break;
        case 1: this.sweepOn = !!(v & 0x80); this.sweepPeriod = (v >> 4) & 7; this.sweepNeg = !!(v & 8); this.sweepShift = v & 7; this.sweepReload = true; break;
        case 2: this.period = (this.period & 0x700) | v; break;
        case 3: this.period = (this.period & 0xff) | ((v & 7) << 8); if (this.enabled) this.len = LENGTHS[v >> 3]; this.step = 0; this.env.start = true; break;
      }
    }
    target() {
      const d = this.period >> this.sweepShift;
      return this.sweepNeg ? this.period - d - (this.second ? 0 : 1) : this.period + d;
    }
    clockTimer() { if (this.timer === 0) { this.timer = this.period; this.step = (this.step + 1) & 7; } else this.timer--; }
    clockSweep() {
      const t = this.target();
      if (this.sweepDiv === 0 && this.sweepOn && this.sweepShift && this.period >= 8 && t <= 0x7ff) this.period = t;
      if (this.sweepDiv === 0 || this.sweepReload) { this.sweepDiv = this.sweepPeriod; this.sweepReload = false; } else this.sweepDiv--;
    }
    clockLength() { if (this.len > 0 && !this.env.loop) this.len--; }
    out() {
      if (!this.len || this.period < 8 || this.target() > 0x7ff || !DUTY[this.duty][this.step]) return 0;
      return this.env.out();
    }
  }

  class Triangle {
    constructor() { this.enabled = false; this.len = 0; this.control = false; this.linReload = 0; this.lin = 0; this.reloadFlag = false; this.period = 0; this.timer = 0; this.step = 0; }
    write(r, v) {
      switch (r) {
        case 0: this.control = !!(v & 0x80); this.linReload = v & 0x7f; break;
        case 2: this.period = (this.period & 0x700) | v; break;
        case 3: this.period = (this.period & 0xff) | ((v & 7) << 8); if (this.enabled) this.len = LENGTHS[v >> 3]; this.reloadFlag = true; break;
      }
    }
    clockTimer() {
      if (this.timer === 0) { this.timer = this.period; if (this.len && this.lin && this.period > 1) this.step = (this.step + 1) & 31; }
      else this.timer--;
    }
    clockLinear() {
      if (this.reloadFlag) this.lin = this.linReload; else if (this.lin > 0) this.lin--;
      if (!this.control) this.reloadFlag = false;
    }
    clockLength() { if (this.len > 0 && !this.control) this.len--; }
    out() { return TRI[this.step]; }
  }

  class Noise {
    constructor() { this.env = new Envelope(); this.enabled = false; this.len = 0; this.mode = false; this.period = 4; this.timer = 0; this.shift = 1; }
    write(r, v) {
      switch (r) {
        case 0: this.env.loop = !!(v & 0x20); this.env.constant = !!(v & 0x10); this.env.vol = v & 15; break;
        case 2: this.mode = !!(v & 0x80); this.period = NOISE_PERIOD[v & 15]; break;
        case 3: if (this.enabled) this.len = LENGTHS[v >> 3]; this.env.start = true; break;
      }
    }
    clockTimer() {
      if (this.timer === 0) {
        this.timer = this.period;
        const fb = (this.shift & 1) ^ ((this.shift >> (this.mode ? 6 : 1)) & 1);
        this.shift = (this.shift >> 1) | (fb << 14);
      } else this.timer--;
    }
    clockLength() { if (this.len > 0 && !this.env.loop) this.len--; }
    out() { return (this.shift & 1) || !this.len ? 0 : this.env.out(); }
  }

  class DMC {
    constructor(apu) { this.apu = apu; this.irqOn = false; this.loop = false; this.rate = 428; this.timer = 0; this.level = 0;
      this.addr0 = 0xc000; this.len0 = 1; this.addr = 0xc000; this.remain = 0; this.buffer = -1; this.shift = 0; this.bits = 8; this.silent = true; this.irq = false; }
    write(r, v) {
      switch (r) {
        case 0: this.irqOn = !!(v & 0x80); this.loop = !!(v & 0x40); this.rate = DMC_RATE[v & 15]; if (!this.irqOn) this.irq = false; break;
        case 1: this.level = v & 0x7f; break;
        case 2: this.addr0 = 0xc000 | (v << 6); break;
        case 3: this.len0 = (v << 4) | 1; break;
      }
    }
    restart() { this.addr = this.addr0; this.remain = this.len0; }
    fill() {
      if (this.buffer < 0 && this.remain > 0) {
        this.buffer = this.apu.nes.peek(this.addr);
        this.apu.nes.stall += 4; // the CPU pauses while the sample byte is fetched
        this.addr = this.addr === 0xffff ? 0x8000 : this.addr + 1;
        if (--this.remain === 0) { if (this.loop) this.restart(); else if (this.irqOn) this.irq = true; }
      }
    }
    clockTimer() {
      if (this.timer > 0) { this.timer--; return; }
      this.timer = this.rate - 1;
      if (!this.silent) {
        if (this.shift & 1) { if (this.level <= 125) this.level += 2; } else if (this.level >= 2) this.level -= 2;
      }
      this.shift >>= 1;
      if (--this.bits <= 0) {
        this.bits = 8;
        if (this.buffer < 0) this.silent = true; else { this.silent = false; this.shift = this.buffer; this.buffer = -1; }
      }
      this.fill();
    }
  }

  class APU {
    constructor(nes, sampleRate) {
      this.nes = nes;
      this.p1 = new Pulse(false); this.p2 = new Pulse(true); this.tri = new Triangle(); this.noise = new Noise(); this.dmc = new DMC(this);
      this.frameMode = 0; this.irqInhibit = false; this.frameIrq = false; this.frameCycle = 0; this.odd = false; this.frameReset = 0;
      this.sampleRate = sampleRate; this.step = CPU_HZ / sampleRate; this.acc = 0; this.sum = 0; this.n = 0;
      this.hp = 0; this.hpPrev = 0; this.lp = 0;
      this.out = new Float32Array(4096); this.outLen = 0;
      // mixer lookup tables (non-linear mix of the analog output)
      this.pulseTab = new Float32Array(31); for (let i = 1; i < 31; i++) this.pulseTab[i] = 95.52 / (8128 / i + 100);
      this.tndTab = new Float32Array(203); for (let i = 1; i < 203; i++) this.tndTab[i] = 163.67 / (24329 / i + 100);
    }
    write(a, v) {
      if (a < 0x4004) this.p1.write(a & 3, v);
      else if (a < 0x4008) this.p2.write(a & 3, v);
      else if (a < 0x400c) this.tri.write(a & 3, v);
      else if (a < 0x4010) this.noise.write(a & 3, v);
      else if (a < 0x4014) this.dmc.write(a & 3, v);
      else if (a === 0x4015) {
        this.p1.enabled = !!(v & 1); if (!this.p1.enabled) this.p1.len = 0;
        this.p2.enabled = !!(v & 2); if (!this.p2.enabled) this.p2.len = 0;
        this.tri.enabled = !!(v & 4); if (!this.tri.enabled) this.tri.len = 0;
        this.noise.enabled = !!(v & 8); if (!this.noise.enabled) this.noise.len = 0;
        this.dmc.irq = false;
        if (v & 16) { if (this.dmc.remain === 0) { this.dmc.restart(); this.dmc.fill(); } } else this.dmc.remain = 0;
      } else if (a === 0x4017) {
        this.frameMode = v >> 7; this.irqInhibit = !!(v & 0x40);
        if (this.irqInhibit) this.frameIrq = false;
        // The new mode takes effect 3 or 4 CPU cycles later, depending on alignment.
        this.frameReset = this.odd ? 5 : 4; // 3–4 cycle hardware delay, counted from the next tick
      }
    }
    readStatus() {
      const v = (this.p1.len ? 1 : 0) | (this.p2.len ? 2 : 0) | (this.tri.len ? 4 : 0) | (this.noise.len ? 8 : 0) |
        (this.dmc.remain ? 16 : 0) | (this.frameIrq ? 0x40 : 0) | (this.dmc.irq ? 0x80 : 0);
      this.frameIrq = false;
      return v;
    }
    get irq() { return this.frameIrq || this.dmc.irq; }
    quarter() { this.p1.env.clock(); this.p2.env.clock(); this.noise.env.clock(); this.tri.clockLinear(); }
    half() { this.p1.clockLength(); this.p2.clockLength(); this.tri.clockLength(); this.noise.clockLength(); this.p1.clockSweep(); this.p2.clockSweep(); }
    // one CPU cycle
    tick() {
      this.tri.clockTimer();
      this.odd = !this.odd;
      if (this.odd) { this.p1.clockTimer(); this.p2.clockTimer(); this.noise.clockTimer(); }
      this.dmc.clockTimer();
      if (this.frameReset > 0 && --this.frameReset === 0) {
        this.frameCycle = 0;
        if (this.frameMode) { this.quarter(); this.half(); }
      }
      const f = ++this.frameCycle;
      if (this.frameMode === 0) {
        if (f === 7457 || f === 22371) this.quarter();
        else if (f === 14913) { this.quarter(); this.half(); }
        else if (f === 29828) { if (!this.irqInhibit) this.frameIrq = true; }
        else if (f === 29829) { this.quarter(); this.half(); if (!this.irqInhibit) this.frameIrq = true; }
        else if (f >= 29830) { if (!this.irqInhibit) this.frameIrq = true; this.frameCycle = 0; }
      } else {
        if (f === 7457 || f === 22371) this.quarter();
        else if (f === 14913 || f === 37281) { this.quarter(); this.half(); }
        if (f >= 37282) this.frameCycle = 0;
      }
      // mix and resample
      let s = this.pulseTab[this.p1.out() + this.p2.out()] + this.tndTab[3 * this.tri.out() + 2 * this.noise.out() + this.dmc.level];
      if (this.ext) s += this.ext.output();
      this.sum += s; this.n++;
      this.acc += 1;
      if (this.acc >= this.step) {
        this.acc -= this.step;
        const x = this.sum / this.n; this.sum = 0; this.n = 0;
        // ~37 Hz high-pass (removes DC like the console's output capacitor) and gentle low-pass
        this.hp = 0.9952 * (this.hp + x - this.hpPrev); this.hpPrev = x;
        this.lp += (this.hp - this.lp) * 0.75;
        if (this.outLen < this.out.length) this.out[this.outLen++] = this.lp;
      }
    }
    saveState() {
      const pack = (o) => { const r = {}; for (const k in o) { const v = o[k]; if (typeof v !== "object" || v === null || ArrayBuffer.isView(v)) r[k] = v; else if (v instanceof Envelope) r[k] = Object.assign({}, v); } return r; };
      return { p1: pack(this.p1), p2: pack(this.p2), tri: pack(this.tri), noise: pack(this.noise), dmc: Object.assign({}, this.dmc, { apu: undefined }),
        frameMode: this.frameMode, irqInhibit: this.irqInhibit, frameIrq: this.frameIrq, frameCycle: this.frameCycle, odd: this.odd, frameReset: this.frameReset };
    }
    loadState(o) {
      for (const ch of ["p1", "p2", "tri", "noise"]) for (const k in o[ch]) { if (k === "env") Object.assign(this[ch].env, o[ch].env); else this[ch][k] = o[ch][k]; }
      for (const k in o.dmc) if (k !== "apu" && o.dmc[k] !== undefined) this.dmc[k] = o.dmc[k];
      Object.assign(this, { frameMode: o.frameMode, irqInhibit: o.irqInhibit, frameIrq: o.frameIrq, frameCycle: o.frameCycle, odd: o.odd, frameReset: o.frameReset || 0 });
    }
  }

  // ===================================================================
  // The machine
  // ===================================================================
  class NES {
    constructor(rom, opts = {}) {
      const cart = parseROM(rom);
      cart.cpuCycles = () => this.cpu.cycles;
      cart.ppu = () => this.ppu;
      cart.vram = new Uint8Array(0x1000);
      cart.busConflict = cart.mapper === 2 && cart.prg.length <= 0x20000 && false;
      cart.nes = this;
      const M = MAPPERS[cart.mapper];
      if (!M) throw new Error(`This NES game uses cartridge hardware RetroPixels doesn't support yet (mapper ${cart.mapper}).`);
      this.cart = cart;
      this.mapper = M.prototype ? new M(cart) : M(cart);
      this.battery = cart.battery || !!this.mapper.batteryData;
      if (cart.trainer) this.mapper.prgRam.set(cart.trainer, 0x1000);
      this.ram = new Uint8Array(0x800);
      this.ppu = new PPU(this);
      this.ppu.vram = cart.vram;
      this.apu = new APU(this, opts.sampleRate || 48000);
      this.pads = [0, 0]; this.padShift = [0, 0]; this.strobe = 0;
      this.stall = 0; this.dmaPage = -1;
      this.openBus = 0;
      const self = this;
      this.cpu = new global.RPCPU6502({
        read(a) { self.clock(); const v = self.cpuRead(a); self.openBus = v; return v; },
        write(a, v) { self.clock(); self.openBus = v; self.cpuWrite(a, v); },
      }, { decimal: false, machinePolls: true });
      this.width = 256; this.height = 240; this.fps = FPS;
      if (opts.sram && this.battery) {
        if (this.mapper.loadBattery) this.mapper.loadBattery(opts.sram);
        else this.mapper.prgRam.set(opts.sram.subarray(0, Math.min(opts.sram.length, this.mapper.prgRam.length)));
      }
      this.apu.ext = this.mapper.audio || null; // expansion sound on the cartridge
      this.cpu.s = 0; // the stack pointer powers up at 0; the reset sequence moves it to $FD
      this.cpu.reset();
    }
    // Advance everything by one CPU cycle.
    clock() {
      const p = this.ppu;
      // The CPU samples its interrupt lines one PPU dot into each cycle.
      p.tick(); this.cpu.poll(); p.tick(); p.tick();
      this.apu.tick();
      this.mapper.cpuClock();
      this.cpu.irqLine = this.apu.irq || this.mapper.irq;
    }
    peek(a) { // read without spending a cycle (used by the DMC)
      if (a < 0x2000) return this.ram[a & 0x7ff];
      const v = this.mapper.cpuRead(a); return v < 0 ? this.openBus : v;
    }
    cpuRead(a) {
      if (a < 0x2000) return this.ram[a & 0x7ff];
      if (a < 0x4000) return this.ppu.cpuRead(a & 7);
      if (a === 0x4015) return this.apu.readStatus() | (this.openBus & 0x20);
      if (a === 0x4016 || a === 0x4017) {
        const i = a & 1;
        let bit;
        if (this.strobe) bit = this.pads[i] & 1;
        else { bit = this.padShift[i] & 1; this.padShift[i] = (this.padShift[i] >> 1) | 0x80; }
        return (this.openBus & 0xe0) | bit;
      }
      if (a < 0x4020) return this.openBus;
      const v = this.mapper.cpuRead(a);
      return v < 0 ? this.openBus : v;
    }
    cpuWrite(a, v) {
      if (a < 0x2000) { this.ram[a & 0x7ff] = v; return; }
      if (a < 0x4000) { this.ppu.cpuWrite(a & 7, v); return; }
      if (a === 0x4014) { this.dmaPage = v; return; }
      if (a === 0x4016) {
        this.strobe = v & 1;
        if (this.strobe) { this.padShift[0] = this.pads[0]; this.padShift[1] = this.pads[1]; }
        return;
      }
      if (a < 0x4018) { this.apu.write(a, v); return; }
      this.mapper.cpuWrite(a, v);
    }
    // Sprite DMA: the CPU stops while 256 bytes are copied to the PPU.
    doDma() {
      const page = this.dmaPage << 8; this.dmaPage = -1;
      this.clock(); this.cpu.cycles++;
      if (this.cpu.cycles & 1) { this.clock(); this.cpu.cycles++; }
      for (let i = 0; i < 256; i++) {
        this.clock(); this.cpu.cycles++; const v = this.cpuRead(page | i);
        this.clock(); this.cpu.cycles++; this.ppu.cpuWrite(4, v);
      }
    }

    // ---- frontend interface ----
    // buttons bit order: A, B, Select, Start, Up, Down, Left, Right
    setInput(player, bits) { if (player < 2) this.pads[player] = bits & 0xff; if (this.strobe) this.padShift[player] = this.pads[player]; }
    runFrame() {
      const ppu = this.ppu;
      ppu.frameDone = false;
      let guard = 0;
      while (!ppu.frameDone && guard++ < 200000) {
        this.cpu.step();
        if (this.dmaPage >= 0) this.doDma();
        while (this.stall > 0) { this.stall--; this.clock(); this.cpu.cycles++; }
      }
    }
    get screen() { return this.ppu.screen; }
    takeAudio() { const a = this.apu.out.slice(0, this.apu.outLen); this.apu.outLen = 0; return a; }
    reset() { if (this.mapper.reset) this.mapper.reset(); this.cpu.reset(); this.apu.write(0x4015, 0); }
    getSRAM() { return this.battery ? (this.mapper.batteryData ? this.mapper.batteryData() : this.mapper.prgRam) : null; }
    saveState() {
      return { v: 1, cpu: this.cpu.saveState(), ram: this.ram, ppu: this.ppu.saveState(), apu: this.apu.saveState(),
        mapper: this.mapper.saveState(), strobe: this.strobe, padShift: this.padShift.slice() };
    }
    loadState(s) {
      this.cpu.loadState(s.cpu); this.ram.set(s.ram); this.ppu.loadState(s.ppu); this.apu.loadState(s.apu);
      this.mapper.loadState(s.mapper); this.strobe = s.strobe; this.padShift = s.padShift.slice();
    }
  }
  NES.info = { system: "nes", name: "NES", width: 256, height: 240, aspect: 4 / 3, fps: FPS, players: 2,
    buttons: ["A", "B", "Select", "Start", "Up", "Down", "Left", "Right"] };

  global.RPCores = global.RPCores || {};
  global.RPCores.nes = NES;
  // Parts the extra cartridge files (nes_mappers.js, nes_audio.js) build on.
  global.RPNes = { Mapper, MIRROR, MAPPERS, M1, M4, M206, CPU_HZ, packFields, unpackFields };
})(typeof window !== "undefined" ? window : globalThis);
