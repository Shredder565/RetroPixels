// Runs the web Konami-2 processor (web/emu/konami2.js) on memory of
// pseudo-random bytes and writes its registers after every step, for
// konami2_test.go to compare with the Go processor.
//
// usage: node jsref.js <web/emu dir> <seed> <steps>
// output per step: u16 a, b, x, y, u, s, pc, dp, cc, cycles, lines, and a u16
// checksum of memory (every 4096 steps, else 0).
"use strict";
const fs = require("fs"), path = require("path");
const [emu, seedArg, stepsArg] = process.argv.slice(2);
require(path.join(emu, "konami2.js"));
let s = BigInt(seedArg) | 1n;
const M = (1n << 64n) - 1n;
const rnd = () => { s ^= (s << 13n) & M; s ^= s >> 7n; s ^= (s << 17n) & M; return Number(s & 0xffffffffn); };
const mem = new Uint8Array(65536);
for (let i = 0; i < mem.length; i++) mem[i] = rnd() >>> 24;
const cpu = new globalThis.RPKonami2({
  read: (a) => mem[a], write: (a, v) => { mem[a] = v; },
  setLines: () => {}, ack: (k) => { if (k === "irq") cpu.irq = false; else if (k === "firq") cpu.firq = false; },
});
cpu.reset();
const steps = +stepsArg;
const out = Buffer.alloc(steps * 24);
for (let i = 0; i < steps; i++) {
  const r = rnd();
  if ((r & 0xff) === 1) cpu.irq = true;
  if ((r & 0xff) === 2) cpu.firq = true;
  if ((r & 0xfff) === 3) cpu.nmi();
  const c = cpu.step();
  let sum = 0;
  if ((i & 4095) === 4095) for (let k = 0; k < 65536; k++) sum = (sum * 31 + mem[k]) & 0xffff;
  const v = [cpu.a, cpu.b, cpu.x, cpu.y, cpu.u, cpu.s, cpu.pc, cpu.dp, cpu.cc, c, cpu.lines, sum];
  v.forEach((x, k) => out.writeUInt16LE(x & 0xffff, i * 24 + k * 2));
}
fs.writeSync(fs.openSync("/dev/stdout", "w"), out);
