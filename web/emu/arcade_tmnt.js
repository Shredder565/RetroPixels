// RetroPixels — Konami "TMNT" arcade board (Teenage Mutant Ninja Turtles, 1989). Original
// code, written from the documented behaviour of the board:
//   - a Motorola 68000 at 8 MHz (RPM68K) with 384 KB of program ROM, 16 KB of work RAM
//     and 2 KB of palette RAM (1024 colours of 15 bits);
//   - the K052109 tilemap chip (k052109.js: a fixed layer and two scrolling layers) and
//     the K051960 / K051937 sprite chips (k051960.js); this board's ROMs keep every sprite as
//     one whole picture, laid out as the g7 PROM says (see spriteCells), and all graphics as packed 4-bit pixels (decodePacked);
//   - a Z80 sound processor at 3.58 MHz (RPZ80) with a YM2151 FM chip (ym2151.js), a
//     K007232 PCM chip (k007232.js) for drums and effects, a uPD7759 speech chip
//     (upd7759.js) and the title song, a recording kept in its own 512 KB ROM and played
//     through a DAC at 20 kHz;
//   - inputs for up to four players, three banks of DIP switches.
// Picture: 304 x 224 at about 59.2 frames a second (6 MHz pixel clock, 384 x 264).
//
// The 68000 sees (byte addresses):
//   000000-05ffff program ROM          060000-063fff work RAM
//   080000-080fff palette (odd bytes)  0a0000 coins / service (read), control (write)
//   0a0002 / 0a0004 / 0a0006 / 0a0014  players 1 / 2 / 3 / 4      0a0008 sound command (write)
//   0a0010 / 0a0012 / 0a0018  DIP switches 1 / 2 / 3 (0a0010 write: watchdog)
//   0c0000 layer priority (write)      100000-107fff K052109 (a word = attribute byte : code byte)
//   140000-140007 K051937 registers    140400-1407ff K051960 sprite list
// Control (0a0000 write): bits 0-1 coin counters, bit 3 going low interrupts the Z80,
// bit 5 enables the vertical-blank interrupt (level 5), bit 7 lets the 68000 read the tile ROMs.
// The Z80 sees: 0000-7fff ROM, 8000-87ff RAM, 9000 sound control (bit 1 = uPD7759 run, bit 2 =
// title song play), a000 command from the 68000, b000-b00d K007232, c000-c001 YM2151,
// d000 uPD7759 sample number, e000 uPD7759 start, f000 uPD7759 busy (bit 0).
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 8000000, Z80_HZ = 3579545, UPD_HZ = 640000, TITLE_HZ = 20000;
  const LINES = 264, LINE_CYC = 512;            // 384 pixels at 6 MHz = 512 cycles at 8 MHz
  const FPS = CPU_HZ / (LINE_CYC * LINES);       // about 59.19
  const Z80_LINE = Z80_HZ / (FPS * LINES);
  const W = 304, H = 224, MAP_X0 = 104, MAP_Y0 = 16, VBLANK_LINE = 240;
  const DC_KEEP = 1 - 2 * Math.PI * 20 / (Z80_HZ / 64);   // 20 Hz coupling filter at the mixing rate
  const TILE_COLORBASE = [0, 32, 40], SPRITE_COLORBASE = 16;

  // ---- the board's ROM wiring ----

  // Program ROMs: pairs of chips, even (high) and odd (low) bytes.
  function interleave(even, odd) {
    const out = new Uint8Array(even.length * 2);
    for (let i = 0; i < even.length; i++) { out[i * 2] = even[i]; out[i * 2 + 1] = odd[i]; }
    return out;
  }
  // Two 16-bit-wide ROMs side by side make one 32-bit word: bytes 0-1 from the first, 2-3 from the second.
  function wordPair(a, b) {
    const out = new Uint8Array(a.length * 2);
    for (let i = 0; i < a.length; i += 2) {
      out[i * 2] = a[i]; out[i * 2 + 1] = a[i + 1]; out[i * 2 + 2] = b[i]; out[i * 2 + 3] = b[i + 1];
    }
    return out;
  }

  // Sprite ROM layout. The K051960 numbers a sprite's 16x16 cells in Konami's zig-zag order inside
  // blocks of 64 codes (an 8 x 8 sheet of cells: code bits 0, 2, 4 step across, bits 1, 3, 5 step
  // down). This board's ROMs don't hold cells, though: they hold whole pictures, stored row after
  // row (2 words of 8 pixels per cell across), the pictures of a block one after the other. The
  // PROM at g7 (256 x 4 bits, one entry per block of 64 codes) gives the picture size the block is
  // stored in (0 1x1, 1 1x2, 2 2x1, 3 2x2, 4 2x4, 5 4x2, 6 4x4, 7 8x8 cells across x down: the
  // K051960's size numbers with the wide and tall ones swapped round),
  // and the board's address wiring follows it: in a block of W x H pictures, the code bits that step
  // through a picture's own cells pick the cell inside the picture, and the other code bits (pushed
  // together, lowest first) pick the picture. So cell c of a block sits at word
  //   (c's block) * 2048 + picture * W * H * 32 + (cell row * 16) * 2W + cell column * 2.
  // RetroPixels sorts the ROMs into plain 16x16 cells this way once (spriteCells), so a sprite of any
  // size, even one that doesn't match its block's pictures, shows what the board shows.
  const SPR_W = [1, 2, 1, 2, 4, 2, 4, 8], SPR_H = [1, 1, 2, 2, 2, 4, 4, 8];
  // where cell (code & 63) of a block stored as `size` pictures starts, and its column/row in the picture
  function cellPlace(size, c) {
    const nx = Math.log2(SPR_W[size]), ny = Math.log2(SPR_H[size]);
    let n = 0, k = 0, cx = 0, cy = 0;
    for (let b = 0; b < 6; b++) {
      const bit = (c >> b) & 1, i = b >> 1;
      if (b & 1) { if (i < ny) { cy |= bit << i; continue; } }
      else if (i < nx) { cx |= bit << i; continue; }
      n |= bit << k++;
    }
    return { start: n * SPR_W[size] * SPR_H[size] * 32, cx, cy };
  }
  const PROM_SIZE = [0, 2, 1, 3, 5, 4, 6, 7];
  function spriteCells(packed, prom) {
    const cells = packed.length >> 8, out = new Uint8Array(cells * 256);
    for (let c = 0; c < cells; c++) {
      const size = PROM_SIZE[prom[(c >> 6) & 255] & 7], p = cellPlace(size, c & 63), row = 2 * SPR_W[size];
      const base = (c >> 6) * 2048 + p.start;
      for (let y = 0; y < 16; y++) {
        const w = base + (p.cy * 16 + y) * row + p.cx * 2;
        out.set(packed.subarray(w * 8, w * 8 + 16), c * 256 + y * 16);
      }
    }
    return out;
  }
  // Without the PROM (an incomplete set), each sprite is taken to be stored at its own size.
  function spriteStart(code, size) { return (code & ~63) * 32 + cellPlace(size, code & 63).start; }

  // This board stores its graphics packed: each 32-bit ROM word is a row of 8 pixels of 4 bits,
  // the leftmost in the high half of the last byte, the next in its low half, and so on back to
  // the rightmost in the low half of the first byte (the font only reads right this way).
  function decodePacked(rom) {
    const n = rom.length >> 2, pens = new Uint8Array(n * 8);
    for (let w = 0; w < n; w++) for (let k = 0; k < 4; k++) {
      const v = rom[w * 4 + 3 - k];
      pens[w * 8 + k * 2] = v >> 4; pens[w * 8 + k * 2 + 1] = v & 15;
    }
    return pens;
  }

  // One byte of a packed row as the video chips show it to the CPU: bit n of each of the row's
  // 8 pixels, the leftmost pixel in bit 7.
  function planarByte(pens, row, n) {
    const at = (row * 8) % pens.length;
    let v = 0;
    for (let x = 0; x < 8; x++) v |= ((pens[at + x] >> n) & 1) << (7 - x);
    return v;
  }

  // Title song: 16-bit words (low byte first), a 3-bit exponent over a 10-bit value.
  function decodeTitle(rom) {
    const n = rom.length >> 1, out = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      const w = rom[i * 2] | (rom[i * 2 + 1] << 8);
      const e = w >> 13, m = ((w >> 3) & 0x3ff) - 0x200;
      out[i] = (m * (1 << e)) / (0x200 * 128);
    }
    return out;
  }

  // The layer-mixing PROM (see draw): the set's own chip when it's there, otherwise the same
  // choices worked out by rule (fixed layer in front, then layer A or the sprites as the latch
  // says, layer B at the back; shadows darken everything but the fixed layer and a layer A in front).
  function priorityTable(prom) {
    const t = new Uint8Array(256);
    const ok = prom.length >= 256 && prom.some((v) => (v & 15) !== 0);
    for (let i = 0; i < 256; i++) {
      if (ok) { t[i] = prom[i] & 7; continue; }
      const aOn = i & 1, bOn = i & 2, sOn = i & 4, fOn = i & 8, sh = i & 16, aFront = i & 64;
      let sel;
      if (fOn) sel = 3;
      else if (aOn && (aFront || !sOn)) sel = 0;
      else if (sOn) sel = 2;
      else if (bOn) sel = 1;
      else sel = 0;
      const dark = sh && sel !== 3 && !(sel === 0 && aOn && aFront) && (aOn || bOn || sOn);
      t[i] = sel | (dark ? 0 : 4);
    }
    return t;
  }

  class TMNTBoard {
    constructor(game, files, opts) {
      opts = opts || {};
      this.game = game;
      this.sampleRate = opts.sampleRate || 48000;
      this.dips = opts.dips || {};
      for (const d of TMNTBoard.dips) if (this.dips[d.id] === undefined) this.dips[d.id] = d.default;
      const get = (role) => { const f = game.files.find((x) => x.role === role); const d = f && RPArcade.findFile(f, files); return d || new Uint8Array(f ? f.size : 0); };
      this.rom = new Uint8Array(0x60000);
      this.rom.set(interleave(get("cpu_even"), get("cpu_odd")), 0);
      this.rom.set(interleave(get("cpu2_even"), get("cpu2_odd")).subarray(0, 0x20000), 0x40000);
      this.zrom = new Uint8Array(0x8000); this.zrom.set(get("audio").subarray(0, 0x8000));
      const tiles = wordPair(get("tiles_a"), get("tiles_b"));
      const spr = new Uint8Array(0x200000);
      spr.set(wordPair(get("sprites_a"), get("sprites_b")), 0);
      spr.set(wordPair(get("sprites_c"), get("sprites_d")), 0x100000);
      this.title = decodeTitle(get("title"));
      this.prioProm = priorityTable(get("prom_mixer"));

      this.ram = new Uint8Array(0x4000);
      this.palRam = new Uint8Array(0x800);
      this.palette = new Uint32Array(1024);
      this.zram = new Uint8Array(0x800);
      this.screen = new Uint32Array(W * H);
      this.width = W; this.height = H;
      this.bufB = new Uint16Array(W * H); this.bufA = new Uint16Array(W * H); this.bufF = new Uint16Array(W * H);
      this.bufS = new Uint16Array(W * H); this.bufSh = new Uint8Array(W * H);
      this.shade = new Uint32Array(1024);
      this.inputs = [{}, {}, {}, {}];

      this.k052109 = new global.RPK052109({
        rom: tiles, decode: decodePacked,
        tileCallback: (t) => {
          // board wiring: attribute bits 0-1 -> code bits 8-9, bit 4 -> code bit 10, bank bits -> 11-14;
          // attribute bits 5-7 pick one of 8 colours from the layer's colour base
          const c = t.color;
          t.code = t.code | ((c & 0x03) << 8) | ((c & 0x10) << 6) | ((c & 0x0c) << 9) | (t.bank << 13);
          t.color = TILE_COLORBASE[t.layer] + ((c & 0xe0) >> 5);
          t.flipX = false;
        },
        // ROM reading (the self-test's mask ROM check): the chip's address bits 0-1 pick the byte of
        // a row and bits 2-12 the row; register 0x1e00 adds row bits 11-15 and the top two bits of
        // bank register 0 (code bits 13-14 on this board) row bits 16-17. The CPU sees the row in
        // the chips' own planar form (byte n = bit n of the 8 pixels), not as the packed ROM bytes.
        romRead: (off, k) => {
          const row = ((k.bankReg(0) >> 2) << 16) | (k.ram[0x1e00] << 11) | ((off >> 2) & 0x7ff);
          return planarByte(k.pens, row, off & 3);
        },
      });
      // the scrolling layers sit 6 pixels further left in the chip's map than the fixed layer
      // (measured against the real game's picture: walls and floors line up with the sprites this way)
      this.k052109.dx[1] = this.k052109.dx[2] = -6;
      const g7 = get("prom_g7"), sprPacked = decodePacked(spr);
      const haveG7 = g7.length >= 256 && g7.some((v) => v !== 0);
      this.k051960 = new global.RPK051960({
        rom: spr, decode: () => sprPacked,
        pens: haveG7 ? spriteCells(sprPacked, g7) : undefined, spriteStart: haveG7 ? undefined : spriteStart,
        // ROM reading: the chip's address bit 22 (reg 3 bit 6) is wired to the second pair of ROMs;
        // the CPU sees a word in the chips' planar form
        readbackAddr: (w) => (w & 0x3ffff) | (((w >> 22) & 1) << 18),
        romByte: (w, n) => planarByte(sprPacked, w, n),
        spriteCallback: (t) => {
          // attribute bit 4 -> code bit 13; bits 0-3 = colour; bit 7 = pen 15 is a shadow
          // (it darkens what's under it instead of being drawn)
          t.code |= (t.color & 0x10) << 9;
          t.shadow = !!(t.color & 0x80);
          t.color = SPRITE_COLORBASE + (t.color & 0x0f);
        },
      });
      this.k007232 = new global.RPK007232({ clock: Z80_HZ, rom: get("pcm"),
        // the port drives two 4-bit volume latches: the high nibble sets channel A, the low nibble channel B
        // (a linear scale, 15 = full)
        onPort: (v) => { this.k007232.setVolume(0, (v >> 4) * 0x11, (v >> 4) * 0x11); this.k007232.setVolume(1, (v & 15) * 0x11, (v & 15) * 0x11); } });
      this.upd = new global.RPUPD7759({ clock: UPD_HZ, rom: get("speech") });
      this.ym = new global.RPYM2151({ clock: Z80_HZ });
      this.out = new RPArcade.Resampler(Z80_HZ / 64, this.sampleRate);

      const self = this;
      this.cpu = new global.RPM68K({
        read8: (a) => self.read8(a), read16: (a) => self.read16(a),
        write8: (a, v) => self.write8(a, v), write16: (a, v) => self.write16(a, v),
        ack: () => { self.cpu.setIRQ(0); return -1; },
      });
      this.z80 = new global.RPZ80({
        read: (a) => self.zread(a), write: (a, v) => self.zwrite(a, v),
        in: () => 0xff, out: () => {}, vector: () => 0xff,
        ack: () => { self.z80.irq = false; },
      });
      this.reset();
    }

    reset() {
      this.ram.fill(0); this.palRam.fill(0); this.palette.fill(0xff000000); this.shade.fill(0xff000000); this.zram.fill(0);
      this.k052109.reset(); this.k051960.reset(); this.k007232.reset(); this.upd.reset(); this.ym.reset();
      this.ctrl = 0; this.priority = 0; this.soundLatch = 0; this.sres = 0;
      this.titlePos = -1; this.titleAcc = 0; this.updAcc = 0; this.mixClk = 0; this.zcyc = 0; this.updDC = 0; this.updPrev = 0; this.updV = 0; this.kL = 0;
      this.line = 0; this.frame = 0;
      this.cpu.cycles = 0; this.cpu.setIRQ(0); this.cpu.reset(); this.cpu.cycles = 0;
      this.z80.reset(); this.z80.irq = false;
      this.upd.resetLine(0);
      this.out.clear();
    }

    setInput(p1, p2, p3, p4) { this.inputs = [p1 || {}, p2 || {}, p3 || {}, p4 || {}]; }

    // ---- inputs (all active low) ----
    playerByte(p) {
      const i = this.inputs[p] || {}, p1 = this.inputs[0] || {};
      const start = i["Start (1P)"] || (p === 1 && p1["Start (2P)"]);
      // The 4-player cabinet has no start buttons: a player joins by pressing attack, so Start
      // presses attack as well (and the bit 7 line, for the 2-player versions).
      return ~((i.Left ? 1 : 0) | (i.Right ? 2 : 0) | (i.Up ? 4 : 0) | (i.Down ? 8 : 0) |
        (i["Button 1"] || start ? 0x10 : 0) | (i["Button 2"] ? 0x20 : 0) | (start ? 0x80 : 0)) & 0xff;
    }
    coinByte() {
      const p = this.inputs, p1 = p[0] || {};
      let v = 0;
      if (p1.Coin) v |= 1;
      if ((p[1] && p[1].Coin) || p1["Coin 2"]) v |= 2;
      if (p[2] && p[2].Coin) v |= 4;
      if (p[3] && p[3].Coin) v |= 8;
      // a service button for each player's coin slot (credits without a coin)
      for (let n = 0; n < 4; n++) if (p[n] && p[n].Service) v |= 0x10 << n;
      return ~v & 0xff;
    }
    dsw(n) {
      const d = this.dips;
      if (n === 1) return d.coinage & 0xff;
      if (n === 2) return ((d.lives & 3) | 0x1c | ((d.difficulty & 3) << 5) | (d.demo ? 0 : 0x80)) & 0xff;
      return (0xfa | (d.flip ? 0 : 1) | (d.test ? 0 : 4)) & 0xff;
    }

    // ---- 68000 memory ----
    ioRead(a) {
      switch (a & 0x1e) {
        case 0x00: return this.coinByte();
        case 0x02: return this.playerByte(0);
        case 0x04: return this.playerByte(1);
        case 0x06: return this.playerByte(2);
        case 0x10: return this.dsw(1);
        case 0x12: return this.dsw(2);
        case 0x14: return this.playerByte(3);
        case 0x18: return this.dsw(3);
      }
      return 0xff;
    }
    // K052109 through this board's wiring: the 68000's A12 isn't connected
    vOff(a) { const w = (a >> 1) & 0x3fff; return ((w & 0x3000) >> 1) | (w & 0x07ff); }
    read16(a) {
      if (a < 0x60000) return (this.rom[a] << 8) | this.rom[a + 1];
      if (a >= 0x60000 && a < 0x64000) { a -= 0x60000; return (this.ram[a] << 8) | this.ram[a + 1]; }
      if (a >= 0x80000 && a < 0x81000) return 0xff00 | this.palRam[(a - 0x80000) >> 1];
      if (a >= 0xa0000 && a < 0xa0020) return 0xff00 | this.ioRead(a);
      if (a >= 0x100000 && a < 0x108000) { const o = this.vOff(a); return (this.k052109.read(o) << 8) | this.k052109.read(o + 0x2000); }
      if (a >= 0x140000 && a < 0x140008) return (this.k051960.readReg(a & 7) << 8) | this.k051960.readReg((a & 7) + 1);
      if (a >= 0x140400 && a < 0x140800) return (this.k051960.read(a) << 8) | this.k051960.read(a + 1);
      return 0xffff;
    }
    read8(a) {
      if (a < 0x60000) return this.rom[a];
      if (a >= 0x60000 && a < 0x64000) return this.ram[a - 0x60000];
      if (a >= 0x80000 && a < 0x81000) return (a & 1) ? this.palRam[(a - 0x80000) >> 1] : 0xff;
      if (a >= 0xa0000 && a < 0xa0020) return (a & 1) ? this.ioRead(a) : 0xff;
      if (a >= 0x100000 && a < 0x108000) return this.k052109.read(this.vOff(a) + ((a & 1) ? 0x2000 : 0));
      if (a >= 0x140000 && a < 0x140008) return this.k051960.readReg(a & 7);
      if (a >= 0x140400 && a < 0x140800) return this.k051960.read(a);
      return 0xff;
    }
    write16(a, v) {
      if (a >= 0x60000 && a < 0x64000) { a -= 0x60000; this.ram[a] = v >> 8; this.ram[a + 1] = v; return; }
      if (a >= 0x100000 && a < 0x108000) { const o = this.vOff(a); this.k052109.write(o, v >> 8); this.k052109.write(o + 0x2000, v); return; }
      if (a >= 0x140400 && a < 0x140800) { this.k051960.write(a, v >> 8); this.k051960.write(a + 1, v); return; }
      if (a >= 0x140000 && a < 0x140008) { this.k051960.writeReg(a & 7, v >> 8); this.k051960.writeReg((a & 7) + 1, v); return; }
      this.write8(a | 1, v & 0xff); // byte-wide devices sit on the low byte
    }
    write8(a, v) {
      v &= 0xff;
      if (a >= 0x60000 && a < 0x64000) { this.ram[a - 0x60000] = v; return; }
      if (a >= 0x80000 && a < 0x81000) { if (a & 1) this.palWrite((a - 0x80000) >> 1, v); return; }
      if (a >= 0x100000 && a < 0x108000) { this.k052109.write(this.vOff(a) + ((a & 1) ? 0x2000 : 0), v); return; }
      if (a >= 0x140000 && a < 0x140008) { this.k051960.writeReg(a & 7, v); return; }
      if (a >= 0x140400 && a < 0x140800) { this.k051960.write(a, v); return; }
      if (!(a & 1)) return;
      switch (a) {
        case 0xa0001: this.ctrlWrite(v); return;
        case 0xa0009: this.soundLatch = v; return;
        case 0xa0011: return; // watchdog
        case 0xc0001: this.priority = v; return;
      }
    }
    ctrlWrite(v) {
      const old = this.ctrl;
      this.ctrl = v;
      if ((old & 8) && !(v & 8)) { this.syncZ80(); this.z80.irq = true; }
      if (!(v & 0x20)) this.cpu.setIRQ(0);
      this.k052109.rmrd = !!(v & 0x80);
    }
    palWrite(i, v) {
      this.palRam[i] = v;
      const n = i >> 1, c = (this.palRam[n * 2] << 8) | this.palRam[n * 2 + 1];
      const r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
      const R = (r << 3) | (r >> 2), G = (g << 3) | (g >> 2), B = (b << 3) | (b >> 2);
      this.palette[n] = RPArcade.rgb(R, G, B);
      this.shade[n] = RPArcade.rgb(R * 0.75, G * 0.75, B * 0.75); // a shadow keeps 3/4 of the brightness
    }

    // ---- Z80 memory ----
    zread(a) {
      if (a < 0x8000) return this.zrom[a];
      if (a < 0x8800) return this.zram[a - 0x8000];
      if (a >= 0x8800 && a < 0x9000) return 0xff;
      switch (a & 0xf000) {
        case 0x9000: return this.sres;
        case 0xa000: return this.soundLatch;
        case 0xb000: return (a & 0xf) < 14 ? this.k007232.read(a & 0xf) : 0xff;
        case 0xc000: return (a & 1) ? this.ym.read() : 0xff;
        case 0xf000: return this.upd.busy() ? 0 : 1;
      }
      return 0xff;
    }
    zwrite(a, v) {
      if (a >= 0x8000 && a < 0x8800) { this.zram[a - 0x8000] = v; return; }
      switch (a & 0xf000) {
        case 0x9000: {
          const old = this.sres; this.sres = v;
          this.upd.resetLine(v & 2);
          if ((v & 4) && !(old & 4)) { this.titlePos = 0; this.titleAcc = 0; }
          else if (!(v & 4)) this.titlePos = -1;
          return;
        }
        case 0xb000: if ((a & 0xf) < 14) this.k007232.write(a & 0xf, v); return;
        case 0xc000: this.ym.write(a & 1, v); return;
        case 0xd000: this.upd.port(v); return;
        case 0xe000: this.upd.start(v & 1); return;
      }
    }

    // ---- running ----
    // Run the Z80 (and the sound chips with it) up to where the 68000 is now.
    syncZ80() { this.runZ80To(Math.min(this.cpu.cycles, LINES * LINE_CYC) / LINE_CYC * Z80_LINE); }
    runZ80To(target) {
      while (this.zcyc < target) { const c = this.z80.step(); this.zcyc += c; this.soundClocks(c); }
    }
    soundClocks(c) {
      this.mixClk += c;
      const ym = this.ym, k = this.k007232, upd = this.upd;
      while (this.mixClk >= 64) {
        this.mixClk -= 64;
        ym.tick();
        k.clocks(64);
        if (k.accN) { this.kL = k.acc[0] / k.accN; k.acc[0] = k.acc[1] = 0; k.accN = 0; }
        this.updAcc += 64 * UPD_HZ / Z80_HZ;
        const n = Math.floor(this.updAcc); this.updAcc -= n;
        if (n) { upd.clocks(n); if (upd.accN) { this.updV = upd.acc / upd.accN; upd.acc = 0; upd.accN = 0; } }
        let title = 0;
        if (this.titlePos >= 0) {
          if (this.titlePos < this.title.length) title = this.title[this.titlePos];
          this.titleAcc += TITLE_HZ * 64 / Z80_HZ;
          while (this.titleAcc >= 1) { this.titleAcc -= 1; this.titlePos++; }
          if (this.titlePos >= this.title.length) this.titlePos = -1;
        }
        const fm = (ym.out[0] + ym.out[1]) / 2 / 16384;
        // the speech chip reaches the amplifier through a coupling capacitor: its slow offsets
        // (an ADPCM sample that wanders away from the centre) fade out instead of clicking at the end
        const u = this.updV || 0;
        this.updDC = u - this.updPrev + DC_KEEP * this.updDC; this.updPrev = u;
        // mixing levels measured against the real board's balance (music, effects, speech, title song)
        let v = fm * 0.765 + (this.kL || 0) * 0.29 + this.updDC * 0.315 + title * 0.395;
        if (v > 1) v = 1; else if (v < -1) v = -1;
        this.out.add(v);
      }
    }

    runFrame() {
      const cpu = this.cpu;
      for (let line = 0; line < LINES; line++) {
        this.line = line;
        if (line === VBLANK_LINE) {
          this.draw();
          if (this.ctrl & 0x20) cpu.setIRQ(5);
        }
        cpu.run((line + 1) * LINE_CYC);
        this.runZ80To((line + 1) * Z80_LINE);
      }
      cpu.cycles -= LINES * LINE_CYC;
      this.zcyc -= LINES * Z80_LINE;
      this.frame++;
    }

    takeAudio() { return this.out.take(); }

    // ---- picture ----
    // How the layers are mixed: the priority PROM at g19 (256 x 4 bits) is looked up for every
    // pixel. Its address is
    //   bit 0  layer A pixel is not see-through     bit 1  layer B (always on: B is the backdrop,
    //   bit 2  sprite pixel is not see-through              its pen 0 shows where nothing else is drawn)
    //   bit 3  fixed layer pixel is not see-through  bit 4  a sprite's shadow is on this pixel
    //   bit 6  priority latch bit 2 (layer A in front of the sprites); bits 5 and 7 make no difference
    // and its output bits 0-1 pick what is shown (0 layer A, 1 layer B, 2 sprites, 3 fixed layer),
    // bit 2 low darkens it (shadow). So the fixed layer is always in front and never shadowed,
    // layer B is always at the back, the latch puts layer A in front of or behind the sprites,
    // and a shadow doesn't reach layer A while it is in front. (Worked out from the PROM's
    // contents and checked pixel by pixel against the real game's picture.)
    draw() {
      const k = this.k052109, s = this.k051960, pal = this.palette, scr = this.screen;
      const opt = { x0: MAP_X0, y0: MAP_Y0 };
      this.bufA.fill(0); this.bufF.fill(0); this.bufS.fill(0); this.bufSh.fill(0);
      k.render(2, this.bufB, W, H, { x0: MAP_X0, y0: MAP_Y0, opaque: true });
      k.render(1, this.bufA, W, H, opt);
      k.render(0, this.bufF, W, H, opt);
      s.render(this.bufS, W, H, { x0: MAP_X0, y0: MAP_Y0, shadow: this.bufSh });
      const prom = this.prioProm, latch = (this.priority & 4) << 4;
      // (the K052109 flips its own layers; the sprites are turned round here when their chip's flip is on)
      const B = this.bufB, A = this.bufA, F = this.bufF, S = this.bufS, Sh = this.bufSh, shade = this.shade;
      const last = s.flipScreen ? W * H - 1 : -1;
      for (let i = 0; i < W * H; i++) {
        const j = last >= 0 ? last - i : i, sp = S[j];
        const a = A[i], f = F[i];
        const o = prom[((a & 15) ? 1 : 0) | 2 | ((sp & 15) ? 4 : 0) | ((f & 15) ? 8 : 0) | (Sh[j] << 4) | latch];
        const sel = o & 3, c = sel === 0 ? a : sel === 1 ? B[i] : sel === 2 ? sp : f;
        scr[i] = (o & 4) ? pal[c & 1023] : shade[c & 1023];
      }
    }

    saveState() {
      return { v: 1, cpu: this.cpu.saveState(), z80: this.z80.getState(), ram: this.ram.slice(), palRam: this.palRam.slice(), zram: this.zram.slice(),
        k052109: this.k052109.saveState(), k051960: this.k051960.saveState(), k007232: this.k007232.saveState(), upd: this.upd.saveState(), ym: this.ym.saveState(),
        m: [this.ctrl, this.priority, this.soundLatch, this.sres, this.titlePos, this.titleAcc, this.updAcc, this.mixClk, this.zcyc, this.frame, this.updDC, this.updPrev] };
    }
    loadState(s) {
      this.cpu.loadState(s.cpu); this.z80.setState(s.z80);
      this.ram.set(s.ram); this.palRam.set(s.palRam); this.zram.set(s.zram);
      this.k052109.loadState(s.k052109); this.k051960.loadState(s.k051960); this.k007232.loadState(s.k007232); this.upd.loadState(s.upd); this.ym.loadState(s.ym);
      [this.ctrl, this.priority, this.soundLatch, this.sres, this.titlePos, this.titleAcc, this.updAcc, this.mixClk, this.zcyc, this.frame] = s.m;
      this.updDC = s.m[10] || 0; this.updPrev = s.m[11] || 0;
      this.k052109.rmrd = !!(this.ctrl & 0x80);
      for (let i = 0; i < 0x800; i += 2) this.palWrite(i, this.palRam[i]);
      this.out.clear();
      this.draw();
    }
  }

  TMNTBoard.id = "tmnt";
  TMNTBoard.title = "Konami TMNT";
  TMNTBoard.width = W;
  TMNTBoard.height = H;
  TMNTBoard.rotate = 0;
  TMNTBoard.fps = FPS;
  TMNTBoard.players = 4;
  TMNTBoard.buttons = ["Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"];
  TMNTBoard.dips = [
    { id: "coinage", label: "Coins", default: 0xff, options: [{ label: "1 coin, 1 credit", value: 0xff }, { label: "1 coin, 2 credits", value: 0xee },
      { label: "2 coins, 1 credit", value: 0x22 }, { label: "Free play", value: 0x00 }] },
    { id: "lives", label: "Lives", default: 2, options: [{ label: "1", value: 3 }, { label: "2", value: 2 }, { label: "3", value: 1 }, { label: "5", value: 0 }] },
    { id: "difficulty", label: "Difficulty", default: 2, options: [{ label: "Easy", value: 3 }, { label: "Normal", value: 2 }, { label: "Hard", value: 1 }, { label: "Very hard", value: 0 }] },
    { id: "demo", label: "Sound in attract mode", default: 1, options: [{ label: "On", value: 1 }, { label: "Off", value: 0 }] },
    { id: "flip", label: "Flip screen", default: 0, options: [{ label: "Off", value: 0 }, { label: "On", value: 1 }] },
    { id: "test", label: "Service mode (test screen)", default: 0, options: [{ label: "Off", value: 0 }, { label: "On", value: 1 }] },
  ];

  // The chips every version shares; the program ROMs differ by region / version letter.
  const COMMON = [
    { name: "963e20.g13", size: 0x8000, role: "audio" },
    { name: "963a28.h27", size: 0x80000, role: "tiles_a" }, { name: "963a29.k27", size: 0x80000, role: "tiles_b" },
    { name: "963a17.h4", size: 0x80000, role: "sprites_a" }, { name: "963a15.k4", size: 0x80000, role: "sprites_b" },
    { name: "963a18.h6", size: 0x80000, role: "sprites_c" }, { name: "963a16.k6", size: 0x80000, role: "sprites_d" },
    { name: "963a30.g7", size: 0x100, role: "prom_g7" }, // g7: how each block of sprite pictures is stored { name: "963a31.g19", size: 0x100, role: "prom_mixer" }, // g19 mixes the layers (see draw)
    { name: "963a26.c13", size: 0x20000, role: "pcm" }, { name: "963a27.d18", size: 0x20000, role: "speech" },
    { name: "963a25.d5", size: 0x80000, role: "title" },
  ];
  const version = (id, title, v, extra) => Object.assign({
    id, title, year: 1989, maker: "Konami", loose: true,
    files: [
      { name: `963-${v}23.j17`, size: 0x20000, role: "cpu_even" }, { name: `963-${v}24.k17`, size: 0x20000, role: "cpu_odd" },
      { name: `963-${v}21.j15`, size: 0x10000, role: "cpu2_even" }, { name: `963-${v}22.k15`, size: 0x10000, role: "cpu2_odd" },
      ...COMMON,
    ],
  }, extra || {});
  TMNTBoard.games = [
    version("tmnt", "Teenage Mutant Ninja Turtles (World, 4 players)", "x"),
    version("tmntu", "Teenage Mutant Ninja Turtles (US, 4 players)", "r", { parent: "tmnt" }),
    version("tmntua", "Teenage Mutant Ninja Turtles (US, 4 players, older)", "n", { parent: "tmnt" }),
    version("tmht", "Teenage Mutant Hero Turtles (UK, 4 players)", "f", { parent: "tmnt" }),
    version("tmntj", "Teenage Mutant Ninja Turtles (Japan, 4 players)", "2", { parent: "tmnt" }),
    version("tmht2p", "Teenage Mutant Hero Turtles (UK, 2 players)", "u", { parent: "tmnt", players: 2 }),
    version("tmnt2pj", "Teenage Mutant Ninja Turtles (Japan, 2 players)", "1", { parent: "tmnt", players: 2 }),
  ];
  // helpers for RetroPixels' own tests (building fake sets)
  TMNTBoard.spriteStart = spriteStart;
  TMNTBoard.spriteCells = spriteCells;
  TMNTBoard.priorityTable = priorityTable;

  RPArcade.boards.push(TMNTBoard);
})(typeof window !== "undefined" ? window : globalThis);
