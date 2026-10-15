// Runs the web MCS-48 core (web/emu/mcs48.js) on a ROM of random code and
// writes its state after every step, for mcs48_test.go.
//
// usage: node fuzz.js <web/emu dir> <rom image (4 KB)> <steps> <expander 0/1>
// output per step: the 19 state values, cycles, the I/O sum, then the 64 bytes of RAM
"use strict";
const fs = require("fs"), path = require("path");
const [emu, romPath, stepsArg, expArg] = process.argv.slice(2);
require(path.join(emu, "mcs48.js"));
const rom = new Uint8Array(fs.readFileSync(romPath));
let step = 0, count = 0, sum = 0;
const mix = (v) => { sum = (Math.imul(sum, 33) + v) >>> 0; };
const bus = {
  rom: (a) => rom[a],
  readX: (a) => (a * 7 + count++) & 0xff,
  writeX: (a, v) => mix(0x10000 | (a << 8) | v),
  outP: (p, v) => mix(0x20000 | (p << 8) | v),
  inP: (p) => (p * 13 + count++ * 3) & 0xff,
  t0: () => ((step >> 3) & 1) === 1,
  t1: () => ((step * 7 >> 2) & 1) === 1,
  intLine: () => step % 97 < 5,
};
if (expArg === "1") bus.expander = (op, port, v) => { mix(0x30000 | (op << 12) | (port << 8) | v); return (op * 5 + port + v + count++) & 0xff; };
const cpu = new globalThis.RPMCS48(bus, { ram: 64 });
const steps = +stepsArg;
const out = fs.openSync("/dev/stdout", "w");
const REC = 21 * 4 + 64;
const buf = Buffer.alloc(REC * 2048);
let n = 0;
function flush() { let o = 0; while (o < n) o += fs.writeSync(out, buf, o, n - o); n = 0; }
for (step = 0; step < steps; step++) {
  const t = cpu.step();
  const s = cpu.getState();
  for (let k = 0; k < 19; k++) { buf.writeInt32LE(s.r[k], n); n += 4; }
  buf.writeInt32LE(t, n); n += 4;
  buf.writeUInt32LE(sum, n); n += 4;
  buf.set(s.ram, n); n += 64;
  if (n >= buf.length) flush();
}
flush();
