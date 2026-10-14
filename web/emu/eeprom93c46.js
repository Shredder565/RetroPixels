// RetroPixels — 93C46 serial EEPROM (64 words of 16 bits). Original code,
// written from the chip's documented serial protocol.
//
// Konami boards keep their settings and high scores in one of these. The game
// drives three lines (chip select, clock, data in) and reads one (data out):
//   - with chip select high, each rising clock edge shifts one bit in:
//     a start bit (1), a 2-bit opcode, a 6-bit address, then data for writes;
//   - READ (10 aaaaaa): data out goes to 0 (the "dummy" bit) right after the
//     address, then each further rising edge puts out the next bit, D15 first;
//     reading carries on into the following words while the clock keeps going;
//   - WRITE (01 aaaaaa + 16 bits), ERASE (11 aaaaaa), and with opcode 00 the
//     address's top two bits pick EWDS (00), WRAL (01), ERAL (10) or EWEN (11).
//     Writes and erases only work after EWEN (and until EWDS). They finish at
//     once here, so data out reads 1 ("ready") when chip select comes back.
//   - chip select low ends the command.
//
// Interface:
//   const e = new RPEeprom93C46(initialBytes?)   initialBytes: 128 bytes (big-endian words), e.g. from getSRAM
//   e.write(cs, clk, di)    set the three input lines (booleans / 0-1)
//   e.read() -> 0 / 1       the data-out line
//   e.bytes -> Uint8Array(128)   the contents (big-endian words) for getSRAM
//   e.dirty                 true after a write changed the contents
//   e.getState() / e.setState(o)
"use strict";

(function (global) {
  class RPEeprom93C46 {
    constructor(initial) {
      this.bytes = new Uint8Array(128).fill(0xff);
      if (initial && initial.length) this.bytes.set(initial.subarray ? initial.subarray(0, 128) : initial.slice(0, 128));
      this.cs = 0; this.clk = 0; this.di = 0; this.dout = 1;
      this.shift = 0; this.bits = 0;    // command bits gathered so far
      this.state = 0;                    // 0 idle (waiting for start bit), 1 command, 2 reading, 3 write data
      this.addr = 0; this.out = 0; this.outBits = 0;
      this.enabled = false;
      this.dirty = false;
    }

    word(a) { return (this.bytes[a * 2] << 8) | this.bytes[a * 2 + 1]; }
    setWord(a, v) {
      if (this.word(a) !== v) this.dirty = true;
      this.bytes[a * 2] = v >> 8; this.bytes[a * 2 + 1] = v & 0xff;
    }

    write(cs, clk, di) {
      cs = cs ? 1 : 0; clk = clk ? 1 : 0; di = di ? 1 : 0;
      if (!cs) {
        if (this.cs) { this.state = 0; this.bits = 0; this.shift = 0; }
        this.cs = 0; this.clk = clk; this.di = di; this.dout = 1;
        return;
      }
      if (!this.cs) { this.state = 0; this.bits = 0; this.shift = 0; this.dout = 1; }
      const rising = clk && !this.clk;
      this.cs = 1; this.clk = clk; this.di = di;
      if (rising) this.clock(di);
    }

    clock(di) {
      switch (this.state) {
        case 0: // wait for the start bit
          if (di) { this.state = 1; this.bits = 0; this.shift = 0; }
          return;
        case 1: // opcode + address
          this.shift = (this.shift << 1) | di; this.bits++;
          if (this.bits === 8) this.command(this.shift >> 6, this.shift & 0x3f);
          return;
        case 2: // reading: next bit out
          if (this.outBits === 0) { this.addr = (this.addr + 1) & 63; this.out = this.word(this.addr); this.outBits = 16; }
          this.dout = (this.out >> 15) & 1; this.out = (this.out << 1) & 0xffff; this.outBits--;
          return;
        case 3: // gathering 16 data bits
          this.shift = ((this.shift << 1) | di) & 0xffff; this.bits++;
          if (this.bits === 16) {
            if (this.writeAll) { for (let a = 0; a < 64; a++) this.setWord(a, this.shift); }
            else this.setWord(this.addr, this.shift);
            this.state = 4; this.dout = 1;
          }
          return;
        default: return; // finished: wait for chip select to drop
      }
    }

    command(op, a) {
      this.addr = a;
      switch (op) {
        case 2: // READ
          this.state = 2; this.dout = 0; this.out = this.word(a); this.outBits = 16;
          return;
        case 1: // WRITE
          if (!this.enabled) { this.state = 4; return; }
          this.state = 3; this.bits = 0; this.shift = 0; this.writeAll = false;
          return;
        case 3: // ERASE
          if (this.enabled) this.setWord(a, 0xffff);
          this.state = 4; this.dout = 1;
          return;
        default:
          switch (a >> 4) {
            case 0: this.enabled = false; break;                                   // EWDS
            case 1: if (this.enabled) { this.state = 3; this.bits = 0; this.shift = 0; this.writeAll = true; return; } break; // WRAL
            case 2: if (this.enabled) for (let i = 0; i < 64; i++) this.setWord(i, 0xffff); break; // ERAL
            case 3: this.enabled = true; break;                                    // EWEN
          }
          this.state = 4; this.dout = 1;
      }
    }

    read() { return this.dout; }

    getState() {
      return { bytes: this.bytes.slice(), r: [this.cs, this.clk, this.di, this.dout, this.shift, this.bits, this.state, this.addr,
        this.out, this.outBits, this.enabled ? 1 : 0, this.writeAll ? 1 : 0] };
    }
    setState(o) {
      this.bytes.set(o.bytes);
      let en, wa;
      [this.cs, this.clk, this.di, this.dout, this.shift, this.bits, this.state, this.addr, this.out, this.outBits, en, wa] = o.r;
      this.enabled = !!en; this.writeAll = !!wa;
    }
  }

  global.RPEeprom93C46 = RPEeprom93C46;
})(typeof window !== "undefined" ? window : globalThis);
