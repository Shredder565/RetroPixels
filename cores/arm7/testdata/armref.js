// Runs web/emu/arm7.js on a program in a flat 64K memory and prints its
// state after every instruction, for arm7_test.go to compare with the Go core.
// usage: node armref.js <web/emu dir> <memory image> <steps> <thumb 0|1>
// output: u32 steps run, then per step 18 u32: r0-r15, CPSR, pc; at the end
// the memory image.
"use strict";
const fs = require("fs"), path = require("path");
const [emu, img, stepsArg, thumb] = process.argv.slice(2);
require(path.join(emu, "arm7.js"));
const mem = new Uint8Array(fs.readFileSync(img));
const r8 = (a) => mem[a & 0xffff];
const r16 = (a) => { a &= ~1; return r8(a) | (r8(a + 1) << 8); };
const r32 = (a) => { a &= ~3; return (r8(a) | (r8(a + 1) << 8) | (r8(a + 2) << 16) | (r8(a + 3) << 24)) | 0; };
const w8 = (a, v) => { a &= 0xffff; if (a >= 0x100) mem[a] = v; }; // the vectors stay put
const bus = {
  read8: r8, read16: r16, read32: r32, fetch16: r16, fetch32: r32,
  write8: (a, v) => w8(a, v & 0xff),
  write16: (a, v) => { a &= ~1; w8(a, v & 0xff); w8(a + 1, (v >> 8) & 0xff); },
  write32: (a, v) => { a &= ~3; w8(a, v & 0xff); w8(a + 1, (v >> 8) & 0xff); w8(a + 2, (v >> 16) & 0xff); w8(a + 3, (v >>> 24) & 0xff); },
};
const c = new globalThis.RPARM7(bus);
c.reset();
for (let i = 0; i < 13; i++) c.r[i] = (i * 0x9e3779b9) | 0;
c.r[13] = 0x8000;
if (thumb === "1") { c.t = 1; c.branch(0x100); } else c.branch(0x100);
const steps = +stepsArg;
const out = Buffer.alloc(steps * 18 * 4);
let done = 0;
for (let s = 0; s < steps; s++) {
  // The JS core throws when an MSR switches to Thumb without a jump (a 32-bit
  // word is then in the Thumb pipeline); the run ends there.
  try { c.step(); } catch (e) { break; }
  done++;
  const o = s * 72;
  for (let i = 0; i < 16; i++) out.writeInt32LE(c.r[i], o + i * 4);
  out.writeUInt32LE(c.getCPSR() >>> 0, o + 64);
  out.writeUInt32LE(c.pc >>> 0, o + 68);
}
const head = Buffer.alloc(4); head.writeUInt32LE(done);
process.stdout.write(head);
process.stdout.write(out.subarray(0, done * 72));
process.stdout.write(Buffer.from(mem));
