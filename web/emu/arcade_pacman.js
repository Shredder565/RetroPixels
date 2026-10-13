// RetroPixels — Arcade board: Namco Pac-Man hardware (Puck Man / Pac-Man). Original code.
//
// One Z80 at 3.072 MHz with 16 KB of program ROM, 1 KB of tile RAM and 1 KB of
// tile colour RAM, 1 KB of work RAM (its last 16 bytes hold the 8 sprites'
// number / flip / colour), a 36x28 tile screen with 8 hardware sprites, colours
// from two PROMs (a 32-byte colour PROM and a 256-byte lookup PROM), and
// Namco's 3-voice wavetable sound, whose 8 waveforms come from a PROM in the
// set. The game sets the interrupt vector with an OUT to port 0 and runs in
// interrupt mode 2; the interrupt comes at the start of each vertical blank.
// The monitor is turned on its side, so the board draws 288x224 and the
// framework turns the picture upright. Written from the documented hardware
// behaviour.
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 3072000;           // 18.432 MHz / 6
  const LINE_CYC = 192;             // 384 pixel clocks per line, the CPU runs at half the pixel clock
  const LINES = 264;                // 224 shown + 40 of vertical blank
  const SHOWN = 224;
  const W = 288, H = 224;
  const SOUND_HZ = CPU_HZ / 32;     // the sound chip makes a sample every 32 CPU clocks (96 kHz)
  const SAMPLES_PER_LINE = LINE_CYC / 32;

  const B = { up: 1, left: 2, right: 4, down: 8 };

  // The PROMs have two common names (Midway "82s12x" and Namco "pm1-x").
  const PROMS = [
    { name: "82s123.7f", alt: ["pm1-1.7f"], size: 32, role: "colour" },
    { name: "82s126.4a", alt: ["pm1-4.4a"], size: 256, role: "lookup" },
    { name: "82s126.1m", alt: ["pm1-3.1m"], size: 256, role: "wave" },
    { name: "82s126.3m", alt: ["pm1-2.3m"], size: 256, role: "timing", optional: true },
  ];
  const PROMS_NAMCO = [
    { name: "pm1-1.7f", alt: ["82s123.7f"], size: 32, role: "colour" },
    { name: "pm1-4.4a", alt: ["82s126.4a"], size: 256, role: "lookup" },
    { name: "pm1-3.1m", alt: ["82s126.1m"], size: 256, role: "wave" },
    { name: "pm1-2.3m", alt: ["82s126.3m"], size: 256, role: "timing", optional: true },
  ];

  class PacmanBoard {
    constructor(game, files, opts) {
      this.game = game;
      this.dips = opts.dips;
      this.rom = RPArcade.region(game, files, "cpu", 0x4000);
      this.tilesRom = RPArcade.region(game, files, "tiles", 0x1000);
      this.spritesRom = RPArcade.region(game, files, "sprites", 0x1000);
      this.colourProm = RPArcade.region(game, files, "colour", 32);
      this.lookupProm = RPArcade.region(game, files, "lookup", 256);
      this.waveProm = RPArcade.region(game, files, "wave", 256);
      this.decodeGraphics();
      this.decodeColours();

      this.vram = new Uint8Array(0x400);   // tile numbers
      this.cram = new Uint8Array(0x400);   // tile colours
      this.ram = new Uint8Array(0x400);    // work RAM; 0x3f0-0x3ff = sprite number/flip and colour
      this.spritePos = new Uint8Array(16); // 0x5060-0x506f: x, y for each sprite
      this.latch = new Uint8Array(8);      // 0x5000-0x5007: irq enable, sound on, aux, flip, lamps, lockout, counter
      this.snd = new Uint8Array(32);       // 0x5040-0x505f: the sound chip's 4-bit registers
      this.acc = new Uint32Array(3);       // the three voices' wave positions (20 bits)
      this.screen = new Uint32Array(W * H);
      this.out = new RPArcade.Resampler(SOUND_HZ, opts.sampleRate || 48000);
      this.in0 = 0xff; this.in1 = 0xff;
      this.p1 = {}; this.p2 = {};

      const self = this;
      this.cpu = new global.RPZ80({
        read: (a) => self.read(a),
        write: (a, v) => self.write(a, v),
        in: () => 0xff,
        out: (port, v) => { if ((port & 0xff) === 0) self.vector = v; },
        vector: () => { self.irqLine = false; self.cpu.irq = false; return self.vector; },
      });
      this.reset();
    }

    reset() {
      this.cpu.reset();
      this.vram.fill(0); this.cram.fill(0); this.ram.fill(0); this.spritePos.fill(0);
      this.latch.fill(0); this.snd.fill(0); this.acc.fill(0);
      this.vector = 0xff; this.irqLine = false; this.cpu.irq = false;
      this.watchdog = 0; this.cycles = 0; this.frame = 0;
    }

    // ---- graphics ----
    // Each 8x8 tile is 16 bytes: bytes 8-15 give pixels 0-3 of each line, bytes 0-7 pixels 4-7.
    // In a byte, pixel n (0-3) takes its high bit from bit 7-n and its low bit from bit 3-n.
    // A 16x16 sprite is four such 4-pixel-wide strips side by side, top half first.
    decodeGraphics() {
      const px = (rom, base, x, y, strips) => {
        const b = rom[base + strips[x >> 2] + y], n = x & 3;
        return (((b >> (7 - n)) & 1) << 1) | ((b >> (3 - n)) & 1);
      };
      this.tiles = new Uint8Array(256 * 64);
      for (let t = 0; t < 256; t++) for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++)
        this.tiles[t * 64 + y * 8 + x] = px(this.tilesRom, t * 16, x, y, [8, 0]);
      this.sprites = new Uint8Array(64 * 256);
      for (let s = 0; s < 64; s++) for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++)
        this.sprites[s * 256 + y * 16 + x] = px(this.spritesRom, s * 64 + (y < 8 ? 0 : 32), x, y & 7, [8, 16, 24, 0]);
    }

    // Colour PROM: 3 bits red, 3 bits green, 2 bits blue through weighted resistors.
    // Lookup PROM: for each of 64 palettes, which colour each of the 4 pixel values uses.
    decodeColours() {
      const colours = new Uint32Array(16);
      for (let i = 0; i < 16; i++) {
        const c = this.colourProm[i], bit = (n) => (c >> n) & 1;
        colours[i] = RPArcade.rgb(0x21 * bit(0) + 0x47 * bit(1) + 0x97 * bit(2),
          0x21 * bit(3) + 0x47 * bit(4) + 0x97 * bit(5), 0x51 * bit(6) + 0xae * bit(7));
      }
      this.pens = new Uint32Array(256); this.clear = new Uint8Array(256);
      for (let i = 0; i < 256; i++) {
        const c = this.lookupProm[i] & 15;
        this.pens[i] = colours[c]; this.clear[i] = c === 0 ? 1 : 0; // colour 0 is see-through on sprites
      }
    }

    // Where the tile under native tile column tc (0-35, along the scan line) and row tr (0-27) lives.
    // The middle 32 columns are the playfield; the 2 at each end are the score / credit lines,
    // which upright are the top and bottom two rows.
    static tileOffset(tc, tr) {
      if (tc < 2) return 0x3c0 + tc * 32 + 2 + tr;
      if (tc >= 34) return (tc - 34) * 32 + 2 + tr;
      return 0x40 + tr * 32 + (tc - 2);
    }

    render() {
      const scr = this.screen, tiles = this.tiles, pens = this.pens;
      for (let tr = 0; tr < 28; tr++) for (let tc = 0; tc < 36; tc++) {
        const o = PacmanBoard.tileOffset(tc, tr);
        const t = this.vram[o] * 64, pal = (this.cram[o] & 0x1f) << 2;
        for (let y = 0; y < 8; y++) {
          let d = (tr * 8 + y) * W + tc * 8;
          for (let x = 0; x < 8; x++) scr[d++] = pens[pal | tiles[t + y * 8 + x]];
        }
      }
      // sprites: 7 first so sprite 0 ends up on top; they only show over the 256-pixel playfield
      const spr = this.sprites, clear = this.clear;
      for (let n = 7; n >= 0; n--) {
        const attr = this.ram[0x3f0 + n * 2], pal = (this.ram[0x3f1 + n * 2] & 0x3f) << 2;
        const sx = 272 - this.spritePos[n * 2 + 1], sy = this.spritePos[n * 2] - 31;
        const base = (attr >> 2) * 256, fx = attr & 1 ? 15 : 0, fy = attr & 2 ? 15 : 0;
        for (let y = 0; y < 16; y++) {
          const yy = (sy + y) & 0xff; // the line counter wraps at 256
          if (yy >= H) continue;
          for (let x = 0; x < 16; x++) {
            const xx = sx + x;
            if (xx < 16 || xx >= 272) continue;
            const p = pal | spr[base + ((y ^ fy) << 4) + (x ^ fx)];
            if (!clear[p]) scr[yy * W + xx] = pens[p];
          }
        }
      }
      // flip screen (cocktail tables flip the picture for player 2)
      if (this.latch[3] & 1) scr.reverse();
    }

    // ---- sound ----
    soundSample() {
      if (!(this.latch[1] & 1)) { this.out.add(0); return; }
      const r = this.snd, wave = this.waveProm;
      let mix = 0;
      for (let v = 0; v < 3; v++) {
        const base = v === 0 ? 0x10 : v === 1 ? 0x16 : 0x1b;
        // voice 1 has a 20-bit frequency; voices 2 and 3 lack the lowest 4 bits
        let freq = 0;
        if (v === 0) freq = r[0x10] | (r[0x11] << 4) | (r[0x12] << 8) | (r[0x13] << 12) | (r[0x14] << 16);
        else freq = (r[base] << 4) | (r[base + 1] << 8) | (r[base + 2] << 12) | (r[base + 3] << 16);
        const vol = r[v === 0 ? 0x15 : v === 1 ? 0x1a : 0x1f];
        const sel = r[v === 0 ? 0x05 : v === 1 ? 0x0a : 0x0f] & 7;
        const a = (this.acc[v] + freq) & 0xfffff; this.acc[v] = a;
        mix += ((wave[(sel << 5) | (a >> 15)] & 15) - 8) * vol;
      }
      this.out.add(mix / 360 * 0.7);
    }
    takeAudio() { return this.out.take(); }

    // ---- memory ----
    read(a) {
      a &= 0x7fff; // the top address line isn't connected
      if (a < 0x4000) return this.rom[a];
      if (a < 0x5000) {
        switch (a & 0xc00) {
          case 0x000: return this.vram[a & 0x3ff];
          case 0x400: return this.cram[a & 0x3ff];
          case 0xc00: return this.ram[a & 0x3ff];
          default: return 0xff;
        }
      }
      if (a < 0x6000) {
        switch (a & 0xc0) {
          case 0x00: return this.readIn0();
          case 0x40: return this.readIn1();
          case 0x80: return this.readDsw();
          default: return 0xff;
        }
      }
      return 0xff;
    }
    write(a, v) {
      a &= 0x7fff;
      if (a < 0x4000) return;
      if (a < 0x5000) {
        switch (a & 0xc00) {
          case 0x000: this.vram[a & 0x3ff] = v; return;
          case 0x400: this.cram[a & 0x3ff] = v; return;
          case 0xc00: this.ram[a & 0x3ff] = v; return;
          default: return;
        }
      }
      if (a < 0x6000) {
        const r = a & 0xff;
        if (r < 0x40) {
          this.latch[r & 7] = v & 1;
          if ((r & 7) === 0 && !(v & 1)) { this.irqLine = false; this.cpu.irq = false; }
        } else if (r < 0x60) {
          const n = r & 0x1f; v &= 15; this.snd[n] = v;
          // writing a wave position register sets that part of the voice's position
          const voice = n < 5 ? 0 : n >= 6 && n < 10 ? 1 : n >= 11 && n < 15 ? 2 : -1;
          if (voice >= 0) {
            const shift = voice === 0 ? n * 4 : voice === 1 ? (n - 5) * 4 : (n - 10) * 4;
            this.acc[voice] = (this.acc[voice] & ~(15 << shift) & 0xfffff) | (v << shift);
          }
        } else if (r < 0x70) this.spritePos[r & 15] = v;
        else if (r >= 0xc0) this.watchdog = 0;
      }
    }

    // ---- controls (pressed = 0) ----
    readIn0() {
      const p = this.p1, d = this.dips;
      let v = 0xff;
      if (p.Up) v &= ~B.up; if (p.Left) v &= ~B.left; if (p.Right) v &= ~B.right; if (p.Down) v &= ~B.down;
      if (d.rack === 1) v &= ~0x10;
      if (p.Coin) v &= ~0x20;
      if (p["Coin 2"]) v &= ~0x40;
      if (p.Service) v &= ~0x80;
      return v;
    }
    readIn1() {
      const p = this.p1, q = this.p2, d = this.dips;
      let v = 0xff;
      // the second player's stick (used on cocktail tables)
      if (q.Up) v &= ~B.up; if (q.Left) v &= ~B.left; if (q.Right) v &= ~B.right; if (q.Down) v &= ~B.down;
      if (d.test === 1) v &= ~0x10;
      if (p["Start (1P)"] || q["Start (1P)"]) v &= ~0x20;
      if (p["Start (2P)"] || q["Start (2P)"]) v &= ~0x40;
      if (d.cabinet === 1) v &= ~0x80;
      return v;
    }
    readDsw() {
      const d = this.dips;
      return (d.coins & 3) | ((d.lives & 3) << 2) | ((d.bonus & 3) << 4) | ((d.difficulty & 1) << 6) | ((d.names & 1) << 7);
    }
    setInput(p1, p2) { this.p1 = p1; this.p2 = p2 || {}; }

    // ---- running ----
    runFrame() {
      const cpu = this.cpu;
      for (let line = 0; line < LINES; line++) {
        if (line === SHOWN) {
          this.render();
          if (this.latch[0] & 1) { this.irqLine = true; cpu.irq = true; }
          // the watchdog restarts the board if the game stops checking in
          if (++this.watchdog >= 16) { this.reset(); return; }
        }
        if (line === 0 && this.irqLine) { this.irqLine = false; cpu.irq = false; } // interrupt modes 0/1 never acknowledge through the vector
        const end = this.cycles + LINE_CYC;
        while (this.cycles < end) this.cycles += cpu.step();
        for (let s = 0; s < SAMPLES_PER_LINE; s++) this.soundSample();
      }
      this.cycles -= LINES * LINE_CYC;
      this.frame++;
    }

    saveState() {
      return { v: 1, cpu: this.cpu.getState(), vram: this.vram.slice(), cram: this.cram.slice(), ram: this.ram.slice(),
        spritePos: this.spritePos.slice(), latch: this.latch.slice(), snd: this.snd.slice(), acc: this.acc.slice(),
        m: [this.vector, this.irqLine ? 1 : 0, this.watchdog, this.cycles, this.frame] };
    }
    loadState(s) {
      this.cpu.setState(s.cpu);
      this.vram.set(s.vram); this.cram.set(s.cram); this.ram.set(s.ram); this.spritePos.set(s.spritePos);
      this.latch.set(s.latch); this.snd.set(s.snd); this.acc.set(s.acc);
      [this.vector, , this.watchdog, this.cycles, this.frame] = s.m;
      this.irqLine = !!s.m[1]; this.cpu.irq = this.irqLine;
      this.out.clear();
      this.render();
    }
  }

  PacmanBoard.id = "pacman";
  PacmanBoard.title = "Namco Pac-Man";
  PacmanBoard.width = W;
  PacmanBoard.height = H;
  PacmanBoard.rotate = 90;
  PacmanBoard.fps = CPU_HZ / (LINE_CYC * LINES); // about 60.61
  PacmanBoard.buttons = ["Up", "Down", "Left", "Right", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"];
  PacmanBoard.dips = [
    { id: "coins", label: "Coins", default: 1, options: [{ label: "1 coin, 1 game", value: 1 }, { label: "1 coin, 2 games", value: 2 },
      { label: "2 coins, 1 game", value: 3 }, { label: "Free play", value: 0 }] },
    { id: "lives", label: "Lives", default: 2, options: [{ label: "1", value: 0 }, { label: "2", value: 1 }, { label: "3", value: 2 }, { label: "5", value: 3 }] },
    { id: "bonus", label: "Extra life at", default: 0, options: [{ label: "10,000 points", value: 0 }, { label: "15,000 points", value: 1 },
      { label: "20,000 points", value: 2 }, { label: "No extra life", value: 3 }] },
    { id: "difficulty", label: "Difficulty", default: 1, options: [{ label: "Normal", value: 1 }, { label: "Hard", value: 0 }] },
    { id: "names", label: "Ghost names", default: 1, options: [{ label: "Normal", value: 1 }, { label: "Alternate", value: 0 }] },
    { id: "cabinet", label: "Cabinet", default: 0, options: [{ label: "Upright", value: 0 }, { label: "Cocktail table", value: 1 }] },
    { id: "rack", label: "Rack test (skip levels)", default: 0, options: [{ label: "Off", value: 0 }, { label: "On", value: 1 }] },
    { id: "test", label: "Service mode (test screen)", default: 0, options: [{ label: "Off", value: 0 }, { label: "On", value: 1 }] },
  ];
  PacmanBoard.games = [
    { id: "puckman", title: "Puck Man", year: 1980, maker: "Namco",
      files: [
        { name: "pm1_prg1.6e", size: 0x800, role: "cpu", offset: 0x0000 }, { name: "pm1_prg2.6k", size: 0x800, role: "cpu", offset: 0x0800 },
        { name: "pm1_prg3.6f", size: 0x800, role: "cpu", offset: 0x1000 }, { name: "pm1_prg4.6m", size: 0x800, role: "cpu", offset: 0x1800 },
        { name: "pm1_prg5.6h", size: 0x800, role: "cpu", offset: 0x2000 }, { name: "pm1_prg6.6n", size: 0x800, role: "cpu", offset: 0x2800 },
        { name: "pm1_prg7.6j", size: 0x800, role: "cpu", offset: 0x3000 }, { name: "pm1_prg8.6p", size: 0x800, role: "cpu", offset: 0x3800 },
        { name: "pm1_chg1.5e", size: 0x800, role: "tiles", offset: 0 }, { name: "pm1_chg2.5h", size: 0x800, role: "tiles", offset: 0x800 },
        { name: "pm1_chg3.5f", size: 0x800, role: "sprites", offset: 0 }, { name: "pm1_chg4.5j", size: 0x800, role: "sprites", offset: 0x800 },
        ...PROMS_NAMCO,
      ] },
    // older dumps of the same game with 4 KB program chips
    { id: "puckman", title: "Puck Man", year: 1980, maker: "Namco",
      files: [
        { name: "namcopac.6e", size: 0x1000, role: "cpu", offset: 0x0000 }, { name: "namcopac.6f", size: 0x1000, role: "cpu", offset: 0x1000 },
        { name: "namcopac.6h", size: 0x1000, role: "cpu", offset: 0x2000 }, { name: "namcopac.6j", size: 0x1000, role: "cpu", offset: 0x3000 },
        { name: "pacman.5e", size: 0x1000, role: "tiles" }, { name: "pacman.5f", size: 0x1000, role: "sprites" },
        ...PROMS,
      ] },
    { id: "pacman", title: "Pac-Man", year: 1980, maker: "Namco (Midway license)", parent: "puckman",
      files: [
        { name: "pacman.6e", size: 0x1000, role: "cpu", offset: 0x0000 }, { name: "pacman.6f", size: 0x1000, role: "cpu", offset: 0x1000 },
        { name: "pacman.6h", size: 0x1000, role: "cpu", offset: 0x2000 }, { name: "pacman.6j", size: 0x1000, role: "cpu", offset: 0x3000 },
        { name: "pacman.5e", size: 0x1000, role: "tiles" }, { name: "pacman.5f", size: 0x1000, role: "sprites" },
        ...PROMS,
      ] },
  ];

  RPArcade.boards.push(PacmanBoard);
})(typeof window !== "undefined" ? window : globalThis);
