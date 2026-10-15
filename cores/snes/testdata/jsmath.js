// Computes Math functions for the inputs Go wrote (jsmath_test.go): each
// record is an op code (f64) and two arguments; writes one f64 per record.
const fs = require("fs");
const inp = fs.readFileSync(process.argv[2]);
const n = inp.length / 24, out = Buffer.alloc(n * 8);
const F = [Math.sin, Math.cos, Math.tan, Math.exp, Math.log2, (a, b) => Math.hypot(a, b), Math.round, Math.atan];
for (let i = 0; i < n; i++) {
  const op = inp.readDoubleLE(i * 24), a = inp.readDoubleLE(i * 24 + 8), b = inp.readDoubleLE(i * 24 + 16);
  out.writeDoubleLE(F[op](a, b), i * 8);
}
fs.writeFileSync(process.argv[3], out);
