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
// a shot "pew"). RetroPixels makes its own models of these circuits from the
// same control registers, their rates, shapes and levels measured from how the
// board sounds. Written from the documented hardware behaviour.
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 3072000;
  const LINE_CYC = 192;       // 384 pixel clocks per line at 6.144 MHz, the CPU at half that
  const LINES = 264;
  const FIRST = 16, LAST = 240; // lines 16-239 are shown
  const W = 256, H = LAST - FIRST;

  // ===================================================================
  // The starfield: a 17-bit shift register stepping twice a pixel, 512 steps a
  // line, without stopping from frame to frame (so the field drifts one half
  // pixel a frame). A star shows where its output has been 0 for 8 steps and
  // then 1; the 6 bits before those give the star's colour. Stars on every
  // other 8-pixel stretch show, alternating line by line. (Worked out from how
  // the board's stars look.)
  // ===================================================================
  const STAR_PERIOD = 131071, STAR_T0 = 79139; // where the register is at the first line of the first frame
  let starAt = null; // each step's star colour (0 = none)
  function makeStars() {
    if (starAt) return starAt;
    const bits = new Uint8Array(STAR_PERIOD);
    let r = 1;
    for (let t = 0; t < STAR_PERIOD; t++) { bits[t] = r & 1; r = (r >> 1) | (((r ^ (r >> 12)) & 1) << 16); }
    starAt = new Uint8Array(STAR_PERIOD);
    for (let t = 0; t < STAR_PERIOD; t++) {
      let ok = bits[(t + 8) % STAR_PERIOD] === 1;
      for (let j = 0; j < 8 && ok; j++) ok = bits[(t + j) % STAR_PERIOD] === 0;
      if (!ok) continue;
      let c = 0;
      for (let k = 0; k < 6; k++) c |= bits[(t - 1 - k + STAR_PERIOD) % STAR_PERIOD] << k;
      starAt[t] = c;
    }
    return starAt;
  }
  const STAR_LEVELS = [0x00, 0xc2, 0xd6, 0xff];

  // ===================================================================
  // Sound: RetroPixels' own models of the board's sound circuits, with their
  // rates, shapes and levels measured from how they sound.
  // ===================================================================
  // The tune: the pitch register sets the rate of a 16-step counter, and the
  // two volume bits mix its outputs into one of four waveforms.
  const TONE_TABLE = [
    [8.8, 41.3, 8.0, 40.5, -41.7, -8.4, -40.9, -7.6, 8.8, 41.3, 8.0, 40.5, -41.7, -8.4, -40.9, -7.6],
    [-125.1, -81.1, -121.7, -77.8, 82.9, 123.4, 79.4, 120.1, -125.1, -81.1, -121.7, -77.8, 82.9, 123.4, 79.4, 120.1],
    [-4.9, 41.3, -5.5, 40.6, 63.0, 108.1, 60.2, 105.3, -113.0, -65.0, -110.0, -62.1, -38.1, 8.6, -37.6, 9.1],
    [-147.8, -109.8, -143.6, -105.6, 31.2, 66.3, 29.6, 64.7, -64.4, -27.8, -62.9, -26.2, 109.3, 143.0, 105.1, 138.9],
  ];
  const TONE_CLOCK = 1536000; // counter steps a second times (256 - pitch)
  // The three background voices: oscillators whose low part lasts a fixed time
  // and whose high part a slow sweep (shared by the three, its speed set by
  // 6004-6007) stretches, so each falls in pitch to about half and snaps back.
  const FS_TOP = [238.6, 334.2, 483.6];       // Hz at the sweep's start
  const FS_DEPTH = [0.482, 0.487, 0.518];     // how far it falls
  const FS_LOW = [3.22e-3, 2.27e-3, 1.515e-3]; // s
  const LFO_BITS = [34.8, 73.5, 160, 351.5];
  const FS_LEVEL = 0.138, FS_SNAP = 0.018, FS_HOLD = 0.008;
  const HIT_CLK = 9000;
  const FIRE_CLK = 7920;  // the shot's pulse clock
  const FIRE_SQ = 7700;   // its unsteady square's mean edge rate
  const FIRE_ON = 0.036;  // s the shot takes to start
  const FIRE_OFF = 0.078; // s it keeps going after 6805 goes off
  // the sound state kept in save states, in order
  const SND_FLOATS = ["tone", "lfo", "fsPh0", "fsPh1", "fsPh2", "fsLP", "fsHP", "fsHPin", "hitEnv", "hitPh", "hitBit",
    "bp0", "bp1", "bp2", "bp3", "fireT", "fireSince", "fireOn", "fireLvl", "firePh", "fireSqPh", "fireBit", "firePulse",
    "fireX", "fireY", "due"];
  function newSound() {
    const s = { noise: 1, fnoise: 0x1234 };
    for (const k of SND_FLOATS) s[k] = 0;
    s.fireT = 10;
    return s;
  }
  // a 17-bit noise register: steps it and returns its new low bit
  function stepNoise(s, k) {
    const n = s[k], bit = (n ^ (n >> 5)) & 1;
    s[k] = (n >> 1) | (bit << 16);
    return s[k] & 1;
  }
  // how much of [a, b) lies in [0, h)
  function overlap(a, b, h) { return Math.max(0, Math.min(b, h) - Math.max(a, 0)); }

  class GalaxianBoard {
    constructor(game, files, opts) {
      this.game = game;
      this.dips = opts.dips;
      this.sampleRate = opts.sampleRate || 48000;
      this.rom = RPArcade.region(game, files, "cpu", 0x4000);
      const gfx = RPArcade.region(game, files, "gfx", 0x1000);
      this.prom = RPArcade.region(game, files, "colour", 32);
      this.decodeGraphics(gfx);
      // 3 bits each of red and green and 2 of blue, through the board's resistors (the levels measured)
      this.pens = new Uint32Array(32);
      for (let i = 0; i < 32; i++) {
        const c = this.prom[i], bit = (n) => (c >> n) & 1;
        this.pens[i] = RPArcade.rgb(0x1d * bit(0) + 0x3e * bit(1) + 0x85 * bit(2),
          0x1d * bit(3) + 0x3e * bit(4) + 0x85 * bit(5), 0x45 * bit(6) + 0x94 * bit(7));
      }
      this.starAt = makeStars();
      this.starPens = new Uint32Array(64);
      for (let i = 0; i < 64; i++) this.starPens[i] = RPArcade.rgb(STAR_LEVELS[i & 3], STAR_LEVELS[(i >> 2) & 3], STAR_LEVELS[i >> 4]);

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
      this.snd = newSound();
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
        const line = y - FIRST, at = this.starAt;
        let t = (((STAR_T0 - this.frame + line * 512) % STAR_PERIOD) + STAR_PERIOD) % STAR_PERIOD;
        for (let h = 0; h < 512; h++) {
          const c = at[t];
          if (c && ((line ^ (h >> 4)) & 1) && !used[h >> 1]) scr[row + (h >> 1)] = this.starPens[c];
          if (++t === STAR_PERIOD) t = 0;
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

    // ---- sound ----
    makeSound(n) {
      const st = this.snd, l68 = this.latch6800, dt = 1 / this.sampleRate;
      // the tune
      const steps = this.pitch !== 0xff ? TONE_CLOCK / (256 - this.pitch) * dt : 0; // counter steps a sample
      const table = TONE_TABLE[l68[6] | (l68[7] << 1)];
      // the sweep's speed
      let sweep = 709.6;
      for (let i = 0; i < 4; i++) sweep -= LFO_BITS[i] * this.latch6000[4 + i];
      sweep /= 115;
      const lpK = 1 - Math.exp(-2 * Math.PI * 500 * dt), hpK = Math.exp(-dt / 0.013);
      // the hit's band-pass (165 Hz, Q 2.75)
      const k = Math.tan(Math.PI * 165 * dt), nrm = 1 / (1 + k / 2.75 + k * k);
      const b0 = k / 2.75 * nrm, a1 = 2 * (k * k - 1) * nrm, a2 = (1 - k / 2.75 + k * k) * nrm;
      const hitUp = 1 - Math.exp(-dt / 0.05), hitDown = Math.exp(-dt / 0.85);
      const fireHP = Math.exp(-2 * Math.PI * 8870 * dt), fireUp = 1 - Math.exp(-dt / 0.08);
      const fsPh = [st.fsPh0, st.fsPh1, st.fsPh2];
      for (let i = 0; i < n; i++) {
        let v = 0;
        // the tune: the table averaged over the sample
        if (steps > 0) {
          if (steps >= 16) st.tone = (st.tone + steps) % 16;
          else {
            let acc = 0, left = steps, p = st.tone;
            while (left > 0) {
              const idx = Math.floor(p), take = Math.min(left, idx + 1 - p);
              acc += table[idx & 15] * take;
              left -= take; p += take;
              if (p >= 16) p -= 16;
            }
            st.tone = p;
            v += acc / steps / 1000;
          }
        }
        // the background voices
        let u = st.lfo;
        if (u < 1) st.lfo += sweep * dt;
        else {
          u = 2 - u;
          st.lfo += dt / FS_SNAP;
          if (st.lfo >= 2) st.lfo -= 2;
        }
        let x = 0;
        const hold = st.lfo >= 1 && (st.lfo - 1) * FS_SNAP < FS_HOLD; // the voices stay low as the sweep snaps back
        for (let j = 0; j < 3; j++) {
          if (!l68[j] || hold) { fsPh[j] = 0; continue; }
          const T = 1 / (FS_TOP[j] * (1 - FS_DEPTH[j] * u)), h = Math.max(0, T - FS_LOW[j]);
          let p = fsPh[j], hi = overlap(p, p + dt, h);
          if ((p += dt) >= T) { p -= T; hi += overlap(0, p, h); }
          fsPh[j] = p;
          x += hi / dt;
        }
        st.fsLP += (x * FS_LEVEL - st.fsLP) * lpK;
        st.fsHP = hpK * (st.fsHP + st.fsLP - st.fsHPin);
        st.fsHPin = st.fsLP;
        v += st.fsHP;
        // the hit: noise through a band-pass, its level charging while 6803 is on
        if (l68[3]) st.hitEnv += (1 - st.hitEnv) * hitUp;
        else st.hitEnv *= hitDown;
        {
          let p = st.hitPh + HIT_CLK * dt, nx;
          if (p >= 1) {
            const frac = (p - Math.floor(p)) / (HIT_CLK * dt);
            nx = st.hitBit * (1 - frac);
            while (p >= 1) { p--; st.hitBit = stepNoise(st, "noise") * 2 - 1; }
            nx += st.hitBit * frac;
          } else nx = st.hitBit;
          st.hitPh = p;
          const y = b0 * nx - b0 * st.bp1 - a1 * st.bp2 - a2 * st.bp3;
          st.bp1 = st.bp0; st.bp0 = nx; st.bp3 = st.bp2; st.bp2 = y;
          v += y * (l68[3] ? 1 : st.hitEnv) * 1.4;
        }
        // the shot: a short while after 6805 goes on, an unsteady square, then
        // after a quarter second short pulses on a ~7.9 kHz clock; a while
        // after 6805 goes off, the square again, fading; all heard through a
        // steep high-pass
        {
          st.fireSince += dt;
          st.fireT += dt;
          if (l68[5]) st.fireLvl += (1 - st.fireLvl) * fireUp;
          const want = l68[5];
          if (want !== st.fireOn) {
            const delay = want ? FIRE_ON : FIRE_OFF - 0.075 * Math.max(0, 1 - st.fireT); // shorter after a short shot
            if (st.fireSince >= delay) { st.fireOn = want; st.fireT = 0; }
          }
          const pulses = st.fireOn !== 0 && st.fireT >= 0.25;
          let p = st.firePh + FIRE_CLK * dt;
          while (p >= 1) {
            p--;
            let r = 0;
            for (let q = 0; q < 7; q++) r = (r << 1) | stepNoise(st, "fnoise");
            if (pulses && r < 51) st.firePulse = 1.25 / 48000;
          }
          st.firePh = p;
          for (st.fireSqPh -= dt; st.fireSqPh <= 0;) {
            let r = 0;
            for (let q = 0; q < 7; q++) r = (r << 1) | stepNoise(st, "fnoise");
            st.fireBit = 1 - st.fireBit;
            st.fireSqPh += (0.3 + 1.4 * r / 128) / FIRE_SQ;
          }
          let fx = 0;
          if (pulses) {
            fx = Math.min(st.firePulse, dt) / dt;
            st.firePulse = Math.max(0, st.firePulse - dt);
          } else if (st.fireOn !== 0) fx = st.fireBit;
          else if (st.fireT < 3) fx = st.fireBit * st.fireLvl * Math.exp(-Math.max(0, st.fireT - 0.05) / 0.17);
          st.fireY = fireHP * (st.fireY + fx - st.fireX);
          st.fireX = fx;
          v += st.fireY;
        }
        if (this.audioN >= this.audio.length) { const a = new Float32Array(this.audio.length * 2); a.set(this.audio); this.audio = a; }
        this.audio[this.audioN++] = Math.max(-1, Math.min(1, v));
      }
      st.fsPh0 = fsPh[0]; st.fsPh1 = fsPh[1]; st.fsPh2 = fsPh[2];
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
        if (n === 5 && (v & 1) !== was) {
          this.snd.fireSince = 0;
          if (v & 1) this.snd.fireLvl = 0;
        }
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
      return { v: 2, cpu: this.cpu.getState(), ram: this.ram.slice(), vram: this.vram.slice(), obj: this.obj.slice(),
        l60: this.latch6000.slice(), l68: this.latch6800.slice(), l70: this.latch7000.slice(),
        m: [this.pitch, this.cycles, this.frame],
        snd: [s.noise, s.fnoise].concat(SND_FLOATS.map((k) => s[k])) };
    }
    loadState(o) {
      if (o.v !== 2) throw new Error("Galaxian: save state from another version");
      this.cpu.setState(o.cpu);
      this.ram.set(o.ram); this.vram.set(o.vram); this.obj.set(o.obj);
      this.latch6000.set(o.l60); this.latch6800.set(o.l68); this.latch7000.set(o.l70);
      [this.pitch, this.cycles, this.frame] = o.m;
      const s = this.snd, a = o.snd;
      s.noise = a[0]; s.fnoise = a[1];
      SND_FLOATS.forEach((k, i) => { s[k] = a[2 + i]; });
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
