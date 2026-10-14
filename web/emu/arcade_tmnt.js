// RetroPixels — Konami "TMNT" arcade board (Teenage Mutant Ninja Turtles, 1989). Original
// code, written from the documented behaviour of the board:
//   - a Motorola 68000 at 8 MHz (RPM68K) with 384 KB of program ROM, 16 KB of work RAM
//     and 2 KB of palette RAM (1024 colours of 15 bits);
//   - the K052109 tilemap chip (k052109.js: a fixed layer and two scrolling layers) and
//     the K051960 / K051937 sprite chips (k051960.js); the sprite ROMs' address lines are
//     rearranged on this board by a PROM (see unscrambleSprites);
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

  // Sprite ROM address wiring. The K051960 asks for 32-bit word L = code * 32 + bottom * 16 +
  // right * 8 + row. On this board each block of 64 codes holds sprites of one size class, given
  // by the PROM at g19 (indexed by the code's top 8 bits); the ROMs store such a sprite's pixel
  // rows one after another across its whole width, so the address lines are reordered:
  // lowest first the right-half bit, then the cell-across bits, then the row bits, then the rest.
  // Size classes 0-7 cover 1, 1, 2, 2, 4, 4, 8, 8 cells across; the odd classes put the
  // cell-down bits ahead of the remaining code bits. (RetroPixels' reading of the board; not
  // yet checked against a real set.)
  const CLASS_W = [0, 0, 1, 1, 2, 2, 3, 3]; // log2 of cells across
  function spritePermutation(cls) {
    // source bits of L, listed from the physical address's bit 0 upward (11 bits: rows, halves, 6 code bits)
    const CA = (i) => i;                    // L bit i
    const xbits = [5, 7, 9], ybits = [6, 8, 10];
    const order = [CA(3)];
    const nx = CLASS_W[cls];
    for (let i = 0; i < nx; i++) order.push(xbits[i]);
    order.push(0, 1, 2, 4);
    const rest = [5, 6, 7, 8, 9, 10].filter((b) => !order.includes(b));
    if (cls & 1) rest.sort((a, b) => (ybits.includes(b) ? 1 : 0) - (ybits.includes(a) ? 1 : 0) || a - b);
    return order.concat(rest);
  }
  const PERMS = [0, 1, 2, 3, 4, 5, 6, 7].map(spritePermutation);
  // physical word address of logical word address L
  function spritePhys(L, prom) {
    const cls = prom.length ? prom[(L >> 11) & 0xff] & 7 : 0, perm = PERMS[cls];
    let P = L & ~0x7ff;
    for (let i = 0; i < 11; i++) P |= ((L >> perm[i]) & 1) << i;
    return P;
  }
  function unscrambleSprites(rom, prom) {
    const out = new Uint8Array(rom.length), words = rom.length >> 2;
    for (let L = 0; L < words; L++) {
      const P = spritePhys(L, prom) % words;
      out[L * 4] = rom[P * 4]; out[L * 4 + 1] = rom[P * 4 + 1]; out[L * 4 + 2] = rom[P * 4 + 2]; out[L * 4 + 3] = rom[P * 4 + 3];
    }
    return out;
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
      const prom = get("prom_sprites");
      this.title = decodeTitle(get("title"));

      this.ram = new Uint8Array(0x4000);
      this.palRam = new Uint8Array(0x800);
      this.palette = new Uint32Array(1024);
      this.zram = new Uint8Array(0x800);
      this.screen = new Uint32Array(W * H);
      this.width = W; this.height = H;
      this.bufB = new Uint16Array(W * H); this.bufA = new Uint16Array(W * H); this.bufF = new Uint16Array(W * H);
      this.bufS = new Uint16Array(W * H);
      this.inputs = [{}, {}, {}, {}];

      this.k052109 = new global.RPK052109({
        rom: tiles,
        tileCallback: (t) => {
          // board wiring: attribute bits 0-1 -> code bits 8-9, bit 4 -> code bit 10, bank bits -> 11-14;
          // attribute bits 5-7 pick one of 8 colours from the layer's colour base
          const c = t.color;
          t.code = t.code | ((c & 0x03) << 8) | ((c & 0x10) << 6) | ((c & 0x0c) << 9) | (t.bank << 13);
          t.color = TILE_COLORBASE[t.layer] + ((c & 0xe0) >> 5);
          t.flipX = false;
        },
      });
      this.k051960 = new global.RPK051960({
        rom: unscrambleSprites(spr, prom),
        spriteCallback: (t) => {
          // attribute bit 4 -> code bit 13; bits 0-3 = colour
          t.code |= (t.color & 0x10) << 9;
          t.color = SPRITE_COLORBASE + (t.color & 0x0f);
        },
      });
      this.k007232 = new global.RPK007232({ clock: Z80_HZ, rom: get("pcm"),
        onPort: (v) => { this.k007232.setVolume(0, (v & 15) * 0x11, (v & 15) * 0x11); this.k007232.setVolume(1, (v >> 4) * 0x11, (v >> 4) * 0x11); } });
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
      this.ram.fill(0); this.palRam.fill(0); this.palette.fill(0xff000000); this.zram.fill(0);
      this.k052109.reset(); this.k051960.reset(); this.k007232.reset(); this.upd.reset(); this.ym.reset();
      this.ctrl = 0; this.priority = 0; this.soundLatch = 0; this.sres = 0;
      this.titlePos = -1; this.titleAcc = 0; this.updAcc = 0; this.mixClk = 0; this.zcyc = 0;
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
      return ~((i.Left ? 1 : 0) | (i.Right ? 2 : 0) | (i.Up ? 4 : 0) | (i.Down ? 8 : 0) |
        (i["Button 1"] ? 0x10 : 0) | (i["Button 2"] ? 0x20 : 0) | (start ? 0x80 : 0)) & 0xff;
    }
    coinByte() {
      const p = this.inputs, p1 = p[0] || {};
      let v = 0;
      if (p1.Coin) v |= 1;
      if ((p[1] && p[1].Coin) || p1["Coin 2"]) v |= 2;
      if (p[2] && p[2].Coin) v |= 4;
      if (p[3] && p[3].Coin) v |= 8;
      if (p1.Service) v |= 0x10;
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
      this.palette[n] = RPArcade.rgb((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
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
        let v = fm * 0.45 + (this.kL || 0) * 0.25 + (this.updV || 0) * 0.3 + title * 0.35;
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
    draw() {
      const k = this.k052109, s = this.k051960, pal = this.palette, scr = this.screen;
      const opt = { x0: MAP_X0, y0: MAP_Y0 };
      this.bufB.fill(0); this.bufA.fill(0); this.bufF.fill(0); this.bufS.fill(0);
      k.render(2, this.bufB, W, H, { x0: MAP_X0, y0: MAP_Y0, opaque: true });
      k.render(1, this.bufA, W, H, opt);
      k.render(0, this.bufF, W, H, opt);
      s.render(this.bufS, W, H, { x0: MAP_X0, y0: MAP_Y0 });
      // order: layer B, then sprites and layer A (which one is in front set by the priority latch), then the fixed layer
      const spritesBehindA = ((this.priority >> 2) & 1) === 1;
      // (the K052109 flips its own layers; the sprites are turned round here when their chip's flip is on)
      const B = this.bufB, A = this.bufA, F = this.bufF, S = this.bufS, last = s.flipScreen ? W * H - 1 : -1;
      for (let i = 0; i < W * H; i++) {
        let c = B[i];
        const sp = last >= 0 ? S[last - i] : S[i];
        if (spritesBehindA) { if (sp) c = sp; if (A[i] & 15) c = A[i]; }
        else { if (A[i] & 15) c = A[i]; if (sp) c = sp; }
        if (F[i] & 15) c = F[i];
        scr[i] = pal[c & 1023];
      }
    }

    saveState() {
      return { v: 1, cpu: this.cpu.saveState(), z80: this.z80.getState(), ram: this.ram.slice(), palRam: this.palRam.slice(), zram: this.zram.slice(),
        k052109: this.k052109.saveState(), k051960: this.k051960.saveState(), k007232: this.k007232.saveState(), upd: this.upd.saveState(), ym: this.ym.saveState(),
        m: [this.ctrl, this.priority, this.soundLatch, this.sres, this.titlePos, this.titleAcc, this.updAcc, this.mixClk, this.zcyc, this.frame] };
    }
    loadState(s) {
      this.cpu.loadState(s.cpu); this.z80.setState(s.z80);
      this.ram.set(s.ram); this.palRam.set(s.palRam); this.zram.set(s.zram);
      this.k052109.loadState(s.k052109); this.k051960.loadState(s.k051960); this.k007232.loadState(s.k007232); this.upd.loadState(s.upd); this.ym.loadState(s.ym);
      [this.ctrl, this.priority, this.soundLatch, this.sres, this.titlePos, this.titleAcc, this.updAcc, this.mixClk, this.zcyc, this.frame] = s.m;
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
    { name: "963a30.g7", size: 0x100, role: "prom_priority" }, { name: "963a31.g19", size: 0x100, role: "prom_sprites" },
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
  TMNTBoard.spritePhys = spritePhys;

  RPArcade.boards.push(TMNTBoard);
})(typeof window !== "undefined" ? window : globalThis);
