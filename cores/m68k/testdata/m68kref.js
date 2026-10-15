// Runs the web 68000 (web/emu/m68k.js) in Node on the cases m68k_test.go
// writes (a processor state, an instruction stream and a few steps each) and
// writes every case's result to stdout, for the test to compare with the Go
// processor.
//
// usage: node m68kref.js <web/emu dir> <cases file>
// case: u32 seed, 8 u32 D, 8 u32 A, u32 PC, USP, SSP, SR, u32 irq, u32 ack
// (0 = autovector, else the vector Ack returns), u32 flags (1 stopped, 2 nmiSeen,
// 4 TAS writes), u32 steps, u32 words, words x u32 (written at PC).
// Memory not written holds a pattern made from the seed and the address.
// output per case: u32 cycles, 8 D, 8 A, PC, USP, SSP, SR, flags, irq,
// u32 accesses, accesses x (u32 kind, u32 addr, u32 value); kind 1 read8,
// 2 read16, 3 write8, 4 write16, 5 ack, 6 reset.
"use strict";
const fs = require("fs"), path = require("path");
const [emu, file] = process.argv.slice(2);
require(path.join(emu, "m68k.js"));
let seed = 0, mem = new Map(), log = [], ackVec = 0;
const pat = (a) => { const h = (Math.imul(a, 0x9e3779b1) + seed) >>> 0; return ((h >>> 13) ^ (h >>> 5) ^ (h >>> 24)) & 0xff; };
const rb = (a) => { const v = mem.get(a); return v === undefined ? pat(a) : v; };
const cpu = new globalThis.RPM68K({
  read8: (a) => { const v = rb(a); log.push(1, a, v); return v; },
  read16: (a) => { const v = (rb(a) << 8) | rb(a + 1); log.push(2, a, v); return v; },
  write8: (a, v) => { mem.set(a, v); log.push(3, a, v); },
  write16: (a, v) => { mem.set(a, v >> 8); mem.set(a + 1, v & 0xff); log.push(4, a, v); },
  ack: (lv) => { log.push(5, lv, ackVec); return ackVec ? ackVec : -1; },
  reset: () => { log.push(6, 0, 0); },
});
const inp = fs.readFileSync(file);
const u = new Uint32Array(inp.buffer, inp.byteOffset, inp.length / 4);
const out = fs.openSync("/dev/stdout", "w");
let parts = [], partLen = 0;
function flush() {
  const all = Buffer.concat(parts); parts = []; partLen = 0;
  for (let off = 0; off < all.length;) off += fs.writeSync(out, all, off, all.length - off);
}
let k = 0;
while (k < u.length) {
  seed = u[k++];
  const d = Array.from(u.subarray(k, k + 8)); k += 8;
  const a = Array.from(u.subarray(k, k + 8)); k += 8;
  const pc = u[k++], usp = u[k++], ssp = u[k++], sr = u[k++], irq = u[k++];
  ackVec = u[k++];
  const flags = u[k++], steps = u[k++], nw = u[k++];
  mem = new Map();
  for (let i = 0; i < nw; i++) { const w = u[k++], ad = (pc + i * 2) & 0xffffff; mem.set(ad, w >> 8); mem.set((ad + 1) & 0xffffff, w & 0xff); }
  cpu.loadState({ d, a, pc, usp, ssp, sr, cycles: 0, stopped: !!(flags & 1), irq, nmiSeen: !!(flags & 2) });
  cpu.tasWrites = !!(flags & 4);
  log = [];
  for (let s = 0; s < steps; s++) cpu.step();
  const st = cpu.saveState();
  const r = new Uint32Array(1 + 8 + 8 + 4 + 2 + 1 + log.length);
  let j = 0;
  r[j++] = cpu.cycles;
  for (const v of st.d) r[j++] = v;
  for (const v of st.a) r[j++] = v;
  r[j++] = st.pc; r[j++] = st.usp; r[j++] = st.ssp; r[j++] = st.sr;
  r[j++] = (st.stopped ? 1 : 0) | (st.nmiSeen ? 2 : 0); r[j++] = st.irq;
  r[j++] = log.length / 3;
  for (const v of log) r[j++] = v;
  const b = Buffer.from(r.buffer);
  parts.push(b); partLen += b.length;
  if (partLen > 1 << 22) flush();
}
flush();
