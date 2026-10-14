// RetroPixels — Space Invaders arcade board (Taito, 1978; Midway in America).
// Original code, written from the documented behaviour of the board:
//   - an Intel 8080 at 1.9968 MHz (RP8080), 8 KB of program ROM and 8 KB of RAM,
//     7 KB of which is the picture (1 bit per pixel, 256 x 224, sideways monitor);
//   - the external 16-bit shift register that the game uses to slide sprites
//     sideways (write the amount on port 2, bytes on port 4, read the result on port 3);
//   - two interrupts a frame: RST 1 as the beam reaches the middle of the screen
//     and RST 2 as it reaches the bottom;
//   - the cabinet's coloured strips glued over the black-and-white monitor
//     (shown as a tint, can be turned off);
//   - the sound: on the real board each effect is its own analogue circuit,
//     switched on and off by bits of ports 3 and 5. RetroPixels makes its own
//     version of each effect in code (no recordings).
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 1996800;
  const FPS = 60;
  const LINES = 262;                  // scan lines per frame (224 shown)
  const FRAME_CYC = CPU_HZ / FPS;     // 33280
  const W = 256, H = 224;

  // Coloured strips, in the player's view (224 wide, 256 tall, top first):
  // [from row, to row, colour]; the bottom row is partly green (the spare cannons).
  const WHITE = 0xffffffff, RED = 0xff4040ff, GREEN = 0xff40ff40;
  function overlayColour(dx, dy) {
    if (dy >= 32 && dy < 64) return RED;
    if (dy >= 184 && dy < 240) return GREEN;
    if (dy >= 240 && dx >= 16 && dx < 134) return GREEN;
    return WHITE;
  }

  // Sound effects (each one is RetroPixels' own simple synthesis of what the circuit sounds like).
  // Voice slots in the state array: 0 UFO, 1 shot, 2 cannon hit, 3 invader hit, 4 extra cannon,
  // 5..8 the four march notes, 9 UFO hit.
  const NV = 10;
  const MARCH_HZ = [98, 87, 78, 69];

  class InvadersBoard {
    constructor(game, files, opts) {
      opts = opts || {};
      this.game = game;
      this.sampleRate = opts.sampleRate || 48000;
      // the framework changes values in this object when the player changes a setting
      this.dips = opts.dips || InvadersBoard.dipDefaults();
      for (const d of InvadersBoard.dips) if (this.dips[d.id] === undefined) this.dips[d.id] = d.default;
      // program ROM: 8 KB at 0000
      this.rom = new Uint8Array(0x2000);
      for (const fi of game.files) {
        const data = files.get(fi.name.toLowerCase());
        if (data && fi.role === "cpu") this.rom.set(data.subarray(0, fi.size), fi.offset || 0);
      }
      this.ram = new Uint8Array(0x2000);
      this.screen = new Uint32Array(W * H);
      this.width = W; this.height = H;
      this.p1 = {}; this.p2 = {};
      this.tint = new Uint32Array(W * H);
      this.makeTint();
      const self = this;
      this.cpu = new global.RP8080({
        read(a) {
          a &= 0x3fff; // the upper address lines aren't decoded: everything repeats every 16 KB
          return a < 0x2000 ? self.rom[a] : self.ram[a - 0x2000];
        },
        write(a, v) { a &= 0x3fff; if (a >= 0x2000) self.ram[a - 0x2000] = v; },
        in(p) { return self.portIn(p & 7); },
        out(p, v) { self.portOut(p & 7, v); },
      });
      this.audioBuf = new Float32Array(4096);
      this.audio = this.audioBuf.subarray(0, 0);
      this.voices = new Float64Array(NV * 4); // per voice: time since start, phase, phase2, on flag
      this.reset();
    }

    // colour of each pixel when lit (the strips), in the board's own (unrotated) layout
    makeTint() {
      this.tintOn = this.dips.overlay;
      for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        this.tint[y * W + x] = this.tintOn ? overlayColour(y, 255 - x) : WHITE;
      }
    }

    static dipDefaults() { const o = {}; for (const d of InvadersBoard.dips) o[d.id] = d.default; return o; }

    reset() {
      this.cpu.reset();
      this.ram.fill(0);
      this.shiftData = 0; this.shiftAmount = 0;
      this.port3 = 0; this.port5 = 0;
      this.pendingIrq = 0;
      this.voices.fill(0);
      this.noise = 1; this.lp = 0; this.hp = 0; this.hpPrev = 0; this.sampleFrac = 0;
      this.ufoLfo = 0;
      this.cyc = 0;
      this.screen.fill(0xff000000);
    }

    setInput(p1, p2) { this.p1 = p1 || {}; this.p2 = p2 || {}; }

    portIn(p) {
      const i = this.p1, j = this.p2, d = this.dips;
      switch (p) {
        case 0: return 0x0e;
        case 1:
          return (i.Coin ? 0x01 : 0) | (i["Start (2P)"] ? 0x02 : 0) | (i["Start (1P)"] ? 0x04 : 0) | 0x08 |
            (i.Fire ? 0x10 : 0) | (i.Left ? 0x20 : 0) | (i.Right ? 0x40 : 0);
        case 2:
          // the second player's controls (on an upright cabinet the players share the first set,
          // so either controller works for player 2)
          return (d.lives & 3) | (d.bonus ? 0x08 : 0) |
            (i.Fire || j.Fire ? 0x10 : 0) | (i.Left || j.Left ? 0x20 : 0) | (i.Right || j.Right ? 0x40 : 0) | (d.coinInfo ? 0 : 0x80);
        case 3: return (this.shiftData >> (8 - this.shiftAmount)) & 0xff;
        default: return 0;
      }
    }

    portOut(p, v) {
      switch (p) {
        case 2: this.shiftAmount = v & 7; break;
        case 4: this.shiftData = (this.shiftData >> 8) | (v << 8); break;
        case 3: this.soundBits(0, this.port3, v); this.port3 = v; break;
        case 5: this.soundBits(1, this.port5, v); this.port5 = v; break;
        default: break; // 6 is the watchdog
      }
    }

    // Starts effects on a 0 -> 1 change of their bit.
    soundBits(bank, old, v) {
      const V = this.voices, rise = v & ~old;
      const start = (n) => { V[n * 4] = 0; V[n * 4 + 1] = 0; V[n * 4 + 2] = 0; V[n * 4 + 3] = 1; };
      if (bank === 0) {
        if (rise & 0x02) start(1);
        if (rise & 0x04) start(2);
        if (rise & 0x08) start(3);
        if (rise & 0x10) start(4);
      } else {
        for (let k = 0; k < 4; k++) if (rise & (1 << k)) start(5 + k);
      }
    }

    nextNoise() {
      // 17-bit shift register noise
      const n = this.noise, bit = ((n >> 0) ^ (n >> 3)) & 1;
      this.noise = (n >> 1) | (bit << 16);
      return (n & 1) ? 1 : -1;
    }

    // One output sample of all the effects.
    mix() {
      const V = this.voices, dt = 1 / this.sampleRate, TAU = Math.PI * 2;
      if (!(this.port3 & 0x20)) { // the amplifier is switched off (attract mode on some settings)
        for (let n = 1; n < NV; n++) V[n * 4] += dt;
        return 0;
      }
      let s = 0;
      const nz = this.nextNoise();
      // UFO: a whining tone that sweeps up and down while bit 0 of port 3 is on
      this.ufoLfo = (this.ufoLfo + 6.5 * dt) % 1;
      if (this.port3 & 0x01) {
        const tri = this.ufoLfo < 0.5 ? this.ufoLfo * 4 - 1 : 3 - this.ufoLfo * 4;
        const hz = 560 + 220 * tri;
        V[1] = (V[1] + hz * dt) % 1;
        s += (V[1] < 0.5 ? 0.09 : -0.09) * 0.7 + Math.sin(V[1] * TAU) * 0.05;
      }
      // shot: a short falling "pew" with a hiss
      if (V[7]) {
        const t = V[4] += dt;
        if (t > 0.32) V[7] = 0;
        else {
          const hz = 1500 * Math.exp(-t * 7) + 200;
          V[5] = (V[5] + hz * dt) % 1;
          const env = Math.exp(-t * 9);
          s += env * ((V[5] < 0.5 ? 0.08 : -0.08) + nz * 0.05);
        }
      }
      // cannon hit: a long low rumble of noise
      if (V[11]) {
        const t = V[8] += dt;
        if (t > 1.2) V[11] = 0;
        else {
          V[9] += (nz - V[9]) * 0.06; // low-pass the noise
          V[10] = (V[10] + (55 + 40 * Math.exp(-t * 3)) * dt) % 1;
          const env = Math.exp(-t * 2.6);
          s += env * (V[9] * 0.55 + (V[10] < 0.5 ? 0.06 : -0.06));
        }
      }
      // invader hit: a quick burst that drops in pitch
      if (V[15]) {
        const t = V[12] += dt;
        if (t > 0.3) V[15] = 0;
        else {
          V[13] += (nz - V[13]) * 0.25;
          V[14] = (V[14] + (900 * Math.exp(-t * 12) + 120) * dt) % 1;
          const env = Math.exp(-t * 11);
          s += env * (V[13] * 0.22 + (V[14] < 0.5 ? 0.08 : -0.08));
        }
      }
      // extra cannon: a bright warbling chime
      if (V[19]) {
        const t = V[16] += dt;
        if (t > 0.9) V[19] = 0;
        else {
          V[17] = (V[17] + (1250 + 120 * Math.sin(t * TAU * 12)) * dt) % 1;
          s += Math.exp(-t * 3.5) * Math.sin(V[17] * TAU) * 0.12;
        }
      }
      // the four march notes: low thumps
      for (let k = 0; k < 4; k++) {
        const o = (5 + k) * 4;
        if (!V[o + 3]) continue;
        const t = V[o] += dt;
        if (t > 0.14) { V[o + 3] = 0; continue; }
        V[o + 1] = (V[o + 1] + MARCH_HZ[k] * dt) % 1;
        const ph = V[o + 1];
        const wave = ph < 0.3 ? 1 : ph < 0.5 ? 0.3 : -0.7;
        s += wave * Math.exp(-t * 26) * 0.3;
      }
      // UFO hit: a fast warble while bit 4 of port 5 is on
      if (this.port5 & 0x10) {
        const o = 36;
        V[o] += dt;
        const sweep = (V[o] * 9) % 1;
        V[o + 1] = (V[o + 1] + (1800 - 1400 * sweep) * dt) % 1;
        s += (V[o + 1] < 0.5 ? 0.08 : -0.08);
      } else V[36] = 0;
      return s;
    }

    // Makes the sound for one stretch of the frame.
    makeAudio(cycles) {
      this.sampleFrac += cycles * this.sampleRate / CPU_HZ;
      let n = Math.floor(this.sampleFrac);
      this.sampleFrac -= n;
      while (n-- > 0) {
        let s = this.mix();
        // soften the edges a little and remove any DC offset
        this.lp += (s - this.lp) * 0.45;
        this.hp = 0.995 * (this.hp + this.lp - this.hpPrev); this.hpPrev = this.lp;
        if (this.audioN >= this.audioBuf.length) { const b = new Float32Array(this.audioBuf.length * 2); b.set(this.audioBuf); this.audioBuf = b; }
        this.audioBuf[this.audioN++] = Math.max(-1, Math.min(1, this.hp));
      }
    }

    drawLine(y) {
      const ram = this.ram, scr = this.screen, tint = this.tint, base = 0x400 + y * 32, row = y * W;
      for (let bx = 0; bx < 32; bx++) {
        const v = ram[base + bx], o = row + bx * 8;
        for (let k = 0; k < 8; k++) scr[o + k] = (v >> k) & 1 ? tint[o + k] : 0xff000000;
      }
    }

    runFrame() {
      const cpu = this.cpu;
      if (this.tintOn !== this.dips.overlay) this.makeTint();
      this.audioN = 0;
      let done = 0;
      for (let line = 0; line < LINES; line++) {
        // interrupts: RST 1 when the beam is half way down, RST 2 when it reaches the bottom
        if (line === 96) this.pendingIrq = 0xcf;
        else if (line === 224) this.pendingIrq = 0xd7;
        const end = Math.round(FRAME_CYC * (line + 1) / LINES);
        const start = this.cyc;
        while (this.cyc < end) {
          if (this.pendingIrq) {
            const t = cpu.interrupt(this.pendingIrq);
            if (t) { this.cyc += t; this.pendingIrq = 0; continue; }
          }
          this.cyc += cpu.step();
        }
        if (line < H) this.drawLine(line);
        this.makeAudio(this.cyc - start);
        done = end;
      }
      this.cyc -= done;
      this.audio = this.audioBuf.subarray(0, this.audioN);
    }

    takeAudio() { const a = this.audio.slice(); this.audio = this.audioBuf.subarray(0, 0); return a; }

    saveState() {
      return { v: 1, cpu: this.cpu.getState(), ram: this.ram.slice(), voices: this.voices.slice(),
        m: [this.shiftData, this.shiftAmount, this.port3, this.port5, this.pendingIrq, this.cyc, this.noise,
          this.lp, this.hp, this.hpPrev, this.sampleFrac, this.ufoLfo] };
    }
    loadState(s) {
      this.cpu.setState(s.cpu); this.ram.set(s.ram); this.voices.set(s.voices);
      [this.shiftData, this.shiftAmount, this.port3, this.port5, this.pendingIrq, this.cyc, this.noise,
        this.lp, this.hp, this.hpPrev, this.sampleFrac, this.ufoLfo] = s.m;
      for (let y = 0; y < H; y++) this.drawLine(y);
    }
  }

  InvadersBoard.id = "invaders";
  InvadersBoard.title = "Space Invaders";
  InvadersBoard.width = W;
  InvadersBoard.height = H;
  InvadersBoard.rotate = 270;  // the monitor is mounted on its side
  InvadersBoard.fps = FPS;
  InvadersBoard.buttons = ["Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)"];
  InvadersBoard.dips = [
    { id: "lives", label: "Cannons", options: [{ label: "3", value: 0 }, { label: "4", value: 1 }, { label: "5", value: 2 }, { label: "6", value: 3 }], default: 0 },
    { id: "bonus", label: "Extra cannon at", options: [{ label: "1500 points", value: 0 }, { label: "1000 points", value: 1 }], default: 0 },
    { id: "coinInfo", label: "Coin info on the title screen", options: [{ label: "Shown", value: 1 }, { label: "Hidden", value: 0 }], default: 1 },
    { id: "overlay", label: "Coloured screen strips", options: [{ label: "On", value: 1 }, { label: "Off (black and white)", value: 0 }], default: 1 },
  ];
  // The ROM set: four 2 KB chips (the common naming).
  InvadersBoard.games = [
    { id: "invaders", title: "Space Invaders", year: 1978, maker: "Taito / Midway",
      files: [
        { name: "invaders.h", size: 0x800, role: "cpu", offset: 0x0000 },
        { name: "invaders.g", size: 0x800, role: "cpu", offset: 0x0800 },
        { name: "invaders.f", size: 0x800, role: "cpu", offset: 0x1000 },
        { name: "invaders.e", size: 0x800, role: "cpu", offset: 0x1800 },
      ] },
  ];

  RPArcade.boards.push(InvadersBoard);
})(typeof window !== "undefined" ? window : globalThis);
