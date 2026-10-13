// RetroPixels — Arcade board: Namco Galaxian hardware. Original code.
//
// One Z80 at 3.072 MHz with up to 16 KB of program ROM (Galaxian uses 10 KB),
// 1 KB of work RAM, a 32x32 tile screen where each tile column has its own
// scroll and colour, 8 hardware sprites, 8 "shells and missile" bullets drawn
// by the hardware itself, a twinkling starfield behind everything, and
// colours from one 32-byte PROM. The interrupt is the NMI at the start of
// vertical blank. The monitor is on its side: the board draws 256x224 and the
// framework turns the picture upright.
//
// Sound on the real board is analog circuitry (a pitch counter for the tunes,
// three swooping background oscillators, a noise generator for explosions and
// a shot "pew"). RetroPixels makes its own simpler versions of these sounds
// from the same control registers — it sounds like Galaxian, not identical.
// Written from the documented hardware behaviour.
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 3072000;
  const LINE_CYC = 192;       // 384 pixel clocks per line at 6.144 MHz, the CPU at half that
  const LINES = 264;
  const FIRST = 16, LAST = 240; // lines 16-239 are shown
  const W = 256, H = LAST - FIRST;

  // ===================================================================
  // Starfield (RetroPixels' own pattern): a fixed field of coloured stars that
  // drifts down the screen and twinkles in four groups.
  // ===================================================================
  function makeStars() {
    const stars = [];
    let r = 0x1d872;
    const next = () => { // 17-bit shift register
      const bit = ((r >> 16) ^ (r >> 4)) & 1;
      r = ((r << 1) | bit) & 0x1ffff;
      return r;
    };
    for (let y = 0; y < 256; y++) {
      for (let x = 0; x < 512; x++) {
        const v = next();
        if ((v & 0x1fe00) === 0x1fe00 && (v & 0x3f)) stars.push({ x, y, colour: v & 0x3f, group: (v >> 6) & 3 });
      }
    }
    return stars;
  }
  const STAR_LEVELS = [0x00, 0x97, 0xd7, 0xff];

  class GalaxianBoard {
    constructor(game, files, opts) {
      this.game = game;
      this.dips = opts.dips;
      this.sampleRate = opts.sampleRate || 48000;
      this.rom = RPArcade.region(game, files, "cpu", 0x4000);
      const gfx = RPArcade.region(game, files, "gfx", 0x1000);
      this.prom = RPArcade.region(game, files, "colour", 32);
      this.decodeGraphics(gfx);
      this.pens = new Uint32Array(32);
      for (let i = 0; i < 32; i++) {
        const c = this.prom[i], bit = (n) => (c >> n) & 1;
        this.pens[i] = RPArcade.rgb(0x21 * bit(0) + 0x47 * bit(1) + 0x97 * bit(2),
          0x21 * bit(3) + 0x47 * bit(4) + 0x97 * bit(5), 0x51 * bit(6) + 0xae * bit(7));
      }
      this.starList = makeStars();
      this.starPens = new Uint32Array(64);
      for (let i = 0; i < 64; i++) this.starPens[i] = RPArcade.rgb(STAR_LEVELS[i & 3], STAR_LEVELS[(i >> 2) & 3], STAR_LEVELS[i >> 4]);
      // stars indexed by line for drawing
      this.starsByLine = Array.from({ length: 256 }, () => []);
      for (const s of this.starList) this.starsByLine[s.y].push(s);

      this.ram = new Uint8Array(0x400);
      this.vram = new Uint8Array(0x400);
      this.obj = new Uint8Array(0x100);  // 0x00-0x3f column scroll/colour, 0x40-0x5f sprites, 0x60-0x7f bullets
      this.latch6000 = new Uint8Array(8); // lamps, coin lockout/counter, background sound frequency bits
      this.latch6800 = new Uint8Array(8); // sound: FS1-3, hit, -, fire, vol1, vol2
      this.latch7000 = new Uint8Array(8); // NMI enable, stars on, flip x / y
      this.screen = new Uint32Array(W * H);
      this.line = new Uint8Array(W); // per-pixel "something drawn" flags for one line
      this.p1 = {}; this.p2 = {};
      this.audio = new Float32Array(2048); this.audioN = 0;

      const self = this;
      this.cpu = new global.RPZ80({
        read: (a) => self.read(a),
        write: (a, v) => self.write(a, v),
        in: () => 0xff, out: () => {},
      });
      this.reset();
    }

    reset() {
      this.cpu.reset();
      this.ram.fill(0); this.vram.fill(0); this.obj.fill(0);
      this.latch6000.fill(0); this.latch6800.fill(0); this.latch7000.fill(0);
      this.pitch = 0xff; this.cycles = 0; this.frame = 0;
      this.snd = { tone: 0, bg: [0, 0, 0], lfo: 0, noise: 1, hitEnv: 0, fireEnv: 0, fireF: 0, firePh: 0, lp: 0, due: 0 };
    }

    // ---- graphics: two ROMs, the first gives each pixel's high bit, the second its low bit ----
    decodeGraphics(gfx) {
      const half = gfx.length >> 1, px = (o, bit) => (((gfx[o] >> bit) & 1) << 1) | ((gfx[o + half] >> bit) & 1);
      this.tiles = new Uint8Array(256 * 64);
      for (let t = 0; t < 256; t++) for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++)
        this.tiles[t * 64 + y * 8 + x] = px(t * 8 + y, 7 - x);
      // a 16x16 sprite is four 8x8 blocks: top-left, top-right, bottom-left, bottom-right
      this.sprites = new Uint8Array(64 * 256);
      for (let s = 0; s < 64; s++) for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++)
        this.sprites[s * 256 + y * 16 + x] = px(s * 32 + (y & 7) + (x & 8) + ((y & 8) << 1), 7 - (x & 7));
    }

    // Draw one line of the picture (y = the hardware's line number, 16-239).
    drawLine(y) {
      const scr = this.screen, row = (y - FIRST) * W, obj = this.obj, pens = this.pens, used = this.line;
      used.fill(0);
      // tiles: each 8-pixel column scrolls on its own and has its own colour
      for (let col = 0; col < 32; col++) {
        const ty = (y + obj[col * 2]) & 0xff, pal = (obj[col * 2 + 1] & 7) << 2;
        const t = this.vram[((ty >> 3) << 5) | col] * 64 + (ty & 7) * 8;
        for (let x = 0; x < 8; x++) {
          const p = this.tiles[t + x], o = col * 8 + x;
          scr[row + o] = pens[pal | p];
          used[o] = p ? 1 : 0;
        }
      }
      // sprites (sprite 0 drawn last, on top). The first three match one line later.
      for (let n = 7; n >= 0; n--) {
        const b = 0x40 + n * 4;
        const sy = (240 - obj[b] + (n < 3 ? 1 : 0)) & 0xff;
        const r = (y - sy) & 0xff;
        if (r >= 16) continue;
        const attr = obj[b + 1], pal = (obj[b + 2] & 7) << 2, sx = obj[b + 3] + 1;
        const yy = attr & 0x80 ? 15 - r : r, fx = attr & 0x40 ? 15 : 0, base = (attr & 0x3f) * 256 + yy * 16;
        for (let x = 0; x < 16; x++) {
          const o = sx + x;
          if (o >= W) break;
          const p = this.sprites[base + (x ^ fx)];
          if (p) { scr[row + o] = pens[pal | p]; used[o] = 1; }
        }
      }
      // bullets: one enemy shell (white) and the player's missile (yellow) per line, 4 pixels long
      let shell = -1, missile = -1;
      for (let n = 0; n < 8; n++) {
        const match = (n < 3 ? y - 1 : y) & 0xff;
        if (((obj[0x60 + n * 4 + 1] + match) & 0xff) !== 0xff) continue;
        if (n === 7) missile = n; else shell = n;
      }
      for (const n of [shell, missile]) {
        if (n < 0) continue;
        const x0 = 255 - obj[0x60 + n * 4 + 3] - 4, c = n === 7 ? RPArcade.rgb(0xff, 0xff, 0x00) : RPArcade.rgb(0xff, 0xff, 0xff);
        for (let x = x0; x < x0 + 4; x++) if (x >= 0 && x < W) { scr[row + x] = c; used[x] = 1; }
      }
      // stars, where nothing else is drawn
      if (this.latch7000[4]) {
        const sl = this.starsByLine[y], scroll = this.frame & 511, f = this.frame;
        for (let i = 0; i < sl.length; i++) {
          const s = sl[i];
          // two groups blink at a time, changing a few times a second
          if (((f >> 5) + s.group) & 2) continue;
          const x = (s.x + scroll) & 511;
          if (x < W && !used[x]) scr[row + x] = this.starPens[s.colour];
        }
      }
    }

    flipPicture() {
      const s = this.screen;
      if (this.latch7000[6]) for (let y = 0; y < H; y++) s.subarray(y * W, y * W + W).reverse();
      if (this.latch7000[7]) for (let y = 0; y < H >> 1; y++) {
        const a = s.slice(y * W, y * W + W);
        s.copyWithin(y * W, (H - 1 - y) * W, (H - y) * W);
        s.set(a, (H - 1 - y) * W);
      }
    }

    // ---- sound (RetroPixels' own simple versions of the board's sounds) ----
    makeSound(n) {
      const st = this.snd, rate = this.sampleRate, l68 = this.latch6800;
      // the tune: a square wave from the pitch register (0xff = silent), two volume bits
      const toneHz = this.pitch === 0xff ? 0 : 96000 / (2 * (256 - this.pitch));
      const toneVol = 0.06 * (1 + l68[6] + 2 * l68[7]);
      // background: three oscillators whose speed follows the 4-bit value at 0x6004-0x6007
      const lfo = this.latch6000[4] | (this.latch6000[5] << 1) | (this.latch6000[6] << 2) | (this.latch6000[7] << 3);
      const bgBase = 140 + lfo * 22;
      for (let i = 0; i < n; i++) {
        let v = 0;
        if (toneHz) { st.tone = (st.tone + toneHz / rate) % 1; v += st.tone < 0.5 ? toneVol : -toneVol; }
        for (let k = 0; k < 3; k++) {
          if (!l68[k]) continue;
          st.bg[k] = (st.bg[k] + bgBase * (1 + k * 0.26) / rate) % 1;
          v += (st.bg[k] < 0.5 ? 0.035 : -0.035);
        }
        // explosion: filtered noise that fades
        if (st.hitEnv > 0.001) {
          const bit = ((st.noise >> 0) ^ (st.noise >> 2)) & 1;
          st.noise = (st.noise >> 1) | (bit << 16);
          st.lp += ((st.noise & 1 ? 1 : -1) - st.lp) * 0.08;
          v += st.lp * st.hitEnv * 0.5;
          st.hitEnv *= l68[3] ? 0.99996 : 0.9997;
        }
        // shot: a falling tone that fades
        if (st.fireEnv > 0.001) {
          st.firePh = (st.firePh + st.fireF / rate) % 1;
          v += (st.firePh < 0.5 ? 1 : -1) * st.fireEnv * 0.18;
          st.fireF *= 0.99993; st.fireEnv *= 0.99985;
        }
        if (this.audioN >= this.audio.length) { const a = new Float32Array(this.audio.length * 2); a.set(this.audio); this.audio = a; }
        this.audio[this.audioN++] = v;
      }
    }
    takeAudio() { const out = this.audio.slice(0, this.audioN); this.audioN = 0; return out; }

    // ---- memory ----
    read(a) {
      a &= 0x7fff;
      if (a < 0x4000) return this.rom[a];
      if (a < 0x4800) return this.ram[a & 0x3ff];
      if (a < 0x5000) return 0xff;
      if (a < 0x5800) return this.vram[a & 0x3ff];
      if (a < 0x6000) return this.obj[a & 0xff];
      if (a < 0x6800) return this.readIn0();
      if (a < 0x7000) return this.readIn1();
      if (a < 0x7800) return this.readDsw();
      return 0xff; // watchdog
    }
    write(a, v) {
      a &= 0x7fff;
      if (a < 0x4000) return;
      if (a < 0x4800) { this.ram[a & 0x3ff] = v; return; }
      if (a < 0x5000) return;
      if (a < 0x5800) { this.vram[a & 0x3ff] = v; return; }
      if (a < 0x6000) { this.obj[a & 0xff] = v; return; }
      if (a < 0x6800) { this.latch6000[a & 7] = v & 1; return; }
      if (a < 0x7000) {
        const n = a & 7, was = this.latch6800[n];
        this.latch6800[n] = v & 1;
        if (n === 3 && (v & 1) && !was) this.snd.hitEnv = 1;
        if (n === 5 && (v & 1) && !was) { this.snd.fireEnv = 1; this.snd.fireF = 1800; }
        return;
      }
      if (a < 0x7800) {
        this.latch7000[a & 7] = v & 1;
        return;
      }
      this.pitch = v;
    }

    // ---- controls (pressed = 1) ----
    readIn0() {
      const p = this.p1, d = this.dips;
      return (p.Coin ? 1 : 0) | (p["Coin 2"] ? 2 : 0) | (p.Left ? 4 : 0) | (p.Right ? 8 : 0) | (p.Fire ? 0x10 : 0) |
        (d.cabinet ? 0x20 : 0) | (d.test ? 0x40 : 0) | (p.Service ? 0x80 : 0);
    }
    readIn1() {
      const p = this.p1, q = this.p2, d = this.dips;
      return ((p["Start (1P)"] || q["Start (1P)"]) ? 1 : 0) | ((p["Start (2P)"] || q["Start (2P)"]) ? 2 : 0) |
        (q.Left ? 4 : 0) | (q.Right ? 8 : 0) | (q.Fire ? 0x10 : 0) | ((d.coins & 3) << 6);
    }
    readDsw() { const d = this.dips; return (d.bonus & 3) | ((d.lives & 1) << 2); }
    setInput(p1, p2) { this.p1 = p1; this.p2 = p2 || {}; }

    // ---- running ----
    runFrame() {
      const cpu = this.cpu, perLine = this.sampleRate / (this.constructor.fps * LINES);
      for (let y = 0; y < LINES; y++) {
        if (y === LAST) {
          this.flipPicture();
          if (this.latch7000[1]) cpu.nmi(); // vertical blank
        }
        const end = this.cycles + LINE_CYC;
        while (this.cycles < end) this.cycles += cpu.step();
        if (y >= FIRST && y < LAST) this.drawLine(y);
        this.snd.due += perLine;
        const n = Math.floor(this.snd.due); this.snd.due -= n;
        this.makeSound(n);
      }
      this.cycles -= LINES * LINE_CYC;
      this.frame++;
    }

    saveState() {
      const s = this.snd;
      return { v: 1, cpu: this.cpu.getState(), ram: this.ram.slice(), vram: this.vram.slice(), obj: this.obj.slice(),
        l60: this.latch6000.slice(), l68: this.latch6800.slice(), l70: this.latch7000.slice(),
        m: [this.pitch, this.cycles, this.frame],
        snd: [s.tone, s.bg[0], s.bg[1], s.bg[2], s.noise, s.hitEnv, s.fireEnv, s.fireF, s.firePh, s.lp, s.due] };
    }
    loadState(o) {
      this.cpu.setState(o.cpu);
      this.ram.set(o.ram); this.vram.set(o.vram); this.obj.set(o.obj);
      this.latch6000.set(o.l60); this.latch6800.set(o.l68); this.latch7000.set(o.l70);
      [this.pitch, this.cycles, this.frame] = o.m;
      const s = this.snd, a = o.snd;
      [s.tone, s.bg[0], s.bg[1], s.bg[2], s.noise, s.hitEnv, s.fireEnv, s.fireF, s.firePh, s.lp, s.due] = a;
      this.audioN = 0;
      for (let y = FIRST; y < LAST; y++) this.drawLine(y);
      this.flipPicture();
    }
  }

  GalaxianBoard.id = "galaxian";
  GalaxianBoard.title = "Namco Galaxian";
  GalaxianBoard.width = W;
  GalaxianBoard.height = H;
  GalaxianBoard.rotate = 90;
  GalaxianBoard.fps = CPU_HZ / (LINE_CYC * LINES);
  GalaxianBoard.buttons = ["Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"];
  GalaxianBoard.dips = [
    { id: "coins", label: "Coins", default: 0, options: [{ label: "1 coin, 1 game", value: 0 }, { label: "2 coins, 1 game", value: 1 },
      { label: "1 coin, 2 games", value: 2 }, { label: "Free play", value: 3 }] },
    { id: "bonus", label: "Extra ship at", default: 0, options: [{ label: "7,000 points", value: 0 }, { label: "10,000 points", value: 1 },
      { label: "12,000 points", value: 2 }, { label: "20,000 points", value: 3 }] },
    { id: "lives", label: "Ships", default: 1, options: [{ label: "2", value: 0 }, { label: "3", value: 1 }] },
    { id: "cabinet", label: "Cabinet", default: 0, options: [{ label: "Upright", value: 0 }, { label: "Cocktail table", value: 1 }] },
    { id: "test", label: "Service mode (test screen)", default: 0, options: [{ label: "Off", value: 0 }, { label: "On", value: 1 }] },
  ];
  const FILES = [
    { name: "galmidw.u", size: 0x800, role: "cpu", offset: 0x0000 },
    { name: "galmidw.v", size: 0x800, role: "cpu", offset: 0x0800 },
    { name: "galmidw.w", size: 0x800, role: "cpu", offset: 0x1000 },
    { name: "galmidw.y", size: 0x800, role: "cpu", offset: 0x1800 },
    { name: "7l", alt: ["galmidw.z", "7l.bin"], size: 0x800, role: "cpu", offset: 0x2000 },
    { name: "1h.bin", alt: ["1h", "galmidw.1j", "galaxian.1j"], size: 0x800, role: "gfx", offset: 0 },
    { name: "1k.bin", alt: ["1k", "galmidw.1k", "galaxian.1k"], size: 0x800, role: "gfx", offset: 0x800 },
    { name: "6l.bpr", alt: ["6l", "galaxian.6l"], size: 0x20, role: "colour" },
  ];
  GalaxianBoard.games = [
    { id: "galaxian", title: "Galaxian", year: 1979, maker: "Namco", files: FILES },
    { id: "galmidw", title: "Galaxian (Midway)", year: 1979, maker: "Namco (Midway license)", parent: "galaxian", files: FILES },
  ];

  RPArcade.boards.push(GalaxianBoard);
})(typeof window !== "undefined" ? window : globalThis);
