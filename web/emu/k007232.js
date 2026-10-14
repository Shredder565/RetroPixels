// RetroPixels — Konami K007232 two-channel PCM sound chip. Original code, written from
// the chip's documented behaviour. Used by many Konami arcade boards of the late 1980s
// (TMNT, Contra, Gradius II...).
//
// Each channel plays 7-bit unsigned samples from the sample ROM, starting at a 17-bit
// address, at a pitch set by a 12-bit counter (a byte lasts (4096 - pitch) * 4 input clocks),
// until it meets a byte with bit 7 set (the end mark), where it stops or starts over if its
// loop bit is on. The chip has no volume of its own: boards wire its "external port" register
// to volume latches (setVolume).
//
// ===========================================================================
// INTERFACE
// ===========================================================================
//   const k = new RPK007232({ clock: 3579545, rom, onPort(v) })
//       rom      the sample ROM (Uint8Array); k.bank[0] / k.bank[1] are added to each
//                channel's address for boards with bigger ROMs (byte offsets, default 0)
//       onPort   called when register 12 (the external port) is written
//   k.rate                 native rate = clock / 128 (32 counter steps per tick)
//   k.write(reg, v)        reg 0-13:
//                           0,1 / 6,7   pitch: low 8 bits, then bits 8-11 in the low nibble
//                           2,3,4 / 8,9,10  start address bits 0-7, 8-15, 16 (bit 0)
//                           5 / 11      writing starts the channel from its start address
//                           12          external port (onPort)
//                           13          loop: bit 0 channel A, bit 1 channel B
//   k.read(reg)            reading 5 or 11 also starts the channel (as on the real chip); returns 0
//   k.setVolume(ch, left, right)   0..255 each
//   k.tick()               one native step; k.out[0] / k.out[1] = left / right (about ±1 at full volume)
//   k.clocks(n)            run n input clocks, summing samples into k.acc[0..1] / k.accN
//   k.reset(), k.saveState(), k.loadState(o)
"use strict";

(function (global) {
  class K007232 {
    constructor(opts) {
      opts = opts || {};
      this.clock = opts.clock || 3579545;
      this.rate = this.clock / 128;
      this.rom = opts.rom || new Uint8Array(1);
      this.onPort = opts.onPort || null;
      this.regs = new Uint8Array(16);
      this.addr = new Int32Array(2); this.cnt = new Int32Array(2); this.on = new Uint8Array(2);
      this.vol = new Uint8Array(4); this.bank = [0, 0];
      this.out = new Float64Array(2); this.acc = new Float64Array(2); this.accN = 0; this.clk = 0;
      this.reset();
    }
    reset() {
      this.regs.fill(0); this.addr.fill(0); this.cnt.fill(0); this.on.fill(0); this.clk = 0;
      this.vol.fill(255);
    }
    setVolume(ch, l, r) { this.vol[ch * 2] = l & 255; this.vol[ch * 2 + 1] = r & 255; }
    pitch(ch) { const b = ch * 6; return this.regs[b] | ((this.regs[b + 1] & 15) << 8); }
    start(ch) {
      const b = ch * 6;
      this.addr[ch] = this.regs[b + 2] | (this.regs[b + 3] << 8) | ((this.regs[b + 4] & 1) << 16);
      this.cnt[ch] = this.pitch(ch);
      this.on[ch] = 1;
    }
    write(r, v) {
      r &= 15; v &= 255;
      this.regs[r] = v;
      if (r === 5 || r === 11) this.start(r === 5 ? 0 : 1);
      else if (r === 12 && this.onPort) this.onPort(v);
    }
    read(r) {
      r &= 15;
      if (r === 5 || r === 11) this.start(r === 5 ? 0 : 1);
      return 0;
    }
    sampleAt(ch) {
      const rom = this.rom;
      return rom[(this.addr[ch] + this.bank[ch]) % rom.length];
    }
    tick() {
      let L = 0, R = 0;
      for (let ch = 0; ch < 2; ch++) {
        if (!this.on[ch]) continue;
        // the counter climbs from the pitch value to 0x1000, one step every 4 input clocks (32 per
        // native tick); each overflow starts it again from the pitch value and moves to the next byte
        this.cnt[ch] += 32;
        const span = 0x1000 - this.pitch(ch);
        while (this.cnt[ch] >= 0x1000) {
          this.cnt[ch] -= span;
          this.addr[ch] = (this.addr[ch] + 1) & 0x1ffff;
          if (this.sampleAt(ch) & 0x80) break;
        }
        let s = this.sampleAt(ch);
        if (s & 0x80) {
          if (this.regs[13] & (1 << ch)) { const b = ch * 6; this.addr[ch] = this.regs[b + 2] | (this.regs[b + 3] << 8) | ((this.regs[b + 4] & 1) << 16); s = this.sampleAt(ch); if (s & 0x80) { this.on[ch] = 0; continue; } }
          else { this.on[ch] = 0; continue; }
        }
        const v = ((s & 0x7f) - 0x40) / 64;
        L += v * this.vol[ch * 2] / 255; R += v * this.vol[ch * 2 + 1] / 255;
      }
      this.out[0] = L; this.out[1] = R;
    }
    clocks(n) {
      this.clk += n;
      while (this.clk >= 128) { this.clk -= 128; this.tick(); this.acc[0] += this.out[0]; this.acc[1] += this.out[1]; this.accN++; }
    }
    saveState() {
      return { regs: this.regs.slice(), addr: this.addr.slice(), cnt: this.cnt.slice(), on: this.on.slice(), vol: this.vol.slice(), bank: this.bank.slice(), clk: this.clk };
    }
    loadState(o) {
      this.regs.set(o.regs); this.addr.set(o.addr); this.cnt.set(o.cnt); this.on.set(o.on); this.vol.set(o.vol);
      this.bank = o.bank.slice(); this.clk = o.clk;
    }
  }

  global.RPK007232 = K007232;
})(typeof window !== "undefined" ? window : globalThis);
