// Runs the web Z80 (web/emu/z80.js) in Node on the SingleStepTests/z80 cases
// of one JSON file (each case: a processor state and memory, one Step) and
// writes the result of every case to stdout, for z80_test.go to compare with
// the Go processor.
//
// usage: node z80ref.js <web/emu dir> <tests.json>
// output per case: u32 T-states, 27 x u32 state (getState order), u32 writes,
// writes x (u32 addr, u32 value), u32 outs, outs x (u32 port, u32 value).
"use strict";
const fs = require("fs"), path = require("path");
const [emu, file] = process.argv.slice(2);
require(path.join(emu, "z80.js"));
const mem = new Uint8Array(65536);
let ports = [], writes = [], outs = [];
const z = new globalThis.RPZ80({
  read: (a) => mem[a],
  write: (a, v) => { mem[a] = v; writes.push(a, v); },
  in: (p) => { const e = ports.find((x) => x[0] === p && x[2] === "r"); return e ? e[1] : 0xff; },
  out: (p, v) => { outs.push(p, v); },
});
const KEYS = ["a", "f", "b", "c", "d", "e", "h", "l", "ix", "iy", "sp", "pc", "i", "r", "wz", "af_", "bc_", "de_", "hl_", "iff1", "iff2", "im"];
const tests = JSON.parse(fs.readFileSync(file));
const parts = [];
for (const t of tests) {
  const I = t.initial;
  mem.fill(0);
  for (const [a, v] of I.ram) mem[a] = v;
  ports = t.ports || []; writes = []; outs = [];
  z.setState([...KEYS.map((k) => I[k]), 0, I.ei, I.q, 0, 0]);
  const used = z.step();
  const s = z.getState();
  const u = new Uint32Array(1 + 27 + 1 + writes.length + 1 + outs.length);
  let k = 0;
  u[k++] = used;
  for (const v of s) u[k++] = +v;
  u[k++] = writes.length / 2; for (const v of writes) u[k++] = v;
  u[k++] = outs.length / 2; for (const v of outs) u[k++] = v;
  parts.push(Buffer.from(u.buffer));
}
const all = Buffer.concat(parts);
const out = fs.openSync("/dev/stdout", "w");
for (let off = 0; off < all.length;) off += fs.writeSync(out, all, off, all.length - off);
