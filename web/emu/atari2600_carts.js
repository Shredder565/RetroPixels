// RetroPixels — Atari 2600 cartridges: every bank-switching scheme RetroPixels
// knows, and how a game file's scheme is recognised. Original code, written from
// the documented behaviour of the cartridge hardware.
//
// A cartridge sees the 4K at $1000-$1FFF. Some schemes also watch the whole bus
// (access()), the value on the bus after an access (watch()), or TIA writes
// (tiaWrite()). Detection uses the file size, the hotspots the game's code
// touches (found by following the code from its start vector), a few text
// markers, and a scheme name in the file name, e.g.
// "Game (F8SC).bin" or "Game [3E+].a26".
"use strict";

(function (global) {
  const K = 1024;

  // ------------------------------------------------------------------
  // Byte-search helpers
  // ------------------------------------------------------------------
  function countSig(rom, s, limit) {
    let n = 0;
    const end = rom.length - s.length, first = s[0];
    outer: for (let i = 0; i <= end; i++) {
      if (rom[i] !== first) continue;
      for (let j = 1; j < s.length; j++) if (rom[i + j] !== s[j]) continue outer;
      if (++n >= (limit || 1e9)) return n;
    }
    return n;
  }
  const hasText = (rom, str) => countSig(rom, [...str].map((c) => c.charCodeAt(0)), 1) > 0;
  // Absolute-address instructions (loads, stores, compares, BIT, NOP abs) whose address passes test().
  const ABS_OPS = new Set([0xad, 0xae, 0xac, 0x2c, 0x0c, 0x8d, 0x8e, 0x8c, 0xcd, 0xec, 0xcc, 0xbd, 0xb9, 0x9d, 0x99, 0xbe, 0xbc]);
  function countAbs(rom, test) {
    let n = 0;
    for (let i = 0; i + 2 < rom.length; i++) if (ABS_OPS.has(rom[i]) && test(rom[i + 1] | (rom[i + 2] << 8))) n++;
    return n;
  }
  const INDEXED = new Set([0xbd, 0xb9, 0x9d, 0x99, 0xbe, 0xbc]);
  const PLAIN = new Set([...ABS_OPS].filter((op) => !INDEXED.has(op)));
  const INDEXED_STORES = new Set([0x9d, 0x99]);

  // ------------------------------------------------------------------
  // Following the game's code
  // ------------------------------------------------------------------
  // Data and the middle of instructions are full of byte pairs that look like
  // addresses, so detection only looks at instructions reached by following the
  // code from the cartridge's start vectors.
  //
  // Length of a 6502 instruction, from its opcode's bit fields (aaabbbcc); 0 for
  // those after which the code doesn't simply go on (BRK, RTI, RTS, JMP, the jams).
  function opLength(op) {
    if (op === 0x00 || op === 0x40 || op === 0x60 || op === 0x4c || op === 0x6c) return 0;
    if (op === 0x20) return 3;
    const b = (op >> 2) & 7, c = op & 3;
    if (c === 2 && (b === 4 || (b === 0 && op < 0x80))) return 0;
    if (b === 3 || b === 7) return 3;                 // absolute, absolute,X
    if (b === 2) return c & 1 ? 2 : 1;                 // immediate, or implied / accumulator
    if (b === 6) return c & 1 ? 3 : 1;                 // absolute,Y, or implied
    return 2;                                          // zero page forms, (zp,X), (zp),Y, immediate, branches
  }
  // Follow the code in rom[start .. start+size), built to run at address home,
  // from the entry offsets given. visit(op, operand) sees every instruction with an
  // operand. Jumps and calls are followed when they land in the same block.
  function trace(rom, start, size, home, entries, visit) {
    const seen = new Uint8Array(size), todo = entries.slice();
    while (todo.length) {
      let pc = todo.pop();
      while (pc >= 0 && pc < size - 2 && !seen[pc]) {
        seen[pc] = 1;
        const op = rom[start + pc], len = opLength(op), lo = rom[start + pc + 1], word = lo | (rom[start + pc + 2] << 8);
        if (op === 0x20 || op === 0x4c) { visit(op, word); if ((word & -size & 0xffff) === home) todo.push(word & (size - 1)); }
        else if (len === 3) visit(op, word);
        else if (len === 2) visit(op, lo);
        if ((op & 0x1f) === 0x10) todo.push(pc + 2 + ((lo ^ 0x80) - 0x80)); // branch
        if (!len) break;
        pc += len;
      }
    }
  }
  // Every instruction (op, operand) reached from the start vectors at the end of each 4K
  // (or of the whole file, when it is smaller).
  const vector = (rom, end) => rom[end - 4] | (rom[end - 3] << 8);
  const codeCache = new WeakMap();
  function code(rom) {
    let list = codeCache.get(rom);
    if (list) return list;
    list = [];
    const size = rom.length >= 4096 ? 4096 : rom.length >= 2048 ? 2048 : 1024;
    for (let end = rom.length; end - size >= 0; end -= size) {
      const reset = vector(rom, end), irq = vector(rom, end + 2), home = reset & -size & 0xffff;
      if (!(reset & 0x1000)) continue;
      const entries = [reset & (size - 1)];
      if ((irq & -size & 0xffff) === home) entries.push(irq & (size - 1));
      trace(rom, end - size, size, home, entries, (op, a) => list.push([op, a]));
    }
    codeCache.set(rom, list);
    return list;
  }
  // The different addresses (as the cartridge sees them, 13 bits) that the code's
  // absolute instructions touch and that pass test() (given the address as written).
  // Plain instructions only, unless ops says otherwise; an indexed one is taken to
  // reach one byte past its base.
  function touched(rom, test, ops) {
    const seen = new Set();
    ops = ops || PLAIN;
    for (const [op, a] of code(rom)) {
      if (!ops.has(op)) continue;
      if (test(a)) seen.add(a & 0x1fff);
      if (INDEXED.has(op) && test((a + 1) & 0xffff)) seen.add((a + 1) & 0x1fff);
    }
    return seen;
  }
  const between = (lo, hi) => (a) => (a & 0x1fff) >= lo && (a & 0x1fff) <= hi;
  // Does the code touch the cartridge addresses lo-hi (e.g. the F8 hotspots)?
  const uses = (rom, lo, hi) => touched(rom, between(lo, hi), ABS_OPS).size > 0;
  // Zero-page writes (STA / STX / STY) to address z.
  const writesZp = (rom, z) => code(rom).some(([op, a]) => (op === 0x85 || op === 0x86 || op === 0x84) && a === z);

  // Each scheme's own hotspots, as its hardware decodes them:
  // Parker Brothers E0 - two different slice selects in $1FE0-$1FF7, and none of F8's.
  const isE0 = (rom) => touched(rom, between(0x1fe0, 0x1ff7)).size >= 2 && !uses(rom, 0x1ff8, 0x1ff9);
  // M-Network E7 - two different selects in $1FE0-$1FEB, and none of the plain scheme's (F6 or FA).
  const isE7 = (rom, lo, hi) => touched(rom, between(0x1fe0, 0x1feb)).size >= 2 && !uses(rom, lo, hi);
  // Both halves of a two-bank scheme selected, written as $0xxx addresses: for
  // 0840 address bit 6 picks the bank; for UA it is $0220 / $0240 (or a mirror).
  const both = (set, bank1) => { let a = false, b = false; for (const x of set) { if (bank1(x)) b = true; else a = true; } return a && b; };
  const is0840 = (rom) => both(touched(rom, (a) => a >= 0x0800 && a < 0x1000), (a) => a & 0x40);
  const isUA = (rom) => both(touched(rom, (a) => a < 0x1000 && ((a & 0x1260) === 0x0220 || (a & 0x1260) === 0x0240), ABS_OPS), (a) => (a & 0x1260) === 0x0240);
  // Activision FE: the second 4K is built for $F000 and calls into the first, built
  // for $D000, which calls back; nothing touches F8's hotspots.
  function isFE(rom) {
    if (rom.length !== 8192 || (vector(rom, 8192) & 0xf000) !== 0xf000 || uses(rom, 0x1ff8, 0x1ff9)) return false;
    const into = (list, hi) => list.filter(([op, a]) => op === 0x20 && (a & 0xf000) === hi).map(([, a]) => a & 0xfff);
    const low = [];
    trace(rom, 0, 4096, 0xd000, into(code(rom), 0xd000), (op, a) => low.push([op, a]));
    return into(low, 0xf000).length > 0 && !low.some(([op, a]) => ABS_OPS.has(op) && between(0x1ff8, 0x1ff9)(a));
  }
  // CommaVid: indexed stores into the RAM's write port, $1400-$17FF.
  const isCV = (rom) => touched(rom, (a) => (a & 0x1c00) === 0x1400 && (a & 0x1000), INDEXED_STORES).size > 0;
  // A Superchip cart leaves the RAM's address range in every bank unused (filled with one value).
  // (Some dumps instead show the RAM's read port as a copy of its write port.)
  function hasSuperchip(rom) {
    let filled = true, mirrored = true;
    for (let b = 0; b < rom.length; b += 4096) {
      const v = rom[b];
      for (let i = 1; i < 256; i++) if (rom[b + i] !== v) { filled = false; break; }
      for (let i = 0; i < 128; i++) if (rom[b + i] !== rom[b + 128 + i]) { mirrored = false; break; }
    }
    return filled || mirrored;
  }
  // Tigervision 3F: the code selects banks by writing $3F; 3E also writes $3E (its RAM).
  const tigervision = (rom) => writesZp(rom, 0x3f);
  const ram3E = (rom) => writesZp(rom, 0x3e) && tigervision(rom);

  // Scheme names RetroPixels understands (and a few other names people use for them).
  const NAMES = ["2K", "4K", "CV", "F8", "F8SC", "F6", "F6SC", "F4", "F4SC", "FA", "FA2", "EF", "EFSC", "DF", "DFSC", "BF", "BFSC",
    "F0", "E0", "E7", "FE", "3F", "3E", "3E+", "0840", "UA", "UASW", "X07", "SB", "MDM", "DPC", "DPC+", "AR", "4A50", "CDF", "CDFJ", "CDFJ+"];
  const ALIASES = { "3EP": "3E+", "3EPLUS": "3E+", "DPCP": "DPC+", "MB": "F0", "SC": "F8SC", "SUPERCHARGER": "AR", "ECONOBANKING": "0840" };
  function typeFromName(name) {
    if (!name) return null;
    const tags = String(name).toUpperCase().match(/[[(]([^\])]+)[\])]/g) || [];
    for (const t of tags) {
      for (let w of t.slice(1, -1).split(/[\s,;]+/)) {
        w = w.replace(/^BS[-_]?/, "");
        if (ALIASES[w]) return ALIASES[w];
        if (NAMES.includes(w)) return w;
      }
    }
    return null;
  }

  // Work out the scheme from the file itself.
  function detectType(rom, name) {
    const hint = typeFromName(name);
    if (hint) return hint;
    const n = rom.length;
    if (n >= 8448 && n % 8448 === 0) return "AR";
    if (n === 6144) return "AR6K";
    if (n === 10240 || n === 10495) return "DPC";
    if (hasText(rom, "TJ3E")) return "3E+";
    if (n >= 8192 && n % 8192 === 0 && n <= 8192 * 1024 && hasText(rom.subarray(0, 4096), "MDMC")) return "MDM";
    if (n <= 2048) return isCV(rom) ? "CV" : "2K";
    if (n <= 4096) return isCV(rom) ? "CV" : n === 4096 ? "4K" : "2K";
    if (n === 8192) {
      if (isE0(rom)) return "E0";
      if (is0840(rom)) return "0840";
      if (isUA(rom)) return "UA";
      if (ram3E(rom)) return "3E";
      if (tigervision(rom)) return "3F";
      if (isFE(rom)) return "FE";
      return hasSuperchip(rom) ? "F8SC" : "F8";
    }
    if (n === 12288) {
      if (isE7(rom, 0x1ff8, 0x1ffa)) return "E7";
      return "FA";
    }
    if (n === 16384) {
      if (isE7(rom, 0x1ff6, 0x1ff9)) return "E7";
      if (ram3E(rom)) return "3E";
      if (tigervision(rom)) return "3F";
      return hasSuperchip(rom) ? "F6SC" : "F6";
    }
    if (n === 24 * K || n === 28 * K || n === 29 * K) return "FA2";
    if (n === 32768) {
      if (hasText(rom, "DPC+")) return "DPC+";
      if (hasText(rom, "CDFJ")) return "CDFJ";
      if (hasText(rom, "CDF")) return "CDF";
      if (ram3E(rom)) return "3E";
      if (tigervision(rom)) return "3F";
      return hasSuperchip(rom) ? "F4SC" : "F4";
    }
    if (n === 65536) {
      if (hasText(rom, "CDFJ")) return "CDFJ";
      if (hasText(rom, "EFEF")) return "EF";
      if (hasText(rom, "EFSC")) return "EFSC";
      if (ram3E(rom)) return "3E";
      if (tigervision(rom)) return "3F";
      // X07: hotspots at $080D-$08FD
      if (countAbs(rom, (a) => (a & 0x180f) === 0x080d) >= 2) return "X07";
      // F0 / Megaboy: one hotspot at $1FF0 that steps through the banks
      if (countAbs(rom, (a) => (a & 0x1fff) === 0x1ff0) >= 1 && !countAbs(rom, (a) => (a & 0x1fff) > 0x1fe0 && (a & 0x1fff) <= 0x1fef)) return "F0";
      return hasSuperchip(rom) ? "EFSC" : "EF";
    }
    if (n === 131072) {
      if (hasText(rom, "DFSC")) return "DFSC";
      if (hasText(rom, "DFDF")) return "DF";
      if (ram3E(rom)) return "3E";
      if (tigervision(rom)) return "3F";
      if (countAbs(rom, (a) => (a & 0x1fff) >= 0x1fc0 && (a & 0x1fff) <= 0x1fdf) >= 2) return hasSuperchip(rom) ? "DFSC" : "DF";
      return "SB";
    }
    if (n === 262144) {
      if (hasText(rom, "BFSC")) return "BFSC";
      if (hasText(rom, "BFBF")) return "BF";
      if (ram3E(rom)) return "3E";
      if (tigervision(rom)) return "3F";
      if (countAbs(rom, (a) => (a & 0x1fff) >= 0x1f80 && (a & 0x1fff) <= 0x1fbf) >= 2) return hasSuperchip(rom) ? "BFSC" : "BF";
      return "SB";
    }
    if (n % 2048 === 0 && ram3E(rom)) return "3E";
    if (n % 2048 === 0 && tigervision(rom)) return "3F";
    if (n % 4096 === 0 && n > 65536) return "SB";
    throw new Error("This Atari 2600 cartridge uses hardware RetroPixels doesn't support yet (" + n + " bytes).");
  }

  const UNSUPPORTED = {
    "4A50": "This Atari 2600 game uses the 4A50 cartridge, which RetroPixels doesn't support yet.",
    CDF: "This Atari 2600 game uses a CDF (Harmony / Melody) cartridge, which RetroPixels doesn't support yet.",
    CDFJ: "This Atari 2600 game uses a CDFJ (Harmony / Melody) cartridge, which RetroPixels doesn't support yet.",
    "CDFJ+": "This Atari 2600 game uses a CDFJ+ (Harmony / Melody) cartridge, which RetroPixels doesn't support yet.",
    AR6K: "This looks like a 6K Supercharger dump without load headers. RetroPixels needs the usual Supercharger file (8448 bytes per load).",
  };

  // ------------------------------------------------------------------
  // The base cartridge: plain 2K / 4K ROM
  // ------------------------------------------------------------------
  class Cart {
    constructor(rom, type) {
      this.type = type;
      this.rom = rom;
      this.bank = 0;
      this.snoop = false;    // wants access() for every bus access
      this.tiaHook = false;  // wants tiaWrite() for writes to $00-$3F
    }
    read(a) { return this.rom[a & (this.rom.length - 1)]; }
    write() {}
    getSRAM() { return null; }
    // Save state: every field except the ROM and things that never change.
    saveState() {
      const o = {}, skip = this.fixedKeys();
      for (const k of Object.keys(this)) {
        if (skip.includes(k)) continue;
        const v = this[k];
        if (typeof v === "function") continue;
        o[k] = ArrayBuffer.isView(v) ? v.slice() : Array.isArray(v) ? v.slice() : v;
      }
      return o;
    }
    loadState(o) {
      const skip = this.fixedKeys();
      for (const k in o) {
        if (skip.includes(k)) continue;
        if (ArrayBuffer.isView(this[k])) this[k].set(o[k]);
        else this[k] = Array.isArray(o[k]) ? o[k].slice() : o[k];
      }
    }
    fixedKeys() { return ["rom", "type", "snoop", "tiaHook", "nBanks", "hsLo", "ramSize", "image", "dd", "arm", "flash", "bios", "loads"]; }
  }

  // ------------------------------------------------------------------
  // Hotspot schemes: touching one of a run of addresses near the top
  // selects a 4K bank (F8, F6, F4, FA, FA2, EF, DF, BF), optionally with
  // the 128-byte Superchip RAM (write $1000-$107F, read $1080-$10FF) or
  // CBS's 256 bytes (write $1000-$10FF, read $1100-$11FF).
  // ------------------------------------------------------------------
  const FX = {
    F8: [0xff8, 2, 1], F6: [0xff6, 4, 0], F4: [0xff4, 8, 0], FA: [0xff8, 3, 2], FA2: [0xff5, 7, 0],
    EF: [0xfe0, 16, 15], DF: [0xfc0, 32, 31], BF: [0xf80, 64, 63],
  };
  class HotspotCart extends Cart {
    constructor(rom, type, sram) {
      const base = type.replace(/SC$/, "");
      super(rom, type);
      let [lo, banks, start] = FX[base];
      if (base === "FA2") { if (rom.length === 29 * K) rom = this.rom = rom.subarray(K); banks = rom.length >> 12; }
      this.hsLo = lo; this.nBanks = Math.max(1, Math.min(banks, rom.length >> 12)); this.bank = Math.min(start, this.nBanks - 1);
      this.ramSize = /SC$/.test(type) ? 128 : (base === "FA" || base === "FA2") ? 256 : 0;
      this.ram = this.ramSize ? new Uint8Array(this.ramSize) : null;
      if (this.ram) for (let i = 0; i < this.ramSize; i++) this.ram[i] = (i * 37 + 11) & 0xff;
      if (base === "FA2") { this.flash = new Uint8Array(256); if (sram && sram.length) this.flash.set(sram.subarray(0, 256)); this.flashUsed = !!(sram && sram.length); }
    }
    hot(off) {
      const i = off - this.hsLo;
      if (i >= 0 && i < this.nBanks) this.bank = i;
      else if (this.flash && off === 0xff4) this.flashOp();
    }
    // FA2 on a Harmony cart can keep its 256 bytes of RAM in flash memory:
    // the game puts 1 (load) or 2 (save) in its last RAM byte and touches $1FF4.
    flashOp() {
      const cmd = this.ram[255];
      if (cmd === 1) this.ram.set(this.flash.subarray(0, 255));
      else if (cmd === 2) { this.flash.set(this.ram.subarray(0, 255)); this.flashUsed = true; }
      if (cmd === 1 || cmd === 2) this.ram[255] = 0;
    }
    read(a) {
      const off = a & 0xfff;
      this.hot(off);
      const rs = this.ramSize;
      if (rs && off < 2 * rs) return off >= rs ? this.ram[off - rs] : this.ram[off]; // reading the write port gives junk; use the RAM
      return this.rom[(this.bank << 12) | off];
    }
    write(a, v) {
      const off = a & 0xfff;
      this.hot(off);
      if (this.ramSize && off < this.ramSize) this.ram[off] = v;
    }
    getSRAM() { return this.flash && this.flashUsed ? this.flash.slice() : null; }
  }

  // F0 (Dynacom Megaboy): every touch of $1FF0 steps to the next of 16 banks.
  class F0Cart extends Cart {
    constructor(rom, type) { super(rom, type); this.nBanks = rom.length >> 12; this.bank = this.nBanks - 1; }
    read(a) { const off = a & 0xfff; if (off === 0xff0) this.bank = (this.bank + 1) % this.nBanks; return this.rom[(this.bank << 12) | off]; }
    write(a) { if ((a & 0xfff) === 0xff0) this.bank = (this.bank + 1) % this.nBanks; }
  }

  // ------------------------------------------------------------------
  // Parker Brothers (E0): four 1K slices; the last is fixed to the last 1K.
  // ------------------------------------------------------------------
  class E0Cart extends Cart {
    constructor(rom, type) { super(rom, type); this.slices = [4, 5, 6, 7]; }
    hot(off) { if (off >= 0xfe0 && off <= 0xff7) this.slices[(off - 0xfe0) >> 3] = off & 7; }
    read(a) { const off = a & 0xfff; this.hot(off); return this.rom[this.slices[off >> 10] * 1024 + (off & 0x3ff)]; }
    write(a) { this.hot(a & 0xfff); }
  }

  // ------------------------------------------------------------------
  // M-Network (E7): 2K slice at $1000 (ROM, or 1K of RAM), 256-byte RAM
  // window at $1800, and the end of the last 2K fixed at $1A00-$1FFF.
  // 12K versions simply lack the first two 2K slices.
  // ------------------------------------------------------------------
  class E7Cart extends Cart {
    constructor(rom, type) {
      if (rom.length < 16384) { const r = new Uint8Array(16384); r.set(rom, 16384 - rom.length); rom = r; }
      super(rom, type);
      this.bank = 0; this.ramBank = 0; this.ram = new Uint8Array(2048);
    }
    hot(off) {
      if (off >= 0xfe0 && off <= 0xfe7) this.bank = off - 0xfe0;
      else if (off >= 0xfe8 && off <= 0xfeb) this.ramBank = off - 0xfe8;
    }
    read(a) {
      const off = a & 0xfff, rom = this.rom;
      this.hot(off);
      if (off < 0x800) {
        if (this.bank === 7) return this.ram[off & 0x3ff];
        return rom[this.bank * 2048 + off];
      }
      if (off < 0xa00) return this.ram[1024 + this.ramBank * 256 + (off & 0xff)];
      return rom[7 * 2048 + (off - 0x800)];
    }
    write(a, v) {
      const off = a & 0xfff;
      this.hot(off);
      if (off < 0x400 && this.bank === 7) this.ram[off] = v;
      else if (off >= 0x800 && off < 0x900) this.ram[1024 + this.ramBank * 256 + (off - 0x800)] = v;
    }
  }

  // ------------------------------------------------------------------
  // Activision (FE): the bank follows bit 5 of the high byte of a JSR/RTS
  // return address, seen on the bus right after an access to $01FE.
  // ------------------------------------------------------------------
  class FECart extends Cart {
    constructor(rom, type) { super(rom, type); this.bank = 0; this.lastFE = false; }
    read(a) { return this.rom[(this.bank << 12) | (a & 0xfff)]; }
    watch(a, v) {
      if (this.lastFE) this.bank = (v & 0x20) ? 0 : 1;
      this.lastFE = a === 0x01fe;
    }
  }

  // ------------------------------------------------------------------
  // Tigervision (3F): writing to $00-$3F picks the 2K ROM bank at $1000;
  // the last 2K is fixed at $1800. 3E adds up to 32K of RAM: writing to $3E
  // puts a 1K RAM bank at $1000 (read $1000-$13FF, write $1400-$17FF).
  // ------------------------------------------------------------------
  class TigerCart extends Cart {
    constructor(rom, type) {
      super(rom, type);
      this.tiaHook = true; this.nBanks = rom.length >> 11; this.bank = 0; this.isRam = false;
      if (type === "3E") this.ram = new Uint8Array(32 * K);
    }
    read(a) {
      const off = a & 0xfff;
      if (off >= 0x800) return this.rom[(this.nBanks - 1) * 2048 + (off - 0x800)];
      if (this.isRam) return off < 0x400 ? this.ram[this.bank * 1024 + off] : 0;
      return this.rom[this.bank * 2048 + off];
    }
    write(a, v) {
      const off = a & 0xfff;
      if (this.isRam && off >= 0x400 && off < 0x800) this.ram[this.bank * 1024 + (off - 0x400)] = v;
    }
    tiaWrite(a, v) {
      if (this.ram && a === 0x3e) { this.bank = v & 31; this.isRam = true; }
      else if (this.ram ? a === 0x3f : a < 0x40) { this.bank = v % this.nBanks; this.isRam = false; }
    }
  }

  // ------------------------------------------------------------------
  // 3E+ (Thomas Jentzsch): four 1K segments. Writing to $3F selects a 1K ROM
  // bank, to $3E a 512-byte RAM bank (read in the segment's first half, write
  // in its second); bits 7-6 of the value pick the segment. At power-on every
  // segment shows the first 1K of the ROM (which holds the reset vectors).
  // ------------------------------------------------------------------
  class TJ3ECart extends Cart {
    constructor(rom, type) {
      super(rom, type);
      this.tiaHook = true; this.nBanks = Math.max(1, rom.length >> 10);
      this.ram = new Uint8Array(32 * K);
      this.seg = new Int32Array([0, 0, 0, 0]); // >= 0: ROM bank; < 0: -(RAM bank) - 1
    }
    read(a) {
      const off = a & 0xfff, s = this.seg[off >> 10];
      if (s >= 0) return this.rom[s * 1024 + (off & 0x3ff)];
      return (off & 0x200) ? 0 : this.ram[(-s - 1) * 512 + (off & 0x1ff)];
    }
    write(a, v) {
      const off = a & 0xfff, s = this.seg[off >> 10];
      if (s < 0 && (off & 0x200)) this.ram[(-s - 1) * 512 + (off & 0x1ff)] = v;
    }
    tiaWrite(a, v) {
      if (a === 0x3f) this.seg[v >> 6] = (v & 0x3f) % this.nBanks;
      else if (a === 0x3e) this.seg[v >> 6] = -(v & 0x3f) - 1;
    }
  }

  // ------------------------------------------------------------------
  // CommaVid (CV): 2K ROM at $1800, 1K RAM (read $1000-$13FF, write $1400-$17FF).
  // ------------------------------------------------------------------
  class CVCart extends Cart {
    constructor(rom, type) {
      let ram = new Uint8Array(1024);
      if (rom.length === 4096) { ram.set(rom.subarray(0, 1024)); rom = rom.subarray(2048); } // dumps with a RAM image in front
      else if (rom.length < 2048) { const r = new Uint8Array(2048); for (let i = 0; i < 2048; i++) r[i] = rom[i % rom.length]; rom = r; }
      super(rom, type);
      this.ram = ram;
    }
    read(a) { const off = a & 0xfff; if (off >= 0x800) return this.rom[off & 0x7ff]; return off < 0x400 ? this.ram[off] : this.ram[off & 0x3ff]; }
    write(a, v) { const off = a & 0xfff; if (off >= 0x400 && off < 0x800) this.ram[off & 0x3ff] = v; }
  }

  // ------------------------------------------------------------------
  // Schemes switched by touching addresses outside the cartridge's own
  // 4K: Econobanking (0840), UA, X07, SuperBanking (SB) and MDM.
  // ------------------------------------------------------------------
  class BusCart extends Cart {
    constructor(rom, type) {
      super(rom, type);
      this.snoop = true; this.nBanks = Math.max(1, rom.length >> 12);
      this.bank = type === "SB" ? this.nBanks - 1 : type === "UASW" ? 1 : 0;
      this.locked = false;
    }
    read(a) { return this.rom[(this.bank << 12) | (a & 0xfff)]; }
    access(a) {
      switch (this.type) {
        case "0840": if ((a & 0x1840) === 0x0800) this.bank = 0; else if ((a & 0x1840) === 0x0840) this.bank = 1; break;
        case "UA": if ((a & 0x1260) === 0x0220) this.bank = 0; else if ((a & 0x1260) === 0x0240) this.bank = 1; break;
        case "UASW": if ((a & 0x1260) === 0x0220) this.bank = 1; else if ((a & 0x1260) === 0x0240) this.bank = 0; break;
        case "SB": if ((a & 0x1800) === 0x0800) this.bank = (a & 0xff) % this.nBanks; break;
        case "MDM":
          if (!this.locked && (a & 0x1c00) === 0x0800) { this.bank = (a & 0xff) % this.nBanks; if (a & 0x80) this.locked = true; }
          break;
        case "X07":
          if ((a & 0x180f) === 0x080d) this.bank = (a >> 4) & 15;
          else if ((a & 0x1880) === 0 && (this.bank & 14) === 14) this.bank = 14 | ((a >> 6) & 1);
          break;
      }
    }
  }

  // ------------------------------------------------------------------
  // Pitfall II's DPC chip: 8 data fetchers, a random number generator and
  // three music channels, plus F8-style bank switching.
  // ------------------------------------------------------------------
  const CLOCK = 3579545;
  class DPCCart extends Cart {
    constructor(rom, type) {
      super(rom, type);
      this.bank = 1;
      this.dd = rom.subarray(8192, 10240);
      this.tops = new Uint8Array(8); this.bottoms = new Uint8Array(8); this.counters = new Uint16Array(8);
      this.flags = new Uint8Array(8); this.music = [false, false, false]; this.rand = 1; this.musicCycles = 0;
    }
    read(a, m) {
      const off = a & 0xfff;
      if (off < 0x40) return this.dpcRead(off, m);
      if (off === 0xff8 || off === 0xff9) this.bank = off - 0xff8;
      return this.rom[this.bank * 4096 + off];
    }
    write(a, v) {
      const off = a & 0xfff;
      if (off >= 0x40 && off < 0x80) this.dpcWrite(off, v);
      else if (off === 0xff8 || off === 0xff9) this.bank = off - 0xff8;
    }
    dpcMusic(m) {
      const clocks = Math.floor((m.cycles - this.musicCycles) * 20000 / (CLOCK / 3));
      if (clocks <= 0) return;
      this.musicCycles += clocks * (CLOCK / 3) / 20000;
      for (let x = 5; x < 8; x++) if (this.music[x - 5]) {
        const top = this.tops[x] + 1;
        let low = this.counters[x] & 0xff;
        if (this.tops[x] !== 0) { low -= clocks % top; if (low < 0) low += top; } else low = 0;
        if (low <= this.bottoms[x]) this.flags[x] = 0; else if (low <= this.tops[x]) this.flags[x] = 0xff;
        this.counters[x] = (this.counters[x] & 0x700) | low;
      }
    }
    dpcRead(off, m) {
      const r = this.rand;
      this.rand = ((r << 1) | (~((r >> 7) ^ (r >> 5) ^ (r >> 4) ^ (r >> 3)) & 1)) & 0xff;
      const i = off & 7, fn = (off >> 3) & 7;
      if (fn === 0 && i >= 4) {
        this.dpcMusic(m);
        const k = (this.music[0] && this.flags[5] ? 1 : 0) | (this.music[1] && this.flags[6] ? 2 : 0) | (this.music[2] && this.flags[7] ? 4 : 0);
        return [0x00, 0x04, 0x05, 0x09, 0x06, 0x0a, 0x0b, 0x0f][k];
      }
      if ((this.counters[i] & 0xff) === this.tops[i]) this.flags[i] = 0xff;
      else if ((this.counters[i] & 0xff) === this.bottoms[i]) this.flags[i] = 0;
      let res = 0;
      switch (fn) {
        case 0: res = r; break;
        case 1: res = this.dd[2047 - this.counters[i]]; break;
        case 2: res = this.dd[2047 - this.counters[i]] & this.flags[i]; break;
        case 7: res = this.flags[i]; break;
      }
      if (fn !== 0 && (i < 5 || !this.music[i - 5])) this.counters[i] = (this.counters[i] - 1) & 0x7ff;
      return res;
    }
    dpcWrite(off, v) {
      const i = off & 7, fn = (off >> 3) & 7;
      switch (fn) {
        case 0: this.tops[i] = v; this.flags[i] = 0; break;
        case 1: this.bottoms[i] = v; break;
        case 2: this.counters[i] = (this.counters[i] & 0x700) | v; break;
        case 3: this.counters[i] = (this.counters[i] & 0xff) | ((v & 7) << 8); if (i >= 5) this.music[i - 5] = !!(v & 0x10); break;
        case 6: this.rand = 1; break;
      }
    }
  }

  // ------------------------------------------------------------------
  // Starpath Supercharger (AR): 6K of RAM in three 2K banks plus a 2K BIOS
  // ROM, normally loaded from cassette. The game file holds the tape's
  // loads (8K of data + a 256-byte header each). RetroPixels doesn't use the
  // BIOS: when the game reaches the BIOS's load entry ($F800), the load is
  // copied straight into RAM and the game started, as the BIOS would.
  //
  // Writing to the Supercharger: touching $F000+n latches n; the fifth
  // address change after that writes n to the RAM address on the bus (if
  // writing is on), or into the control register if that address is $FFF8.
  // ------------------------------------------------------------------
  // Control register bits 4-2: which bank is at $F000 / $F800 (-1 = ROM).
  const AR_MAP = [[2, -1], [0, -1], [2, 0], [0, 2], [2, -1], [1, -1], [2, 1], [1, 2]];
  class ARCart extends Cart {
    constructor(rom, type) {
      super(rom, type);
      this.snoop = true;
      this.loads = [];
      for (let o = 0; o + 8448 <= rom.length; o += 8448) this.loads.push(rom.subarray(o, o + 8448));
      this.ram = new Uint8Array(6144);
      for (let i = 0; i < this.ram.length; i++) this.ram[i] = (i * 29 + 3) & 0xff;
      // RetroPixels' stand-in for the BIOS: NOPs, with the reset vectors pointing at the load entry.
      this.bios = new Uint8Array(2048).fill(0xea);
      this.bios[0x7fc] = 0x00; this.bios[0x7fd] = 0xf8; this.bios[0x7fe] = 0x00; this.bios[0x7ff] = 0xf8;
      this.latch = -1; this.count = 0; this.lastA = -1;
      this.inject = []; this.booted = false; this.loadNo = -1;
      this.setConfig(0);
    }
    setConfig(v) {
      this.config = v & 0xff;
      const m = AR_MAP[(v >> 2) & 7];
      this.lo = m[0]; this.hi = m[1]; this.writeOn = !!(v & 2);
    }
    access(a) {
      if (this.latch >= 0 && a !== this.lastA) {
        if (++this.count === 5) {
          if (a & 0x1000) {
            if ((a & 0xfff) === 0xff8) this.setConfig(this.latch);
            else if (this.writeOn) {
              const b = (a & 0x800) ? this.hi : this.lo;
              if (b >= 0) this.ram[b * 2048 + (a & 0x7ff)] = this.latch;
            }
          }
          this.latch = -1;
        }
      }
      this.lastA = a;
      if ((a & 0x1f00) === 0x1000) { this.latch = a & 0xff; this.count = 0; }
    }
    read(a, m) {
      if (this.inject.length) return this.inject.shift();
      const off = a & 0xfff, b = (off & 0x800) ? this.hi : this.lo;
      if (b < 0) {
        if (off === 0x800 && m && m.isOp) return this.loadGame(m);
        return this.bios[off & 0x7ff];
      }
      return this.ram[b * 2048 + (off & 0x7ff)];
    }
    // The BIOS's load entry: copy the wanted load into RAM and jump to its start.
    loadGame(m) {
      const want = this.booted ? m.riot.ram[0x7a] : -1; // the game puts the load number it wants at $FA
      let L = this.loads[0];
      if (want >= 0) for (const l of this.loads) if (l[8192 + 5] === want) { L = l; break; }
      this.booted = true;
      const h = L.subarray(8192);
      let pages = h[3] || 32; if (pages > 32) pages = 32;
      for (let i = 0; i < pages; i++) {
        const dest = h[0x10 + i], bank = dest & 3, page = (dest >> 2) & 7;
        if (bank < 3) this.ram.set(L.subarray(i * 256, i * 256 + 256), bank * 2048 + page * 256);
      }
      this.loadNo = h[5];
      this.setConfig(h[2]);
      this.latch = -1;
      // The BIOS leaves the RIOT timer and the zero-page load number as they were; it finishes with a JMP.
      this.inject = [h[0], h[1]];
      return 0x4c;
    }
  }

  // ------------------------------------------------------------------
  // Creating a cartridge
  // ------------------------------------------------------------------
  const REGISTRY = {};
  function create(rom, opts) {
    opts = opts || {};
    const type = detectType(rom, opts.name);
    if (UNSUPPORTED[type]) throw new Error(UNSUPPORTED[type]);
    if (REGISTRY[type]) return new REGISTRY[type](rom, type, opts.sram);
    const base = type.replace(/SC$/, "");
    if (FX[base]) return new HotspotCart(rom, type, opts.sram);
    switch (type) {
      case "2K": case "4K": {
        const r = new Uint8Array(4096); for (let i = 0; i < 4096; i++) r[i] = rom[i % Math.min(rom.length, 4096)];
        return new Cart(r, type);
      }
      case "CV": return new CVCart(rom, type);
      case "F0": return new F0Cart(rom, type);
      case "E0": return new E0Cart(rom, type);
      case "E7": return new E7Cart(rom, type);
      case "FE": return new FECart(rom, type);
      case "3F": case "3E": return new TigerCart(rom, type);
      case "3E+": return new TJ3ECart(rom, type);
      case "0840": case "UA": case "UASW": case "X07": case "SB": case "MDM": return new BusCart(rom, type);
      case "DPC": return new DPCCart(rom, type);
      case "AR": return new ARCart(rom, type);
    }
    throw new Error("This Atari 2600 game uses the " + type + " cartridge, which RetroPixels doesn't support yet.");
  }

  global.RPAtariCarts = { create, detectType, typeFromName, register(type, C) { REGISTRY[type] = C; }, Cart, countSig, hasText };
})(typeof window !== "undefined" ? window : globalThis);
