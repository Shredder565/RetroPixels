// RetroPixels — Donkey Kong arcade board (Nintendo, 1981). Original code,
// written from the documented behaviour of the board:
//   - a Z80 at 3.072 MHz (RPZ80) with 16 KB of program ROM, work RAM, a 32x32
//     tile map and a sprite list that an Intel 8257 DMA chip copies into the
//     video hardware's own sprite memory once a frame;
//   - 8x8 tiles and 16x16 sprites with 2 bits per pixel, coloured through the
//     set's three PROMs (two make the 256-colour palette, one picks the colour of
//     each group of tiles), with 4 palette banks the game switches between;
//   - an Intel 8035 sound processor (RPMCS48) running the set's own sound program:
//     it reads the tune number from a latch and the music data from the second sound
//     ROM, and plays through an 8-bit DAC whose volume fades out under program control;
//   - Mario's walking, jumping and Donkey Kong's stomping, which are separate
//     analogue circuits on the real board, made by RetroPixels' own code here.
// The monitor is on its side: the board draws 256x224 and the picture is turned
// a quarter turn clockwise.
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 3072000;           // 61.44 MHz / 20
  const LINE_CYC = 192;             // 384 pixel clocks a line at 6.144 MHz
  const LINES = 264;
  const FPS = CPU_HZ / (LINE_CYC * LINES); // about 60.6
  const SND_HZ = 6000000 / 15;      // the 8035's machine cycles a second (6 MHz crystal)
  const W = 256, H = 224, TOP = 16; // raster lines 16..239 are shown
  const BLACK = 0xff000000;

  class DonkeyKongBoard {
    constructor(game, files, opts) {
      opts = opts || {};
      this.game = game;
      this.sampleRate = opts.sampleRate || 48000;
      this.dips = opts.dips || {};
      for (const d of DonkeyKongBoard.dips) if (this.dips[d.id] === undefined) this.dips[d.id] = d.default;
      const region = (role, size) => RPArcade.region(game, files, role, size);
      this.rom = region("cpu", 0x4000);
      this.sndRom = region("sound", 0x1800);       // program at 000-7ff, tune data at 1000-17ff
      const tileRom = region("tiles", 0x1000), spriteRom = region("sprites", 0x2000);
      const proms = region("prom", 0x300);
      this.decodeGraphics(tileRom, spriteRom);
      this.decodePalette(proms);
      this.tileColour = proms.slice(0x200, 0x300);

      this.ram = new Uint8Array(0x1000);    // 6000-6fff
      this.spriteRam = new Uint8Array(0x400); // 7000-73ff (filled by the DMA)
      this.vram = new Uint8Array(0x400);    // 7400-77ff
      this.screen = new Uint32Array(W * H);
      this.raster = new Uint8Array(W * 256); // colour index of each pixel of one frame (0 = black)
      this.width = W; this.height = H;
      this.p1 = {}; this.p2 = {};
      this.audioOut = new Float32Array(4096); this.audioN = 0;

      const self = this;
      this.cpu = new global.RPZ80({
        read: (a) => self.read(a),
        write: (a, v) => self.write(a, v),
        in: () => 0xff,
        out: () => {},
      });
      this.snd = new global.RPMCS48({
        rom: (a) => (a < 0x800 ? self.sndRom[a] : 0xff),
        readX: (a) => {
          // P2 bit 6 selects the tune latch; otherwise P2 bits 0-2 page the tune data ROM
          if (self.sndP2 & 0x40) return self.soundLatch & 0x0f;
          return self.sndRom[0x1000 + (self.sndP2 & 7) * 256 + (a & 0xff)];
        },
        writeX: () => {},
        outP: (p, v) => {
          if (p === 1) self.dac = v;
          else if (p === 2) self.sndP2 = v;
        },
        // port 2 reads back its own latch, except bit 5: sound latch output 3 (inverted)
        inP: (p) => (p === 1 ? self.snd.port(1) : p === 2 ? (self.snd.port(2) & ~0x20) | (self.latch259 & 0x08 ? 0 : 0x20) : 0xff),
        // the test pins read the main board's sound latch outputs 5 and 4 (inverted)
        t0: () => !(self.latch259 & 0x20),
        t1: () => !(self.latch259 & 0x10),
        intLine: () => self.sndIrq,
      }, { ram: 64 });
      this.reset();
    }

    // ---- graphics ----
    decodeGraphics(tileRom, spriteRom) {
      // tiles: 256 of 8x8; low bit from the first ROM, high bit from the second
      this.tiles = new Uint8Array(256 * 64);
      for (let n = 0; n < 256; n++) for (let y = 0; y < 8; y++) {
        const lo = tileRom[n * 8 + y], hi = tileRom[0x800 + n * 8 + y];
        for (let x = 0; x < 8; x++) {
          const b = 7 - x;
          this.tiles[n * 64 + y * 8 + x] = ((lo >> b) & 1) | (((hi >> b) & 1) << 1);
        }
      }
      // sprites: 128 of 16x16. Left half from the 1st/3rd ROM, right half from the 2nd/4th;
      // low bit from the first pair, high bit from the second pair.
      this.sprites = new Uint8Array(128 * 256);
      for (let n = 0; n < 128; n++) for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
        const half = x < 8 ? 0 : 0x800, b = 7 - (x & 7), o = half + n * 16 + y;
        this.sprites[n * 256 + y * 16 + x] = ((spriteRom[o] >> b) & 1) | (((spriteRom[o + 0x1000] >> b) & 1) << 1);
      }
    }

    decodePalette(proms) {
      // Each colour: red 3 bits and green's top bit from the second PROM, green's low 2 bits
      // and blue's 2 bits from the first. The outputs are inverted and go through resistors
      // weighted about 1 : 2 : 4.
      const w3 = (v) => Math.round(255 * ((v & 1) * 0.13 + ((v >> 1) & 1) * 0.28 + ((v >> 2) & 1) * 0.59));
      const w2 = (v) => Math.round(255 * ((v & 1) * 0.32 + ((v >> 1) & 1) * 0.68));
      this.palette = new Uint32Array(256);
      for (let i = 0; i < 256; i++) {
        const lo = ~proms[i] & 0xf, hi = ~proms[0x100 + i] & 0xf;
        const r = (hi >> 1) & 7, g = ((hi & 1) << 2) | ((lo >> 2) & 3), b = lo & 3;
        this.palette[i] = (0xff000000 | (w2(b) << 16) | (w3(g) << 8) | w3(r)) >>> 0;
      }
    }

    reset() {
      this.cpu.reset(); this.snd.reset();
      this.ram.fill(0); this.spriteRam.fill(0); this.vram.fill(0);
      this.latch259 = 0;          // 7d00-7d07: sound triggers
      this.misc = 0;              // 7d80-7d87: bit 0 sound irq, 2 flip, 3 sprite bank, 4 nmi enable, 5 dma, 6-7 palette bank
      this.soundLatch = 0x0f;
      this.sndIrq = false; this.sndP2 = 0xff; this.dac = 0;
      this.dma = new Uint16Array(8); this.dmaFlip = 0; this.dmaMode = 0;
      this.cyc = 0; this.sndCyc = 0; this.sndAcc = 0; this.sndAccN = 0; this.samplePos = 0;
      this.env = 1;
      this.fx = new Float64Array(12); // walk, jump, stomp: time, phase, filter, on
      this.noise = 1; this.hp = 0; this.hpPrev = 0; this.lp = 0;
      this.screen.fill(BLACK);
    }

    setInput(p1, p2) { this.p1 = p1 || {}; this.p2 = p2 || {}; }

    joy(i) {
      return (i.Right ? 0x01 : 0) | (i.Left ? 0x02 : 0) | (i.Up ? 0x04 : 0) | (i.Down ? 0x08 : 0) | (i.Jump ? 0x10 : 0);
    }

    // ---- main processor's memory ----
    read(a) {
      if (a < 0x4000) return this.rom[a];
      if (a >= 0x6000 && a < 0x7000) return this.ram[a - 0x6000];
      if (a >= 0x7000 && a < 0x7400) return this.spriteRam[a - 0x7000];
      if (a >= 0x7400 && a < 0x7800) return this.vram[a - 0x7400];
      switch (a & 0xff80) {
        case 0x7c00: return this.joy(this.p1);
        case 0x7c80: return this.joy(this.p2);
        case 0x7d00: {
          const i = this.p1;
          return (i["Start (1P)"] ? 0x04 : 0) | (i["Start (2P)"] ? 0x08 : 0) | (i.Coin ? 0x80 : 0) |
            ((this.snd.port(2) & 0x10) ? 0 : 0x40); // the sound processor's "busy" line
        }
        case 0x7d80: {
          const d = this.dips;
          return (d.lives & 3) | ((d.bonus & 3) << 2) | 0x80; // 1 coin 1 play, upright cabinet
        }
        default: return 0xff;
      }
    }

    write(a, v) {
      if (a >= 0x6000 && a < 0x7000) { this.ram[a - 0x6000] = v; return; }
      if (a >= 0x7000 && a < 0x7400) { this.spriteRam[a - 0x7000] = v; return; }
      if (a >= 0x7400 && a < 0x7800) { this.vram[a - 0x7400] = v; return; }
      if (a >= 0x7800 && a < 0x7810) { this.dmaWrite(a & 0xf, v); return; }
      if (a === 0x7c00) { this.soundLatch = ~v & 0x0f; return; } // the latch's inverted outputs feed the 8035
      if (a >= 0x7d00 && a < 0x7d08) {
        const bit = 1 << (a & 7), old = this.latch259;
        this.latch259 = (v & 1) ? old | bit : old & ~bit;
        if ((this.latch259 & ~old) & 7) this.startEffect(this.latch259 & ~old & 7);
        return;
      }
      if (a >= 0x7d80 && a < 0x7d88) {
        const bit = 1 << (a & 7), old = this.misc;
        this.misc = (v & 1) ? old | bit : old & ~bit;
        if (bit === 1) this.sndIrq = !!(v & 1);
        if (bit === 0x20 && (v & 1) && !(old & 0x20)) this.runDma();
        return;
      }
    }

    // The 8257 DMA controller: two writes (low, high) per address/count register,
    // then a request copies channel 0's memory to channel 1's.
    dmaWrite(r, v) {
      if (r === 8) { this.dmaFlip = 0; this.dmaMode = v; return; }
      if (r > 7) return;
      const cur = this.dma[r];
      this.dma[r] = this.dmaFlip ? (cur & 0xff) | (v << 8) : (cur & 0xff00) | v;
      this.dmaFlip ^= 1;
    }
    runDma() {
      const src = this.dma[0], dst = this.dma[2], n = (this.dma[1] & 0x3fff) + 1;
      for (let i = 0; i < n; i++) this.write((dst + i) & 0xffff, this.read((src + i) & 0xffff));
    }

    // ---- picture ----
    drawFrame() {
      const raster = this.raster, vram = this.vram, bank = (this.misc >> 6) & 3;
      // tiles
      for (let row = 2; row < 30; row++) for (let col = 0; col < 32; col++) {
        const code = vram[row * 32 + col];
        const colour = ((this.tileColour[col + 32 * (row >> 2)] & 0x0f) + 16 * bank) * 4;
        const g = code * 64;
        for (let y = 0; y < 8; y++) {
          const o = (row * 8 + y) * W + col * 8, t = g + y * 8;
          for (let x = 0; x < 8; x++) { const p = this.tiles[t + x]; raster[o + x] = p ? colour + p : 0; }
        }
      }
      // sprites (later ones in the list cover earlier ones)
      const sr = this.spriteRam, base = (this.misc & 0x08) ? 0x200 : 0;
      for (let o = base; o < base + 0x180; o += 4) {
        const yb = sr[o];
        if (!yb) continue;
        const code = sr[o + 1] & 0x7f, flipY = sr[o + 1] & 0x80, flipX = sr[o + 2] & 0x80;
        const colour = ((sr[o + 2] & 0x0f) + 16 * bank) * 4;
        const sx = sr[o + 3] - 8, sy = 247 - yb, g = code * 256;
        for (let y = 0; y < 16; y++) {
          const ry = sy + y;
          if (ry < TOP || ry >= TOP + H) continue;
          const srcY = flipY ? 15 - y : y;
          for (let x = 0; x < 16; x++) {
            const rx = (sx + x) & 0xff; // sprites wrap around sideways
            const p = this.sprites[g + srcY * 16 + (flipX ? 15 - x : x)];
            if (p) raster[ry * W + rx] = colour + p;
          }
        }
      }
      // to the screen; the game turns the picture over for the second player of a cocktail table
      const pal = this.palette, scr = this.screen, flip = !(this.misc & 0x04);
      for (let y = 0; y < H; y++) {
        const src = (y + TOP) * W;
        if (!flip) for (let x = 0; x < W; x++) { const c = raster[src + x]; scr[y * W + x] = c ? pal[c] : BLACK; }
        else for (let x = 0; x < W; x++) { const c = raster[src + x]; scr[(H - 1 - y) * W + (W - 1 - x)] = c ? pal[c] : BLACK; }
      }
    }

    // ---- sound ----
    startEffect(bits) {
      const F = this.fx;
      for (let k = 0; k < 3; k++) if (bits & (1 << k)) { F[k * 4] = 0; F[k * 4 + 1] = 0; F[k * 4 + 3] = 1; }
    }
    nextNoise() {
      const n = this.noise, bit = (n ^ (n >> 2)) & 1;
      this.noise = (n >> 1) | (bit << 15);
      return (n & 1) ? 1 : -1;
    }
    // walking, jumping and the stomp: RetroPixels' own versions of the analogue circuits
    effects(dt) {
      const F = this.fx;
      let s = 0;
      if (F[3]) { // walk: a short low "boop"
        const t = F[0] += dt;
        if (t > 0.07) F[3] = 0;
        else {
          F[1] = (F[1] + (260 - 1600 * t) * dt) % 1;
          s += (F[1] < 0.5 ? 1 : -1) * 0.16 * Math.exp(-t * 35);
        }
      }
      if (F[7]) { // jump: a quick rising then falling chirp
        const t = F[4] += dt;
        if (t > 0.3) F[7] = 0;
        else {
          const hz = t < 0.06 ? 500 + 9000 * t : 1040 - 1800 * (t - 0.06);
          F[5] = (F[5] + hz * dt) % 1;
          s += Math.sin(F[5] * Math.PI * 2) * 0.14 * Math.exp(-t * 6);
        }
      }
      if (F[11]) { // stomp: a deep thud
        const t = F[8] += dt;
        if (t > 0.5) F[11] = 0;
        else {
          F[10] += (this.nextNoise() - F[10]) * 0.03;
          F[9] = (F[9] + (70 - 60 * t) * dt) % 1;
          s += (Math.sin(F[9] * Math.PI * 2) * 0.3 + F[10] * 0.35) * Math.exp(-t * 9);
        }
      }
      return s;
    }

    // Runs the sound processor up to the main processor's time, making output samples as it goes.
    runSound(untilCyc) {
      const target = untilCyc * SND_HZ / CPU_HZ;
      const perSample = SND_HZ / this.sampleRate, dt = 1 / this.sampleRate;
      while (this.sndCyc < target) {
        const c = this.snd.step();
        this.sndCyc += c;
        // the DAC's volume: full while P2 bit 7 is high, fading away while it's low
        const tau = (this.sndP2 & 0x80) ? 0.002 : 0.12;
        this.env += ((this.sndP2 & 0x80 ? 1 : 0) - this.env) * (1 - Math.exp(-c / SND_HZ / tau));
        this.sndAcc += (this.dac / 255) * this.env * c; this.sndAccN += c;
        this.samplePos += c;
        if (this.samplePos >= perSample) {
          this.samplePos -= perSample;
          let v = this.sndAcc / this.sndAccN * 0.6 + this.effects(dt);
          this.sndAcc = 0; this.sndAccN = 0;
          this.hp = 0.998 * (this.hp + v - this.hpPrev); this.hpPrev = v; // remove the DC level
          this.lp += (this.hp - this.lp) * 0.5;
          if (this.audioN >= this.audioOut.length) { const b = new Float32Array(this.audioOut.length * 2); b.set(this.audioOut); this.audioOut = b; }
          this.audioOut[this.audioN++] = Math.max(-1, Math.min(1, this.lp));
        }
      }
    }

    runFrame() {
      const cpu = this.cpu;
      let done = 0;
      for (let line = 0; line < LINES; line++) {
        if (line === TOP + H) {
          // vertical blank: the picture is finished and the game gets its NMI
          this.drawFrame();
          if (this.misc & 0x10) cpu.nmi();
        }
        const end = (line + 1) * LINE_CYC;
        while (this.cyc < end) this.cyc += cpu.step();
        this.runSound(this.cyc);
        done = end;
      }
      this.cyc -= done;
      this.sndCyc -= done * SND_HZ / CPU_HZ;
    }

    takeAudio() { const a = this.audioOut.slice(0, this.audioN); this.audioN = 0; return a; }

    saveState() {
      return { v: 1, cpu: this.cpu.getState(), snd: this.snd.getState(), ram: this.ram.slice(), spriteRam: this.spriteRam.slice(),
        vram: this.vram.slice(), dma: this.dma.slice(), fx: this.fx.slice(),
        m: [this.latch259, this.misc, this.soundLatch, this.sndIrq ? 1 : 0, this.sndP2, this.dac, this.dmaFlip, this.dmaMode,
          this.cyc, this.sndCyc, this.sndAcc, this.sndAccN, this.samplePos, this.env, this.noise, this.hp, this.hpPrev, this.lp] };
    }
    loadState(s) {
      this.cpu.setState(s.cpu); this.snd.setState(s.snd);
      this.ram.set(s.ram); this.spriteRam.set(s.spriteRam); this.vram.set(s.vram); this.dma.set(s.dma); this.fx.set(s.fx);
      let irq;
      [this.latch259, this.misc, this.soundLatch, irq, this.sndP2, this.dac, this.dmaFlip, this.dmaMode, this.cyc, this.sndCyc,
        this.sndAcc, this.sndAccN, this.samplePos, this.env, this.noise, this.hp, this.hpPrev, this.lp] = s.m;
      this.sndIrq = !!irq;
      this.audioN = 0;
      this.drawFrame();
    }
  }

  DonkeyKongBoard.id = "dkong";
  DonkeyKongBoard.title = "Nintendo Donkey Kong";
  DonkeyKongBoard.width = W;
  DonkeyKongBoard.height = H;
  DonkeyKongBoard.rotate = 90;
  DonkeyKongBoard.fps = FPS;
  DonkeyKongBoard.buttons = ["Up", "Down", "Left", "Right", "Jump", "Coin", "Start (1P)", "Start (2P)"];
  DonkeyKongBoard.dips = [
    { id: "lives", label: "Lives", options: [{ label: "3", value: 0 }, { label: "4", value: 1 }, { label: "5", value: 2 }, { label: "6", value: 3 }], default: 0 },
    { id: "bonus", label: "Extra life at", options: [{ label: "7,000", value: 0 }, { label: "10,000", value: 1 }, { label: "15,000", value: 2 }, { label: "20,000", value: 3 }], default: 0 },
  ];

  const SHARED = [
    { name: "s_3i_b.bin", size: 0x800, role: "sound", offset: 0x0000 },
    { name: "s_3j_b.bin", size: 0x800, role: "sound", offset: 0x1000 },
    { name: "v_5h_b.bin", size: 0x800, role: "tiles", offset: 0x0000 },
    { name: "v_3pt.bin", size: 0x800, role: "tiles", offset: 0x0800 },
    { name: "l_4m_b.bin", size: 0x800, role: "sprites", offset: 0x0000 },
    { name: "l_4n_b.bin", size: 0x800, role: "sprites", offset: 0x0800 },
    { name: "l_4r_b.bin", size: 0x800, role: "sprites", offset: 0x1000 },
    { name: "l_4s_b.bin", size: 0x800, role: "sprites", offset: 0x1800 },
    { name: "c-2k.bpr", size: 0x100, role: "prom", offset: 0x000 },
    { name: "c-2j.bpr", size: 0x100, role: "prom", offset: 0x100 },
    { name: "v-5e.bpr", size: 0x100, role: "prom", offset: 0x200 },
  ];
  DonkeyKongBoard.games = [
    { id: "dkong", title: "Donkey Kong", year: 1981, maker: "Nintendo",
      files: [
        { name: "c_5et_g.bin", size: 0x1000, role: "cpu", offset: 0x0000 },
        { name: "c_5ct_g.bin", size: 0x1000, role: "cpu", offset: 0x1000 },
        { name: "c_5bt_g.bin", size: 0x1000, role: "cpu", offset: 0x2000 },
        { name: "c_5at_g.bin", size: 0x1000, role: "cpu", offset: 0x3000 },
        ...SHARED,
      ] },
  ];

  RPArcade.boards.push(DonkeyKongBoard);
})(typeof window !== "undefined" ? window : globalThis);
