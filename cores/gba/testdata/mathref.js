// The floating-point maths of the web core's BIOS calls (gba.js), for
// math_test.go: the sine table, ArcTan for every input, and ArcTan2 and
// MidiKey2Freq for pseudo-random inputs (little-endian i32s), then Math.pow(2, e)
// over a grid of MidiKey2Freq exponents (f64s), to stdout.
"use strict";
const N = +process.argv[2] || 200000;
const out = [];
for (let i = 0; i < 256; i++) out.push(Math.round(Math.sin(i * Math.PI / 128) * 0x4000));
for (let v = 0; v < 0x10000; v++) { const x = (v << 16) >> 16; out.push(Math.floor(Math.atan(x / 0x4000) * 0x8000 / Math.PI)); }
let s = 0x2545f491;
const rnd = () => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; return s >>> 0; };
for (let i = 0; i < N; i++) {
  const r0 = rnd(), r1 = rnd();
  const x = (r0 << 16) >> 16, y = (r1 << 16) >> 16;
  let a = Math.atan2(y, x); if (a < 0) a += 2 * Math.PI;
  out.push(Math.round(a / (2 * Math.PI) * 0x10000) & 0xffff);
}
for (let i = 0; i < N; i++) {
  const freq = rnd() >>> (rnd() & 15), r1 = rnd() & ((i & 1) ? 0xff : 0xffffffff) | 0, r2 = rnd() & ((i & 2) ? 0xff : 0xffffffff) | 0;
  out.push(Math.floor(freq / Math.pow(2, (180 - r1 - r2 / 256) / 12)) | 0);
}
const b = Buffer.alloc(out.length * 4);
out.forEach((v, i) => b.writeInt32LE(v | 0, i * 4));
// and Math.pow(2, e) itself for every key -1024..2047 and fine adjustment 0..255
const p = Buffer.alloc(3072 * 256 * 8);
let k = 0;
for (let r1 = -1024; r1 < 2048; r1++) for (let r2 = 0; r2 < 256; r2++) p.writeDoubleLE(Math.pow(2, (180 - r1 - r2 / 256) / 12), 8 * k++);
process.stdout.write(Buffer.concat([b, p]));
