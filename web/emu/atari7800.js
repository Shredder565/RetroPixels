// RetroPixels — Atari 7800 emulator. Original code.
//
// 6502 ("Sally") processor, MARIA graphics chip (display-list DMA, all six
// graphics modes, character mode, holey DMA, display-list interrupts and the
// processor time MARIA's DMA takes), TIA sound, RIOT, POKEY sound and the
// cartridge bank-switching schemes in the .a78 header. The console's BIOS is
// not needed: the cartridge starts directly in 7800 mode.
// Uses the TIA sound generator and RIOT from atari2600.js.
"use strict";

(function (global) {
  const MCLK = 7159090;           // MARIA clock
  const LINE = 454;               // MARIA clocks per line
  const LINES = 263;
  const DMA_FIRST = 16, DMA_LAST = 258;
  const OUT_TOP = 17, OUT_H = 240;
  const FPS = MCLK / (LINE * LINES);

  function makePalette() {
    const out = new Uint32Array(256);
    for (let h = 0; h < 16; h++) for (let l = 0; l < 16; l++) {
      let y = l / 15, u = 0, v = 0;
      if (h) {
        const a = (180 - (h - 1) * 25.7) * Math.PI / 180;
        u = Math.cos(a) * 0.3; v = Math.sin(a) * 0.3;
        y = 0.1 + y * 0.85;
      } else y = y * 0.93;
      const c = (x) => Math.max(0, Math.min(255, Math.round(x * 255)));
      const r = c(y + 1.14 * v), g = c(y - 0.395 * u - 0.581 * v), b = c(y + 2.032 * u);
      out[h * 16 + l] = 0xff000000 | (b << 16) | (g << 8) | r;
    }
    return out;
  }
  const PALETTE = makePalette();
  const GREY = PALETTE.map((_, i) => PALETTE[i & 15]);

  // ------------------------------------------------------------------
  // POKEY sound (4 channels), clocked at the processor rate (1.79 MHz)
  // ------------------------------------------------------------------
  function lfsr(len, tap) {
    const n = (1 << len) - 1, out = new Uint8Array(n);
    let r = n;
    for (let i = 0; i < n; i++) { out[i] = r & 1; const fb = (r ^ (r >> tap)) & 1; r = (r >> 1) | (fb << (len - 1)); }
    return out;
  }
  const P4 = lfsr(4, 1), P5 = lfsr(5, 2), P9 = lfsr(9, 4), P17 = lfsr(17, 5);

  class POKEY {
    constructor() {
      this.audf = [0, 0, 0, 0]; this.audc = [0, 0, 0, 0]; this.audctl = 0;
      this.cnt = [1, 1, 1, 1]; this.out = [0, 0, 0, 0]; this.hp = [0, 0];
      this.t = 0; this.base = 0;
    }
    write(r, v) {
      switch (r & 15) {
        case 0: case 2: case 4: case 6: this.audf[(r & 15) >> 1] = v; break;
        case 1: case 3: case 5: case 7: this.audc[(r & 15) >> 1] = v; break;
        case 8: this.audctl = v; break;
        case 9: for (let i = 0; i < 4; i++) this.cnt[i] = this.period(i); break; // STIMER
      }
    }
    read(r) {
      if ((r & 15) === 10) { // RANDOM
        let v = 0;
        if (this.audctl & 0x80) for (let i = 0; i < 8; i++) v |= P9[(this.t + i) % 511] << i;
        else for (let i = 0; i < 8; i++) v |= P17[(this.t + i) % 131071] << i;
        return v;
      }
      return 0xff;
    }
    period(ch) {
      const ac = this.audctl, f = this.audf;
      if (ch === 1 && (ac & 0x10)) return (f[1] << 8 | f[0]) + ((ac & 0x40) ? 7 : 1);
      if (ch === 3 && (ac & 0x08)) return (f[3] << 8 | f[2]) + ((ac & 0x20) ? 7 : 1);
      if ((ch === 0 && (ac & 0x40)) || (ch === 2 && (ac & 0x20))) return f[ch] + 4;
      return f[ch] + 1;
    }
    pulse(ch) {
      const c = this.audc[ch], t = this.t;
      if (!(c & 0x80) && !P5[t % 31]) return;
      if (c & 0x20) this.out[ch] ^= 1;
      else if (c & 0x40) this.out[ch] = P4[t % 15];
      else this.out[ch] = (this.audctl & 0x80) ? P9[t % 511] : P17[t % 131071];
    }
    // Run n processor clocks; returns the average output (0..1).
    run(n) {
      const ac = this.audctl, j12 = ac & 0x10, j34 = ac & 0x08, f0 = ac & 0x40, f2 = ac & 0x20;
      const baseDiv = (ac & 1) ? 114 : 28;
      let sum = 0;
      for (let i = 0; i < n; i++) {
        this.t++;
        const base = ++this.base >= baseDiv; if (base) this.base = 0;
        // channel 1 (and the 1+2 pair)
        if (f0 || base) {
          if (j12) { if (--this.cnt[1] <= 0) { this.cnt[1] = this.period(1); this.pulse(1); } }
          else if (--this.cnt[0] <= 0) { this.cnt[0] = this.period(0); this.pulse(0); }
        }
        if (!j12 && base && --this.cnt[1] <= 0) { this.cnt[1] = this.period(1); this.pulse(1); }
        // channel 3 (and the 3+4 pair)
        if (f2 || base) {
          if (j34) { if (--this.cnt[3] <= 0) { this.cnt[3] = this.period(3); this.pulse(3); } }
          else if (--this.cnt[2] <= 0) { this.cnt[2] = this.period(2); this.pulse(2); if (ac & 4) this.hp[0] = this.out[0]; }
        }
        if (!j34 && base && --this.cnt[3] <= 0) { this.cnt[3] = this.period(3); this.pulse(3); if (ac & 2) this.hp[1] = this.out[1]; }
        if (!(i & 3) || i === n - 1) {
          let s = 0;
          for (let ch = 0; ch < 4; ch++) {
            const c = this.audc[ch], vol = c & 15;
            if (!vol) continue;
            if ((j12 && ch === 0) || (j34 && ch === 2)) continue;
            let bit = (c & 0x10) ? 1 : this.out[ch];
            if (ch === 0 && (ac & 4)) bit ^= this.hp[0];
            if (ch === 1 && (ac & 2)) bit ^= this.hp[1];
            if (bit) s += vol;
          }
          sum += s * (i === n - 1 ? (n - 1) % 4 + 1 : 4);
        }
      }
      return sum / n / 60;
    }
    saveState() { return { audf: this.audf.slice(), audc: this.audc.slice(), audctl: this.audctl, cnt: this.cnt.slice(), out: this.out.slice(), hp: this.hp.slice(), t: this.t, base: this.base }; }
    loadState(o) { for (const k in o) this[k] = Array.isArray(o[k]) ? o[k].slice() : o[k]; }
  }

  // ------------------------------------------------------------------
  // Cartridge
  // ------------------------------------------------------------------
  function parseCart(data) {
    let rom = data, type = 0, name = "";
    if (data.length > 128 && String.fromCharCode(...data.subarray(1, 10)) === "ATARI7800") {
      type = (data[53] << 8) | data[54];
      for (let i = 17; i < 49 && data[i]; i++) name += String.fromCharCode(data[i]);
      rom = data.subarray(128);
    }
    // Headerless dumps: guess from the size.
    if (!type && rom.length > 49152) type = rom.length === 147456 ? 0x0a : 0x02;
    return { rom, type, name: name.trim() };
  }

  class Cart {
    constructor(data) {
      const { rom, type } = parseCart(data);
      this.rom = rom; this.type = type;
      this.pokey = (type & 1) || (type & 0x40) ? new POKEY() : null;
      this.pokeyAt = (type & 0x40) ? 0x0450 : 0x4000;
      this.ram4000 = (type & 4) ? new Uint8Array(0x4000) : null;
      this.bank = 0;
      if (type & 0x0100) this.kind = "activision";
      else if (type & 0x0200) this.kind = "absolute";
      else if (type & 0x02) this.kind = "supergame";
      else this.kind = "flat";
      this.banks = rom.length >> 14;
      this.extra = (type & 0x08) ? 1 : 0; // 144K carts: an extra first bank sits at $4000
      if (this.kind === "flat") this.base = 0x10000 - rom.length;
    }
    read(a) {
      const rom = this.rom;
      if (this.pokey && this.pokeyAt === 0x4000 && a < 0x8000 && !this.ram4000) return this.pokey.read(a);
      switch (this.kind) {
        case "flat": return a >= this.base ? rom[a - this.base] : 0xff;
        case "supergame": {
          if (a >= 0xc000) return rom[(this.banks - 1) * 0x4000 + (a - 0xc000)];
          if (a >= 0x8000) return rom[((this.bank + this.extra) % this.banks) * 0x4000 + (a - 0x8000)];
          if (this.ram4000) return this.ram4000[a - 0x4000];
          if (this.extra) return rom[a - 0x4000];
          return rom[Math.max(0, this.banks - 2) * 0x4000 + (a - 0x4000)];
        }
        case "activision": {
          // fixed 8K pieces, with a switchable 16K window at $A000-$DFFF
          if (a >= 0xa000 && a < 0xe000) return rom[this.bank * 0x4000 + (a - 0xa000)];
          const off = a & 0x1fff;
          switch (a & 0xe000) {
            case 0x4000: return rom[0x1a000 + off];
            case 0x6000: return rom[0x18000 + off];
            case 0x8000: return rom[0x1e000 + off];
            default: return rom[0x1c000 + off];
          }
        }
        case "absolute":
          if (a < 0x8000) return rom[this.bank * 0x4000 + (a - 0x4000)];
          return rom[0x8000 + (a - 0x8000)];
      }
      return 0xff;
    }
    write(a, v) {
      if (this.pokey && this.pokeyAt === 0x4000 && a < 0x8000 && !this.ram4000) { this.pokey.write(a, v); return; }
      // Some SuperGame dumps don't flag their $4000 RAM; a write there means it's present.
      if (!this.ram4000 && this.kind === "supergame" && a < 0x8000 && !this.extra) {
        this.ram4000 = new Uint8Array(0x4000);
        for (let i = 0; i < 0x4000; i++) this.ram4000[i] = this.read(0x4000 + i);
      }
      if (this.ram4000 && a < 0x8000) { this.ram4000[a - 0x4000] = v; return; }
      switch (this.kind) {
        case "supergame": if (a >= 0x8000 && a < 0xc000) this.bank = v & 0xff; return;
        case "activision": if (a >= 0xff80 && a <= 0xff8f) this.bank = a & 7; return;
        case "absolute": if (a === 0x8000) { if (v & 1) this.bank = 0; else if (v & 2) this.bank = 1; } return;
      }
    }
    saveState() { return { bank: this.bank, ram4000: this.ram4000, pokey: this.pokey ? this.pokey.saveState() : null }; }
    loadState(o) { this.bank = o.bank; if (o.ram4000) { if (!this.ram4000) this.ram4000 = new Uint8Array(0x4000); this.ram4000.set(o.ram4000); } if (this.pokey && o.pokey) this.pokey.loadState(o.pokey); }
  }

  // ------------------------------------------------------------------
  // The console
  // ------------------------------------------------------------------
  const UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, B1 = 16, B2 = 32, PAUSE = 64, SELECT = 128, RESET = 256, LDIFF = 512, RDIFF = 1024;

  class Atari7800 {
    constructor(data, opts = {}) {
      this.data = data;
      this.sampleRate = opts.sampleRate || 48000;
      this.width = 320; this.height = OUT_H; this.fps = FPS;
      this.screen = new Uint32Array(320 * OUT_H);
      this.input = [0, 0];
      this.init();
    }
    init() {
      this.cart = new Cart(this.data);
      this.ram = new Uint8Array(0x1000);
      for (let i = 0; i < this.ram.length; i++) this.ram[i] = (i * 151 + 7) & 0xff;
      this.riot = new global.RPAtari.RIOT();
      this.sound = new global.RPAtari.TIASound(this.sampleRate, MCLK / 228);
      this.maria = new Uint8Array(32);       // $20-$3F
      this.ctrl = 0; this.charbase = 0; this.dpp = 0;
      this.line = 0; this.mclk = 0; this.dmaDone = false; this.lineHadDma = false;
      this.audClk = 0; this.riotFrac = 0; this.pokeyClk = 0;
      this.wsync = false; this.frameDone = false;
      // display-list state
      this.dll = 0; this.dl = 0; this.zoneOff = 0; this.h16 = false; this.h8 = false; this.wmode = 0; this.zoneActive = false;
      this.lb = new Uint8Array(512);
      this.tiaVblank = 0; this.latch = [false, false];
      const self = this;
      this.cpu = new global.RPCPU6502({ read(a) { return self.read(a); }, write(a, v) { self.write(a, v); } }, { decimal: true });
      this.updatePorts();
      this.cpu.reset();
    }

    // ---- timing ----
    // Spend c MARIA clocks; runs the line's DMA and line ends as they come.
    spend(c) {
      this.pass(c);
      if (!this.dmaDone && this.mclk >= 28) this.dma();
      while (this.mclk >= LINE) this.endLine();
    }
    // time passing (sound, timer) — also while MARIA holds the processor
    pass(c) {
      this.mclk += c;
      this.audClk += c;
      while (this.audClk >= 228) {
        this.audClk -= 228;
        this.sound.tick();
        if (this.cart.pokey) this.sound.raw[this.sound.rawLen - 1] += this.cart.pokey.run(57) * 0.9;
      }
      this.riotFrac += c;
      while (this.riotFrac >= 4) { this.riotFrac -= 4; this.riot.tick(); }
    }
    access(a) {
      // TIA and RIOT run off the slow clock: those accesses take 1.5 times as long.
      const slow = a < 0x400 && ((a & 0x3e0) === 0 || (a & 0x380) === 0x280) || (a >= 0x480 && a < 0x500);
      this.spend(slow ? 6 : 4);
    }
    endLine() {
      this.mclk -= LINE;
      // show the line MARIA prepared
      const row = this.line + 1 - OUT_TOP;
      if (row >= 0 && row < OUT_H) {
        const pal = (this.ctrl & 0x80) ? GREY : PALETTE, m = this.maria, o = row * 320, scr = this.screen;
        if (this.lineHadDma) {
          const lb = this.lb;
          for (let x = 0; x < 320; x++) { const i = lb[x]; scr[o + x] = pal[(i & 3) ? m[i] : m[0]]; }
        } else scr.fill(pal[m[0]], o, o + 320);
      }
      this.lineHadDma = false;
      this.line++;
      if (this.line >= LINES) { this.line = 0; this.frameDone = true; this.zoneActive = false; }
      this.dmaDone = false;
    }
    vblank() { return this.line < DMA_FIRST || this.line > DMA_LAST; }

    // ---- MARIA ----
    dmaOn() { return ((this.ctrl >> 5) & 3) === 2; }
    loadZone() {
      const r = (a) => this.peek(a);
      const b0 = r(this.dll);
      this.zoneOff = b0 & 15; this.h16 = !!(b0 & 0x40); this.h8 = !!(b0 & 0x20);
      this.dl = (r(this.dll + 1) << 8) | r(this.dll + 2);
      this.dll = (this.dll + 3) & 0xffff;
      return b0 & 0x80; // DLI
    }
    dma() {
      this.dmaDone = true;
      const line = this.line;
      if (line === DMA_FIRST - 1 && this.dmaOn()) {
        // start of the display: fetch the first zone
        this.dll = this.dpp; this.zoneActive = true;
        const dli = this.loadZone();
        this.pass(24);
        if (dli) this.cpu.nmiPending = true;
        return;
      }
      if (line < DMA_FIRST || line > DMA_LAST || !this.dmaOn() || !this.zoneActive) return;
      let cost = 16;
      this.lb.fill(0);
      cost += this.drawLine();
      this.lineHadDma = true;
      let dli = 0;
      if (this.zoneOff === 0) { dli = this.loadZone(); cost += 24; }
      else this.zoneOff--;
      this.pass(Math.min(cost, 420));
      if (dli) this.cpu.nmiPending = true;
    }
    drawLine() {
      const r = (a) => this.peek(a);
      let p = this.dl, cost = 0, guard = 0;
      const off = this.zoneOff, ctrl = this.ctrl, rm = ctrl & 3, kang = !!(ctrl & 4), cw = (ctrl & 0x10) ? 2 : 1;
      while (guard++ < 128) {
        const b1 = r(p + 1);
        if (!(b1 & 0x5f)) break; // end of display list
        let lo, hi, pw, hpos, ind = false;
        if (!(b1 & 0x1f)) { // 5-byte header
          lo = r(p); hi = r(p + 2); pw = r(p + 3); hpos = r(p + 4);
          this.wmode = b1 & 0x80; ind = !!(b1 & 0x20);
          p += 5; cost += 12;
        } else { lo = r(p); hi = r(p + 2); pw = b1; hpos = r(p + 3); p += 4; cost += 8; }
        const width = 32 - (pw & 0x1f) || 32, palette = pw >> 5;
        let x = hpos * 2;
        if (!ind) {
          const addr = (((hi + off) & 0xff) << 8) | lo;
          if (this.holey(addr)) { cost += width * 3; continue; } // nothing fetched
          for (let i = 0; i < width; i++) { x = this.plot(r((addr + i) & 0xffff), x, palette, rm, kang); }
          cost += width * 3;
        } else {
          const cbase = (((this.charbase + off) & 0xff) << 8);
          for (let i = 0; i < width; i++) {
            const ch = r(((hi << 8) | lo) + i & 0xffff);
            const ga = (cbase + ch) & 0xffff;
            cost += 3;
            if (this.holey(ga)) { cost += 3 * cw; x = (x + this.byteWidth(rm) * cw) & 511; continue; }
            for (let k = 0; k < cw; k++) x = this.plot(r((ga + k) & 0xffff), x, palette, rm, kang);
            cost += 3 * cw;
          }
        }
      }
      return cost;
    }
    // hi-res pixels covered by one graphics byte in each mode
    byteWidth(rm) { return this.wmode ? (rm === 3 || rm === 0 ? 4 : 8) : (rm === 2 ? 4 : 8); }
    holey(a) { return (this.h16 && (a & 0x9000) === 0x9000) || (this.h8 && (a & 0x8800) === 0x8800); }
    // Draw one graphics byte at hi-res position x; returns the next position.
    plot(d, x, P, rm, kang) {
      const lb = this.lb;
      const put = (pos, idx) => { lb[pos & 511] = idx; };
      if (rm === 0) {
        if (!this.wmode) { // 160A: 4 pixels x 2 bits
          for (let i = 0; i < 4; i++) {
            const c = (d >> (6 - 2 * i)) & 3;
            if (c || kang) { const v = c ? P * 4 + c : 0; put(x, v); put(x + 1, v); }
            x += 2;
          }
        } else { // 160B: 2 pixels x (2 colour + 2 palette bits)
          for (let i = 0; i < 2; i++) {
            const c = (d >> (i ? 4 : 6)) & 3, pp = (d >> (i ? 0 : 2)) & 3;
            if (c || kang) { const v = c ? ((P & 4) | pp) * 4 + c : 0; put(x, v); put(x + 1, v); }
            x += 2;
          }
        }
      } else if (rm === 2) {
        if (!this.wmode) { // 320B: 4 pixels x 2 bits
          for (let i = 0; i < 4; i++) {
            const c = (((d >> (7 - i)) & 1) << 1) | ((d >> (3 - i)) & 1);
            if (c || kang) put(x, c ? P * 4 + c : 0);
            x += 1;
          }
        } else { // 320D: 8 pixels; the second colour bit comes from the palette number
          for (let i = 0; i < 8; i++) {
            const c = (((d >> (7 - i)) & 1) << 1) | ((P >> ((i & 1) ? 0 : 1)) & 1);
            if (c || kang) put(x, c ? (P & 4) * 4 + c : 0);
            x += 1;
          }
        }
      } else if (rm === 3) {
        if (!this.wmode) { // 320A: 8 pixels x 1 bit, colour 2
          for (let i = 0; i < 8; i++) {
            const on = (d >> (7 - i)) & 1;
            if (on) put(x, P * 4 + 2); else if (kang) put(x, 0);
            x += 1;
          }
        } else { // 320C: 4 pixels on/off, each pair has its own palette
          for (let i = 0; i < 4; i++) {
            const on = (d >> (7 - i)) & 1, pp = (d >> (i < 2 ? 2 : 0)) & 3;
            if (on) put(x, ((P & 4) | pp) * 4 + 2); else if (kang) put(x, 0);
            x += 1;
          }
        }
      } else x += 8;
      return x & 511;
    }

    // ---- memory ----
    // read without side effects or time (MARIA's DMA)
    peek(a) {
      a &= 0xffff;
      if (a >= 0x4000) return this.cart.read(a);
      if (a >= 0x1800) return a < 0x2800 ? this.ram[a - 0x1800] : this.ram[0x800 + (a & 0x7ff)];
      if (a < 0x400) {
        const lo = a & 0xff;
        if (lo >= 0x40 && !(a & 0x200)) return this.ram[0x800 + (a & 0x1ff)];
      }
      if (a >= 0x480 && a < 0x500) return this.riot.ram[a & 0x7f];
      return 0;
    }
    read(a) {
      if (this.wsync) { this.wsync = false; this.spend(LINE - this.mclk); }
      this.access(a);
      a &= 0xffff;
      if (a >= 0x4000) return this.cart.read(a);
      if (a >= 0x1800) return a < 0x2800 ? this.ram[a - 0x1800] : this.ram[0x800 + (a & 0x7ff)];
      if (a < 0x400) {
        const lo = a & 0xff;
        if (a < 0x200) {
          if (lo < 0x20) return this.tiaRead(lo);
          if (lo < 0x40) return this.mariaRead(lo);
          return this.ram[0x800 + (a & 0x1ff)];
        }
        if (a >= 0x280 && a < 0x300) return this.riot.read(a);
        if (this.cart.pokey && this.cart.pokeyAt === 0x450 && a >= 0x450 && a < 0x460) return this.cart.pokey.read(a);
        return 0;
      }
      if (a >= 0x480 && a < 0x500) return this.riot.ram[a & 0x7f];
      return 0;
    }
    write(a, v) {
      this.access(a);
      a &= 0xffff;
      if (a >= 0x4000) { this.cart.write(a, v); return; }
      if (a >= 0x1800) { if (a < 0x2800) this.ram[a - 0x1800] = v; else this.ram[0x800 + (a & 0x7ff)] = v; return; }
      if (a < 0x400) {
        const lo = a & 0xff;
        if (a < 0x200) {
          if (lo < 0x20) { this.tiaWrite(lo, v); return; }
          if (lo < 0x40) { this.mariaWrite(lo, v); return; }
          this.ram[0x800 + (a & 0x1ff)] = v; return;
        }
        if (a >= 0x280 && a < 0x300) { this.riot.write(a, v); return; }
        if (this.cart.pokey && this.cart.pokeyAt === 0x450 && a >= 0x450 && a < 0x460) this.cart.pokey.write(a, v);
        return;
      }
      if (a >= 0x480 && a < 0x500) this.riot.ram[a & 0x7f] = v;
    }
    mariaRead(r) {
      if (r === 0x28) return this.vblank() ? 0x80 : 0;
      return 0;
    }
    mariaWrite(r, v) {
      switch (r) {
        case 0x24: this.wsync = true; return;
        case 0x2c: this.dpp = (v << 8) | (this.dpp & 0xff); return;
        case 0x30: this.dpp = (this.dpp & 0xff00) | v; return;
        case 0x34: this.charbase = v; return;
        case 0x3c: this.ctrl = v; return;
        case 0x28: case 0x38: return;
      }
      this.maria[r - 0x20] = v;
    }
    tiaRead(r) {
      r &= 15;
      const p = r < 10 ? 0 : 1;
      switch (r) {
        case 8: case 10: return (this.input[p] & B2) ? 0x80 : 0;   // right button
        case 9: case 11: return (this.input[p] & B1) ? 0x80 : 0;   // left button
        case 12: case 13: {
          const q = r - 12, down = !!(this.input[q] & (B1 | B2));
          if (this.tiaVblank & 0x40) { if (down) this.latch[q] = true; return this.latch[q] ? 0 : 0x80; }
          return down ? 0 : 0x80;
        }
      }
      return 0;
    }
    tiaWrite(r, v) {
      if (r === 1) { this.tiaVblank = v; if (!(v & 0x40)) this.latch = [false, false]; return; }
      if (r >= 0x15 && r <= 0x1a) this.sound.write(r - 0x15, v);
    }

    // ---- controllers ----
    updatePorts() {
      const dirs = (b) => (b & UP ? 1 : 0) | (b & DOWN ? 2 : 0) | (b & LEFT ? 4 : 0) | (b & RIGHT ? 8 : 0);
      this.riot.portA = 0xff & ~(dirs(this.input[0]) << 4) & ~dirs(this.input[1]);
      const s = this.input[0];
      this.riot.portB = (s & RESET ? 0 : 1) | (s & SELECT ? 0 : 2) | (s & PAUSE ? 0 : 8) | (s & LDIFF ? 0x40 : 0) | (s & RDIFF ? 0x80 : 0) | 0x34;
    }
    setInput(p, bits) { if (p < 2) { this.input[p] = bits; this.updatePorts(); } }

    // ---- frontend interface ----
    runFrame() {
      this.frameDone = false;
      let guard = 0;
      while (!this.frameDone && guard++ < 200000) this.cpu.step();
    }
    takeAudio() { return this.sound.take(); }
    reset() { const keep = this.input.slice(); this.init(); this.input = keep; this.updatePorts(); }
    getSRAM() { return null; }
    saveState() {
      const o = { v: 1, cpu: this.cpu.saveState(), ram: this.ram, maria: this.maria, riot: this.riot.saveState(), sound: this.sound.saveState(), cart: this.cart.saveState() };
      for (const k of ["ctrl", "charbase", "dpp", "line", "mclk", "dmaDone", "lineHadDma", "audClk", "riotFrac", "wsync", "dll", "dl", "zoneOff", "h16", "h8", "wmode", "zoneActive", "tiaVblank"]) o[k] = this[k];
      o.lb = this.lb; o.latch = this.latch.slice();
      return o;
    }
    loadState(s) {
      this.cpu.loadState(s.cpu); this.ram.set(s.ram); this.maria.set(s.maria); this.riot.loadState(s.riot); this.sound.loadState(s.sound); this.cart.loadState(s.cart);
      for (const k of ["ctrl", "charbase", "dpp", "line", "mclk", "dmaDone", "lineHadDma", "audClk", "riotFrac", "wsync", "dll", "dl", "zoneOff", "h16", "h8", "wmode", "zoneActive", "tiaVblank"]) this[k] = s[k];
      this.lb.set(s.lb); this.latch = s.latch.slice();
      this.updatePorts();
    }
  }
  Atari7800.info = { system: "atari7800", name: "Atari 7800", width: 320, height: OUT_H, aspect: 4 / 3, fps: FPS, players: 2,
    buttons: ["Up", "Down", "Left", "Right", "Button 1", "Button 2", "Pause", "Select", "Reset", "Left difficulty", "Right difficulty"] };

  global.RPCores = global.RPCores || {};
  global.RPCores.atari7800 = Atari7800;
})(typeof window !== "undefined" ? window : globalThis);
