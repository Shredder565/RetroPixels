// RetroPixels — Atari 2600 emulator. Original code.
//
// 6507 processor (the shared 6502 core with decimal mode), TIA video and sound,
// RIOT (RAM, I/O, timer) and the common cartridge bank-switching schemes.
// The TIA picture is drawn lazily: pixels are produced up to "now" whenever a
// register that affects them changes and at the end of every line, so the
// result is exact to the colour clock without simulating every clock one by one.
// Also exports the TIA sound generator and the RIOT for the Atari 7800.
"use strict";

(function (global) {
  const CLOCK = 3579545;               // colour clocks per second (NTSC)
  const LINE = 228;                    // colour clocks per line
  const HBLANK = 68;
  const MAXL = 330;                    // lines kept per frame
  const OUT_H = 210;                   // lines shown
  const FPS = CLOCK / (LINE * 262);

  // ------------------------------------------------------------------
  // NTSC palette, computed from the TIA's colour signal: 16 hues (hue 0 is
  // grey, hues 1–15 step around the colour wheel from the burst phase) and
  // 8 brightness levels.
  // ------------------------------------------------------------------
  function makePalette(pal) {
    const LUMA = [0.0, 0.25, 0.4, 0.53, 0.65, 0.75, 0.84, 0.92];
    const out = new Uint32Array(128);
    for (let h = 0; h < 16; h++) for (let l = 0; l < 8; l++) {
      let y = LUMA[l], u = 0, v = 0;
      if (h) {
        const deg = 180 - (h - 1) * 25.7;
        const a = deg * Math.PI / 180, sat = 0.3;
        u = Math.cos(a) * sat; v = Math.sin(a) * sat;
        y = 0.1 + y * 0.85; // colour levels never reach black
      }
      const c = (x) => Math.max(0, Math.min(255, Math.round(x * 255)));
      const r = c(y + 1.14 * v), g = c(y - 0.395 * u - 0.581 * v), b = c(y + 2.032 * u);
      out[h * 8 + l] = 0xff000000 | (b << 16) | (g << 8) | r;
    }
    return out;
  }
  const PALETTE = makePalette(false);

  // ------------------------------------------------------------------
  // TIA sound: two channels, clocked twice per line (~31.4 kHz).
  // ------------------------------------------------------------------
  function polyBits(len, taps) {
    const out = new Uint8Array((1 << len) - 1);
    let r = (1 << len) - 1;
    for (let i = 0; i < out.length; i++) {
      out[i] = r & 1;
      let fb = 0; for (const t of taps) fb ^= (r >> t) & 1;
      r = (r >> 1) | (fb << (len - 1));
    }
    return out;
  }
  const POLY4 = polyBits(4, [0, 1]);    // period 15
  const POLY5 = polyBits(5, [0, 2]);    // period 31
  const POLY9 = polyBits(9, [0, 4]);    // period 511
  const DIV31 = new Uint8Array(31); DIV31[0] = 1; DIV31[18] = 1;

  class TIASound {
    constructor(sampleRate, inRate) {
      this.rate = sampleRate; this.inRate = inRate || CLOCK / 114;
      this.audc = [0, 0]; this.audf = [0, 0]; this.audv = [0, 0];
      this.div = [0, 0]; this.p4 = [0, 0]; this.p5 = [0, 0]; this.p9 = [0, 0]; this.out = [0, 0];
      this.raw = new Float32Array(4096); this.rawLen = 0;
      this.frac = 0; this.last = 0; this.hp = 0; this.prev = 0;
      this.buf = new Float32Array(4096); this.bufLen = 0;
    }
    write(r, v) { // r: 0 AUDC0, 1 AUDC1, 2 AUDF0, 3 AUDF1, 4 AUDV0, 5 AUDV1
      const ch = r & 1;
      if (r < 2) this.audc[ch] = v & 15; else if (r < 4) this.audf[ch] = v & 31; else this.audv[ch] = v & 15;
    }
    // one ~31.4 kHz step
    tick() {
      let mix = 0;
      for (let ch = 0; ch < 2; ch++) {
        const c = this.audc[ch];
        const top = (c & 0x0c) === 0x0c ? (this.audf[ch] + 1) * 3 - 1 : this.audf[ch];
        if (++this.div[ch] > top) {
          this.div[ch] = 0;
          if (c === 0 || c === 0x0b) this.out[ch] = 1;
          else {
            this.p5[ch] = (this.p5[ch] + 1) % 31;
            const p5 = this.p5[ch];
            if ((c & 2) === 0 || ((c & 1) === 0 && DIV31[p5]) || ((c & 1) && POLY5[p5])) {
              if (c & 4) this.out[ch] ^= 1;
              else if (c & 8) {
                if (c === 8) { this.p9[ch] = (this.p9[ch] + 1) % 511; this.out[ch] = POLY9[this.p9[ch]]; }
                else this.out[ch] = POLY5[p5];
              } else { this.p4[ch] = (this.p4[ch] + 1) % 15; this.out[ch] = POLY4[this.p4[ch]]; }
            }
          }
        }
        if (this.out[ch]) mix += this.audv[ch];
      }
      if (this.rawLen < this.raw.length) this.raw[this.rawLen++] = mix / 30;
    }
    // Convert what was produced this frame to the output rate.
    flush() {
      const step = this.inRate / this.rate;
      let pos = this.frac;
      const raw = this.raw, n = this.rawLen;
      while (pos < n) {
        const i = pos | 0, f = pos - i;
        const a = i === 0 ? this.last : raw[i - 1], b = raw[i];
        const s = a + (b - a) * f;
        this.hp = 0.995 * (this.hp + s - this.prev); this.prev = s;
        if (this.bufLen < this.buf.length) this.buf[this.bufLen++] = this.hp * 0.9;
        pos += step;
      }
      this.frac = pos - n;
      if (n) this.last = raw[n - 1];
      this.rawLen = 0;
    }
    take() { this.flush(); const a = this.buf.slice(0, this.bufLen); this.bufLen = 0; return a; }
    saveState() { return { audc: this.audc.slice(), audf: this.audf.slice(), audv: this.audv.slice(), div: this.div.slice(), p4: this.p4.slice(), p5: this.p5.slice(), p9: this.p9.slice(), out: this.out.slice(),
      frac: this.frac, last: this.last, hp: this.hp, prev: this.prev }; }
    loadState(o) { for (const k of ["audc", "audf", "audv", "div", "p4", "p5", "p9", "out"]) this[k] = o[k].slice(); for (const k of ["frac", "last", "hp", "prev"]) if (o[k] !== undefined) this[k] = o[k]; this.rawLen = 0; this.bufLen = 0; }
  }

  // ------------------------------------------------------------------
  // RIOT (6532): 128 bytes of RAM, two I/O ports, interval timer.
  // ------------------------------------------------------------------
  class RIOT {
    constructor() {
      this.ram = new Uint8Array(128);
      for (let i = 0; i < 128; i++) this.ram[i] = (i * 73 + 41) & 0xff; // power-on garbage
      this.timer = 0xff; this.interval = 1024; this.sub = 1024; this.flag = false; this.pa7 = false;
      this.ddra = 0; this.ddrb = 0; this.ora = 0; this.orb = 0;
      this.portA = 0xff; this.portB = 0x0b; // set by the machine (inputs)
    }
    tick() {
      if (--this.sub <= 0) {
        this.timer = (this.timer - 1) & 0xff;
        if (this.timer === 0xff) this.flag = true;
        this.sub = this.flag ? 1 : this.interval;
      }
    }
    read(a) {
      if (a & 4) {
        if (a & 1) { const v = (this.flag ? 0x80 : 0) | (this.pa7 ? 0x40 : 0); this.pa7 = false; return v; }
        this.flag = false; return this.timer;
      }
      switch (a & 3) {
        case 0: return (this.ora & this.ddra) | (this.portA & ~this.ddra);
        case 1: return this.ddra;
        case 2: return (this.orb & this.ddrb) | (this.portB & ~this.ddrb);
        default: return this.ddrb;
      }
    }
    write(a, v) {
      if (a & 4) {
        if (a & 0x10) { this.interval = [1, 8, 64, 1024][a & 3]; this.timer = v; this.sub = 1; this.flag = false; }
        return;
      }
      switch (a & 3) {
        case 0: this.ora = v; break;
        case 1: this.ddra = v; break;
        case 2: this.orb = v; break;
        default: this.ddrb = v;
      }
    }
    saveState() { return { ram: this.ram, timer: this.timer, interval: this.interval, sub: this.sub, flag: this.flag, pa7: this.pa7, ddra: this.ddra, ddrb: this.ddrb, ora: this.ora, orb: this.orb }; }
    loadState(o) { this.ram.set(o.ram); for (const k of ["timer", "interval", "sub", "flag", "pa7", "ddra", "ddrb", "ora", "orb"]) this[k] = o[k]; }
  }

  // ------------------------------------------------------------------
  // TIA video
  // ------------------------------------------------------------------
  const COPIES = [[0], [0, 16], [0, 32], [0, 16, 32], [0, 64], [0], [0, 32, 64], [0]];

  // collision bit for every pair (bit index -> register/bit, see read())
  // object bits: 1 P0, 2 P1, 4 M0, 8 M1, 16 BL, 32 PF
  const CX = new Uint16Array(64);
  (function () {
    const pairs = [[4, 2, 0], [4, 1, 1], [8, 1, 2], [8, 2, 3], [1, 32, 4], [1, 16, 5], [2, 32, 6], [2, 16, 7],
      [4, 32, 8], [4, 16, 9], [8, 32, 10], [8, 16, 11], [16, 32, 12], [1, 2, 14], [4, 8, 15]];
    for (let m = 0; m < 64; m++) { let c = 0; for (const [a, b, bit] of pairs) if ((m & a) && (m & b)) c |= 1 << bit; CX[m] = c; }
  })();

  // playfield bit index (0..19) for each of the 40 four-pixel blocks
  const PF_LEFT = [];
  for (let i = 0; i < 20; i++) PF_LEFT.push(i);

  class TIA {
    constructor(m) {
      this.m = m;
      this.lines = new Uint32Array(MAXL * 160);
      this.screen = new Uint32Array(160 * OUT_H);
      this.hclock = 0; this.line = 0; this.rendered = 0;
      this.frameDone = false; this.frameLines = 262;
      this.ystart = 34; this.fitWant = -1; this.fitCount = 0;
      this.firstVis = -1; this.lastVis = -1;
      this.vsync = 0; this.vblank = 0;
      this.colup0 = 0; this.colup1 = 0; this.colupf = 0; this.colubk = 0;
      this.ctrlpf = 0; this.refp0 = 0; this.refp1 = 0; this.pf0 = 0; this.pf1 = 0; this.pf2 = 0; this.pfBits = 0;
      this.grp0n = 0; this.grp0o = 0; this.grp1n = 0; this.grp1o = 0;
      this.enam0 = 0; this.enam1 = 0; this.enabln = 0; this.enablo = 0;
      this.vdelp0 = 0; this.vdelp1 = 0; this.vdelbl = 0; this.resmp0 = 0; this.resmp1 = 0;
      this.nusiz0 = 0; this.nusiz1 = 0;
      this.hmp0 = 0; this.hmp1 = 0; this.hmm0 = 0; this.hmm1 = 0; this.hmbl = 0;
      this.p0 = 0; this.p1 = 0; this.m0 = 0; this.m1 = 0; this.bl = 0;
      this.noMain0 = false; this.noMain1 = false;
      this.hmoveBlank = false; this.lateHmove = false;
      this.cx = 0;
      this.latch = 0; this.fireLatched = [false, false];
      this.wsync = false;
      this.dumpStart = 0;
    }

    // ---- timing ----
    clock3() {
      const h = this.hclock + 3;
      if (h >= 114 && this.hclock < 114) this.m.sound.tick();
      if (h >= LINE) {
        this.renderTo(LINE);
        this.endLine();
        this.hclock = h - LINE;
        this.m.sound.tick();
      } else this.hclock = h;
    }
    endLine() {
      if (!(this.vblank & 2) && this.line < MAXL) { if (this.firstVis < 0) this.firstVis = this.line; this.lastVis = this.line; }
      this.line++;
      this.rendered = 0; this.hmoveBlank = false; this.noMain0 = this.noMain1 = false;
      if (this.lateHmove) { this.lateHmove = false; this.applyHmove(8); }
      if (this.line >= 300 && this.line >= this.frameLines + 40) this.endFrame(); // game isn't sending VSYNC
      else if (this.line >= MAXL + 20) this.endFrame();
    }
    endFrame() {
      const n = Math.min(this.line, MAXL);
      this.frameLines = Math.max(200, this.line);
      // keep the area the game draws on screen (like a TV), adjusting only when it stays put
      let want = this.ystart;
      if (this.firstVis >= 0) {
        if (this.firstVis < this.ystart) want = this.firstVis;
        else if (this.lastVis >= this.ystart + OUT_H) want = Math.min(this.firstVis, this.lastVis - OUT_H + 1);
      }
      want = Math.max(0, Math.min(want, MAXL - OUT_H));
      if (want !== this.ystart) {
        if (want === this.fitWant) { if (++this.fitCount >= 3) { this.ystart = want; this.fitCount = 0; } }
        else { this.fitWant = want; this.fitCount = 1; }
      } else this.fitCount = 0;
      for (let y = n; y < MAXL; y++) this.lines.fill(0xff000000, y * 160, y * 160 + 160);
      this.screen.set(this.lines.subarray(this.ystart * 160, (this.ystart + OUT_H) * 160));
      this.frameDone = true;
      this.line = 0; this.firstVis = -1; this.lastVis = -1;
    }

    // ---- drawing ----
    renderTo(end) {
      const start = this.rendered;
      if (end <= start) return;
      this.rendered = end;
      if (end <= HBLANK || this.line >= MAXL) return;
      const x0 = Math.max(start, HBLANK) - HBLANK, x1 = end - HBLANK;
      const buf = this.lines, row = this.line * 160;
      if (this.vblank & 2) { buf.fill(0xff000000, row + x0, row + x1); return; }
      const P = PALETTE;
      const c0 = P[this.colup0 >> 1], c1 = P[this.colup1 >> 1], cpf = P[this.colupf >> 1], cbk = P[this.colubk >> 1];
      const g0 = this.vdelp0 ? this.grp0o : this.grp0n, g1 = this.vdelp1 ? this.grp1o : this.grp1n;
      const ebl = this.vdelbl ? this.enablo : this.enabln;
      const em0 = this.enam0 && !this.resmp0, em1 = this.enam1 && !this.resmp1;
      const pri = this.ctrlpf & 4, score = (this.ctrlpf & 2) && !pri, reflect = this.ctrlpf & 1;
      const pfb = this.pfBits;
      const ns0 = this.nusiz0 & 7, ns1 = this.nusiz1 & 7;
      const ms0 = 1 << ((this.nusiz0 >> 4) & 3), ms1 = 1 << ((this.nusiz1 >> 4) & 3), bs = 1 << ((this.ctrlpf >> 4) & 3);
      const cp0 = COPIES[ns0], cp1 = COPIES[ns1];
      let cx = this.cx;
      for (let x = x0; x < x1; x++) {
        if (this.hmoveBlank && x < 8) { buf[row + x] = 0xff000000; continue; }
        let m = 0;
        // playfield
        const blk = x >> 2;
        const idx = blk < 20 ? blk : (reflect ? 39 - blk : blk - 20);
        if ((pfb >> idx) & 1) m |= 32;
        // players
        if (g0 && this.playerBit(x, this.p0, ns0, cp0, this.refp0, g0, this.noMain0)) m |= 1;
        if (g1 && this.playerBit(x, this.p1, ns1, cp1, this.refp1, g1, this.noMain1)) m |= 2;
        if (em0 && this.missileBit(x, this.m0, ns0, cp0, ms0)) m |= 4;
        if (em1 && this.missileBit(x, this.m1, ns1, cp1, ms1)) m |= 8;
        if (ebl) { let d = x - this.bl; if (d < 0) d += 160; if (d < bs) m |= 16; }
        cx |= CX[m];
        let col;
        if (pri) {
          if (m & 48) col = cpf; else if (m & 5) col = c0; else if (m & 10) col = c1; else col = cbk;
        } else {
          if (m & 5) col = c0; else if (m & 10) col = c1;
          else if (m & 32) col = score ? (x < 80 ? c0 : c1) : cpf;
          else if (m & 16) col = cpf; else col = cbk;
        }
        buf[row + x] = col;
      }
      this.cx = cx;
    }
    playerBit(x, pos, ns, copies, refl, g, noMain) {
      let d = x - pos; if (d < 0) d += 160;
      let bit = -1;
      if (ns === 5) { d -= 1; if (d < 0) d += 160; if (d < 16) bit = d >> 1; }
      else if (ns === 7) { d -= 1; if (d < 0) d += 160; if (d < 32) bit = d >> 2; }
      else for (let i = 0; i < copies.length; i++) {
        const o = copies[i];
        if (d >= o && d < o + 8) { if (!(o === 0 && noMain)) bit = d - o; break; }
      }
      if (bit < 0) return 0;
      if (ns >= 5 && noMain) return 0;
      return (g >> (refl ? bit : 7 - bit)) & 1;
    }
    missileBit(x, pos, ns, copies, size) {
      let d = x - pos; if (d < 0) d += 160;
      if (ns === 5 || ns === 7) return d < size;
      for (let i = 0; i < copies.length; i++) { const o = copies[i]; if (d >= o && d < o + size) return true; }
      return false;
    }
    updatePF() {
      let b = 0;
      for (let i = 0; i < 4; i++) if (this.pf0 & (0x10 << i)) b |= 1 << i;
      for (let i = 0; i < 8; i++) if (this.pf1 & (0x80 >> i)) b |= 1 << (4 + i);
      for (let i = 0; i < 8; i++) if (this.pf2 & (1 << i)) b |= 1 << (12 + i);
      this.pfBits = b;
    }

    // ---- movement ----
    static hm(v) { const n = (v >> 4) & 15; return n >= 8 ? n - 16 : n; } // +7 (left) .. -8 (right)
    applyHmove(extra) {
      const mv = (pos, hm) => { let p = pos - (TIA.hm(hm) + extra); p %= 160; if (p < 0) p += 160; return p; };
      this.p0 = mv(this.p0, this.hmp0); this.p1 = mv(this.p1, this.hmp1);
      this.m0 = mv(this.m0, this.hmm0); this.m1 = mv(this.m1, this.hmm1); this.bl = mv(this.bl, this.hmbl);
      if (this.resmp0) this.m0 = this.lockedMissile(this.p0, this.nusiz0);
      if (this.resmp1) this.m1 = this.lockedMissile(this.p1, this.nusiz1);
    }
    lockedMissile(p, nus) { const ns = nus & 7; return (p + (ns === 5 ? 6 : ns === 7 ? 10 : 3)) % 160; }
    resetPos(isPlayer) {
      const h = this.hclock, edge = HBLANK + (this.hmoveBlank ? 8 : 0);
      if (h < edge) return isPlayer ? 3 : 2;
      return (h - HBLANK + (isPlayer ? 5 : 4)) % 160;
    }

    // ---- registers ----
    write(r, v) {
      // Changes that affect the picture: draw everything up to now (plus the
      // chip's short internal delay) with the old values first.
      const h = this.hclock;
      switch (r) {
        case 0x00: { // VSYNC
          this.renderTo(h);
          if ((v & 2) && !(this.vsync & 2)) { this.vsync = v; this.endFrame(); this.rendered = h; return; }
          this.vsync = v; return;
        }
        case 0x01: { // VBLANK
          this.renderTo(Math.min(LINE, h + 1));
          if ((this.vblank & 0x80) && !(v & 0x80)) this.dumpStart = this.m.cycles;
          if (!(v & 0x40)) this.fireLatched = [false, false];
          this.vblank = v; return;
        }
        case 0x02: this.wsync = true; return;
        case 0x03: return; // RSYNC
        case 0x04: this.renderTo(h); this.nusiz0 = v; return;
        case 0x05: this.renderTo(h); this.nusiz1 = v; return;
        case 0x06: this.renderTo(h); this.colup0 = v; return;
        case 0x07: this.renderTo(h); this.colup1 = v; return;
        case 0x08: this.renderTo(h); this.colupf = v; return;
        case 0x09: this.renderTo(h); this.colubk = v; return;
        case 0x0a: this.renderTo(Math.min(LINE, h + 1)); this.ctrlpf = v; return;
        case 0x0b: this.renderTo(Math.min(LINE, h + 1)); this.refp0 = v & 8; return;
        case 0x0c: this.renderTo(Math.min(LINE, h + 1)); this.refp1 = v & 8; return;
        case 0x0d: this.renderTo(Math.min(LINE, h + 2)); this.pf0 = v; this.updatePF(); return;
        case 0x0e: this.renderTo(Math.min(LINE, h + 2)); this.pf1 = v; this.updatePF(); return;
        case 0x0f: this.renderTo(Math.min(LINE, h + 2)); this.pf2 = v; this.updatePF(); return;
        case 0x10: this.renderTo(h); this.p0 = this.resetPos(true); if (h >= HBLANK) this.noMain0 = true; if (this.resmp0) this.m0 = this.lockedMissile(this.p0, this.nusiz0); return;
        case 0x11: this.renderTo(h); this.p1 = this.resetPos(true); if (h >= HBLANK) this.noMain1 = true; if (this.resmp1) this.m1 = this.lockedMissile(this.p1, this.nusiz1); return;
        case 0x12: this.renderTo(h); this.m0 = this.resetPos(false); return;
        case 0x13: this.renderTo(h); this.m1 = this.resetPos(false); return;
        case 0x14: this.renderTo(h); this.bl = this.resetPos(false); return;
        case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a:
          this.m.sound.write(r - 0x15, v); return;
        case 0x1b: this.renderTo(Math.min(LINE, h + 1)); this.grp0n = v; this.grp1o = this.grp1n; return;
        case 0x1c: this.renderTo(Math.min(LINE, h + 1)); this.grp1n = v; this.grp0o = this.grp0n; this.enablo = this.enabln; return;
        case 0x1d: this.renderTo(Math.min(LINE, h + 1)); this.enam0 = v & 2; return;
        case 0x1e: this.renderTo(Math.min(LINE, h + 1)); this.enam1 = v & 2; return;
        case 0x1f: this.renderTo(Math.min(LINE, h + 1)); this.enabln = v & 2; return;
        case 0x20: this.hmp0 = v; return;
        case 0x21: this.hmp1 = v; return;
        case 0x22: this.hmm0 = v; return;
        case 0x23: this.hmm1 = v; return;
        case 0x24: this.hmbl = v; return;
        case 0x25: this.renderTo(h); this.vdelp0 = v & 1; return;
        case 0x26: this.renderTo(h); this.vdelp1 = v & 1; return;
        case 0x27: this.renderTo(h); this.vdelbl = v & 1; return;
        case 0x28: this.renderTo(h); if (this.resmp0 && !(v & 2)) this.m0 = this.lockedMissile(this.p0, this.nusiz0); this.resmp0 = v & 2; return;
        case 0x29: this.renderTo(h); if (this.resmp1 && !(v & 2)) this.m1 = this.lockedMissile(this.p1, this.nusiz1); this.resmp1 = v & 2; return;
        case 0x2a: // HMOVE
          this.renderTo(h);
          if (h < HBLANK) { this.hmoveBlank = true; this.applyHmove(0); }
          else if (h >= 219) this.lateHmove = true;   // at the very end of a line: moves on the next, without the black bar
          else this.applyHmove(0);
          return;
        case 0x2b: this.hmp0 = this.hmp1 = this.hmm0 = this.hmm1 = this.hmbl = 0; return;
        case 0x2c: this.renderTo(h); this.cx = 0; return;
        default: return;
      }
    }
    read(r) {
      r &= 15;
      if (r < 8) {
        this.renderTo(this.hclock);
        const c = this.cx;
        return (((c >> (2 * r)) & 1) << 7) | (((c >> (2 * r + 1)) & 1) << 6);
      }
      if (r < 12) return this.m.paddleBit(r - 8);
      if (r < 14) {
        const p = r - 12;
        const down = this.m.fireDown(p);
        if (this.vblank & 0x40) { if (down) this.fireLatched[p] = true; return this.fireLatched[p] ? 0 : 0x80; }
        return down ? 0 : 0x80;
      }
      return 0;
    }

    saveState() {
      const o = {};
      for (const k of Object.keys(this)) {
        if (k === "m" || k === "lines" || k === "screen") continue;
        const v = this[k]; o[k] = Array.isArray(v) ? v.slice() : v;
      }
      return o;
    }
    loadState(o) { for (const k in o) this[k] = Array.isArray(o[k]) ? o[k].slice() : o[k]; }
  }

  // ------------------------------------------------------------------
  // Cartridges
  // ------------------------------------------------------------------
  function hasSig(rom, sigs) {
    for (const s of sigs) {
      outer: for (let i = 0; i + s.length <= rom.length; i++) {
        for (let j = 0; j < s.length; j++) if (rom[i + j] !== s[j]) continue outer;
        return true;
      }
    }
    return false;
  }
  function countSig(rom, s) {
    let n = 0;
    outer: for (let i = 0; i + s.length <= rom.length; i++) {
      for (let j = 0; j < s.length; j++) if (rom[i + j] !== s[j]) continue outer;
      n++;
    }
    return n;
  }
  // A Superchip cart leaves the RAM's address range in every bank unused (filled with one value).
  // (Some dumps instead show the RAM's read port as a copy of its write port.)
  function hasSuperchip(rom) {
    let filled = true, mirrored = true;
    for (let b = 0; b < rom.length; b += 4096) {
      const v = rom[b];
      for (let i = 1; i < 256; i++) if (rom[b + i] !== v) { filled = false; break; }
      for (let i = 0; i < 128; i++) if (rom[b + i] !== rom[b + 128 + i]) { mirrored = false; break; }
    }
    return filled || mirrored;
  }

  function detectType(rom) {
    const n = rom.length;
    if (n === 10240 || n === 10495) return "DPC";
    if (n <= 4096) return "4K";
    if (n === 8192) {
      if (hasSig(rom, [[0x8d, 0xe0, 0x1f], [0x8d, 0xe0, 0x5f], [0x8d, 0xe9, 0xff], [0x0c, 0xe0, 0x1f], [0xad, 0xe0, 0x1f], [0xad, 0xe9, 0xff], [0xad, 0xed, 0xff], [0xad, 0xf3, 0xbf]])) return "E0";
      if (hasSig(rom, [[0x20, 0x00, 0xd0, 0xc6, 0xc5], [0x20, 0xc3, 0xf8, 0xa5, 0x82], [0xd0, 0xfb, 0x20, 0x73, 0xfe], [0x20, 0x00, 0xf0, 0x84, 0xd6]])) return "FE";
      if (countSig(rom, [0x85, 0x3f]) >= 2) return "3F";
      return hasSuperchip(rom) ? "F8SC" : "F8";
    }
    if (n === 12288) return "FA";
    if (n === 16384) {
      if (hasSig(rom, [[0xad, 0xe2, 0xff], [0xad, 0xe5, 0xff], [0xad, 0xe5, 0x1f], [0xad, 0xe7, 0x1f], [0x0c, 0xe7, 0x1f], [0x8d, 0xe7, 0xff], [0x8d, 0xe7, 0x1f]])) return "E7";
      if (countSig(rom, [0x85, 0x3f]) >= 2) return "3F";
      return hasSuperchip(rom) ? "F6SC" : "F6";
    }
    if (n === 32768) {
      if (countSig(rom, [0x85, 0x3f]) >= 2) return "3F";
      return hasSuperchip(rom) ? "F4SC" : "F4";
    }
    if (n === 65536) return countSig(rom, [0x85, 0x3f]) >= 2 ? "3F" : (hasSuperchip(rom) ? "EFSC" : "EF");
    if (n % 2048 === 0 && countSig(rom, [0x85, 0x3f]) >= 2) return "3F";
    throw new Error("This Atari 2600 cartridge uses hardware RetroPixels doesn't support yet (" + n + " bytes).");
  }

  class Cart {
    constructor(rom) {
      this.type = detectType(rom);
      const t = this.type;
      if (rom.length < 4096 && t === "4K") { // 2K and smaller carts repeat
        const r = new Uint8Array(4096); for (let i = 0; i < 4096; i++) r[i] = rom[i % rom.length]; rom = r;
      }
      this.rom = rom.length > 4096 && t === "4K" ? rom.subarray(0, 4096) : rom;
      this.ram = null; this.ramSize = 0; this.bank = 0;
      this.hsLo = -1; this.hsHi = -1;
      const fx = { F8: [0xff8, 0xff9, 1], F6: [0xff6, 0xff9, 0], F4: [0xff4, 0xffb, 0], FA: [0xff8, 0xffa, 2], EF: [0xfe0, 0xfef, 15] };
      const base = t.replace("SC", "");
      if (fx[base]) {
        [this.hsLo, this.hsHi, this.bank] = fx[base];
        if (t.endsWith("SC")) { this.ramSize = 128; this.ram = new Uint8Array(128); }
        if (t === "FA") { this.ramSize = 256; this.ram = new Uint8Array(256); }
        this.kind = "fx";
      } else if (t === "DPC") {
        this.kind = "dpc"; this.hsLo = 0xff8; this.hsHi = 0xff9; this.bank = 1;
        this.dd = rom.subarray(8192, 10240);
        this.tops = new Uint8Array(8); this.bottoms = new Uint8Array(8); this.counters = new Uint16Array(8);
        this.flags = new Uint8Array(8); this.music = [false, false, false]; this.rand = 1; this.musicCycles = 0;
      } else if (t === "E0") { this.kind = "e0"; this.slices = [4, 5, 6, 7]; }
      else if (t === "E7") { this.kind = "e7"; this.bank = 0; this.ramBank = 0; this.ram = new Uint8Array(2048); }
      else if (t === "FE") { this.kind = "fe"; this.bank = 0; this.lastFE = false; }
      else if (t === "3F") { this.kind = "3f"; this.bank = 0; this.last = (rom.length >> 11) - 1; }
      else this.kind = "4k";
    }
    hotspot(off) { if (off >= this.hsLo && off <= this.hsHi) this.bank = off - this.hsLo; }
    read(a, m) {
      const off = a & 0xfff, rom = this.rom;
      switch (this.kind) {
        case "4k": return rom[off];
        case "fx": {
          this.hotspot(off);
          if (this.ram && off >= this.ramSize && off < 2 * this.ramSize) return this.ram[off - this.ramSize];
          return rom[this.bank * 4096 + off];
        }
        case "dpc": {
          if (off < 0x40) return this.dpcRead(off, m);
          this.hotspot(off);
          return rom[this.bank * 4096 + off];
        }
        case "e0": {
          if (off >= 0xfe0 && off <= 0xff7) this.slices[(off - 0xfe0) >> 3] = off & 7;
          return rom[this.slices[off >> 10] * 1024 + (off & 0x3ff)];
        }
        case "e7": {
          if (off >= 0xfe0 && off <= 0xfe7) this.bank = off - 0xfe0;
          else if (off >= 0xfe8 && off <= 0xfeb) this.ramBank = off - 0xfe8;
          if (off < 0x800) {
            if (this.bank === 7) return off >= 0x400 ? this.ram[off - 0x400] : 0;
            return rom[this.bank * 2048 + off];
          }
          if (off < 0x900) return 0;
          if (off < 0xa00) return this.ram[1024 + this.ramBank * 256 + (off - 0x900)];
          return rom[7 * 2048 + (off - 0x800)];
        }
        case "fe": return rom[this.bank * 4096 + off];
        case "3f": return off < 0x800 ? rom[this.bank * 2048 + off] : rom[this.last * 2048 + (off - 0x800)];
      }
      return 0;
    }
    write(a, v) {
      const off = a & 0xfff;
      switch (this.kind) {
        case "fx": this.hotspot(off); if (this.ram && off < this.ramSize) this.ram[off] = v; return;
        case "dpc": if (off >= 0x40 && off < 0x80) this.dpcWrite(off, v); else this.hotspot(off); return;
        case "e0": if (off >= 0xfe0 && off <= 0xff7) this.slices[(off - 0xfe0) >> 3] = off & 7; return;
        case "e7":
          if (off >= 0xfe0 && off <= 0xfe7) this.bank = off - 0xfe0;
          else if (off >= 0xfe8 && off <= 0xfeb) this.ramBank = off - 0xfe8;
          else if (off < 0x400 && this.bank === 7) this.ram[off] = v;
          else if (off >= 0x800 && off < 0x900) this.ram[1024 + this.ramBank * 256 + (off - 0x800)] = v;
          return;
      }
    }
    // Activision (FE): the bank follows the high byte of a JSR/RTS address on the stack.
    snoop(a, v) {
      if (this.lastFE) this.bank = (v & 0x20) ? 0 : 1;
      this.lastFE = a === 0x01fe;
    }
    tiaWrite(a, v) { if (this.kind === "3f" && a < 0x40) this.bank = (v % ((this.rom.length >> 11))) | 0; }

    // ---- DPC (Pitfall II) ----
    dpcMusic(m) {
      const now = m.cycles;
      const clocks = Math.floor((now - this.musicCycles) * 20000 / (CLOCK / 3));
      if (clocks <= 0) return;
      this.musicCycles += clocks * (CLOCK / 3) / 20000;
      for (let x = 5; x < 8; x++) if (this.music[x - 5]) {
        const top = this.tops[x] + 1;
        let low = this.counters[x] & 0xff;
        if (this.tops[x] !== 0) { low -= clocks % top; if (low < 0) low += top; } else low = 0;
        if (low <= this.bottoms[x]) this.flags[x] = 0; else if (low <= this.tops[x]) this.flags[x] = 0xff;
        this.counters[x] = (this.counters[x] & 0x700) | low;
      }
    }
    dpcRead(off, m) {
      const r = this.rand;
      this.rand = ((r << 1) | (~((r >> 7) ^ (r >> 5) ^ (r >> 4) ^ (r >> 3)) & 1)) & 0xff;
      const i = off & 7, fn = (off >> 3) & 7;
      let res = 0;
      if (fn === 0 && i >= 4) {
        this.dpcMusic(m);
        const k = (this.music[0] && this.flags[5] ? 1 : 0) | (this.music[1] && this.flags[6] ? 2 : 0) | (this.music[2] && this.flags[7] ? 4 : 0);
        return [0x00, 0x04, 0x05, 0x09, 0x06, 0x0a, 0x0b, 0x0f][k];
      }
      if ((this.counters[i] & 0xff) === this.tops[i]) this.flags[i] = 0xff;
      else if ((this.counters[i] & 0xff) === this.bottoms[i]) this.flags[i] = 0;
      switch (fn) {
        case 0: res = r; break;
        case 1: res = this.dd[2047 - this.counters[i]]; break;
        case 2: res = this.dd[2047 - this.counters[i]] & this.flags[i]; break;
        case 7: res = this.flags[i]; break;
      }
      if (i < 5 || !this.music[i - 5]) this.counters[i] = (this.counters[i] - 1) & 0x7ff;
      return res;
    }
    dpcWrite(off, v) {
      const i = off & 7, fn = (off >> 3) & 7;
      switch (fn) {
        case 0: this.tops[i] = v; this.flags[i] = 0; break;
        case 1: this.bottoms[i] = v; break;
        case 2: this.counters[i] = (this.counters[i] & 0x700) | v; break;
        case 3: this.counters[i] = (this.counters[i] & 0xff) | ((v & 7) << 8); if (i >= 5) this.music[i - 5] = !!(v & 0x10); break;
        case 6: this.rand = 1; break;
      }
    }
    saveState() {
      const o = { bank: this.bank };
      for (const k of ["ram", "slices", "ramBank", "lastFE", "tops", "bottoms", "counters", "flags", "music", "rand", "musicCycles"])
        if (this[k] !== undefined && this[k] !== null) o[k] = Array.isArray(this[k]) ? this[k].slice() : this[k];
      return o;
    }
    loadState(o) {
      for (const k in o) {
        if (ArrayBuffer.isView(this[k])) this[k].set(o[k]);
        else this[k] = Array.isArray(o[k]) ? o[k].slice() : o[k];
      }
    }
  }

  // ------------------------------------------------------------------
  // The console
  // ------------------------------------------------------------------
  // buttons bit order (see info.buttons)
  const UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, FIRE = 16, SELECT = 32, RESET = 64, LDIFF = 128, RDIFF = 256, BW = 512;

  class Atari2600 {
    constructor(rom, opts = {}) {
      this.romData = rom;
      this.sampleRate = opts.sampleRate || 48000;
      this.width = 160; this.height = OUT_H; this.fps = FPS;
      this.init();
    }
    init() {
      this.cart = new Cart(this.romData);
      this.riot = new RIOT();
      this.sound = new TIASound(this.sampleRate);
      this.tia = new TIA(this);
      this.cycles = 0; this.bus = 0;
      this.input = [0, 0]; this.analog = [0, 0]; this.paddle = [0.5, 0.5, 0.5, 0.5];
      this.paddleMode = false; this.paddleReads = 0; this.paddleFrames = 0;
      const self = this;
      this.cpu = new global.RPCPU6502({
        read(a) { return self.read(a); },
        write(a, v) { self.write(a, v); },
      }, { decimal: true });
      this.updatePorts();
      this.cpu.reset();
    }
    get screen() { return this.tia.screen; }

    cycle() { this.cycles++; this.tia.clock3(); this.riot.tick(); }
    read(a) {
      if (this.tia.wsync) { this.tia.wsync = false; while (this.tia.hclock !== 0) this.cycle(); }
      this.cycle();
      a &= 0x1fff;
      let v;
      if (a & 0x1000) v = this.cart.read(a, this);
      else if (a & 0x80) v = (a & 0x200) ? this.riot.read(a) : this.riot.ram[a & 0x7f];
      else { v = (this.tia.read(a) & 0xc0) | (this.bus & 0x3f); if ((a & 15) >= 8 && (a & 15) < 12) this.paddleReads++; }
      if (this.cart.kind === "fe") this.cart.snoop(a, v);
      this.bus = v;
      return v;
    }
    write(a, v) {
      this.cycle();
      a &= 0x1fff; this.bus = v;
      if (a & 0x1000) this.cart.write(a, v);
      else if (a & 0x80) { if (a & 0x200) this.riot.write(a, v); else this.riot.ram[a & 0x7f] = v; }
      else { this.tia.write(a & 0x3f, v); if (this.cart.kind === "3f") this.cart.tiaWrite(a, v); }
      if (this.cart.kind === "fe") this.cart.snoop(a, v);
    }

    // ---- controllers ----
    fireDown(p) { return !this.paddleMode && !!(this.input[p] & FIRE); }
    paddleBit(i) {
      const t = this.tia;
      if (t.vblank & 0x80) return 0;
      // the paddle's capacitor charges at a rate set by the knob
      const need = 76 * (40 + (1 - this.paddle[i]) * 120);
      return this.cycles - t.dumpStart >= need ? 0x80 : 0;
    }
    updatePorts() {
      let a = 0xff;
      const dirs = (b) => (b & UP ? 1 : 0) | (b & DOWN ? 2 : 0) | (b & LEFT ? 4 : 0) | (b & RIGHT ? 8 : 0);
      if (this.paddleMode) {
        if (this.input[0] & FIRE) a &= ~0x80;
        if (this.input[1] & FIRE) a &= ~0x40;
      } else {
        a &= ~(dirs(this.input[0]) << 4);
        a &= ~dirs(this.input[1]);
      }
      this.riot.portA = a;
      const s = this.input[0];
      this.riot.portB = (s & RESET ? 0 : 1) | (s & SELECT ? 0 : 2) | (s & BW ? 0 : 8) | (s & LDIFF ? 0x40 : 0) | (s & RDIFF ? 0x80 : 0) | 0x34;
    }
    setInput(p, bits) { if (p < 2) { this.input[p] = bits; this.updatePorts(); } }
    setAnalog(p, x) { if (p < 2) this.analog[p] = x; }
    movePaddles() {
      for (let p = 0; p < 2; p++) {
        const x = this.analog[p];
        if (Math.abs(x) > 0.1) this.paddle[p] = Math.max(0, Math.min(1, (x + 1) / 2));
        else {
          if (this.input[p] & LEFT) this.paddle[p] = Math.max(0, this.paddle[p] - 0.02);
          if (this.input[p] & RIGHT) this.paddle[p] = Math.min(1, this.paddle[p] + 0.02);
        }
      }
    }

    // ---- frontend interface ----
    runFrame() {
      const t = this.tia;
      t.frameDone = false;
      if (this.paddleMode) this.movePaddles();
      this.paddleReads = 0;
      const limit = this.cycles + 76 * 400;
      while (!t.frameDone) {
        this.cpu.step();
        if (this.cycles > limit) { t.endFrame(); break; }
      }
      // Paddle games read the paddle ports constantly; joystick games never do.
      if (!this.paddleMode) {
        if (this.paddleReads > 40) { if (++this.paddleFrames >= 2) { this.paddleMode = true; this.updatePorts(); } }
        else this.paddleFrames = 0;
      }
    }
    takeAudio() { return this.sound.take(); }
    reset() { const keep = this.input.slice(); this.init(); this.input = keep; this.updatePorts(); }
    getSRAM() { return null; }
    saveState() {
      return { v: 1, cpu: this.cpu.saveState(), riot: this.riot.saveState(), tia: this.tia.saveState(), sound: this.sound.saveState(),
        cart: this.cart.saveState(), cycles: this.cycles, bus: this.bus, paddleMode: this.paddleMode, paddle: this.paddle.slice() };
    }
    loadState(s) {
      this.cpu.loadState(s.cpu); this.riot.loadState(s.riot); this.tia.loadState(s.tia); this.sound.loadState(s.sound);
      this.cart.loadState(s.cart); this.cycles = s.cycles; this.bus = s.bus; this.paddleMode = s.paddleMode; this.paddle = s.paddle.slice();
      this.updatePorts();
    }
  }
  Atari2600.info = { system: "atari2600", name: "Atari 2600", width: 160, height: OUT_H, aspect: 4 / 3, fps: FPS, players: 2,
    buttons: ["Up", "Down", "Left", "Right", "Fire", "Select", "Reset", "Left difficulty", "Right difficulty", "Color / B&W"] };

  global.RPCores = global.RPCores || {};
  global.RPCores.atari2600 = Atari2600;
  global.RPAtari = { TIASound, RIOT, CLOCK };
})(typeof window !== "undefined" ? window : globalThis);
