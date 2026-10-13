// RetroPixels — Sega Genesis / Mega Drive cartridge hardware. Original code.
//
// Everything a cartridge can add to the 68000's $000000-$3FFFFF area and the
// $A130xx "time" registers: battery SRAM (including the >2 MB switch), serial
// I2C EEPROM saves (24C01 to 24C65, in the Sega, EA, Acclaim and Codemasters
// wirings), the Super Street Fighter II bank mapper (also used by big
// homebrews), Realtec's mapper, and Codemasters' J-Cart with two extra pads.
// Written from the documented cartridge behaviour.
"use strict";

(function (global) {
  // ===================================================================
  // Serial (I2C) EEPROM
  // ===================================================================
  // mode 1: X24C01 — the first byte after START is the address (7 bits) + R/W
  // mode 2: 24C02-24C16 — device byte (1010 + high address bits + R/W), then one address byte
  // mode 3: 24C32-24C65 — device byte, then two address bytes
  const ST_IDLE = 0, ST_ADDR1 = 1, ST_DEV = 2, ST_WORD_HI = 3, ST_WORD_LO = 4, ST_WRITE = 5, ST_READ = 6;
  class EEPROM {
    constructor(size, mode, page) {
      this.mem = new Uint8Array(size).fill(0xff);
      this.mask = size - 1; this.mode = mode; this.pageMask = page - 1;
      this.scl = 1; this.sda = 1; this.out = 1;
      this.state = ST_IDLE; this.bit = 0; this.shift = 0; this.addr = 0; this.dirty = false;
    }
    // The game drives the clock and data lines.
    set(scl, sda) {
      if (this.scl && scl && this.sda !== sda) {
        // data changing while the clock is high: START (falling) or STOP (rising)
        if (!sda) { this.state = this.mode === 1 ? ST_ADDR1 : ST_DEV; this.bit = 0; this.shift = 0; this.out = 1; }
        else { this.state = ST_IDLE; this.out = 1; }
      } else if (!this.scl && scl) this.rise(sda);
      else if (this.scl && !scl) this.fall();
      this.scl = scl; this.sda = sda;
    }
    // Clock rising: the receiver samples SDA. Bit 9 is the acknowledge.
    rise(sda) {
      const st = this.state;
      if (st === ST_IDLE) return;
      if (this.bit < 8) {
        if (st !== ST_READ) this.shift = ((this.shift << 1) | sda) & 0xff;
        this.bit++;
        return;
      }
      // ninth clock
      this.bit = 0;
      if (st === ST_READ) {
        if (sda) this.state = ST_IDLE;                         // no acknowledge from the game: stop sending
        else this.addr = (this.addr + 1) & this.mask;          // acknowledged: next byte
        return;
      }
      const b = this.shift; this.shift = 0;
      switch (st) {
        case ST_ADDR1: this.addr = (b >> 1) & this.mask; this.state = (b & 1) ? ST_READ : ST_WRITE; break;
        case ST_DEV:
          if ((b & 0xf0) !== 0xa0) { this.state = ST_IDLE; break; }
          if (this.mode === 2) this.addr = ((((b >> 1) & 7) << 8) | (this.addr & 0xff)) & this.mask;
          this.state = (b & 1) ? ST_READ : (this.mode === 3 ? ST_WORD_HI : ST_WORD_LO);
          break;
        case ST_WORD_HI: this.addr = ((b << 8) | (this.addr & 0xff)) & this.mask; this.state = ST_WORD_LO; break;
        case ST_WORD_LO: this.addr = ((this.addr & ~0xff) | b) & this.mask; this.state = ST_WRITE; break;
        case ST_WRITE:
          if (this.mem[this.addr] !== b) { this.mem[this.addr] = b; this.dirty = true; }
          this.addr = (this.addr & ~this.pageMask) | ((this.addr + 1) & this.pageMask);
          break;
      }
    }
    // Clock falling: the EEPROM puts its next bit (or the acknowledge) on SDA.
    fall() {
      const st = this.state;
      if (st === ST_READ) this.out = this.bit < 8 ? (this.mem[this.addr] >> (7 - this.bit)) & 1 : 1;
      else if (st !== ST_IDLE) this.out = this.bit === 8 ? 0 : 1;
      else this.out = 1;
    }
    // What the game reads on SDA: the line is low if either side pulls it low.
    level() { return this.out & this.sda; }
    saveState() { return { mem: this.mem.slice(), r: [this.scl, this.sda, this.out, this.state, this.bit, this.shift, this.addr] }; }
    loadState(o) { this.mem.set(o.mem.subarray(0, this.mem.length)); [this.scl, this.sda, this.out, this.state, this.bit, this.shift, this.addr] = o.r; }
  }

  // How each maker wired the EEPROM: [address, bit] for SDA in, SCL, SDA out.
  const WIRING = {
    sega: { sda: [0x200001, 0], scl: [0x200001, 1], out: [0x200001, 0] },
    ea: { sda: [0x200001, 7], scl: [0x200001, 6], out: [0x200001, 7] },
    acclaim16: { sda: [0x200001, 0], scl: [0x200001, 1], out: [0x200001, 1] },
    acclaim32: { sda: [0x200001, 0], scl: [0x200000, 0], out: [0x200001, 0] },
    codemasters: { sda: [0x300000, 0], scl: [0x300000, 1], out: [0x380001, 7] },
  };
  // chip: [bytes, mode, page size]
  const X24C01 = [128, 1, 4], C02 = [256, 2, 8], C04 = [512, 2, 16], C08 = [1024, 2, 16], C16 = [2048, 2, 16], C65 = [8192, 3, 64];
  // Games with an EEPROM, by the product code in the header (without the "-xx" revision).
  const EEPROM_GAMES = {
    // Sega
    "MK-1215": ["sega", X24C01],   // Wonder Boy in Monster World
    "G-4060": ["sega", X24C01],    // Wonder Boy V: Monster World III
    "MK-1079": ["sega", X24C01],   // Evander Holyfield's "Real Deal" Boxing
    "MK-1063": ["sega", X24C01],   // Sports Talk Baseball
    "MK-12056": ["sega", X24C01],  // Mega Man: The Wily Wars
    "T-12046": ["sega", X24C01],   // Rockman Mega World
    "T-12053": ["sega", X24C01],   // Mega Man: The Wily Wars (Europe)
    "MK-1199": ["sega", X24C01],   // Greatest Heavyweights
    "G-4524": ["sega", X24C01],    // Game Toshokan
    "G-5538": ["sega", X24C01],    // Ninja Burai Densetsu
    "G-4528": ["sega", X24C01],    // Honoo no Toukyuuji Dodge Danpei
    // Electronic Arts
    "T-50176": ["ea", X24C01],     // Rings of Power
    "T-50396": ["ea", X24C01],     // NHLPA Hockey '93
    "T-50446": ["ea", X24C01],     // John Madden Football '93
    "T-50516": ["ea", X24C01],     // John Madden Football '93: Championship Edition
    "T-50286": ["ea", X24C01],     // Bill Walsh College Football
    // Acclaim
    "T-081326": ["acclaim16", C02], // NBA Jam
    "T-81033": ["acclaim16", C02],  // NBA Jam (Japan)
    "T-81406": ["acclaim32", C04],  // NBA Jam Tournament Edition
    "T-081276": ["acclaim32", C02], // NFL Quarterback Club
    "T-081586": ["acclaim32", C16], // NFL Quarterback Club '96
    "T-81576": ["acclaim32", C16],  // College Slam
    "T-81476": ["acclaim32", C65],  // Frank Thomas Big Hurt Baseball
  };
  const headerText = (rom, from, to) => String.fromCharCode(...rom.subarray(from, to));
  const gameCode = (rom) => headerText(rom, 0x180, 0x18e).replace(/^\s*(GM|AI|BR|AL)\s+/, "").trim().split(/[\s-]+(?=\d\d$)|\s+/)[0];
  const gameNames = (rom) => (headerText(rom, 0x120, 0x150) + " " + headerText(rom, 0x150, 0x180)).toUpperCase().replace(/\s+/g, " ");
  function findLong(rom, addr, limit) {
    const b0 = addr >>> 24, b1 = (addr >> 16) & 255, b2 = (addr >> 8) & 255, b3 = addr & 255, n = Math.min(rom.length, limit || rom.length) - 3;
    for (let i = 0; i < n; i += 2) if (rom[i + 3] === b3 && rom[i + 2] === b2 && rom[i + 1] === b1 && rom[i] === b0) return true;
    return false;
  }
  function eepromFor(rom) {
    const code = gameCode(rom);
    for (const k in EEPROM_GAMES) if (code === k || code.startsWith(k + "-")) return EEPROM_GAMES[k];
    const names = gameNames(rom);
    // Codemasters: their codes vary, so recognise the games by name and by the EEPROM's read address.
    if (/MICRO ?MACHINES|BRIAN ?LARA|SHANE ?WARNE/.test(names) && findLong(rom, 0x380001)) {
      if (/BRIAN ?LARA.*96|SHANE ?WARNE/.test(names)) return ["codemasters", C65];
      if (/96|TURBO TOURNAMENT/.test(names) && !/2/.test(names.replace(/96/g, ""))) return ["codemasters", C16];
      return ["codemasters", C08];
    }
    // Header marks a serial EEPROM at $200001
    if (rom[0x1b0] === 0x52 && rom[0x1b1] === 0x41 && rom[0x1b2] === 0xe8 && rom[0x1b3] === 0x40) return ["sega", X24C01];
    return null;
  }

  // ===================================================================
  // Games that need the 3-button pad (they misread a 6-button pad).
  // ===================================================================
  const THREE_BUTTON = [/MS\.? ?PAC-?MAN/, /GOLDEN AXE II\b/, /FORGOTTEN WORLDS/, /SUNSET RIDERS/, /ARCH RIVALS/, /SECOND SAMURAI/,
    /UNNECESSARY ROUGHNESS/, /CAPTAIN AMERICA/];
  function needsThreeButton(rom) {
    const io = headerText(rom, 0x190, 0x1a0).toUpperCase();
    if (io.includes("6")) return false; // the game says it knows the 6-button pad
    const names = gameNames(rom);
    return THREE_BUTTON.some((re) => re.test(names));
  }

  // Virtua Racing's SVP chip runs its own program from a ROM inside the chip.
  function isSVP(rom) {
    const code = gameCode(rom), names = gameNames(rom);
    return /VIRTUA ?RACING/.test(names) || code === "MK-1229" || code === "G-7001";
  }

  // Realtec carts keep their header in the last 8K: at power-on that 8K is seen everywhere.
  const isRealtec = (rom, size) => size === 0x80000 && headerText(rom, 0x100, 0x104) !== "SEGA" && headerText(rom, 0x7e100, 0x7e104) === "SEGA";

  // ===================================================================
  // The cartridge
  // ===================================================================
  class Cart {
    constructor(rom, realSize, sram, input) {
      this.rom = rom; this.romSize = realSize; this.romMask = rom.length - 1;
      this.input = input; // function(p) -> pad bits, for the J-Cart's pads
      if (isSVP(rom)) throw new Error("Virtua Racing uses Sega's SVP chip, which runs a program stored inside the chip itself. RetroPixels can't play it without that program, so this game isn't supported.");
      // ---- battery SRAM at $200000 ----
      this.sram = new Uint8Array(0x10000).fill(0xff);
      const hasRA = rom[0x1b0] === 0x52 && rom[0x1b1] === 0x41;
      this.sramStart = 0x200000; this.sramEnd = 0x20ffff;
      if (hasRA) {
        const st = ((rom[0x1b4] << 24) | (rom[0x1b5] << 16) | (rom[0x1b6] << 8) | rom[0x1b7]) >>> 0;
        const en = ((rom[0x1b8] << 24) | (rom[0x1b9] << 16) | (rom[0x1ba] << 8) | rom[0x1bb]) >>> 0;
        if (st >= 0x200000 && st < 0x400000 && en >= st) { this.sramStart = st & ~1; this.sramEnd = Math.min(en | 1, this.sramStart + 0xffff); }
      }
      this.sramMapped = realSize <= 0x200000; // bigger games switch it in through $A130F1
      this.sramProtect = false;
      this.sramUsed = hasRA;
      if (sram && sram.length) { this.sram.set(sram.subarray(0, 0x10000)); this.sramUsed = true; }
      // ---- EEPROM ----
      const ee = eepromFor(rom);
      if (ee) {
        const [wiring, chip] = ee;
        this.eeWire = WIRING[wiring]; this.eeprom = new EEPROM(chip[0], chip[1], chip[2]);
        this.sramMapped = false; this.sramUsed = true;
        if (sram && sram.length) this.eeprom.mem.set(sram.subarray(0, chip[0]));
      }
      // ---- mappers ----
      this.mapper = isRealtec(rom, realSize) ? "realtec" : realSize > 0x400000 ? "ssf2" : "";
      this.banks = new Uint8Array(8); for (let i = 0; i < 8; i++) this.banks[i] = i;
      this.rt = new Uint8Array(3); // Realtec registers ($400000, $402000, $404000)
      // ---- J-Cart: two more pads read at $38FFFE ----
      this.jcart = findLong(rom, 0x38fffe, 0x80000) || findLong(rom, 0x38ffff, 0x80000);
      this.jth = 0;
      this.threeButton = needsThreeButton(rom);
      // 64K pages of the 68000's view of the cartridge
      this.pageBase = new Int32Array(64); this.pageMask = new Int32Array(64);
      this.remap();
    }
    // The console's reset button also resets the mapper registers.
    reset() {
      for (let i = 0; i < 8; i++) this.banks[i] = i;
      this.rt.fill(0); this.jth = 0;
      this.sramMapped = this.romSize <= 0x200000 && !this.eeprom; this.sramProtect = false;
      this.remap();
    }
    remap() {
      for (let p = 0; p < 64; p++) {
        let base = p << 16, mask = 0xffff;
        if (this.mapper === "ssf2") base = (this.banks[p >> 3] << 19) | ((p & 7) << 16);
        else if (this.mapper === "realtec") {
          const on = this.rt[0] & 1;
          if (!on) { base = 0x7e000; mask = 0x1fff; } // the boot 8K everywhere
          else {
            const bank = ((this.rt[0] & 6) << 2) | (this.rt[2] & 7), size = (this.rt[1] & 0x1f) || 1;
            base = ((bank + ((p >> 1) % size)) << 17) | ((p & 1) << 16);
          }
        }
        this.pageBase[p] = base & this.romMask; this.pageMask[p] = mask;
      }
    }
    romByte(a) { const p = a >> 16; return this.rom[(this.pageBase[p] + (a & this.pageMask[p])) & this.romMask]; }
    // ---- 68000 reads ($000000-$3FFFFF) ----
    read8(a) {
      if (a >= 0x200000) {
        if (this.eeprom && (a & ~1) === (this.eeWire.out[0] & ~1)) {
          const w = this.eeWire.out;
          return a === w[0] ? this.eeprom.level() << w[1] : 0;
        }
        if (this.jcart && (a & ~1) === 0x38fffe) { const w = this.jcartWord(); return (a & 1) ? w & 0xff : w >> 8; }
        if (this.sramMapped && a >= this.sramStart && a <= this.sramEnd) return this.sram[a - 0x200000];
      }
      return this.romByte(a);
    }
    read16(a) {
      if (a >= 0x200000) {
        if ((this.eeprom && a === (this.eeWire.out[0] & ~1)) || (this.jcart && a === 0x38fffe)) return (this.read8(a) << 8) | this.read8(a + 1);
        if (this.sramMapped && a >= this.sramStart && a <= this.sramEnd) { const o = a - 0x200000; return (this.sram[o] << 8) | this.sram[o + 1]; }
      }
      const p = a >> 16, o = (this.pageBase[p] + (a & this.pageMask[p])) & this.romMask;
      return (this.rom[o] << 8) | this.rom[o + 1];
    }
    // ---- 68000 writes ----
    write8(a, v) {
      if (this.eeprom && (a === this.eeWire.sda[0] || a === this.eeWire.scl[0])) { this.eeWrite([[a, v]]); return; }
      if (this.jcart && (a & ~1) === 0x38fffe) { if (a & 1) this.jth = v & 1; return; }
      if (this.mapper === "realtec" && a >= 0x400000) return;
      if (this.sramMapped && !this.sramProtect && a >= this.sramStart && a <= this.sramEnd) {
        const o = a - 0x200000;
        if (this.sram[o] !== v) { this.sram[o] = v; this.sramUsed = true; }
      }
    }
    write16(a, v) {
      if (this.eeprom && ((a & ~1) === (this.eeWire.sda[0] & ~1) || (a & ~1) === (this.eeWire.scl[0] & ~1))) { this.eeWrite([[a, v >> 8], [a + 1, v & 0xff]]); return; }
      if (this.jcart && a === 0x38fffe) { this.jth = v & 1; return; }
      this.write8(a, v >> 8); this.write8(a + 1, v & 0xff);
    }
    eeWrite(pairs) {
      const w = this.eeWire, e = this.eeprom;
      let sda = e.sda, scl = e.scl, hit = false;
      for (const [ad, b] of pairs) {
        if (ad === w.sda[0]) { sda = (b >> w.sda[1]) & 1; hit = true; }
        if (ad === w.scl[0]) { scl = (b >> w.scl[1]) & 1; hit = true; }
      }
      if (hit) e.set(scl, sda);
    }
    // Writes to $400000-$7FFFFF (Realtec's registers live there).
    highWrite(a, v) {
      if (this.mapper !== "realtec") return;
      if (a === 0x400000 || a === 0x400001) this.rt[0] = v; else if (a === 0x402000 || a === 0x402001) this.rt[1] = v; else if (a === 0x404000 || a === 0x404001) this.rt[2] = v; else return;
      this.remap();
    }
    // The $A130xx registers: SRAM switch at $A130F1, SSF2 banks at $A130F3-$A130FF.
    timeWrite(a, v) {
      if (a === 0xa130f1) { this.sramMapped = (v & 1) !== 0 || this.romSize <= 0x200000; if (this.eeprom) this.sramMapped = false; this.sramProtect = (v & 2) !== 0; }
      else if (this.mapper === "ssf2" && (a & 1) && a > 0xa130f1 && a < 0xa13100) { this.banks[(a & 0xf) >> 1] = v & 0x3f; this.remap(); }
    }
    // J-Cart: pads 3 (low byte) and 4 (high byte) as 3-button pads, with the TH line the game sets.
    jcartWord() {
      const pad = (bits) => {
        const on = (n) => (bits & (1 << n) ? 0 : 1); // bit order: A B C Start X Y Z Mode Up Down Left Right
        if (this.jth) return 0x40 | (on(2) << 5) | (on(1) << 4) | (on(11) << 3) | (on(10) << 2) | (on(9) << 1) | on(8);
        return (on(3) << 5) | (on(0) << 4) | (on(9) << 1) | on(8);
      };
      return (pad(this.input(3)) << 8) | pad(this.input(2));
    }
    getSRAM() { if (this.eeprom) return this.eeprom.mem.slice(); return this.sramUsed ? this.sram.slice(0, Math.max(0x4000, this.sramEnd - 0x200000 + 1)) : null; }
    saveState() {
      return { sram: this.sram.slice(), banks: this.banks.slice(), rt: this.rt.slice(), m: [this.sramMapped ? 1 : 0, this.sramProtect ? 1 : 0, this.jth],
        ee: this.eeprom ? this.eeprom.saveState() : null };
    }
    loadState(o) {
      this.sram.set(o.sram); this.banks.set(o.banks); if (o.rt) this.rt.set(o.rt);
      this.sramMapped = !!o.m[0]; this.sramProtect = !!o.m[1]; this.jth = o.m[2] || 0;
      if (this.eeprom && o.ee) this.eeprom.loadState(o.ee);
      this.remap();
    }
  }

  global.RPGenesisCart = { Cart, EEPROM, eepromFor, needsThreeButton, isSVP, gameCode };
})(typeof window !== "undefined" ? window : globalThis);
