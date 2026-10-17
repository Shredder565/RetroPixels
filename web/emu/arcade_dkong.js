// RetroPixels — Donkey Kong arcade board (Nintendo, 1981) and Donkey Kong Jr. (1982).
// Original code, written from the documented behaviour of the board and from
// measurements of how it sounds and looks:
//   - a Z80 at 3.072 MHz (RPZ80) with the program ROMs, work RAM, a 32x32 tile map
//     and a sprite list that an Intel 8257 DMA chip copies into the video
//     hardware's own sprite memory once a frame;
//   - 8x8 tiles and 16x16 sprites with 2 bits per pixel, coloured through the
//     set's three PROMs (two make the 256-colour palette, one picks the colour of
//     each group of tiles), with 4 palette banks the game switches between;
//   - an Intel 8035 sound processor (RPMCS48) running the set's own sound program
//     through an 8-bit DAC whose volume fades out under program control, and the
//     board's filter after it;
//   - the board's analogue sound effects (Donkey Kong: walking, jumping, the
//     stomp; Junior: walking/climbing, jumping, falling), made by RetroPixels' own
//     code to sound like the circuits.
// The monitor is on its side: the board draws 256x224 and the picture is turned
// a quarter turn clockwise. Junior has 24 KB of program (its three ROMs hold
// their 2 KB parts out of order), twice the tiles, a 4 KB sound program and more
// sound effects played by the 8035. cores/arcade/dkong.go is the same board in Go.
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
  const TAU = 2 * Math.PI;

  // where each 2 KB part of Junior's three program ROMs goes
  const JR_ROM = [0x0000, 0x0800, 0x3000, 0x3800, 0x2000, 0x4800, 0x1000, 0x5800, 0x4000, 0x2800, 0x5000, 0x1800];

  // The colour levels each PROM pattern gives on the monitor, measured: the
  // resistor ladders aren't linear, and each gun picks up a little of the other two.
  const LEV_R = [0, 37.1, 72, 105.2, 142.4, 171.8, 201.4, 231.3];
  const LEV_G = [0, 38.3, 75.2, 107.9, 148.1, 177.5, 210.2, 239.6];
  const LEV_B = [0, 138.2, 217.9, 255];
  function colour(r, g, b) {
    const lr = LEV_R[r], lg = LEV_G[g], lb = LEV_B[b];
    const c = (v) => Math.min(255, Math.floor(v + 0.5));
    return RPArcade.rgb(c(lr + 0.0709 * lg + 0.0157 * lb), c(lg + 0.0366 * lr + 0.0151 * lb), c(lb + 0.0387 * lr + 0.0724 * lg));
  }

  // One filter section, made from its analogue form by the bilinear transform.
  function biquad(b0, b1, b2, a1, a2) { return { b0, b1, b2, a1, a2, x1: 0, x2: 0, y1: 0, y2: 0 }; }
  function runBiquad(f, x) {
    const y = f.b0 * x + f.b1 * f.x1 + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
    f.x2 = f.x1; f.x1 = x; f.y2 = f.y1; f.y1 = y;
    return y;
  }
  // a first-order low-pass (or high-pass) at fc Hz
  function pole1(fc, rate, high) {
    const k = Math.tan(Math.PI * fc / rate), n = 1 / (1 + k);
    return high ? biquad(n, -n, 0, (k - 1) * n, 0) : biquad(k * n, k * n, 0, (k - 1) * n, 0);
  }
  // a second-order low-pass at fc Hz with the given Q
  function pole2(fc, q, rate) {
    const k = Math.tan(Math.PI * fc / rate), k2 = k * k, n = 1 / (1 + k / q + k2);
    return biquad(k2 * n, 2 * k2 * n, k2 * n, 2 * (k2 - 1) * n, (1 - k / q + k2) * n);
  }
  // The DAC's output stage, measured. Donkey Kong's is a band-pass with its peak
  // around 150 Hz that falls away steeply above 1 kHz; Junior's is flat from about
  // 40 Hz to 1.5 kHz.
  function dacFilter(rate, jr) {
    if (jr) return [pole1(34.4, rate, true), pole1(2090, rate, false), pole1(4469, rate, false), pole2(1941, 0.714, rate), biquad(0.4156, 0, 0, 0, 0)];
    return [pole1(88, rate, true), pole1(272, rate, false), pole1(831, rate, false), pole2(1837, 0.81, rate), biquad(-2.96, 0, 0, 0, 0)];
  }
  const frac = (x) => x - Math.floor(x);

  class DonkeyKongBoard {
    constructor(game, files, opts) {
      opts = opts || {};
      this.game = game;
      this.jr = game.id === "dkongjr";
      this.sampleRate = opts.sampleRate || 48000;
      this.dips = opts.dips || {};
      for (const d of (game.dips || DonkeyKongBoard.dips)) if (this.dips[d.id] === undefined) this.dips[d.id] = d.default;
      const region = (role, size) => RPArcade.region(game, files, role, size);
      let nTiles = 256;
      if (this.jr) {
        const parts = region("cpu", 0x6000);
        this.rom = new Uint8Array(0x6000);
        JR_ROM.forEach((at, i) => this.rom.set(parts.subarray(i * 0x800, i * 0x800 + 0x800), at));
        this.sndRom = region("sound", 0x1000); // program at 000-fff
        nTiles = 512;
      } else {
        this.rom = region("cpu", 0x4000);
        this.sndRom = region("sound", 0x1800); // program at 000-7ff, tune data at 1000-17ff
      }
      const tileRom = region("tiles", nTiles * 16), spriteRom = region("sprites", 0x2000);
      const proms = region("prom", 0x300);
      this.decodeGraphics(tileRom, spriteRom, nTiles);
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
        rom: (a) => (a < self.sndRom.length && (self.jr || a < 0x800) ? self.sndRom[a] : 0xff),
        readX: (a) => {
          // Donkey Kong: P2 bit 6 selects the tune latch; otherwise P2 bits 0-2 page
          // the tune data ROM. Junior: always the latch.
          if (self.jr || (self.sndP2 & 0x40)) return self.soundLatch & 0x0f;
          return self.sndRom[0x1000 + (self.sndP2 & 7) * 256 + (a & 0xff)];
        },
        writeX: () => {},
        outP: (p, v) => {
          if (p === 1) self.dac = v;
          else if (p === 2) self.sndP2 = v;
        },
        // port 2 reads back its own latch, except bit 5: sound latch output 3 (inverted),
        // and on Junior bit 4: output 6 (inverted) and bit 6: bit 3 of the sound command (inverted)
        inP: (p) => {
          if (p === 1) return self.snd.port(1);
          if (p !== 2) return 0xff;
          let v = (self.snd.port(2) & ~0x20) | (self.latch259 & 0x08 ? 0 : 0x20);
          if (self.jr) v = (v & ~0x50) | (self.latch259 & 0x40 ? 0 : 0x10) | (self.soundLatch & 0x08 ? 0 : 0x40);
          return v;
        },
        // the test pins read the main board's sound latch outputs 5 and 4 (inverted)
        t0: () => !(self.latch259 & 0x20),
        t1: () => !(self.latch259 & 0x10),
        intLine: () => self.sndIrq,
      }, { ram: 64 });
      this.reset();
    }

    // ---- graphics ----
    decodeGraphics(tileRom, spriteRom, nTiles) {
      // tiles: 8x8; low bit from the first ROM, high bit from the second
      this.tiles = new Uint8Array(nTiles * 64);
      const half = nTiles * 8;
      for (let n = 0; n < nTiles; n++) for (let y = 0; y < 8; y++) {
        const lo = tileRom[n * 8 + y], hi = tileRom[half + n * 8 + y];
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

    // Each colour: red 3 bits and green's top bit from the second PROM, green's low 2 bits
    // and blue's 2 bits from the first; the outputs are inverted. Colour 0 of each group
    // of 4 is the black background.
    decodePalette(proms) {
      this.palette = new Uint32Array(256).fill(BLACK);
      for (let i = 0; i < 256; i++) {
        if ((i & 3) === 0) continue;
        const lo = ~proms[i] & 0xf, hi = ~proms[0x100 + i] & 0xf;
        this.palette[i] = colour((hi >> 1) & 7, ((hi & 1) << 2) | ((lo >> 2) & 3), lo & 3);
      }
    }

    reset() {
      this.cpu.reset(); this.snd.reset();
      this.ram.fill(0); this.spriteRam.fill(0); this.vram.fill(0);
      this.latch259 = 0;          // 7d00-7d07: sound triggers
      this.misc = 0;              // 7d80-7d87: bit 0 sound irq, 2 flip, 3 sprite bank, 4 nmi enable, 5 dma, 6-7 palette bank
      this.soundLatch = 0x0f; this.gfxBank = 0;
      this.sndIrq = false; this.sndP2 = 0xff; this.dac = 0;
      this.dma = new Uint16Array(8); this.dmaFlip = 0; this.dmaMode = 0;
      this.cyc = 0; this.sndCyc = 0; this.sndAcc = 0; this.sndAccN = 0; this.samplePos = 0;
      this.env = 1;
      this.filt = dacFilter(this.sampleRate, this.jr);
      // the effects' times (-1 = off) and phases; see effects()
      this.fx = new Float64Array([-1, -1, 0, -1, 0, -1, 0, 125, 0, 0, 0, 0]);
      this.noise = 1;
      this.screen.fill(BLACK);
    }

    setInput(p1, p2) { this.p1 = p1 || {}; this.p2 = p2 || {}; }

    joy(i) {
      return (i.Right ? 0x01 : 0) | (i.Left ? 0x02 : 0) | (i.Up ? 0x04 : 0) | (i.Down ? 0x08 : 0) | (i.Jump ? 0x10 : 0);
    }

    // ---- main processor's memory ----
    read(a) {
      if (a < this.rom.length) return this.rom[a];
      if (a >= 0x6000 && a < 0x7000) return this.ram[a - 0x6000];
      if (a >= 0x7000 && a < 0x7400) return this.spriteRam[a - 0x7000];
      if (a >= 0x7400 && a < 0x7800) return this.vram[a - 0x7400];
      switch (a & 0xff80) {
        case 0x7c00: return this.joy(this.p1);
        case 0x7c80: return this.joy(this.p2);
        case 0x7d00: {
          const i = this.p1;
          return (i.Service ? 0x01 : 0) | (i["Start (1P)"] ? 0x04 : 0) | (i["Start (2P)"] ? 0x08 : 0) | (i.Coin ? 0x80 : 0) |
            ((this.snd.port(2) & 0x10) ? 0 : 0x40); // the sound processor's "busy" line
        }
        case 0x7d80: {
          const d = this.dips;
          return (d.lives & 3) | ((d.bonus & 3) << 2) | ((d.coins & 7) << 4) | (d.cabinet === 0 ? 0x80 : 0);
        }
        default: return 0xff;
      }
    }

    write(a, v) {
      if (a >= 0x6000 && a < 0x7000) { this.ram[a - 0x6000] = v; return; }
      if (a >= 0x7000 && a < 0x7400) { this.spriteRam[a - 0x7000] = v; return; }
      if (a >= 0x7400 && a < 0x7800) { this.vram[a - 0x7400] = v; return; }
      if (a >= 0x7800 && a < 0x7810) { this.dmaWrite(a & 0xf, v); return; }
      if (a === 0x7c00) { this.soundLatch = this.jr ? v & 0x0f : ~v & 0x0f; return; } // Donkey Kong's 8035 sees the latch inverted
      if (a === 0x7c80) { if (this.jr) this.gfxBank = v & 1; return; }
      if (a >= 0x7d00 && a < 0x7d08) {
        const bit = 1 << (a & 7), old = this.latch259;
        this.latch259 = (v & 1) ? old | bit : old & ~bit;
        this.trigger(old, this.latch259);
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
      const raster = this.raster, vram = this.vram, bank = (this.misc >> 6) & 3, tbank = this.gfxBank * 256;
      // tiles
      for (let row = 2; row < 30; row++) for (let col = 0; col < 32; col++) {
        const code = vram[row * 32 + col] + tbank;
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
    // Starts the effects whose latch outputs changed: on Donkey Kong a rising edge
    // starts walk, jump and stomp (and the walk's falling edge its second half); on
    // Junior a falling edge starts each, and output 7 picks the climbing sound
    // instead of the walking one.
    trigger(old, now) {
      const F = this.fx, up = now & ~old, down = old & ~now;
      if (!this.jr) {
        if (up & 1) { F[0] = 0; F[1] = -1; }
        if ((down & 1) && F[0] >= 0) F[1] = 0;
        if (up & 2) F[3] = 0;
        if (up & 4) F[5] = 0;
        return;
      }
      if (down & 1) { F[0] = 0; F[1] = 0; F[2] = (now >> 7) & 1; }
      if (down & 2) { F[3] = 0; F[4] = 0; }
      if (down & 4) { F[5] = 0; F[6] = 0; F[8] = 0; }
    }
    // the next value of a 16-bit noise register, 0 or 1
    rand() {
      const n = this.noise, bit = (n ^ (n >> 2) ^ (n >> 3) ^ (n >> 5)) & 1;
      this.noise = (n >> 1) | (bit << 15);
      return n & 1;
    }
    // One sample of the analogue sound effects. fx holds, for each effect, its
    // time since it started (-1 = off) and its phases.
    effects(dt) {
      if (this.jr) return this.jrEffects(dt);
      const F = this.fx;
      let s = 0;
      // walk: a low "boop" that falls in pitch, wobbling, and a quieter higher one
      // when the trigger lets go
      if (F[0] >= 0) {
        const t = F[0];
        let f, a;
        const r = F[1];
        if (r < 0) {
          const rise = Math.min(1, Math.max(0, (t - 0.035) / 0.01));
          f = 272 + 290 * Math.exp(-t / 0.005) + 90 * rise * rise;
          a = 0.26 * Math.exp(-Math.max(0, t - 0.004) / 0.024);
        } else {
          f = 585 * (1 + 0.19 * Math.sin(TAU * (t - 0.06) / 0.08)); a = 0.035;
          if (r > 0.03) a *= Math.exp(-(r - 0.03) / 0.023);
          F[1] = r + dt;
          if (r > 0.3) F[0] = -1;
        }
        F[2] = frac(F[2] + f * dt);
        s += a * Math.sin(TAU * F[2]);
        F[0] = t + dt;
      }
      // jump: a tone wobbling between about 330 and 475 Hz for half a second
      if (F[3] >= 0) {
        const t = F[3];
        const u = frac((t - 0.068) / 0.112);
        const tri = u >= 0.5 ? 2 - 2 * u : 2 * u;
        let a = 0.09 * (1 - Math.exp(-t / 0.008));
        if (t > 0.27) a *= Math.exp(-(t - 0.27) / 0.12);
        F[4] = frac(F[4] + (333 + 142 * tri) * dt);
        s += a * Math.sin(TAU * F[4]);
        F[3] = t + dt;
        if (t > 0.51) F[3] = -1;
      }
      // stomp: a deep rumble around 125 Hz whose pitch jumps about every cycle
      if (F[5] >= 0) {
        const t = F[5];
        let a = 0.4 * Math.exp(-1.95 * t - 3.17 * t * t) * (1 - Math.exp(-t / 0.008));
        if (t > 0.62) a *= Math.exp(-(t - 0.62) / 0.03);
        let ph = F[6] + F[7] * dt;
        if (ph >= 1) {
          ph--;
          F[7] = 125 * (0.8 + 0.4 * (this.rand() * 0.5 + this.rand() * 0.25 + this.rand() * 0.125 + this.rand() * 0.125));
        }
        F[6] = ph;
        s += a * Math.sin(TAU * ph);
        F[5] = t + dt;
        if (t > 0.7) F[5] = -1;
      }
      return s;
    }
    // Junior's walk/climb, jump and fall
    jrEffects(dt) {
      const F = this.fx;
      let s = 0;
      // walk (a short buzz) or climb (a short falling tone): 55 ms
      if (F[0] >= 0) {
        const t = F[0];
        const a = t > 0.055 ? Math.exp(-(t - 0.055) / 0.004) : 1;
        if (F[2] !== 0) {
          const f = 320 * Math.exp(-t / 0.064);
          F[1] = frac(F[1] + f * dt);
          s += 0.12 * a * (Math.sin(TAU * F[1]) + 0.25 * Math.sin(2 * TAU * F[1]));
        } else {
          F[1] += (this.rand() * 2 - 1 - F[1]) * 0.3;
          s += 0.09 * a * F[1];
        }
        F[0] = t + dt;
        if (t > 0.08) F[0] = -1;
      }
      // jump: a wobbling tone around 600 Hz that dies away, then a quiet tail
      if (F[3] >= 0) {
        const t = F[3];
        let a = 0.24 * Math.exp(-t / 0.155);
        if (t > 0.265) a = 0.028 * Math.exp(-Math.max(0, t - 0.5) / 0.15);
        const f = 600 * (1 + 0.1 * Math.sin(TAU * t / 0.045));
        F[4] = frac(F[4] + f * dt);
        s += a * (Math.sin(TAU * F[4]) + 0.2 * Math.sin(3 * TAU * F[4]));
        F[3] = t + dt;
        if (t > 1.3) F[3] = -1;
      }
      // fall: a low noisy crash
      if (F[5] >= 0) {
        const t = F[5];
        let a = 0.45 * Math.exp(-t / 0.1);
        if (t > 0.22) a += 0.1 * Math.exp(-(t - 0.22) / 0.12);
        F[6] += (this.rand() * 2 - 1 - F[6]) * 0.045;
        F[8] += (F[6] - F[8]) * 0.045;
        s += a * F[8] * 8;
        F[5] = t + dt;
        if (t > 0.8) F[5] = -1;
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
        let on = 0, tau = 0.1;
        if (this.sndP2 & 0x80) { on = 1; tau = 0.002; }
        this.env += (on - this.env) * (1 - Math.exp(-c / SND_HZ / tau));
        this.sndAcc += this.dac / 255 * this.env * c; this.sndAccN += c;
        this.samplePos += c;
        if (this.samplePos >= perSample) {
          this.samplePos -= perSample;
          let v = this.sndAcc / this.sndAccN;
          this.sndAcc = 0; this.sndAccN = 0;
          for (const f of this.filt) v = runBiquad(f, v);
          v += this.effects(dt);
          if (this.audioN >= this.audioOut.length) { const b = new Float32Array(this.audioOut.length * 2); b.set(this.audioOut); this.audioOut = b; }
          this.audioOut[this.audioN++] = Math.max(-1, Math.min(1, v));
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
      return { v: 2, cpu: this.cpu.getState(), snd: this.snd.getState(), ram: this.ram.slice(), spriteRam: this.spriteRam.slice(),
        vram: this.vram.slice(), dma: this.dma.slice(), fx: this.fx.slice(),
        m: [this.latch259, this.misc, this.soundLatch, this.sndIrq ? 1 : 0, this.sndP2, this.dac, this.dmaFlip, this.dmaMode,
          this.cyc, this.sndCyc, this.sndAcc, this.sndAccN, this.samplePos, this.env, this.noise, this.gfxBank],
        filt: this.filt.map((f) => [f.x1, f.x2, f.y1, f.y2]) };
    }
    loadState(s) {
      if (s.v !== 2) throw new Error("This save state is from an older version of the Donkey Kong board.");
      this.cpu.setState(s.cpu); this.snd.setState(s.snd);
      this.ram.set(s.ram); this.spriteRam.set(s.spriteRam); this.vram.set(s.vram); this.dma.set(s.dma); this.fx.set(s.fx);
      let irq;
      [this.latch259, this.misc, this.soundLatch, irq, this.sndP2, this.dac, this.dmaFlip, this.dmaMode, this.cyc, this.sndCyc,
        this.sndAcc, this.sndAccN, this.samplePos, this.env, this.noise, this.gfxBank] = s.m;
      this.sndIrq = !!irq;
      s.filt.forEach((v, i) => { const f = this.filt[i]; [f.x1, f.x2, f.y1, f.y2] = v; });
      this.audioN = 0;
      this.drawFrame();
    }
  }

  const COINS = [
    { label: "1 coin, 1 game", value: 0 }, { label: "2 coins, 1 game", value: 1 }, { label: "1 coin, 2 games", value: 2 },
    { label: "3 coins, 1 game", value: 3 }, { label: "1 coin, 3 games", value: 4 }, { label: "4 coins, 1 game", value: 5 },
    { label: "1 coin, 4 games", value: 6 }, { label: "5 coins, 1 game", value: 7 },
  ];
  const LIVES = { id: "lives", label: "Lives", options: [{ label: "3", value: 0 }, { label: "4", value: 1 }, { label: "5", value: 2 }, { label: "6", value: 3 }], default: 0 };
  const CABINET = { id: "cabinet", label: "Cabinet", options: [{ label: "Upright", value: 0 }, { label: "Cocktail table", value: 1 }], default: 0 };

  DonkeyKongBoard.id = "dkong";
  DonkeyKongBoard.title = "Nintendo Donkey Kong";
  DonkeyKongBoard.width = W;
  DonkeyKongBoard.height = H;
  DonkeyKongBoard.rotate = 90;
  DonkeyKongBoard.fps = FPS;
  DonkeyKongBoard.buttons = ["Up", "Down", "Left", "Right", "Jump", "Coin", "Start (1P)", "Start (2P)", "Service"];
  DonkeyKongBoard.dips = [
    LIVES,
    { id: "bonus", label: "Extra life at", options: [{ label: "7,000", value: 0 }, { label: "10,000", value: 1 }, { label: "15,000", value: 2 }, { label: "20,000", value: 3 }], default: 0 },
    { id: "coins", label: "Coins", options: COINS, default: 0 },
    CABINET,
  ];
  const JR_DIPS = [
    LIVES,
    { id: "bonus", label: "Extra life at", options: [{ label: "10,000", value: 0 }, { label: "15,000", value: 1 }, { label: "20,000", value: 2 }, { label: "25,000", value: 3 }], default: 0 },
    { id: "coins", label: "Coins", options: COINS, default: 0 },
    CABINET,
  ];

  DonkeyKongBoard.games = [
    { id: "dkong", title: "Donkey Kong", year: 1981, maker: "Nintendo",
      files: [
        { name: "c_5et_g.bin", size: 0x1000, role: "cpu", offset: 0x0000 },
        { name: "c_5ct_g.bin", size: 0x1000, role: "cpu", offset: 0x1000 },
        { name: "c_5bt_g.bin", size: 0x1000, role: "cpu", offset: 0x2000 },
        { name: "c_5at_g.bin", size: 0x1000, role: "cpu", offset: 0x3000 },
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
      ] },
    { id: "dkongjr", title: "Donkey Kong Junior", year: 1982, maker: "Nintendo", dips: JR_DIPS,
      files: [
        { name: "dkj.5b", alt: ["djr1-c_5b_f-2.5b"], size: 0x2000, role: "cpu", offset: 0x0000 },
        { name: "dkj.5c", alt: ["djr1-c_5c_f-2.5c"], size: 0x2000, role: "cpu", offset: 0x2000 },
        { name: "dkj.5e", alt: ["djr1-c_5e_f-2.5e"], size: 0x2000, role: "cpu", offset: 0x4000 },
        { name: "c_3h.bin", alt: ["djr1-c_3h.3h"], size: 0x1000, role: "sound", offset: 0x0000 },
        { name: "dkj.3n", alt: ["djr1-v.3n"], size: 0x1000, role: "tiles", offset: 0x0000 },
        { name: "dkj.3p", alt: ["djr1-v.3p"], size: 0x1000, role: "tiles", offset: 0x1000 },
        { name: "v_7c.bin", alt: ["djr1-v_7c.7c"], size: 0x800, role: "sprites", offset: 0x0000 },
        { name: "v_7d.bin", alt: ["djr1-v_7d.7d"], size: 0x800, role: "sprites", offset: 0x0800 },
        { name: "v_7e.bin", alt: ["djr1-v_7e.7e"], size: 0x800, role: "sprites", offset: 0x1000 },
        { name: "v_7f.bin", alt: ["djr1-v_7f.7f"], size: 0x800, role: "sprites", offset: 0x1800 },
        { name: "c-2e.bpr", alt: ["djr1-c-2e.2e"], size: 0x100, role: "prom", offset: 0x000 },
        { name: "c-2f.bpr", alt: ["djr1-c-2f.2f"], size: 0x100, role: "prom", offset: 0x100 },
        { name: "v-2n.bpr", alt: ["djr1-v-2n.2n"], size: 0x100, role: "prom", offset: 0x200 },
      ] },
  ];

  RPArcade.boards.push(DonkeyKongBoard);
})(typeof window !== "undefined" ? window : globalThis);
