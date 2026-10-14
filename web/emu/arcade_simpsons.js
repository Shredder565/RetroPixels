// RetroPixels — Konami The Simpsons board (1991). Original code, written from the
// documented behaviour of the board:
//   - a Konami 053248 ("Konami-2", RPKonami2) at 3 MHz. Its 64 KB address space:
//       0000-0fff  the K052109's RAM, or the palette (2048 colours, xBBBBBGGGGGRRRRR)
//       1000-1fff  the K052109's RAM and registers; 1f80-1fff are the board's ports
//       2000-3fff  the K052109's RAM, or the sprite RAM
//       4000-5fff  work RAM
//       6000-7fff  an 8 KB window on the program ROMs, picked by the processor's SETLINES
//       8000-ffff  the last 32 KB of the program ROMs
//     Two bits of the EEPROM port choose what shows at 0000 and 2000.
//   - a K052109 tilemap chip, K053247 / K053246 sprites and a K053251 mixer;
//   - sprite "DMA": at the start of the vertical blank (when the K053246 allows it) the
//     list in sprite RAM is copied into the sprite chip, skipping sprites that are off or
//     whose drawing-order byte is 0, and a little later the processor gets its FIRQ (when
//     the board allows it); the IRQ comes at the vertical blank when the K052109 allows it.
//     The picture is made as the blank ends (with the scrolling the game set during it);
//   - a serial EEPROM (128 bytes) for settings and high scores; the game reads its data
//     line on bit 4 of 1f81 and its ready line on bit 5. A set without the .nv file starts
//     with the test switch held, so the game writes its own factory settings;
//   - a Z80 at 3.58 MHz with banked program ROM, a YM2151 and a K053260; reading 1fc4
//     interrupts it, and it arms its own NMI by writing fa00.
// The 4-player set reads four controllers; the 2-player sets read two.
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 3000000;
  const Z80_HZ = 3579545;
  const YM_HZ = 3579545;
  const LINE_CYC = 192;                 // 64 us a line
  const LINES = 264;
  const FPS = CPU_HZ / (LINE_CYC * LINES);
  const W = 288, H = 224, TOP = 16, LEFT = 112;
  const NMI_DELAY = 179;                // Z80 cycles from arming the sound NMI to the NMI (50 us)
  const FIRQ_DELAY = 256;               // main cycles from the sprite copy to its FIRQ
  const PCM_GAIN = 1.6;                 // K053260 level against the YM2151 (MAME's mix is ~4 dB more PCM-heavy)
  const SHADE = new Uint8Array(256);    // a shadow leaves 60% of each colour component
  for (let i = 0; i < 256; i++) SHADE[i] = Math.floor(i * 0.6);

  // Gather files into one region: each file's bytes go `width` at a time, every `stride` bytes.
  function interleave(game, files, role, width, stride) {
    const parts = game.files.filter((f) => f.role === role);
    let size = 0;
    for (const f of parts) size = Math.max(size, (f.base || 0) + f.size * stride / width);
    const out = new Uint8Array(size).fill(0xff);
    for (const f of parts) {
      const d = RPArcade.findFile(f, files); if (!d) continue;
      const lane = f.lane || 0, base = f.base || 0;
      for (let i = 0, o = base + lane; i < d.length; i += width, o += stride) for (let k = 0; k < width; k++) out[o + k] = d[i + k];
    }
    return out;
  }

  class SimpsonsBoard {
    constructor(game, files, opts) {
      opts = opts || {};
      this.game = game;
      this.sampleRate = opts.sampleRate || 48000;
      this.players = game.players || 4;
      this.prg = RPArcade.region(game, files, "cpu", 0x80000);       // the four program ROMs end to end
      this.fixedBase = this.prg.length - 0x8000;
      this.sndRom = RPArcade.region(game, files, "sound", 0x20000);
      const tileRom = interleave(game, files, "tiles", 2, 4);
      const spriteRom = interleave(game, files, "sprites", 2, 8);
      const pcm = RPArcade.region(game, files, "pcm", 0x140000);

      this.ram = new Uint8Array(0x2000);       // 4000-5fff
      this.palRam = new Uint8Array(0x1000);
      this.sprRam = new Uint8Array(0x2000);
      this.sndRam = new Uint8Array(0x800);
      this.palette = new Uint32Array(2048);
      this.screen = new Uint32Array(W * H);
      this.width = W; this.height = H;

      const self = this;
      this.layerBase = [0, 0, 0];
      this.spriteBase = 0;
      this.k052109 = new global.RPK052109({
        rom: tileRom,
        tileCallback: (t) => {
          const c = t.color;
          t.code |= ((c & 0x3f) << 8) | (t.bank << 14);
          t.color = self.layerBase[t.layer] + ((c & 0xc0) >> 6);
        },
      });
      this.k052109.dx = [0, -6, -6];   // the scrolling layers sit 6 pixels right of the fixed layer on this board
      this.k053247 = new global.RPK053245({
        // dx: with the X offset the game writes (0x15) subtracted, sprites land on the backgrounds the way the game
        // draws them: the character-select portraits centred in their frames, the robbers leaving the
        // jewellers' door, the bonus-stage ranking labels over their score panels
        type: "053247", rom: spriteRom, layout: "packed", dx: -59, dy: -39, codeCounters: true, zoomStep: true, subtractOffsetX: true,
        spriteCallback: (t) => {
          t.pri = (t.attr & 0x0f80) >> 6;
          t.color = self.spriteBase + (t.attr & 0x1f);
          t.shadow = (t.attr >> 10) & 3;   // shadow type (0 = none): the sprite's pen 15 is a see-through shadow
        },
      });
      this.k053251 = new global.RPK053251();
      this.k053260 = new global.RPK053260(pcm, YM_HZ);
      this.ym = new global.RPYM2151({ clock: YM_HZ });
      let nv = opts.sram && opts.sram.length >= 128 ? opts.sram : null;
      if (!nv) { const f = game.files.find((x) => x.role === "eeprom"); const d = f && RPArcade.findFile(f, files); if (d && d.length >= 128) nv = d; }
      this.eeprom = new global.RPEeprom93C46(nv, { wordBits: 8, addrBits: 9, write11: true });   // 128 bytes, see eeprom93c46.js

      this.cpu = new global.RPKonami2({
        read: (a) => self.read(a), write: (a, v) => self.write(a, v),
        setLines: (v) => { self.bank = v & 0x3f; },
        ack: (kind) => { if (kind === "irq") self.cpu.irq = false; else if (kind === "firq") self.cpu.firq = false; },
      });
      this.z80 = new global.RPZ80({
        read: (a) => self.zRead(a), write: (a, v) => self.zWrite(a, v),
        in: () => 0xff, out: () => {},
        ack: () => { self.z80.irq = false; },
      });

      const n = W * H;
      this.layerPens = [new Uint16Array(n), new Uint16Array(n), new Uint16Array(n)];
      this.sprPens = new Uint16Array(n); this.sprPri = new Uint8Array(n); this.sprShadow = new Uint8Array(n);
      this.mixPens = new Uint16Array(n); this.mixShade = new Uint8Array(n);
      this.inputs = [{}, {}, {}, {}];
      this.resampler = new RPArcade.Resampler(YM_HZ / 64, this.sampleRate);
      this.pcmBuf = new Float32Array(64);
      this.reset();
    }

    reset() {
      this.ram.fill(0); this.palRam.fill(0); this.sprRam.fill(0); this.sndRam.fill(0);
      this.palette.fill(0xff000000);
      this.k052109.reset(); this.k053247.reset(); this.k053251.reset(); this.ym.reset();
      this.k053260 = new global.RPK053260(this.k053260.rom, YM_HZ);
      this.bank = 0; this.videoBank = 0; this.firqEnabled = false; this.eepromOut = 0; this.ctrl = 0;
      this.firqAt = -1; this.vblank = false;
      this.cpu.irq = false; this.cpu.firq = false; this.cpu.reset();
      this.z80.reset(); this.z80.irq = false;
      this.sndBank = 0; this.nmiAt = -1;
      this.cyc = 0; this.zCyc = 0; this.ymFrac = 0;
      // A blank EEPROM (a set without its .nv file) fails the game's start-up check. An operator
      // fixes that by holding the test switch while switching on: the game's test mode then writes
      // its own factory settings. Do the same for the first couple of seconds.
      this.testHold = this.eeprom.bytes.every((v) => v === 0xff) ? 120 : 0;
      this.resampler.clear();
    }

    setInput(...ps) { for (let i = 0; i < 4; i++) this.inputs[i] = ps[i] || {}; }

    playerByte(p) {
      const i = this.inputs[p] || {};
      let v = (i.Left ? 1 : 0) | (i.Right ? 2 : 0) | (i.Up ? 4 : 0) | (i.Down ? 8 : 0) |
        (i["Button 1"] ? 0x10 : 0) | (i["Button 2"] ? 0x20 : 0);
      if (i["Start (1P)"] || (p === 1 && this.inputs[0]["Start (2P)"])) v |= 0x80;
      return ~v & 0xff;
    }
    coinByte() {
      let v = 0;
      for (let p = 0; p < 4; p++) {
        const i = this.inputs[p] || {};
        if (i.Coin || (p === 1 && this.inputs[0]["Coin 2"])) v |= 1 << p;
        if (i.Service) v |= 0x10 << p;
      }
      return ~v & 0xff;
    }
    testByte() {
      // bit 0 test switch (low = on: Service + Start on the 1st controller), bit 4 EEPROM data,
      // bit 5 EEPROM ready (writes finish at once here); the other bits read high
      const i = this.inputs[0] || {};
      return 0xce | (this.eeprom.read() << 4) | 0x20 | ((i.Service && i["Start (1P)"]) || this.testHold > 0 ? 0 : 1);
    }

    // ---- main processor memory ----
    read(a) {
      if (a >= 0x8000) return this.prg[this.fixedBase + (a - 0x8000)];
      if (a >= 0x6000) return this.prg[(this.bank * 0x2000 + (a - 0x6000)) % this.prg.length];
      if (a >= 0x4000) return this.ram[a - 0x4000];
      if (a < 0x1000) return (this.videoBank & 1) ? this.palRam[a] : this.k052109.read(a);
      if (a >= 0x2000) return (this.videoBank & 2) ? this.sprRam[a - 0x2000] : this.k052109.read(a);
      if (a >= 0x1f80) {
        switch (a) {
          case 0x1f80: return this.coinByte();
          case 0x1f81: return this.testByte();
          case 0x1f90: case 0x1f91: case 0x1f92: case 0x1f93: return this.playerByte(a - 0x1f90);
          case 0x1fc4: this.z80.irq = true; return 0;                       // sound interrupt
          case 0x1fc6: case 0x1fc7: return this.k053260.mainRead(a & 1);
          case 0x1fc8: case 0x1fc9: return this.k053247.regRead(a & 1);   // sprite ROM read
          default: return 0xff;
        }
      }
      return this.k052109.read(a);
    }
    write(a, v) {
      if (a >= 0x6000) return;
      if (a >= 0x4000) { this.ram[a - 0x4000] = v; return; }
      if (a < 0x1000) {
        if (this.videoBank & 1) { this.palRam[a] = v; this.updateColour(a >> 1); }
        else this.k052109.write(a, v);
        return;
      }
      if (a >= 0x2000) { if (this.videoBank & 2) this.sprRam[a - 0x2000] = v; else this.k052109.write(a, v); return; }
      if (a >= 0x1f80) {
        if (a >= 0x1fa0 && a < 0x1fa8) { this.k053247.regWrite(a & 7, v); return; }
        if (a >= 0x1fb0 && a < 0x1fc0) { this.k053251.write(a & 15, v); return; }
        switch (a) {
          case 0x1fc0: // coin counters; bit 3 tile ROM reading, bit 4 sprite ROM reading
            this.ctrl = v; this.k052109.rmrd = !!(v & 0x08); return;
          case 0x1fc2: // EEPROM (bit 7 data, bit 3 chip select, bit 4 clock), video banks (bits 0-1), FIRQ enable (bit 2)
            if (v === 0xff) return;
            this.eepromOut = v;
            this.eeprom.write(v & 0x08, v & 0x10, v & 0x80);
            this.videoBank = v & 3;
            this.firqEnabled = !!(v & 4);
            return;
          case 0x1fc6: case 0x1fc7: this.k053260.mainWrite(a & 1, v); return;
          default: return;
        }
      }
      this.k052109.write(a, v);
    }

    updateColour(i) {
      const v = (this.palRam[i * 2] << 8) | this.palRam[i * 2 + 1];
      const c5 = (x) => (x << 3) | (x >> 2);
      this.palette[i] = RPArcade.rgb(c5(v & 31), c5((v >> 5) & 31), c5((v >> 10) & 31));
    }

    // Copy the sprite list into the sprite chip: sprites that are on first, in list order,
    // then the rest of the chip's list switched off.
    spriteDma() {
      const src = this.sprRam, dst = this.k053247.buffer;
      let o = 0;
      for (let i = 0; i < 256; i++) {
        const s = i * 16;
        // a sprite whose drawing-order byte is 0 isn't copied (the game parks sprites that way,
        // e.g. a knocked-down enemy blinking out)
        if ((src[s] & 0x80) && src[s + 1]) { dst.set(src.subarray(s, s + 16), o); o += 16; }
      }
      for (; o < 256 * 16; o += 16) dst.fill(0, o, o + 16);
    }

    // ---- Z80 sound board ----
    zRead(a) {
      if (a < 0x8000) return this.sndRom[a];
      if (a < 0xc000) return this.sndRom[(this.sndBank * 0x4000 + (a - 0x8000)) % this.sndRom.length];
      if (a >= 0xf000 && a < 0xf800) return this.sndRam[a - 0xf000];
      if (a === 0xf801) return this.ym.read();
      if (a >= 0xfc00 && a < 0xfc30) return this.k053260.read(a - 0xfc00);
      return 0xff;
    }
    zWrite(a, v) {
      if (a >= 0xf000 && a < 0xf800) { this.sndRam[a - 0xf000] = v; return; }
      if (a === 0xf800 || a === 0xf801) { this.ym.write(a & 1, v); return; }
      if (a === 0xfa00) { this.nmiAt = this.zCyc + NMI_DELAY; return; }
      if (a >= 0xfc00 && a < 0xfc30) { this.k053260.write(a - 0xfc00, v); return; }
      if (a === 0xfe00) { this.sndBank = v & 7; return; }
    }

    runSound(cycles) {
      const end = this.zCyc + cycles;
      while (this.zCyc < end) {
        if (this.nmiAt >= 0 && this.zCyc >= this.nmiAt) { this.nmiAt = -1; this.z80.nmi(); }
        this.zCyc += this.z80.step();
      }
      this.ymFrac += cycles * (YM_HZ / 64) / Z80_HZ;
      const n = Math.floor(this.ymFrac); this.ymFrac -= n;
      if (n > this.pcmBuf.length) this.pcmBuf = new Float32Array(n * 2);
      const pcm = this.pcmBuf; pcm.fill(0, 0, n);
      this.k053260.mix(pcm, 0, n, YM_HZ / 64);
      for (let i = 0; i < n; i++) {
        this.ym.tick();
        this.resampler.add((this.ym.out[0] + this.ym.out[1]) / 65536 + pcm[i] * PCM_GAIN);
      }
    }

    // ---- picture ----
    drawFrame() {
      const k51 = this.k053251, n = W * H;
      this.spriteBase = k51.paletteBase(1);
      this.layerBase[0] = k51.paletteBase(2); this.layerBase[1] = k51.paletteBase(3); this.layerBase[2] = k51.paletteBase(4);
      for (let l = 0; l < 3; l++) { this.layerPens[l].fill(0); this.k052109.render(l, this.layerPens[l], W, H, { x0: LEFT, y0: TOP }); }
      this.sprPens.fill(0); this.sprPri.fill(0); this.sprShadow.fill(0);
      this.k053247.render(this.sprPens, this.sprPri, this.sprShadow, W, H);
      this.mix();
      const pal = this.palette, scr = this.screen, pens = this.mixPens, shade = this.mixShade;
      for (let i = 0; i < n; i++) {
        const c = pal[pens[i] & 2047];
        scr[i] = shade[i] ? (0xff000000 | (SHADE[(c >>> 16) & 255] << 16) | (SHADE[(c >>> 8) & 255] << 8) | SHADE[c & 255]) >>> 0 : c;
      }
    }

    // The K053251's choice for every pixel: the input with the lowest priority value whose pen
    // isn't see-through (pen 0), else the background colour; ties go to the sprite. Sprite pixels
    // bring their own priority. A sprite's shadow (its pen 15 when the sprite is a shadow type)
    // leaves any sprite under it alone and darkens the layer or background that wins there, when
    // that one's priority is no better than the shadow type's priority (registers 6-8 for types 1-3).
    mix() {
      const k51 = this.k053251, n = W * H;
      const sp = this.sprPens, spri = this.sprPri, ssh = this.sprShadow;
      const l0 = this.layerPens[0], l1 = this.layerPens[1], l2 = this.layerPens[2];
      const p2 = k51.priority(2), p3 = k51.priority(3), p4 = k51.priority(4);
      const bg = k51.paletteBase(0) * 16, out = this.mixPens, shade = this.mixShade, regs = k51.regs;
      for (let i = 0; i < n; i++) {
        let best = 255, pen = bg, spr = false;
        const s = sp[i];
        if (s & 15) { best = spri[i]; pen = s; spr = true; }
        let v = l2[i]; if ((v & 15) && p4 < best) { best = p4; pen = v; spr = false; }
        v = l1[i]; if ((v & 15) && p3 < best) { best = p3; pen = v; spr = false; }
        v = l0[i]; if ((v & 15) && p2 < best) { best = p2; pen = v; spr = false; }
        out[i] = pen;
        const sh = ssh[i];
        shade[i] = sh && !spr && (regs[5 + sh] & 0x3f) <= best ? 1 : 0;
      }
    }

    runFrame() {
      const cpu = this.cpu;
      if (this.testHold > 0) this.testHold--;
      for (let line = 0; line < LINES; line++) {
        if (line === TOP + H) {
          this.vblank = true;
          // the sprite list is copied as the blank starts
          if (this.k053247.dmaEnabled) { this.spriteDma(); this.firqAt = this.cyc + FIRQ_DELAY; }
          if (this.k052109.irqEnabled) cpu.irq = true;
        } else if (line === TOP) {
          // the picture is made as the blank ends, so scrolling and colours the game sets
          // during the blank show at once (the sprites are the list copied at its start)
          this.vblank = false;
          this.drawFrame();
        }
        const end = (line + 1) * LINE_CYC;
        while (this.cyc < end) {
          if (this.firqAt >= 0 && this.cyc >= this.firqAt) { this.firqAt = -1; if (this.firqEnabled) cpu.firq = true; }
          this.cyc += cpu.step();
        }
        this.runSound(LINE_CYC * Z80_HZ / CPU_HZ);
      }
      this.cyc -= LINES * LINE_CYC;
      if (this.firqAt >= 0) this.firqAt -= LINES * LINE_CYC;
    }

    takeAudio() { return this.resampler.take(); }
    getSRAM() { return this.eeprom.bytes.slice(); }

    saveState() {
      return { v: 1, cpu: this.cpu.getState(), z80: this.z80.getState(), ram: this.ram.slice(), palRam: this.palRam.slice(),
        sprRam: this.sprRam.slice(), sndRam: this.sndRam.slice(),
        k052109: this.k052109.saveState(), k053247: this.k053247.getState(), k053251: this.k053251.getState(),
        k053260: this.k053260.getState(), ym: this.ym.saveState(), eeprom: this.eeprom.getState(),
        m: [this.bank, this.videoBank, this.firqEnabled ? 1 : 0, this.eepromOut, this.ctrl, this.firqAt, this.vblank ? 1 : 0,
          this.sndBank, this.nmiAt, this.cyc, this.zCyc, this.ymFrac, this.z80.irq ? 1 : 0, this.testHold] };
    }
    loadState(s) {
      this.cpu.setState(s.cpu); this.z80.setState(s.z80);
      this.ram.set(s.ram); this.palRam.set(s.palRam); this.sprRam.set(s.sprRam); this.sndRam.set(s.sndRam);
      this.k052109.loadState(s.k052109); this.k052109.dx = [0, -6, -6]; this.k053247.setState(s.k053247); this.k053251.setState(s.k053251);
      this.k053260.setState(s.k053260); this.ym.loadState(s.ym); this.eeprom.setState(s.eeprom);
      let fe, vb, zi;
      [this.bank, this.videoBank, fe, this.eepromOut, this.ctrl, this.firqAt, vb, this.sndBank, this.nmiAt, this.cyc, this.zCyc, this.ymFrac, zi, this.testHold] = s.m;
      this.testHold = this.testHold | 0;
      this.firqEnabled = !!fe; this.vblank = !!vb; this.z80.irq = !!zi;
      this.k052109.rmrd = !!(this.ctrl & 0x08);
      for (let i = 0; i < 2048; i++) this.updateColour(i);
      this.resampler.clear();
      this.drawFrame();
    }
  }

  SimpsonsBoard.id = "simpsons";
  SimpsonsBoard.title = "Konami The Simpsons";
  SimpsonsBoard.width = W;
  SimpsonsBoard.height = H;
  SimpsonsBoard.rotate = 0;
  SimpsonsBoard.fps = FPS;
  SimpsonsBoard.players = 4;
  SimpsonsBoard.buttons = ["Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Service"];
  SimpsonsBoard.dips = [];

  const SHARED = [
    { name: "072-e03.6g", size: 0x20000, role: "sound" },
    { name: "072-b07.18h", size: 0x80000, role: "tiles", lane: 0 },
    { name: "072-b06.16h", size: 0x80000, role: "tiles", lane: 2 },
    { name: "072-b08.3n", size: 0x100000, role: "sprites", lane: 0 },
    { name: "072-b09.8n", size: 0x100000, role: "sprites", lane: 2 },
    { name: "072-b10.12n", size: 0x100000, role: "sprites", lane: 4 },
    { name: "072-b11.16l", size: 0x100000, role: "sprites", lane: 6 },
    { name: "072-d05.1f", size: 0x100000, role: "pcm", offset: 0 },
    { name: "072-d04.1d", size: 0x40000, role: "pcm", offset: 0x100000 },
  ];
  // program ROMs in address order (the last one holds the fixed 32 KB at 8000-ffff)
  const version = (id, title, players, prg, nv) => ({
    id, title, year: 1991, maker: "Konami", players, loose: true, parent: id === "simpsons" ? undefined : "simpsons",
    files: [
      ...prg.map((name, i) => ({ name, size: 0x20000, role: "cpu", offset: i * 0x20000 })),
      ...SHARED,
      { name: nv, size: 0x80, role: "eeprom", optional: true },
    ],
  });
  SimpsonsBoard.games = [
    version("simpsons", "The Simpsons (4 players, World)", 4, ["072-g02.16c", "072-g01.17c", "072-j13.13c", "072-j12.15c"], "simpsons.12c.nv"),
    version("simpsons4pe", "The Simpsons (4 players, older)", 4, ["072-g02.16c", "072-g01.17c", "072-m13.13c", "072-l12.15c"], "simpsons4pe.12c.nv"),
    version("simpsons2p", "The Simpsons (2 players, World)", 2, ["072-g02.16c", "072-p01.17c", "072-013.13c", "072-012.15c"], "simpsons2p.12c.nv"),
    version("simpsons2pa", "The Simpsons (2 players, Asia)", 2, ["072-g02.16c", "072-p01.17c", "072-013.13c", "072-012.15c"], "simpsons2pa.12c.nv"),
    version("simpsons2pj", "The Simpsons (2 players, Japan)", 2, ["072-s02.16c", "072-t01.17c", "072-213.13c", "072-212.15c"], "simpsons2pj.12c.nv"),
  ];

  RPArcade.boards.push(SimpsonsBoard);
})(typeof window !== "undefined" ? window : globalThis);
