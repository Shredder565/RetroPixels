// RetroPixels — Konami K053260 PCM sound chip. Original code, written from the
// chip's documented behaviour.
//
// Four sample-playing channels reading a ROM of up to 2 MB. Each channel has a
// 12-bit pitch, a length, a start address, a volume and a pan, and plays either
// signed 8-bit samples or Konami's 4-bit ADPCM (each nibble, low one first,
// picks a step of 0, ±1, ±2, ±4 ... ±64 or -128 added to an 8-bit running value).
// The playback rate is clock / (4096 - pitch) bytes (or nibbles) a second.
//
// Registers (sound processor side, 0x00-0x2f):
//   00-01  read: bytes written by the main processor     02-03  write: bytes for the main processor
//   08+8n  channel n (0-3): +0 pitch low, +1 pitch high (4 bits), +2/+3 length low/high,
//          +4/+5 start low/high, +6 start bits 16-20, +7 volume (7 bits)
//   28     key on: bit n starts channel n (on a 0->1 change), clearing it stops the channel
//   29     read: bit n set while channel n is playing
//   2a     bits 0-3 loop channel n, bits 4-7 channel n plays ADPCM
//   2c     pan of channel 0 (bits 0-2) and 1 (bits 3-5); 2d the same for channels 2 and 3
//          (pan 0 is silent, 1 = left ... 4 = centre ... 7 = right; RetroPixels mixes to mono)
//   2e     read: the next ROM byte from channel 0's start (when 2f bit 0 is set)
//   2f     bit 0 ROM reading through 2e, bit 1 sound output on
// The main processor sees two ports: writes go to 00-01, reads come from 02-03.
//
// Interface:
//   const k = new RPK053260(rom, clockHz)
//   k.write(reg, v) / k.read(reg)            sound processor side
//   k.mainWrite(port, v) / k.mainRead(port)  main processor side (port 0 or 1)
//   k.mix(out, offset, n, outRate)           add n mono samples at outRate into out (Float32Array)
//   k.getState() / k.setState(o)
"use strict";

(function (global) {
  const ADPCM_STEP = [0, 1, 2, 4, 8, 16, 32, 64, -128, -64, -32, -16, -8, -4, -2, -1];
  // mono loudness of each pan setting (constant-power pan, then left + right)
  const PAN_GAIN = [0];
  for (let p = 1; p < 8; p++) { const t = (p - 1) / 6 * Math.PI / 2; PAN_GAIN.push((Math.cos(t) + Math.sin(t)) / Math.SQRT2); }

  class RPK053260 {
    constructor(rom, clockHz) {
      this.rom = rom && rom.length ? rom : new Uint8Array(1);
      this.clock = clockHz || 3579545;
      this.regs = new Uint8Array(0x30);
      this.ports = new Uint8Array(4);
      // per channel: position (bytes, or nibbles in ADPCM), fraction, ADPCM value, playing flag
      this.pos = new Float64Array(4); this.frac = new Float64Array(4);
      this.acc = new Int32Array(4); this.playing = new Uint8Array(4);
      this.romRead = 0;
    }

    pitch(n) { const b = 8 + n * 8; return this.regs[b] | ((this.regs[b + 1] & 15) << 8); }
    length(n) { const b = 8 + n * 8; return this.regs[b + 2] | (this.regs[b + 3] << 8); }
    start(n) { const b = 8 + n * 8; return this.regs[b + 4] | (this.regs[b + 5] << 8) | ((this.regs[b + 6] & 0x1f) << 16); }
    volume(n) { return this.regs[8 + n * 8 + 7] & 0x7f; }
    pan(n) { const r = this.regs[0x2c + (n >> 1)]; return (n & 1) ? (r >> 3) & 7 : r & 7; }

    write(reg, v) {
      reg &= 0x3f; v &= 0xff;
      if (reg >= 0x30) return;
      if (reg === 2 || reg === 3) { this.ports[reg] = v; return; }
      if (reg < 2) return;
      if (reg === 0x28) {
        const old = this.regs[0x28];
        for (let n = 0; n < 4; n++) {
          const bit = 1 << n;
          if ((v & bit) && !(old & bit)) { this.pos[n] = 0; this.frac[n] = 0; this.acc[n] = -1; this.playing[n] = 1; } // acc -1: first nibble not read yet
          else if (!(v & bit)) this.playing[n] = 0;
        }
      }
      if (reg >= 0x08 && reg < 0x10) this.romRead = 0; // channel 0's start changed: reading starts again
      this.regs[reg] = v;
    }

    read(reg) {
      reg &= 0x3f;
      if (reg < 2) return this.ports[reg];
      if (reg === 0x29) { let s = 0; for (let n = 0; n < 4; n++) if (this.playing[n]) s |= 1 << n; return s; }
      if (reg === 0x2e) {
        if (!(this.regs[0x2f] & 1)) return 0;
        const a = (this.start(0) + this.romRead++) % this.rom.length;
        return this.rom[a];
      }
      return 0;
    }

    mainWrite(port, v) { this.ports[port & 1] = v & 0xff; }
    mainRead(port) { return this.ports[2 + (port & 1)]; }

    mix(out, offset, n, outRate) {
      if (!(this.regs[0x2f] & 2)) return;
      const rom = this.rom, size = rom.length;
      for (let c = 0; c < 4; c++) {
        if (!this.playing[c]) continue;
        const gain = this.volume(c) / 127 * PAN_GAIN[this.pan(c)] / 128 * 0.35;
        if (!gain) { this.advanceSilent(c, n, outRate); continue; }
        const step = this.clock / (4096 - this.pitch(c)) / outRate;
        const adpcm = (this.regs[0x2a] >> (4 + c)) & 1, loop = (this.regs[0x2a] >> c) & 1;
        const start = this.start(c), len = this.length(c), end = adpcm ? len * 2 : len;
        let pos = this.pos[c], frac = this.frac[c], acc = this.acc[c];
        const nib = (p, a) => { const byte = rom[(start + (p >> 1)) % size]; return (a + ADPCM_STEP[(p & 1) ? byte >> 4 : byte & 15]) & 0xff; };
        if (adpcm && acc < 0) acc = nib(pos, 0);
        let cur = adpcm ? (acc << 24) >> 24 : (rom[(start + pos) % size] << 24) >> 24;
        for (let i = 0; i < n; i++) {
          out[offset + i] += cur * gain;
          frac += step;
          while (frac >= 1) {
            frac -= 1; pos++;
            if (pos >= end) {
              if (!loop) { this.playing[c] = 0; break; }
              pos = 0; acc = 0;
            }
            if (adpcm) {
              acc = nib(pos, acc);
              cur = (acc << 24) >> 24;
            } else cur = (rom[(start + pos) % size] << 24) >> 24;
          }
          if (!this.playing[c]) break;
        }
        this.pos[c] = pos; this.frac[c] = frac; this.acc[c] = acc;
      }
    }
    // a channel at zero volume still moves along (so its "playing" bit ends at the right time)
    advanceSilent(c, n, outRate) {
      const adpcm = (this.regs[0x2a] >> (4 + c)) & 1, loop = (this.regs[0x2a] >> c) & 1;
      const end = adpcm ? this.length(c) * 2 : this.length(c);
      const t = this.frac[c] + n * this.clock / (4096 - this.pitch(c)) / outRate;
      let pos = this.pos[c] + Math.floor(t);
      this.frac[c] = t - Math.floor(t);
      if (pos >= end) { if (loop && end) pos %= end; else this.playing[c] = 0; }
      this.pos[c] = pos;
    }

    getState() {
      return { regs: this.regs.slice(), ports: this.ports.slice(), pos: this.pos.slice(), frac: this.frac.slice(),
        acc: this.acc.slice(), playing: this.playing.slice(), romRead: this.romRead };
    }
    setState(o) {
      this.regs.set(o.regs); this.ports.set(o.ports); this.pos.set(o.pos); this.frac.set(o.frac);
      this.acc.set(o.acc); this.playing.set(o.playing); this.romRead = o.romRead;
    }
  }

  global.RPK053260 = RPK053260;
})(typeof window !== "undefined" ? window : globalThis);
