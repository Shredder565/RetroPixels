// RetroPixels — NEC uPD7759 ADPCM speech chip, "master" (stand-alone) mode. Original code,
// written from the chip's documented behaviour: the chip reads its own sample ROM, which
// starts with a header (last sample number, then the 5A A5 69 55 signature) and a table
// of where each sample starts (16-bit, in 2-byte steps). A sample is one byte the chip skips,
// then a list of commands:
//   0x00          end of the sample
//   0x01-0x3f     silence, (n) x 1024 clocks; the ADPCM state starts over
//   0x40-0x7f     256 ADPCM nibbles (128 bytes) follow, each lasting (n & 0x3f) + 1 steps
//   0x80-0xbf     a count byte follows, then count + 1 nibbles at that speed
//   0xc0-0xff     the following command is played (n & 7) + 1 times
// (one "step" is 4 input clocks). The ADPCM has 16 step sizes; each nibble's sign bit
// and 3 magnitude bits change the 8-bit output and move the step size up or down.
//
// ===========================================================================
// INTERFACE
// ===========================================================================
//   const u = new RPUPD7759({ clock: 640000, rom })
//   u.rate                 native rate = clock / 4
//   u.port(v)              the 8-bit data port (sample number)
//   u.start(line)          the ST pin: a 0 -> 1 change starts the sample in the port
//   u.resetLine(line)      the RESET pin (0 = held in reset: silent and idle)
//   u.busy()               true while a sample plays (the BUSY pin is low then)
//   u.tick()               one native step; u.out = output, -1..1
//   u.clocks(n)            run n input clocks, summing into u.acc / u.accN
//   u.reset(), u.saveState(), u.loadState(o)
"use strict";

(function (global) {
  // Step sizes: 16 states growing geometrically (about 22% a state); magnitudes 0-7 scale them.
  // RetroPixels' own fit: the shape and growth were chosen so that real speech ROMs decode
  // without drifting away from the centre (in state 0, magnitudes 0 and 1 change nothing, so a
  // run of small nibbles is silence).
  const MAG = [0, 0.45, 1.4, 2, 3, 4, 5.6, 8.4];
  const STEP = new Int16Array(16 * 16);
  for (let s = 0; s < 16; s++) {
    const base = Math.pow(1.225, s);
    for (let n = 0; n < 16; n++) {
      const d = Math.round(base * MAG[n & 7]);
      STEP[s * 16 + n] = (n & 8) ? -d : d;
    }
  }
  const ADJUST = [-1, -1, 0, 0, 1, 2, 2, 3, -1, -1, 0, 0, 1, 2, 2, 3];
  const IDLE = 0, CMD = 1, COUNT = 2, NIBBLES = 3, SILENCE = 4;

  class UPD7759 {
    constructor(opts) {
      opts = opts || {};
      this.clock = opts.clock || 640000;
      this.rate = this.clock / 4;
      this.rom = opts.rom || new Uint8Array(16);
      this.acc = 0; this.accN = 0;
      this.reset();
    }
    reset() {
      this.st = 0; this.rst = 1; this.data = 0;
      this.state = IDLE; this.pos = 0; this.left = 0; this.period = 1; this.wait = 0;
      this.sample = 0; this.step = 0; this.nib = 0; this.repeat = 0; this.repeatPos = 0;
      this.out = 0; this.clk = 0;
    }
    rd(a) { return this.rom[a % this.rom.length]; }
    port(v) { this.data = v & 255; }
    resetLine(v) { this.rst = v ? 1 : 0; if (!this.rst) { this.state = IDLE; this.sample = 0; this.out = 0; } }
    start(v) {
      v = v ? 1 : 0;
      if (v && !this.st && this.rst) this.begin(this.data);
      this.st = v;
    }
    busy() { return this.state !== IDLE; }

    begin(n) {
      const last = this.rd(0);
      if (n > last) return;
      const at = 5 + n * 2;
      // the first byte at a sample's start is not a command: the chip skips it (real ROMs keep 0 there)
      this.pos = ((this.rd(at) << 8) | this.rd(at + 1)) * 2 + 1;
      this.sample = 0; this.step = 0; this.repeat = 0;
      this.state = CMD; this.wait = 0;
    }

    // Read the next command byte and set up what it asks for.
    command() {
      const c = this.rd(this.pos++);
      if (c === 0) { this.state = IDLE; this.sample = 0; return; }
      if (c < 0x40) { this.state = SILENCE; this.wait = c * 256; this.sample = 0; this.step = 0; return; }
      if (c < 0xc0) {
        this.period = (c & 0x3f) + 1;
        if (c < 0x80) this.left = 256;
        else this.left = this.rd(this.pos++) + 1;
        this.state = NIBBLES; this.nib = 0; this.wait = 0;
        return;
      }
      this.repeat = (c & 7) + 1; this.repeatPos = this.pos; // play what follows (c & 7) + 1 times
    }

    endBlock() {
      if (this.repeat > 1) { this.repeat--; this.pos = this.repeatPos; }
      else this.repeat = 0;
      this.state = CMD;
    }

    tick() {
      if (this.state === IDLE || !this.rst) { this.out = 0; return; }
      if (this.wait > 0) { this.wait--; this.out = this.sample / 128; return; }
      switch (this.state) {
        case CMD: this.command(); break;
        case SILENCE: this.endBlock(); break;
        case NIBBLES: {
          const b = this.rd(this.pos);
          const n = this.nib ? b & 15 : b >> 4; // high nibble first
          if (this.nib) this.pos++;
          this.nib ^= 1;
          let s = this.sample + STEP[this.step * 16 + n];
          if (s > 127) s = 127; else if (s < -128) s = -128;
          this.sample = s;
          this.step += ADJUST[n];
          if (this.step < 0) this.step = 0; else if (this.step > 15) this.step = 15;
          this.wait = this.period - 1;
          if (--this.left <= 0) { if (this.nib) { this.pos++; this.nib = 0; } this.endBlock(); }
          break;
        }
      }
      this.out = this.sample / 128;
    }
    clocks(n) {
      this.clk += n;
      while (this.clk >= 4) { this.clk -= 4; this.tick(); this.acc += this.out; this.accN++; }
    }
    saveState() {
      const o = {};
      for (const k of ["st", "rst", "data", "state", "pos", "left", "period", "wait", "sample", "step", "nib", "repeat", "repeatPos", "out", "clk"]) o[k] = this[k];
      return o;
    }
    loadState(o) { for (const k in o) this[k] = o[k]; }
  }

  global.RPUPD7759 = UPD7759;
})(typeof window !== "undefined" ? window : globalThis);
