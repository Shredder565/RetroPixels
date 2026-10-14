// RetroPixels — Konami TMNT2 board: Teenage Mutant Ninja Turtles - Turtles in Time
// (1991). Original code, written from the documented behaviour of the board:
//   - a 68000 at 16 MHz (RPM68K) with 512 KB of program ROM, 16 KB of work RAM and
//     palette RAM of 2048 colours (xBBBBBGGGGGRRRRR);
//   - a K052109 tilemap chip (fixed layer + two scrolling layers), K053245 / K053244
//     sprites (zoomed, sorted, with shadows) and a K053251 mixing them by priority;
//   - a byte-wide serial EEPROM (128 bytes) holding the settings and high scores (the game's own
//     service menu sets it up: there are no DIP switches);
//   - a Z80 at 8 MHz with a YM2151 FM chip and a K053260 PCM chip; the main
//     processor talks to it through the K053260's ports and nudges it with an interrupt;
//   - Konami's sprite helper chip: the game describes each sprite as a part of a bigger
//     object (local position, colour) plus the object's position, zoom and flip; on a
//     write to its trigger register the chip works out the sprite's screen position and
//     attributes and writes them into the sprite list. RetroPixels does the same sums
//     its own way (the offset scaling with zoom is a curve fitted to how the chip behaves).
// The 4-player sets read four controllers; the 2-player sets read two.
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };

  const CPU_HZ = 16000000;
  const Z80_HZ = 8000000;
  const YM_HZ = 3579545;
  const LINE_CYC = 1024;                // 384 pixels at 6 MHz = 64 us a line
  const LINES = 264;
  const FPS = CPU_HZ / (LINE_CYC * LINES);
  const W = 320, H = 224, TOP = 16, LEFT = 96; // the K052109's map pixels 96..415, lines 16..239
  const NMI_DELAY = 400;                // Z80 cycles from arming the sound NMI to the NMI (50 us)

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
  RPArcade.interleave = RPArcade.interleave || interleave;

  // The board's serial EEPROM: 128 cells of 8 bits. Same three lines as the 93C46 the other
  // Konami boards use (chip select, clock, data in; one data out), but byte-wide, and the game
  // clocks 9 address bits after the start bit and 2-bit opcode (the top two only matter for the
  // 00 commands; the cell is the low 7). Opcodes, as the game uses them:
  //   10 READ: data out goes 0 (dummy), then each rising clock puts out the next bit, D7 first;
  //   11 WRITE: 8 data bits follow (the cell is erased and written in one go);
  //   01 WRITE as well (the usual 93Cxx code), and with 00 the top address bits pick
  //   EWDS (00), WRAL (01), ERAL (10), EWEN (11). Writes finish at once (always "ready").
  class ByteEeprom {
    constructor(initial) {
      this.bytes = new Uint8Array(128).fill(0xff);
      if (initial && initial.length) this.bytes.set(initial.subarray ? initial.subarray(0, 128) : initial.slice(0, 128));
      this.cs = 0; this.clk = 0; this.dout = 1;
      this.state = 0; this.shift = 0; this.bits = 0; this.addr = 0; this.out = 0; this.enabled = false; this.all = false;
    }
    read() { return this.dout; }
    write(cs, clk, di) {
      cs = cs ? 1 : 0; clk = clk ? 1 : 0; di = di ? 1 : 0;
      if (!cs) {
        if (this.cs && this.state === 3 && this.bits === 0 && this.erasing) this.store(0xff); // a WRITE cut short erases
        this.cs = 0; this.clk = clk; this.state = 0; this.dout = 1; return;
      }
      if (!this.cs) { this.state = 0; this.dout = 1; }
      const rising = clk && !this.clk;
      this.cs = 1; this.clk = clk;
      if (!rising) return;
      switch (this.state) {
        case 0: if (di) { this.state = 1; this.shift = 0; this.bits = 0; } return;
        case 1:
          this.shift = (this.shift << 1) | di;
          if (++this.bits === 11) this.command(this.shift >> 9, this.shift & 0x1ff);
          return;
        case 2:
          this.dout = (this.out >> 7) & 1; this.out = (this.out << 1) & 0xff;
          if (++this.bits === 8) { this.bits = 0; this.addr = (this.addr + 1) & 0x7f; this.out = this.bytes[this.addr]; }
          return;
        case 3:
          this.shift = ((this.shift << 1) | di) & 0xff;
          if (++this.bits === 8) { this.store(this.shift); this.state = 4; this.dout = 1; }
          return;
        default: return;
      }
    }
    store(v) {
      if (!this.enabled) return;
      if (this.all) this.bytes.fill(v); else this.bytes[this.addr] = v;
    }
    command(op, a) {
      this.addr = a & 0x7f; this.all = false; this.erasing = false;
      if (op === 2) { this.state = 2; this.dout = 0; this.out = this.bytes[this.addr]; this.bits = 0; return; }
      if (op === 1 || op === 3) { this.state = 3; this.shift = 0; this.bits = 0; this.erasing = op === 3; return; }
      switch (a >> 7) {
        case 0: this.enabled = false; break;
        case 1: this.state = 3; this.shift = 0; this.bits = 0; this.all = true; return;
        case 2: if (this.enabled) this.bytes.fill(0xff); break;
        case 3: this.enabled = true; break;
      }
      this.state = 4; this.dout = 1;
    }
    getState() {
      return { bytes: this.bytes.slice(), r: [this.cs, this.clk, this.dout, this.state, this.shift, this.bits, this.addr, this.out,
        this.enabled ? 1 : 0, this.all ? 1 : 0, this.erasing ? 1 : 0] };
    }
    setState(o) {
      this.bytes.set(o.bytes);
      let en, all, er;
      [this.cs, this.clk, this.dout, this.state, this.shift, this.bits, this.addr, this.out, en, all, er] = o.r;
      this.enabled = !!en; this.all = !!all; this.erasing = !!er;
    }
  }

  class TMNT2Board {
    constructor(game, files, opts) {
      opts = opts || {};
      this.game = game;
      this.sampleRate = opts.sampleRate || 48000;
      this.players = game.players || 4;
      this.rom = new Uint8Array(0x100000).fill(0xff); // 512 KB fitted; the rest reads as unused
      this.rom.set(interleave(game, files, "cpu", 1, 2).subarray(0, 0x100000));
      this.sndRom = RPArcade.region(game, files, "sound", 0x10000);
      const tileRom = interleave(game, files, "tiles", 2, 4);
      const spriteRom = unscrambleSprites(interleave(game, files, "sprites", 2, 4));
      const pcm = RPArcade.region(game, files, "pcm", 0x200000);

      this.ram = new Uint8Array(0x4000);       // 104000-107fff
      this.palRam = new Uint8Array(0x1000);    // 140000-140fff
      this.sprRam = new Uint8Array(0x4000);    // 180000-183fff (the K053245 sees one word in eight)
      this.ioRam = new Uint8Array(0x80);       // 1c0500-1c057f
      this.prot = new Uint16Array(16);         // 1c0800-1c081f
      this.sndRam = new Uint8Array(0x800);     // Z80 f000-f7ff
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
          t.code |= ((c & 0x03) << 8) | ((c & 0x10) << 6) | ((c & 0x0c) << 9) | (t.bank << 13);
          t.color = self.layerBase[t.layer] + ((c & 0xe0) >> 5);
        },
      });
      this.k053245 = new global.RPK053245({
        type: "053245", rom: spriteRom, layout: "planar", dx: -3, dy: -23,
        spriteCallback: (t) => {
          t.pri = 0x20 | ((t.attr & 0x60) >> 2);
          t.color = self.spriteBase + (t.attr & 0x1f);
        },
      });
      this.k053251 = new global.RPK053251();
      this.k053260 = new global.RPK053260(pcm, YM_HZ);
      this.ym = new global.RPYM2151({ clock: YM_HZ });
      let nv = opts.sram && opts.sram.length >= 128 ? opts.sram : null;
      if (!nv) { const f = game.files.find((x) => x.role === "eeprom"); const d = f && RPArcade.findFile(f, files); if (d && d.length >= 128) nv = d; }
      this.eeprom = new ByteEeprom(nv);

      this.cpu = new global.RPM68K({
        read8: (a) => self.read8(a), read16: (a) => self.read16(a),
        write8: (a, v) => self.write8(a, v), write16: (a, v) => self.write16(a, v),
        ack: () => { self.cpu.setIRQ(0); return -1; },
      });
      this.z80 = new global.RPZ80({
        read: (a) => self.zRead(a), write: (a, v) => self.zWrite(a, v),
        in: () => 0xff, out: () => {},
        ack: () => { self.z80.irq = false; },
      });

      // picture buffers
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
      this.ram.fill(0); this.palRam.fill(0); this.sprRam.fill(0); this.ioRam.fill(0); this.prot.fill(0); this.sndRam.fill(0);
      this.palette.fill(0xff000000);
      this.k052109.reset(); this.k053245.reset(); this.k053251.reset();
      this.ym.reset();
      this.k053260 = new global.RPK053260(this.k053260.rom, YM_HZ);
      this.eepromOut = 0; this.ctrl300 = 0; this.vblank = false; this.dmaBusy = 0;
      // A blank EEPROM (a set without its .nv file): the game only writes its factory settings when
      // it's switched on with the test switch held, so hold it for the first second, as an operator
      // setting up a new board would (the game then carries on to its attract mode by itself).
      this.setupFrames = this.eeprom.bytes.every((x) => x === 0xff) ? 60 : 0;
      this.cpu.setIRQ(0); this.cpu.reset();
      this.z80.reset(); this.z80.irq = false;
      this.nmiAt = -1;          // Z80 cycle when the armed NMI fires (-1 none)
      this.zCyc = 0; this.ymFrac = 0; this.cyc = 0;
      this.resampler.clear();
    }

    setInput(...ps) { for (let i = 0; i < 4; i++) this.inputs[i] = ps[i] || {}; }

    // ---- inputs (active low) ----
    playerByte(p) {
      const i = this.inputs[p] || {};
      let v = (i.Left ? 1 : 0) | (i.Right ? 2 : 0) | (i.Up ? 4 : 0) | (i.Down ? 8 : 0) |
        (i["Button 1"] ? 0x10 : 0) | (i["Button 2"] ? 0x20 : 0) | (i["Button 3"] ? 0x40 : 0);
      const start = i["Start (1P)"] || (p === 1 && this.inputs[0]["Start (2P)"]);
      if (start) v |= 0x80;
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
    eepromByte() {
      // bit 0 EEPROM data, bit 1 EEPROM ready, bit 2 the sprite chip still copying its list (the
      // game waits for it before moving the sprites' offsets), bit 3 vertical blank, bit 5 low
      // = hardest difficulty (a factory jumper; left high), bit 7 test switch (low = on)
      const test = (this.inputs[0] && this.inputs[0].Service && this.inputs[0]["Start (1P)"]) || this.setupFrames > 0;
      return 0x70 | this.eeprom.read() | 2 | (this.dmaBusy > 0 ? 4 : 0) | (this.vblank ? 8 : 0) | (test ? 0 : 0x80);
    }

    // ---- 68000 memory ----
    read16(a) {
      a &= 0xffffff;
      if (a < 0x100000) return (this.rom[a] << 8) | this.rom[a + 1];
      if (a >= 0x104000 && a < 0x108000) { const o = a - 0x104000; return (this.ram[o] << 8) | this.ram[o + 1]; }
      if (a >= 0x140000 && a < 0x141000) { const o = a - 0x140000; return (this.palRam[o] << 8) | this.palRam[o + 1]; }
      if (a >= 0x180000 && a < 0x184000) { const o = a - 0x180000; return (this.sprRam[o] << 8) | this.sprRam[o + 1]; }
      if (a >= 0x600000 && a < 0x604000) { const o = (a - 0x600000) >> 1; return (this.k052109.read(o) << 8) | this.k052109.read(o + 0x2000); }
      return (this.read8(a) << 8) | this.read8(a + 1);
    }
    read8(a) {
      a &= 0xffffff;
      if (a < 0x100000) return this.rom[a];
      if (a >= 0x104000 && a < 0x108000) return this.ram[a - 0x104000];
      if (a >= 0x140000 && a < 0x141000) return this.palRam[a - 0x140000];
      if (a >= 0x180000 && a < 0x184000) return this.sprRam[a - 0x180000];
      if (a >= 0x600000 && a < 0x604000) { const o = (a - 0x600000) >> 1; return this.k052109.read((a & 1) ? o + 0x2000 : o); }
      if (a >= 0x1c0000 && a < 0x1c0008) return (a & 1) ? this.playerByte((a - 0x1c0000) >> 1) : 0xff;
      if (a === 0x1c0101) return this.coinByte();
      if (a === 0x1c0103) return this.eepromByte();
      if (a >= 0x1c0500 && a < 0x1c0580) return this.ioRam[a - 0x1c0500];
      if (a >= 0x1c0800 && a < 0x1c0820) { const w = this.prot[(a - 0x1c0800) >> 1]; return (a & 1) ? w & 0xff : w >> 8; }
      if (a >= 0x5a0000 && a < 0x5a0020) return this.k053245.regRead(((a & 0x1c) >> 1) | (a & 1));
      if (a >= 0x5c0600 && a < 0x5c0604) return (a & 1) ? this.k053260.mainRead((a >> 1) & 1) : 0;
      if (a >= 0x5c0700 && a < 0x5c0720) return (a & 1) ? this.k053251.read((a >> 1) & 15) : 0;
      return (a & 1) ? 0xff : 0xff;
    }
    write16(a, v) {
      a &= 0xffffff; v &= 0xffff;
      if (a >= 0x104000 && a < 0x108000) { const o = a - 0x104000; this.ram[o] = v >> 8; this.ram[o + 1] = v; return; }
      if (a >= 0x140000 && a < 0x141000) { const o = a - 0x140000; this.palRam[o] = v >> 8; this.palRam[o + 1] = v; this.updateColour(o >> 1); return; }
      if (a >= 0x180000 && a < 0x184000) { const o = a - 0x180000; this.sprRam[o] = v >> 8; this.sprRam[o + 1] = v; this.spriteWord(o >> 1); return; }
      if (a >= 0x600000 && a < 0x604000) { const o = (a - 0x600000) >> 1; this.k052109.write(o, v >> 8); this.k052109.write(o + 0x2000, v & 0xff); return; }
      if (a >= 0x1c0800 && a < 0x1c0820) { this.protWrite((a - 0x1c0800) >> 1, v, true, true); return; }
      this.write8(a, v >> 8); this.write8(a + 1, v & 0xff);
    }
    write8(a, v) {
      a &= 0xffffff; v &= 0xff;
      if (a >= 0x104000 && a < 0x108000) { this.ram[a - 0x104000] = v; return; }
      if (a >= 0x140000 && a < 0x141000) { this.palRam[a - 0x140000] = v; this.updateColour((a - 0x140000) >> 1); return; }
      if (a >= 0x180000 && a < 0x184000) { this.sprRam[a - 0x180000] = v; this.spriteWord((a - 0x180000) >> 1); return; }
      if (a >= 0x600000 && a < 0x604000) { const o = (a - 0x600000) >> 1; this.k052109.write((a & 1) ? o + 0x2000 : o, v); return; }
      if (a >= 0x1c0500 && a < 0x1c0580) { this.ioRam[a - 0x1c0500] = v; return; }
      if (a >= 0x1c0800 && a < 0x1c0820) {
        const i = (a - 0x1c0800) >> 1;
        this.protWrite(i, (a & 1) ? (this.prot[i] & 0xff00) | v : (this.prot[i] & 0xff) | (v << 8), !(a & 1), !!(a & 1));
        return;
      }
      if (a === 0x1c0201) { // EEPROM lines: bit 0 data, bit 1 chip select, bit 2 clock
        this.eepromOut = v;
        this.eeprom.write(v & 2, v & 4, v & 1);
        return;
      }
      if (a === 0x1c0301) { // bit 3: the CPU reads the tile ROMs through the K052109
        this.ctrl300 = v;
        this.k052109.rmrd = !!(v & 0x08);
        return;
      }
      if (a >= 0x5a0000 && a < 0x5a0020) {
        const r = ((a & 0x1c) >> 1) | (a & 1);
        if (r === 6) this.dmaBusy = 2; // the copy takes about a line
        this.k053245.regWrite(r, v); return;
      }
      if (a >= 0x5c0600 && a < 0x5c0604) { if (a & 1) this.k053260.mainWrite((a >> 1) & 1, v); return; }
      if (a === 0x5c0604 || a === 0x5c0605) { this.z80.irq = true; return; } // sound interrupt
      if (a >= 0x5c0700 && a < 0x5c0720) { if (a & 1) this.k053251.write((a >> 1) & 15, v); return; }
    }

    updateColour(i) {
      const v = (this.palRam[i * 2] << 8) | this.palRam[i * 2 + 1];
      const c5 = (x) => (x << 3) | (x >> 2);
      this.palette[i] = RPArcade.rgb(c5(v & 31), c5((v >> 5) & 31), c5((v >> 10) & 31));
    }

    // Sprite RAM words whose address bits 0, 4 and 5 (in words) are clear are the K053245's list:
    // word bits 1-3 pick the word of a sprite, bits 6-12 the sprite.
    spriteWord(w) {
      if (w & 0x31) return;
      const k = ((w & 0x0e) >> 1) | ((w & 0x1fc0) >> 3);
      this.k053245.writeWord(k, (this.sprRam[w * 2] << 8) | this.sprRam[w * 2 + 1]);
    }

    // ---- the sprite helper chip ----
    protWrite(i, v, hi, lo) {
      this.prot[i] = v;
      if (i !== 12 || !hi) return;               // the trigger is the high byte of 1c0818
      const P = this.prot;
      if ((P[8] & 0xff00) !== 0x8200) return;
      const src = ((P[1] & 0xff) << 16 | P[0]) & 0xfffffe;
      const dst = ((P[3] & 0xff) << 16 | P[2]) & 0xfffffe;
      const mod = ((P[5] & 0xff) << 16 | P[4]) & 0xfffffe;
      const zLock = (P[8] & 0xff) === 1;
      const rw = (base, k) => this.read16(base + k * 2);
      const s16 = (x) => (x << 16) >> 16;

      const code = rw(src, 0), attr = rw(src, 1);
      let localX = s16(rw(src, 2)), localY = s16(rw(src, 3));
      const flags = rw(mod, 0);
      // word 0 of the sprite: on, keep-aspect, flips and size from the part's attribute
      let w0 = (attr >> 2) & 0x3f00;
      let w6 = (attr & 0x380) | (flags & 0x60);  // mirror Y / mirror X / shadow, plus the object's priority
      const keepAspect = (flags & 0x14) === 0x14;
      if (flags & 0x8000) w0 |= 0x8000;
      if (keepAspect) w0 |= 0x4000;
      if (flags & 0x4000) { w0 ^= 0x1000; localX = -localX; } // the whole object turned sideways
      // colour: the object's colour replaces the part's unless the part uses colour 15 or it's locked
      const partColour = attr & 0x1f, objColour = rw(mod, 0x15) >> 8;
      w6 |= (partColour !== 0x0f && objColour <= 0x1f && !zLock) ? objColour : partColour;

      const zoomX = rw(mod, 0x0e), zoomY = keepAspect ? zoomX : rw(mod, 0x0f);
      const locked = (flags & 0x20) && (!zoomX || zoomX === 0x100);
      if (!locked) { localX = scalePart(localX, zoomX); localY = scalePart(localY, zoomY); }
      if (!zLock) localY += s16(rw(mod, 8));
      const x = localX + s16(rw(mod, 6)), y = localY + s16(rw(mod, 7));
      this.write16(dst, w0);
      this.write16(dst + 4, code);
      this.write16(dst + 8, y & 0xffff);
      this.write16(dst + 12, x & 0xffff);
      this.write16(dst + 24, w6);
    }

    // ---- Z80 sound board ----
    zRead(a) {
      if (a < 0xf000) return this.sndRom[a];
      if (a < 0xf800) return this.sndRam[a - 0xf000];
      if (a === 0xf801) return this.ym.read();
      if (a >= 0xfa00 && a < 0xfa30) return this.k053260.read(a - 0xfa00);
      return 0xff;
    }
    zWrite(a, v) {
      if (a >= 0xf000 && a < 0xf800) { this.sndRam[a - 0xf000] = v; return; }
      if (a === 0xf800 || a === 0xf801) { this.ym.write(a & 1, v); return; }
      if (a >= 0xfa00 && a < 0xfa30) { this.k053260.write(a - 0xfa00, v); return; }
      if (a === 0xfc00) { this.nmiAt = this.zCyc + NMI_DELAY; return; } // arm the NMI
    }

    runSound(cycles) {
      const end = this.zCyc + cycles;
      while (this.zCyc < end) {
        if (this.nmiAt >= 0 && this.zCyc >= this.nmiAt) { this.nmiAt = -1; this.z80.nmi(); }
        this.zCyc += this.z80.step();
      }
      // audio for this stretch of time at the YM2151's own rate
      this.ymFrac += cycles * (YM_HZ / 64) / Z80_HZ;
      let n = Math.floor(this.ymFrac); this.ymFrac -= n;
      if (n > this.pcmBuf.length) this.pcmBuf = new Float32Array(n * 2);
      const pcm = this.pcmBuf; pcm.fill(0, 0, n);
      this.k053260.mix(pcm, 0, n, YM_HZ / 64);
      for (let i = 0; i < n; i++) {
        this.ym.tick();
        this.resampler.add((this.ym.out[0] + this.ym.out[1]) / 65536 + pcm[i]);
      }
    }

    // ---- picture ----
    drawFrame() {
      const k51 = this.k053251, n = W * H;
      this.spriteBase = k51.paletteBase(1);
      this.layerBase[0] = k51.paletteBase(2); this.layerBase[1] = k51.paletteBase(4); this.layerBase[2] = k51.paletteBase(3);
      for (let l = 0; l < 3; l++) { this.layerPens[l].fill(0); this.k052109.render(l, this.layerPens[l], W, H, { x0: LEFT, y0: TOP }); }
      this.sprPens.fill(0); this.sprPri.fill(0); this.sprShadow.fill(0);
      this.k053245.render(this.sprPens, this.sprPri, this.sprShadow, W, H);
      k51.mix(this.mixPens, this.mixShade, n, [
        { ci: 1, pens: this.sprPens, pri: this.sprPri, shadow: this.sprShadow },
        { ci: 3, pens: this.layerPens[2], pri: k51.priority(3) },
        { ci: 4, pens: this.layerPens[1], pri: k51.priority(4) },
        { ci: 2, pens: this.layerPens[0], pri: k51.priority(2) },
      ], k51.paletteBase(0) * 16);
      const pal = this.palette, scr = this.screen, pens = this.mixPens, shade = this.mixShade;
      for (let i = 0; i < n; i++) {
        const c = pal[pens[i] & 2047];
        scr[i] = shade[i] ? (0xff000000 | (((c & 0xfefefe) >>> 1) + ((c & 0xf8f8f8) >>> 3))) >>> 0 : c; // shadow: about 60%
      }
    }

    runFrame() {
      const cpu = this.cpu;
      for (let line = 0; line < LINES; line++) {
        if (line === TOP + H) {
          this.vblank = true;
          this.drawFrame();
          if (this.k052109.irqEnabled) cpu.setIRQ(4); // vertical blank: level 4
        } else if (line === TOP) this.vblank = false;
        const end = (line + 1) * LINE_CYC;
        while (this.cyc < end) this.cyc += cpu.step();
        if (this.dmaBusy > 0) this.dmaBusy--;
        this.runSound(LINE_CYC * Z80_HZ / CPU_HZ);
      }
      this.cyc -= LINES * LINE_CYC;
      if (this.setupFrames > 0) this.setupFrames--;
    }

    takeAudio() { return this.resampler.take(); }
    getSRAM() { return this.eeprom.bytes.slice(); }

    saveState() {
      return { v: 1, cpu: this.cpu.saveState(), z80: this.z80.getState(), ram: this.ram.slice(), palRam: this.palRam.slice(),
        sprRam: this.sprRam.slice(), ioRam: this.ioRam.slice(), prot: this.prot.slice(), sndRam: this.sndRam.slice(),
        k052109: this.k052109.saveState(), k053245: this.k053245.getState(), k053251: this.k053251.getState(),
        k053260: this.k053260.getState(), ym: this.ym.saveState(), eeprom: this.eeprom.getState(),
        m: [this.eepromOut, this.ctrl300, this.vblank ? 1 : 0, this.nmiAt, this.zCyc, this.ymFrac, this.cyc, this.z80.irq ? 1 : 0, this.dmaBusy, this.setupFrames] };
    }
    loadState(s) {
      this.cpu.loadState(s.cpu); this.z80.setState(s.z80);
      this.ram.set(s.ram); this.palRam.set(s.palRam); this.sprRam.set(s.sprRam); this.ioRam.set(s.ioRam); this.prot.set(s.prot); this.sndRam.set(s.sndRam);
      this.k052109.loadState(s.k052109); this.k053245.setState(s.k053245); this.k053251.setState(s.k053251);
      this.k053260.setState(s.k053260); this.ym.loadState(s.ym); this.eeprom.setState(s.eeprom);
      let vb, zi;
      [this.eepromOut, this.ctrl300, vb, this.nmiAt, this.zCyc, this.ymFrac, this.cyc, zi, this.dmaBusy, this.setupFrames] = s.m;
      this.dmaBusy = this.dmaBusy || 0; this.setupFrames = this.setupFrames || 0;
      this.vblank = !!vb; this.z80.irq = !!zi;
      this.k052109.rmrd = !!(this.ctrl300 & 0x08);
      for (let i = 0; i < 2048; i++) this.updateColour(i);
      this.resampler.clear();
      this.drawFrame();
    }
  }

  // The sprite ROMs hold each block of 64 tiles row by row (8 tiles across, then the next row),
  // but the K053245 numbers the tiles of a block in Konami's interleaved order (code bits 0, 2, 4
  // step across, bits 1, 3, 5 step down): the board wires the ROM address lines so the two meet.
  // Reorder the tiles once into the chip's numbering.
  function unscrambleSprites(rom) {
    const out = new Uint8Array(rom.length), n = rom.length >> 7;
    for (let c = 0; c < n; c++) {
      const x = (c & 1) | ((c >> 1) & 2) | ((c >> 2) & 4), y = ((c >> 1) & 1) | ((c >> 2) & 2) | ((c >> 3) & 4);
      const r = (c & ~0x3f) | x | (y << 3);
      out.set(rom.subarray(r << 7, (r + 1) << 7), c << 7);
    }
    return out;
  }

  // How far a part sits from its object's centre once the object is zoomed. The helper chip's
  // zoom value is 0x4f00 for normal size; RetroPixels' curve: bigger values push the parts out
  // faster than linearly, smaller ones pull them in until they meet at about 0x0f00.
  function scalePart(off, zoom) {
    const d = zoom - 0x4f00;
    if (d > 0) return off + Math.trunc(Math.pow(d >> 8, 1.89) * off / 600);
    if (d < 0) { const k = zoom + Math.floor(d * 15 / 64); return k > 0 ? Math.trunc(off * k / 0x4f00) : 0; }
    return off;
  }

  TMNT2Board.id = "tmnt2";
  TMNT2Board.title = "Konami TMNT2 (Turtles in Time)";
  TMNT2Board.width = W;
  TMNT2Board.height = H;
  TMNT2Board.rotate = 0;
  TMNT2Board.fps = FPS;
  TMNT2Board.players = 4;
  TMNT2Board.buttons = ["Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Service"];
  TMNT2Board.dips = [];

  // The chips every version shares; only the four main program ROMs differ (their names carry
  // the version's region letters).
  const SHARED = [
    { name: "063b01.2f", size: 0x10000, role: "sound" },
    { name: "063b12.16k", size: 0x80000, role: "tiles", lane: 0 },
    { name: "063b11.12k", size: 0x80000, role: "tiles", lane: 2 },
    { name: "063b09.7l", size: 0x100000, role: "sprites", lane: 0 },
    { name: "063b07.3l", size: 0x100000, role: "sprites", lane: 2 },
    { name: "063b10.7k", size: 0x80000, role: "sprites", base: 0x200000, lane: 0 },
    { name: "063b08.3k", size: 0x80000, role: "sprites", base: 0x200000, lane: 2 },
    { name: "063b06.1d", size: 0x200000, role: "pcm" },
  ];
  const version = (id, title, code, players, nv) => ({
    id, title, year: 1991, maker: "Konami", players, loose: true, parent: id === "tmnt2" ? undefined : "tmnt2",
    files: [
      { name: `063${code}02.8e`, size: 0x20000, role: "cpu", base: 0, lane: 0 },
      { name: `063${code}03.8g`, size: 0x20000, role: "cpu", base: 0, lane: 1 },
      { name: `063${code}04.10e`, size: 0x20000, role: "cpu", base: 0x40000, lane: 0 },
      { name: `063${code}05.10g`, size: 0x20000, role: "cpu", base: 0x40000, lane: 1 },
      ...SHARED,
      { name: nv, size: 0x80, role: "eeprom", optional: true },
    ],
  });
  TMNT2Board.games = [
    version("tmnt2", "Teenage Mutant Ninja Turtles - Turtles in Time (4 players, US)", "uaa", 4, "tmnt2_uaa.nv"),
    version("tmnt2a", "Teenage Mutant Ninja Turtles - Turtles in Time (4 players, Asia)", "ada", 4, "tmnt2_ada.nv"),
    version("tmnt22pu", "Teenage Mutant Ninja Turtles - Turtles in Time (2 players, US)", "uda", 2, "tmnt22pu.nv"),
    version("tmht22pe", "Teenage Mutant Hero Turtles - Turtles in Time (2 players, Europe)", "eba", 2, "tmht22pe.nv"),
    version("tmht24pe", "Teenage Mutant Hero Turtles - Turtles in Time (4 players, Europe)", "eaa", 4, "tmht24pe.nv"),
    version("tmnt22pj", "Teenage Mutant Ninja Turtles 2 (2 players, Japan)", "jba", 2, "tmnt22pj.nv"),
    version("tmnt24pj", "Teenage Mutant Ninja Turtles 2 (4 players, Japan)", "jaa", 4, "tmnt24pj.nv"),
  ];
  TMNT2Board.scalePart = scalePart;

  RPArcade.boards.push(TMNT2Board);
})(typeof window !== "undefined" ? window : globalThis);
