// Runs web/emu/cpu6502.js on pseudo-random memory and prints one line per
// instruction (registers + a hash of every bus access), for cpu_test.go to
// compare against the Go port. usage: node trace.js <cpu6502.js> <seed> <steps> <decimal 0|1> <machinePolls 0|1>
"use strict";
const fs = require("fs");
const [file, seedS, stepsS, decS, mpS] = process.argv.slice(2);
(0, eval)(fs.readFileSync(file, "utf8"));
let seed = (+seedS >>> 0) || 1;
const rnd = () => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed; };
const mem = new Uint8Array(65536);
for (let i = 0; i < 65536; i++) mem[i] = rnd() & 255;
let h = 0;
const mix = (x) => { h = Math.imul(h ^ x, 16777619) >>> 0; };
let cpu;
const mp = decS !== undefined && +mpS === 1;
const bus = {
  read(a) { mix(a); mix(mem[a]); if (mp) cpu.poll(); return mem[a]; },
  write(a, v) { mix(a | 0x10000); mix(v); if (mp) cpu.poll(); mem[a] = v; },
};
cpu = new globalThis.RPCPU6502(bus, { decimal: +decS === 1, machinePolls: mp });
cpu.reset();
const out = [];
for (let i = 0; i < +stepsS; i++) {
  const r = rnd();
  if ((r & 63) === 0) cpu.nmiPending = true;
  if (((r >>> 8) & 15) === 0) cpu.irqLine = !cpu.irqLine;
  if (cpu.jammed && ((r >>> 12) & 7) === 0) cpu.reset();
  h = 2166136261;
  const n = cpu.step();
  out.push(`${cpu.a} ${cpu.x} ${cpu.y} ${cpu.s} ${cpu.p} ${cpu.pc} ${cpu.cycles} ${n} ${h}`);
}
process.stdout.write(out.join("\n") + "\n");
