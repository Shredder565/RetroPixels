// RetroPixels — SNES DSP-1 / DSP-1A / DSP-1B and DSP-2 cartridge chips. Original code.
//
// These cartridges carry an NEC uPD77C25 signal processor running a fixed
// program. RetroPixels doesn't use that program: it computes what each
// documented command computes (high-level emulation), in the same 16-bit fixed
// point steps the documented algorithms use, so results come out the same as the
// hardware's wherever those algorithms are known. The few constant tables the
// algorithms need (sine, reciprocal seeds, square roots, powers of two) are
// calculated here from their mathematical definitions.
//
// Talking to the chip: the game writes a command byte to the data register (DR),
// then its parameters (16-bit words, low byte first, for DSP-1; bytes for DSP-2),
// then reads the results back from DR. The status register (SR) always says
// "ready" because the work is done instantly.
"use strict";

(function (global) {
  const Kit = global.RPSNESChipKit;
  const s16 = (x) => (x << 16) >> 16;

  // ===================================================================
  // DSP-1 constant tables
  // ===================================================================
  // Sine in 1/256 turns, 1.15 fixed point, cut toward zero (the second half is
  // the first half negated).
  const SIN = new Int16Array(256);
  for (let k = 0; k < 128; k++) { SIN[k] = Math.min(32767, Math.floor(32768 * Math.sin(2 * Math.PI * k / 256) + 1e-9)); SIN[k + 128] = -SIN[k]; }
  // Interpolation step for the low byte of an angle: floor(k * pi).
  const MUL = new Int16Array(256);
  for (let k = 0; k < 256; k++) MUL[k] = Math.floor(k * Math.PI);
  // Power-of-two table used by the normalise / truncate steps. Entry $22+k is
  // 2^k, $31 is $7FFF, $32.. counts down from $4000. Entry $3C holds 1 instead
  // of $10 — a known quirk of the chip's table that changes results slightly.
  const POW = new Int16Array(0x41);
  for (let k = 0; k <= 14; k++) POW[0x22 + k] = 1 << k;
  POW[0x31] = 0x7fff;
  for (let k = 0; k <= 14; k++) POW[0x32 + k] = 0x4000 >> k;
  POW[0x3c] = 0x0001;
  const pow = (i) => (i >= 0 && i <= 0x40 ? POW[i] : 0);
  // Reciprocal seeds for 1.15 values $4000 + 128k: 2^29 / x, rounded.
  const RECIP = new Int16Array(128);
  for (let k = 0; k < 128; k++) RECIP[k] = Math.min(0x7fff, Math.round(2 ** 29 / (0x4000 + 128 * k)));
  // Square roots: entry p is sqrt(p / 64) in 1.15.
  const SQRT = new Int16Array(0x41);
  for (let p = 0; p <= 0x40; p++) SQRT[p] = Math.min(0x7fff, Math.floor(4096 * Math.sqrt(p)));
  // Short polynomial terms used when the view angle gets clipped (sine and
  // cosine series in units of 1/8 turn).
  const Q = Math.PI / 4;
  const K_SIN1 = Math.round(32768 * Q), K_SIN3 = s16(Math.round(-32768 * Q * Q * Q / 6));
  const K_COS2 = s16(Math.round(-32768 * Q * Q / 2)), K_COS4 = Math.round(32768 * Q ** 4 / 24);
  // Highest zenith angle allowed for each height exponent (keeps the horizon on screen).
  const MAX_AZS = [0x38b4, 0x38b7, 0x38ba, 0x38be, 0x38c0, 0x38c4, 0x38c7, 0x38ca, 0x38ce, 0x38d0, 0x38d4, 0x38d7, 0x38da, 0x38dd, 0x38e0, 0x38e4];

  // ===================================================================
  // DSP-1 arithmetic
  // ===================================================================
  function dsin(a) {
    if (a < 0) { if (a === -32768) return 0; return -dsin(-a); }
    const s = SIN[a >> 8] + ((MUL[a & 0xff] * SIN[0x40 + (a >> 8)]) >> 15);
    return s > 32767 ? 32767 : s;
  }
  function dcos(a) {
    if (a < 0) { if (a === -32768) return -32768; a = -a; }
    const s = SIN[0x40 + (a >> 8)] - ((MUL[a & 0xff] * SIN[a >> 8]) >> 15);
    return s < -32768 ? -32767 : s;
  }
  // Shift a 16-bit value left until its top bits differ; value = c * 2^e.
  function norm(m, e) {
    let i = 0x4000, n = 0;
    if (m < 0) { while (i && (m & i)) { i >>= 1; n++; } } else { while (i && !(m & i)) { i >>= 1; n++; } }
    return [n ? s16(m << n) : m, e - n];
  }
  // Normalise a 32-bit product; returns [coefficient, left shifts used].
  function normD(p) {
    p |= 0;
    const lo = p & 0x7fff, m = s16(p >> 15);
    let i = 0x4000, e = 0, c;
    if (m < 0) { while (i && (m & i)) { i >>= 1; e++; } } else { while (i && !(m & i)) { i >>= 1; e++; } }
    if (e > 0) {
      c = s16(m << e);
      if (e < 15) c = s16(c + ((lo * pow(0x40 - e)) >> 15));
      else {
        i = 0x4000;
        if (m < 0) { while (i && (lo & i)) { i >>= 1; e++; } } else { while (i && !(lo & i)) { i >>= 1; e++; } }
        c = e > 15 ? s16((lo * pow(0x12 + e)) << 1) : s16(c + lo);
      }
    } else c = m;
    return [c, e];
  }
  // 1 / (c * 2^e) with c in 1.15 -> [coefficient, exponent] (two Newton steps).
  function inv(c, e) {
    if (c === 0) return [0x7fff, 0x2f];
    let sign = 1;
    if (c < 0) { if (c < -32767) c = -32767; c = -c; sign = -1; }
    while (c < 0x4000) { c <<= 1; e--; }
    let r;
    if (c === 0x4000) {
      if (sign === 1) r = 0x7fff; else { r = -0x4000; e--; }
    } else {
      let i = RECIP[(c - 0x4000) >> 7];
      i = s16((i + ((-i * ((c * i) >> 15)) >> 15)) << 1);
      i = s16((i + ((-i * ((c * i) >> 15)) >> 15)) << 1);
      r = i * sign;
    }
    return [s16(r), s16(1 - e)];
  }
  // c * 2^e as a plain 16-bit number (saturating when it would grow).
  function trunc(c, e) {
    if (e > 0) return c > 0 ? 32767 : c < 0 ? -32767 : 0;
    if (e < 0) return (c * pow(0x31 + e)) >> 15;
    return c;
  }
  const shiftR = (c, e) => (c * pow(0x31 - e)) >> 15;
  const mul15 = (a, b) => (a * b) >> 15;

  // ===================================================================
  // DSP-1 command processor
  // ===================================================================
  class DSP1Core {
    constructor() { this.reset(); }
    reset() {
      this.mat = [new Int16Array(9), new Int16Array(9), new Int16Array(9)];
      // projection set-up from the Parameter command
      this.p = { sinA: 0, cosA: 0, sinZ: 0, cosZ: 0, nx: 0, ny: 0, nz: 0, cx: 0, cy: 0, cz: 0, gx: 0, gy: 0, gz: 0,
        cLes: 0, eLes: 0, gLes: 0, vpC: 0, vpE: 0, vOff: 0, sec1C: 0, sec1E: 0, sec2C: 0, sec2E: 0 };
    }
    // --- matrices ---
    attitude(M, m, az, ay, ax) {
      const sz = dsin(az), cz = dcos(az), sy = dsin(ay), cy = dcos(ay), sx = dsin(ax), cx = dcos(ax);
      m >>= 1;
      const mcz = mul15(m, cz), msz = mul15(m, sz), msx = mul15(m, sx), mcx = mul15(m, cx);
      M[0] = mul15(mcz, cy);
      M[1] = -mul15(msz, cy);
      M[2] = mul15(m, sy);
      M[3] = mul15(msz, cx) + mul15(mul15(mcz, sx), sy);
      M[4] = mul15(mcz, cx) - mul15(mul15(msz, sx), sy);
      M[5] = -mul15(msx, cy);
      M[6] = mul15(msz, sx) - mul15(mul15(mcz, cx), sy);
      M[7] = mul15(mcz, sx) + mul15(mul15(msz, cx), sy);
      M[8] = mul15(mcx, cy);
    }
    // global (X,Y,Z) -> object (F,L,U)
    objective(M, x, y, z) {
      return [s16(mul15(x, M[0]) + mul15(y, M[1]) + mul15(z, M[2])), s16(mul15(x, M[3]) + mul15(y, M[4]) + mul15(z, M[5])), s16(mul15(x, M[6]) + mul15(y, M[7]) + mul15(z, M[8]))];
    }
    // object (F,L,U) -> global (X,Y,Z)
    subjective(M, f, l, u) {
      return [s16(mul15(f, M[0]) + mul15(l, M[3]) + mul15(u, M[6])), s16(mul15(f, M[1]) + mul15(l, M[4]) + mul15(u, M[7])), s16(mul15(f, M[2]) + mul15(l, M[5]) + mul15(u, M[8]))];
    }
    scalar(M, x, y, z) { return [s16(((x * M[0] + y * M[1] + z * M[2]) | 0) >> 15)]; }
    // --- simple maths ---
    multiply(k, i, plus) { return [s16(mul15(k, i) + (plus ? 1 : 0))]; }
    inverse(c, e) { return inv(c, e); }
    triangle(a, r) { return [s16(mul15(dsin(a), r)), s16(mul15(dcos(a), r))]; }
    radius(x, y, z) { const v = ((x * x + y * y + z * z) * 2) | 0; return [v & 0xffff, (v >>> 16) & 0xffff]; }
    range(x, y, z, r, plus) { return [s16((((x * x + y * y + z * z - r * r) | 0) >> 15) + (plus ? 1 : 0))]; }
    distance(x, y, z) {
      const r = (x * x + y * y + z * z) | 0;
      if (r === 0) return [0];
      let [c, e] = normD(r);
      if (e & 1) c = mul15(c, 0x4000);
      const pos = mul15(c, 0x40);
      const n1 = SQRT[pos], n2 = SQRT[pos + 1];
      const d = s16((((n2 - n1) * (c & 0x1ff)) >> 9) + n1);
      return [d >> (e >> 1)];
    }
    rotate(a, x1, y1) {
      const s = dsin(a), c = dcos(a);
      return [s16(mul15(y1, s) + mul15(x1, c)), s16(mul15(y1, c) - mul15(x1, s))];
    }
    polar(az, ay, ax, x, y, z) {
      let s = dsin(az), c = dcos(az);
      let x1 = s16(mul15(y, s) + mul15(x, c)), y1 = s16(mul15(y, c) - mul15(x, s));
      x = x1; y = y1;
      s = dsin(ay); c = dcos(ay);
      let z1 = s16(mul15(x, s) + mul15(z, c)); x1 = s16(mul15(x, c) - mul15(z, s));
      z = z1; x = x1;
      s = dsin(ax); c = dcos(ax);
      y1 = s16(mul15(z, s) + mul15(y, c)); z1 = s16(mul15(z, c) - mul15(y, s));
      return [x, y1, z1];
    }
    // Turn body-relative turning speeds (U, F, L) into new attitude angles.
    gyrate(zr, xr, yr, u, f, l) {
      const [cSec, eSec] = inv(dcos(xr), 0);
      const sy = dsin(yr), cy = dcos(yr);
      // around Z: (U cos Y - F sin Y) / cos X
      let [c, e] = normD(u * cy - f * sy);
      [c, e] = norm(mul15(c, cSec), eSec - e);
      const dz = trunc(c, e);
      // around X
      const xrr = s16(xr + mul15(u, sy) + mul15(f, cy));
      // around Y: L minus the Z turn times tan X
      const [cSin, eSin] = norm(dsin(xr), 0);
      const [c2, e2] = norm(mul15(c, cSin), e + eSin);
      const yrr = s16(yr + l - trunc(c2, e2));
      return [s16(zr + dz), xrr, yrr];
    }
    // --- Mode 7 perspective ---
    parameter(fx, fy, fz, lfe, les, aas, azs) {
      const p = this.p;
      p.sinA = dsin(aas); p.cosA = dcos(aas); p.sinZ = dsin(azs); p.cosZ = dcos(azs);
      p.nx = mul15(p.sinZ, -p.sinA); p.ny = mul15(p.sinZ, p.cosA); p.nz = mul15(p.cosZ, 0x7fff);
      // the eye sits Lfe along the normal from the focus point
      let cx = s16(fx + mul15(lfe, p.nx)), cy = s16(fy + mul15(lfe, p.ny));
      const cz = s16(fz + mul15(lfe, p.nz));
      // the screen centre is Les back toward the focus
      p.gx = s16(cx - mul15(les, p.nx)); p.gy = s16(cy - mul15(les, p.ny)); p.gz = s16(cz - mul15(les, p.nz));
      [p.cLes, p.eLes] = norm(les, 0); p.gLes = les;
      let [c, e] = norm(cz, 0);
      p.vpC = c; p.vpE = e; p.cz = cz;
      // clip the zenith angle so the horizon stays on screen
      let max = MAX_AZS[Math.min(15, -e)], az = azs;
      if (az < 0) { max = -max; if (az < max + 1) az = max + 1; } else if (az > max) az = max;
      const sinZ = dsin(az);
      let cosZ = dcos(az);
      [p.sec1C, p.sec1E] = inv(cosZ, 0);
      [c, e] = norm(mul15(c, p.sec1C), e);
      e += p.sec1E;
      c = mul15(trunc(c, e), sinZ);
      cx = s16(cx + mul15(c, p.sinA)); cy = s16(cy - mul15(c, p.cosA));
      p.cx = cx; p.cy = cy;
      // raster offset of the clipped part
      let vof = 0;
      if (az !== azs || azs === max) {
        let a = azs === -32768 ? -32767 : azs;
        c = s16(a - max); if (c >= 0) c--;
        let aux = s16(~(c << 2));
        c = mul15(aux, K_SIN3); c = s16(mul15(c, aux) + K_SIN1);
        vof = s16(vof - mul15(mul15(c, aux), les));
        c = mul15(aux, aux); aux = s16(mul15(c, K_COS4) + K_COS2);
        cosZ = s16(cosZ + mul15(mul15(c, aux), cosZ));
      }
      p.vOff = mul15(les, cosZ);
      // vanishing line: -Les cos Z / sin Z
      const [cSec, eSec] = inv(sinZ, 0);
      [c, e] = norm(p.vOff, eSec);
      [c, e] = norm(mul15(c, cSec), e);
      if (c === -32768) { c >>= 1; e++; }
      const vva = s16(trunc(s16(-c), e));
      [p.sec2C, p.sec2E] = inv(cosZ, 0);
      return [s16(vof), vva, cx, cy];
    }
    // Mode 7 matrix (8.8) for screen line vs.
    raster(vs) {
      const p = this.p;
      let [c, e] = inv(s16(mul15(vs, p.sinZ) + p.vOff), 7);
      e += p.vpE;
      const c1 = mul15(c, p.vpC);
      let e1 = e + p.sec2E;
      [c, e] = norm(c1, e);
      c = trunc(c, e);
      const an = s16(mul15(c, p.cosA)), cn = s16(mul15(c, p.sinA));
      [c, e1] = norm(mul15(c1, p.sec2C), e1);
      c = trunc(c, e1);
      return [an, s16(mul15(c, -p.sinA)), cn, s16(mul15(c, p.cosA))];
    }
    // world point -> screen (H, V) and scale M (8.8)
    project(x, y, z) {
      const p = this.p;
      let [px, ex] = normD(x - p.gx), [py, ey] = normD(y - p.gy), [pz, ez] = normD(z - p.gz);
      px >>= 1; ex--; py >>= 1; ey--; pz >>= 1; ez--;
      let ref = Math.min(ex, ey, ez);
      px = shiftR(px, ex - ref); py = shiftR(py, ey - ref); pz = shiftR(pz, ez - ref);
      const dot = s16(-mul15(px, p.nx) - mul15(py, p.ny) - mul15(pz, p.nz));
      ref = 16 - ref;
      let aux4 = ref >= 0 ? dot << ref : dot >> -ref; // 32-bit, wraps like the chip's
      if (aux4 === -1) aux4 = 0;
      aux4 >>= 1;
      const den = ((p.gLes & 0xffff) + aux4) | 0;
      let [c10, e2] = normD(den);
      e2 = 15 - e2;
      const [c4, e4] = inv(c10, 0);
      const c2 = mul15(c4, p.cLes);
      // H: along the screen's horizontal
      const hdot = s16(mul15(px, mul15(p.cosA, 0x7fff)) + mul15(py, mul15(p.sinA, 0x7fff)));
      let [c19, e7] = norm(mul15(hdot, c2), 0);
      const h = trunc(c19, p.eLes - e2 + ref + e7);
      // V: along the screen's vertical
      const vdot = s16(mul15(px, mul15(p.cosZ, -p.sinA)) + mul15(py, mul15(p.cosZ, p.cosA)) + mul15(pz, mul15(-p.sinZ, 0x7fff)));
      let [c25, e6] = norm(mul15(vdot, c2), 0);
      const v = trunc(c25, p.eLes - e2 + ref + e6);
      const [c6, e8] = norm(c2, e4);
      return [s16(h), s16(v), s16(trunc(c6, e8 + p.eLes - e2 - 7))];
    }
    // screen (H, V) -> ground point (X, Y)
    target(h, v) {
      const p = this.p;
      let [c, e] = inv(s16(mul15(v, p.sinZ) + p.vOff), 0);
      e += p.vpE;
      const c1 = mul15(c, p.vpC); // height / distance, scaled by 2^e
      // keep the full 32-bit products so far-away points stay accurate
      const [ch, nh] = normD(c1 * h);
      const dh = trunc(ch, e - nh);
      let [cv, nv] = normD(c1 * v);
      const [cs, es] = norm(mul15(cv, p.sec2C), e - nv + p.sec2E);
      const dv = trunc(cs, es);
      return [s16(p.cx + mul15(dh, p.cosA) - mul15(dv, p.sinA)), s16(p.cy + mul15(dh, p.sinA) + mul15(dv, p.cosA))];
    }
    // The chip's constant memory as far as RetroPixels rebuilds it (for the dump command).
    dataRom() {
      const d = new Uint16Array(1024);
      for (let i = 0x22; i <= 0x40; i++) d[i] = POW[i] & 0xffff;
      for (let k = 0; k < 128; k++) d[0x65 + k] = RECIP[k];
      for (let k = 0x10; k <= 0x40; k++) d[0xd5 + k] = SQRT[k];
      d[0x324] = K_COS4 & 0xffff; d[0x325] = K_COS2 & 0xffff; d[0x327] = K_SIN1 & 0xffff; d[0x328] = K_SIN3 & 0xffff;
      return Array.from(d);
    }
    // Command table: [parameter words, handler]
    command(cmd) {
      const M = this.mat;
      const A = M[0], B = M[1], C = M[2];
      switch (cmd) {
        case 0x00: return [2, (k, i) => this.multiply(k, i, false)];
        case 0x20: return [2, (k, i) => this.multiply(k, i, true)];
        case 0x10: case 0x30: return [2, (c, e) => this.inverse(c, e)];
        case 0x04: case 0x24: return [2, (a, r) => this.triangle(a, r)];
        case 0x08: return [3, (x, y, z) => this.radius(x, y, z)];
        case 0x18: return [4, (x, y, z, r) => this.range(x, y, z, r, false)];
        case 0x38: return [4, (x, y, z, r) => this.range(x, y, z, r, true)];
        case 0x28: return [3, (x, y, z) => this.distance(x, y, z)];
        case 0x0c: case 0x2c: return [3, (a, x, y) => this.rotate(a, x, y)];
        case 0x1c: case 0x3c: return [6, (az, ay, ax, x, y, z) => this.polar(az, ay, ax, x, y, z)];
        case 0x01: case 0x05: case 0x31: case 0x35: return [4, (m, z, y, x) => { this.attitude(A, m, z, y, x); return []; }];
        case 0x11: case 0x15: return [4, (m, z, y, x) => { this.attitude(B, m, z, y, x); return []; }];
        case 0x21: case 0x25: return [4, (m, z, y, x) => { this.attitude(C, m, z, y, x); return []; }];
        case 0x0d: case 0x09: case 0x39: case 0x3d: return [3, (x, y, z) => this.objective(A, x, y, z)];
        case 0x19: case 0x1d: return [3, (x, y, z) => this.objective(B, x, y, z)];
        case 0x29: case 0x2d: return [3, (x, y, z) => this.objective(C, x, y, z)];
        case 0x03: case 0x33: return [3, (f, l, u) => this.subjective(A, f, l, u)];
        case 0x13: return [3, (f, l, u) => this.subjective(B, f, l, u)];
        case 0x23: return [3, (f, l, u) => this.subjective(C, f, l, u)];
        case 0x0b: case 0x3b: return [3, (x, y, z) => this.scalar(A, x, y, z)];
        case 0x1b: return [3, (x, y, z) => this.scalar(B, x, y, z)];
        case 0x2b: return [3, (x, y, z) => this.scalar(C, x, y, z)];
        case 0x14: case 0x34: return [6, (z, x, y, u, f, l) => this.gyrate(z, x, y, u, f, l)];
        case 0x02: case 0x12: case 0x22: case 0x32: return [7, (fx, fy, fz, lfe, les, aas, azs) => this.parameter(fx, fy, fz, lfe, les, aas, azs)];
        case 0x0a: case 0x1a: case 0x2a: case 0x3a: return [1, (vs) => this.raster(vs), "raster"];
        case 0x06: case 0x16: case 0x26: case 0x36: return [3, (x, y, z) => this.project(x, y, z)];
        case 0x0e: case 0x1e: case 0x2e: case 0x3e: return [2, (h, v) => this.target(h, v)];
        case 0x07: case 0x0f: case 0x17: case 0x27: case 0x37: return [1, () => [0x0000]];
        case 0x2f: return [1, () => [0x0100]];
        case 0x1f: case 0x3f: return [1, () => this.dataRom()];
      }
      return null;
    }
    saveState() { return { mat: this.mat.map((m) => Array.from(m)), p: Object.assign({}, this.p) }; }
    loadState(o) { o.mat.forEach((m, i) => this.mat[i].set(m)); Object.assign(this.p, o.p); }
  }

  // ===================================================================
  // DR / SR port shared by the DSP chips
  // ===================================================================
  // `words`: parameters/results are 16-bit (DSP-1) or bytes (DSP-2).
  class DSPPort {
    constructor(core, words) { this.core = core; this.words = words; this.reset(); }
    reset() {
      this.cmd = -1; this.need = 0; this.params = []; this.partial = -1;
      this.out = []; this.outPos = 0; this.handler = null; this.raster = false; this.vs = 0;
    }
    writeDR(v) {
      if (this.cmd < 0 || this.outPos < this.out.length) { this.startCommand(v); return; }
      if (this.words) {
        if (this.partial < 0) { this.partial = v; return; }
        this.params.push(s16(this.partial | (v << 8))); this.partial = -1;
      } else this.params.push(v);
      this.more();
    }
    startCommand(v) {
      this.out = []; this.outPos = 0; this.params = []; this.partial = -1; this.raster = false;
      const c = this.core.command(v);
      if (!c) { this.cmd = -1; return; }
      this.cmd = v; this.need = c[0]; this.handler = c[1]; this.raster = c[2] === "raster";
      this.more();
    }
    // run the command once all parameters are in (the handler can ask for more)
    more() {
      if (this.params.length < this.need) return;
      const r = this.handler(...this.params);
      if (r && r.more) { this.need = r.more; return; } // variable-length command
      if (this.raster) this.vs = this.params[0];
      this.setOut(r || []);
      if (!this.out.length && !this.raster) this.cmd = -1;
    }
    setOut(vals) {
      const o = [];
      for (const w of vals) { if (this.words) o.push(w & 0xff, (w >> 8) & 0xff); else o.push(w & 0xff); }
      this.out = o; this.outPos = 0;
    }
    readDR() {
      if (this.outPos >= this.out.length) return 0x80;
      const v = this.out[this.outPos++];
      if (this.outPos >= this.out.length) {
        if (this.raster) { this.vs = s16(this.vs + 1); this.setOut(this.handler(this.vs)); } // next line follows
        else this.cmd = -1;
      }
      return v;
    }
    readSR() { return 0x80; }
    saveState() { return { cmd: this.cmd, need: this.need, params: this.params.slice(), partial: this.partial, out: this.out.slice(), outPos: this.outPos, vs: this.vs }; }
    loadState(o) {
      this.reset();
      const c = o.cmd >= 0 ? this.core.command(o.cmd) : null;
      if (c) { this.cmd = o.cmd; this.handler = c[1]; this.raster = c[2] === "raster"; }
      this.need = o.need; this.params = o.params.slice(); this.partial = o.partial; this.out = o.out.slice(); this.outPos = o.outPos; this.vs = o.vs;
    }
  }

  // ===================================================================
  // DSP-2 (Dungeon Master): bitmap helpers and arithmetic
  // ===================================================================
  class DSP2Core {
    constructor() { this.reset(); }
    reset() { this.transparent = 0; }
    command(cmd) {
      switch (cmd) {
        // 8x8 packed 4-bit pixels (2 per byte, left pixel in the high half) -> SNES 4bpp tile
        case 0x01: return [32, (...px) => {
          const t = new Array(32).fill(0);
          for (let y = 0; y < 8; y++) {
            for (let x = 0; x < 8; x++) {
              const b = px[y * 4 + (x >> 1)], c = x & 1 ? b & 15 : b >> 4, bit = 0x80 >> x;
              if (c & 1) t[y * 2] |= bit;
              if (c & 2) t[y * 2 + 1] |= bit;
              if (c & 4) t[16 + y * 2] |= bit;
              if (c & 8) t[17 + y * 2] |= bit;
            }
          }
          return t;
        }];
        case 0x03: return [1, (c) => { this.transparent = c & 15; return []; }];
        // lay one bitmap over another, skipping the transparent colour
        case 0x05: return [1, (n, ...rest) => {
          if (rest.length < 2 * n) return { more: 1 + 2 * n };
          const o = [], t = this.transparent;
          for (let i = 0; i < n; i++) {
            const a = rest[i], b = rest[n + i];
            o.push(((b >> 4) === t ? a & 0xf0 : b & 0xf0) | ((b & 15) === t ? a & 15 : b & 15));
          }
          return o;
        }];
        // mirror a row of pixels
        case 0x06: return [1, (n, ...rest) => {
          if (rest.length < n) return { more: 1 + n };
          const o = new Array(n);
          for (let i = 0; i < n; i++) { const b = rest[i]; o[n - 1 - i] = ((b << 4) | (b >> 4)) & 0xff; }
          return o;
        }];
        case 0x07: return [4, (a0, a1, b0, b1) => { const r = ((a0 | (a1 << 8)) + (b0 | (b1 << 8))) & 0xffff; return [r & 0xff, r >> 8]; }];
        case 0x08: return [4, (a0, a1, b0, b1) => { const r = ((a0 | (a1 << 8)) - (b0 | (b1 << 8))) & 0xffff; return [r & 0xff, r >> 8]; }];
        case 0x09: return [4, (a0, a1, b0, b1) => {
          const r = ((a0 | (a1 << 8)) * (b0 | (b1 << 8))) >>> 0;
          return [r & 0xff, (r >>> 8) & 0xff, (r >>> 16) & 0xff, r >>> 24];
        }];
        // stretch a row of pixels: out width, in width (pixels), then the row
        case 0x0d: return [2, (outW, inW, ...rest) => {
          const nIn = (inW + 1) >> 1;
          if (rest.length < nIn) return { more: 2 + nIn };
          const o = new Array((outW + 1) >> 1).fill(0);
          for (let i = 0; i < outW; i++) {
            const s = outW ? Math.floor(i * inW / outW) : 0;
            const b = rest[s >> 1] | 0, c = s & 1 ? b & 15 : b >> 4;
            o[i >> 1] |= i & 1 ? c : c << 4;
          }
          return o;
        }];
        case 0x0f: return [0, () => []];
      }
      return [0, () => []];
    }
    saveState() { return { t: this.transparent }; }
    loadState(o) { this.transparent = o.t; }
  }

  // ===================================================================
  // Cartridge glue
  // ===================================================================
  // ranges: { drBanks:[lo,hi], mirror, dr:[o0,o1], sr:[o2,o3] }
  class DSPChip {
    constructor(snes, kind) {
      this.snes = snes; this.kind = kind;
      this.core = kind === 2 ? new DSP2Core() : new DSP1Core();
      this.port = new DSPPort(this.core, kind !== 2);
      const cart = snes.cart;
      // where the chip's two registers appear
      if (kind === 2) this.m = { b0: 0x20, b1: 0x3f, dr0: 0x8000, dr1: 0xbfff, sr0: 0xc000, sr1: 0xffff };
      else if (cart.map !== "lo") this.m = { b0: 0x00, b1: 0x1f, dr0: 0x6000, dr1: 0x6fff, sr0: 0x7000, sr1: 0x7fff };
      else if (cart.size > 0x100000) this.m = { b0: 0x60, b1: 0x6f, dr0: 0x0000, dr1: 0x3fff, sr0: 0x4000, sr1: 0x7fff };
      else this.m = { b0: 0x20, b1: 0x3f, dr0: 0x8000, dr1: 0xbfff, sr0: 0xc000, sr1: 0xffff }; // documented as $30-$3F; $20-$2F mirror
      Kit.claim(snes, this.m.b0, this.m.b1, this.m.dr0, this.m.sr1, true);
    }
    hit(bank, off) { const b = bank & 0x7f, m = this.m; return b >= m.b0 && b <= m.b1 && off >= m.dr0 && off <= m.sr1; }
    read(bank, off) {
      if (!this.hit(bank, off)) return -1;
      return off <= this.m.dr1 ? this.port.readDR() : this.port.readSR();
    }
    write(bank, off, v) {
      if (!this.hit(bank, off)) return false;
      if (off <= this.m.dr1) this.port.writeDR(v);
      return true;
    }
    reset() { this.core.reset(); this.port.reset(); }
    saveState() { return { core: this.core.saveState(), port: this.port.saveState() }; }
    loadState(o) { this.core.loadState(o.core); this.port.loadState(o.port); }
  }

  global.RPSNESChips.push(
    { name: "DSP-2", detect: (c) => Kit.dspKind(c) === 2, create: (snes) => new DSPChip(snes, 2) },
    { name: "DSP-1", detect: (c) => Kit.dspKind(c) === 1, create: (snes) => new DSPChip(snes, 1) },
  );
  // exposed for RetroPixels' own tests
  global.RPSNESDSP = { DSP1Core, DSP2Core, DSPPort, dsin, dcos, norm, normD, inv, trunc };
})(typeof window !== "undefined" ? window : globalThis);
