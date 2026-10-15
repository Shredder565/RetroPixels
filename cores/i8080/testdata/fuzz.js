// Runs the web 8080 core (web/emu/i8080.js) on a memory image of random
// code and writes its registers after every step, for i8080_test.go.
//
// usage: node fuzz.js <web/emu dir> <memory image> <steps>
// output per step: 16 u32: a f b c d e h l sp pc inte halted ei, cycles, port sum, memory sum
"use strict";
const fs = require("fs"), path = require("path");
const [emu, memPath, stepsArg] = process.argv.slice(2);
require(path.join(emu, "i8080.js"));
const mem = new Uint8Array(fs.readFileSync(memPath));
let inCount = 0, outSum = 0, memSum = 0;
const cpu = new globalThis.RP8080({
  read: (a) => mem[a],
  write: (a, v) => { mem[a] = v; memSum = (Math.imul(memSum, 31) + a * 256 + v) >>> 0; },
  in: (p) => (p * 31 + inCount++) & 0xff,
  out: (p, v) => { outSum = (Math.imul(outSum, 33) + p * 256 + v) >>> 0; },
});
const steps = +stepsArg;
const out = fs.openSync("/dev/stdout", "w");
const buf = new Uint32Array(16 * 4096);
let n = 0;
function flush() { const b = Buffer.from(buf.buffer, 0, n * 4); let o = 0; while (o < b.length) o += fs.writeSync(out, b, o, b.length - o); n = 0; }
for (let i = 0; i < steps; i++) {
  let t;
  if (i % 50 === 49) t = cpu.interrupt(0xc7 | (((i / 50) & 7) << 3)) || 0;
  else t = cpu.step();
  // stuck with interrupts off: move on (as the Go test does)
  if (i % 1000 === 999) { const s = cpu.getState(); if (s[11] && !s[10]) { s[11] = 0; s[9] = (s[9] + 0x1234) & 0xffff; cpu.setState(s); } }
  const s = cpu.getState();
  for (let k = 0; k < 13; k++) buf[n++] = s[k];
  buf[n++] = t; buf[n++] = outSum; buf[n++] = memSum;
  if (n >= buf.length) flush();
}
flush();
