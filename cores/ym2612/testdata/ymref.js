// Runs the web YM2612 (web/emu/ym2612.js) in Node on a script of register
// writes and ticks and writes what the chip makes to stdout, for
// ym2612_test.go to compare with the Go chip.
//
// usage: node ymref.js <web/emu dir> <script file>
// script: u32 commands: 0 port value = write; 1 n = n ticks; 2 = read status;
// 3 = reset.
// output: per tick i32 left, i32 right; per status read u32 status.
"use strict";
const fs = require("fs"), path = require("path");
const [emu, file] = process.argv.slice(2);
require(path.join(emu, "ym2612.js"));
const ym = new globalThis.RPYM2612();
const inp = fs.readFileSync(file);
const u = new Uint32Array(inp.buffer, inp.byteOffset, inp.length / 4);
const out = [];
for (let k = 0; k < u.length;) {
  switch (u[k++]) {
    case 0: { const p = u[k++], v = u[k++]; ym.write(p, v); break; }
    case 1: { const n = u[k++]; for (let i = 0; i < n; i++) { ym.tick(); out.push(ym.out[0], ym.out[1]); } break; }
    case 2: out.push(ym.readStatus()); break;
    case 3: ym.reset(); break;
  }
}
const b = Buffer.from(new Int32Array(out).buffer);
const fd = fs.openSync("/dev/stdout", "w");
for (let off = 0; off < b.length;) off += fs.writeSync(fd, b, off, b.length - off);
