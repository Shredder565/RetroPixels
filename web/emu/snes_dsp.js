// RetroPixels — SNES DSP-1 / DSP-1A / DSP-1B and DSP-2 cartridge chips. Original code.
//
// These cartridges carry an NEC uPD77C25 signal processor running a fixed
// program. RetroPixels doesn't use that program: it computes what each
// documented command means (high-level emulation) with its own arithmetic, then
// rounds the answers into the chip's 16-bit formats. A player who has their own
// copy of the chip's program can use it instead (snes_necdsp.js runs it exactly).
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
  // DSP-1 number helpers
  // ===================================================================
  // The chip works in 16-bit fixed point: angles are 1/65536 of a turn, and
  // most fractions are "1.15" numbers (32768 = 1.0). RetroPixels computes each
  // command's result in double precision from its geometric meaning and then
  // rounds it into the chip's formats, so results agree with the hardware to
  // within a few units in the last place rather than bit for bit.
  const TURN = 2 * Math.PI / 65536;
  const sat = (v) => (v > 32767 ? 32767 : v < -32768 ? -32768 : v);  // clip to 16 bits
  const fix = (v) => sat(Math.trunc(v));                               // real -> 16-bit, toward zero
  const frac = (v) => v / 32768;                                       // 1.15 -> real
  function dsin(a) { return sat(Math.round(32768 * Math.sin(s16(a) * TURN))); }
  function dcos(a) { return sat(Math.round(32768 * Math.cos(s16(a) * TURN))); }
  // c * 2^e (c in 1.15) -> its reciprocal in the same form, with the coefficient
  // normalised: (0.5, 1] for positive values, [-1, -0.5) for negative ones.
  function inv(c, e) {
    if (c === 0) return [0x7fff, 0x2f]; // "infinity"
    const r = 1 / (frac(c) * 2 ** e);
    let ex = Math.ceil(Math.log2(Math.abs(r)));
    let m = r / 2 ** ex;
    if (m < 0 && m >= -0.5) { m *= 2; ex--; }             // keep negative coefficients below -0.5
    if (m > 0 && m <= 0.5) { m *= 2; ex--; }              // (guards against log2 rounding)
    let co = Math.round(m * 32768);
    if (co > 32767) co = 32767;
    if (co < -32768) co = -32768;
    return [co, s16(ex)];
  }

  // ===================================================================
  // DSP-1 command processor
  // ===================================================================
  class DSP1Core {
    constructor() { this.reset(); }
    reset() {
      this.mat = [new Int16Array(9), new Int16Array(9), new Int16Array(9)];
      // camera set up by the Parameter command (see parameter() for the meaning)
      this.p = { sinA: 0, cosA: 0, sinZ: 0, cosZ: 0, nx: 0, ny: 0, nz: 0, gx: 0, gy: 0, gz: 0, cx: 0, cy: 0, cz: 0,
        les: 0, vOff: 0, secZ: 1 };
    }
    // --- object attitude matrices: (m / 2) x rotation by Z, then Y, then X ---
    attitude(M, m, az, ay, ax) {
      const sz = Math.sin(az * TURN), cz = Math.cos(az * TURN), sy = Math.sin(ay * TURN), cy = Math.cos(ay * TURN), sx = Math.sin(ax * TURN), cx = Math.cos(ax * TURN);
      const h = m / 2;
      const rows = [
        cz * cy, -sz * cy, sy,
        sz * cx + cz * sx * sy, cz * cx - sz * sx * sy, -sx * cy,
        sz * sx - cz * cx * sy, cz * sx + sz * cx * sy, cx * cy,
      ];
      for (let k = 0; k < 9; k++) M[k] = fix(h * rows[k]);
    }
    // matrix (1.15) times a vector, rows (objective) or columns (subjective)
    static mulVec(M, v, cols) {
      const o = [];
      for (let i = 0; i < 3; i++) {
        let sum = 0;
        for (let k = 0; k < 3; k++) sum += v[k] * (cols ? M[k * 3 + i] : M[i * 3 + k]);
        o.push(s16(Math.floor(sum / 32768)));
      }
      return o;
    }
    objective(M, x, y, z) { return DSP1Core.mulVec(M, [x, y, z], false); }   // global -> object (F, L, U)
    subjective(M, f, l, u) { return DSP1Core.mulVec(M, [f, l, u], true); }   // object -> global
    scalar(M, x, y, z) { return [DSP1Core.mulVec(M, [x, y, z], false)[0]]; } // forward component only
    // --- simple maths ---
    multiply(k, i, plus) { return [s16(((k * i) >> 15) + (plus ? 1 : 0))]; }
    inverse(c, e) { return inv(c, e); }
    triangle(a, r) { return [s16(Math.floor(dsin(a) * r / 32768)), s16(Math.floor(dcos(a) * r / 32768))]; }
    radius(x, y, z) { const v = ((x * x + y * y + z * z) * 2) | 0; return [v & 0xffff, (v >>> 16) & 0xffff]; }
    range(x, y, z, r, plus) { return [s16((((x * x + y * y + z * z - r * r) | 0) >> 15) + (plus ? 1 : 0))]; }
    distance(x, y, z) { return [fix(Math.sqrt(x * x + y * y + z * z))]; }
    // turn the point (x, y) by angle a
    static turn(a, x, y) {
      const s = dsin(a), c = dcos(a);
      return [s16(Math.floor((y * s + x * c) / 32768)), s16(Math.floor((y * c - x * s) / 32768))];
    }
    rotate(a, x1, y1) { return DSP1Core.turn(a, x1, y1); }
    // rotate a point about Z, then Y, then X
    polar(az, ay, ax, x, y, z) {
      [x, y] = DSP1Core.turn(az, x, y);
      [z, x] = DSP1Core.turn(ay, z, x);
      [y, z] = DSP1Core.turn(ax, y, z);
      return [x, y, z];
    }
    // Turn body-relative turning speeds (U, F, L) into new attitude angles
    // (Z, X, Y), the usual Euler-angle kinematics.
    gyrate(zr, xr, yr, u, f, l) {
      const sy = Math.sin(yr * TURN), cy = Math.cos(yr * TURN), cx = Math.cos(xr * TURN), tx = Math.tan(xr * TURN);
      const w = u * cy - f * sy; // turning speed about the body's up axis, seen from the side
      const dz = cx ? fix(w / cx) : (w > 0 ? 32767 : w < 0 ? -32767 : 0);
      const dx = Math.trunc(u * sy + f * cy);
      const dy = fix(w * tx);
      return [s16(zr + dz), s16(xr + dx), s16(yr + l - dy)];
    }
    // --- Mode 7 perspective ---
    // The camera looks at the focus point F from Lfe away, along the direction
    // given by azimuth A (around Z) and zenith angle Z (0 = straight down); the
    // screen sits Les in front of the eye. Returns [raster offset, horizon line,
    // ground point at the screen centre X, Y].
    parameter(fx, fy, fz, lfe, les, aas, azs) {
      const p = this.p;
      p.sinA = dsin(aas); p.cosA = dcos(aas); p.sinZ = dsin(azs); p.cosZ = dcos(azs);
      const sA = frac(p.sinA), cA = frac(p.cosA);
      // unit vector from the focus toward the eye
      p.nx = -frac(p.sinZ) * sA; p.ny = frac(p.sinZ) * cA; p.nz = frac(p.cosZ);
      const ex = s16(Math.trunc(fx + lfe * p.nx)), ey = s16(Math.trunc(fy + lfe * p.ny)), ez = s16(Math.trunc(fz + lfe * p.nz));
      p.gx = s16(Math.trunc(ex - les * p.nx)); p.gy = s16(Math.trunc(ey - les * p.ny)); p.gz = s16(Math.trunc(ez - les * p.nz));
      p.cz = ez; p.les = les;
      // A camera tilted close to the horizon is held at a limit angle so the
      // horizon stays on screen; the rest of the tilt becomes a raster offset.
      const LIMIT = 0x38c0;
      const az = Math.max(-LIMIT, Math.min(LIMIT, s16(azs))), over = (s16(azs) - az) * TURN;
      const sZ = Math.sin(az * TURN), cZ = Math.cos(az * TURN) * Math.cos(over);
      const vof = over ? fix(les * Math.sin(over)) : 0;
      // where the view axis meets the ground (z = 0)
      const t = cZ ? ez / Math.cos(az * TURN) : 0;
      p.cx = s16(Math.trunc(ex + t * sZ * sA)); p.cy = s16(Math.trunc(ey - t * sZ * cA));
      p.vOff = fix(les * cZ);
      p.secZ = 1 / (Math.abs(cZ) < 1e-6 ? 1e-6 : cZ);
      const vva = sZ ? fix(-les * cZ / sZ) : -32767; // the horizon's screen line
      return [vof, vva, p.cx, p.cy];
    }
    // ground distance scale for screen line vs (relative to the centre line)
    lineScale(vs) {
      const p = this.p;
      const den = p.vOff + ((vs * p.sinZ) >> 15);
      return p.cz / (den || 0.5);
    }
    // Mode 7 matrix (8.8: A, B, C, D) for screen line vs
    raster(vs) {
      const p = this.p, s = 256 * this.lineScale(vs), sA = frac(p.sinA), cA = frac(p.cosA);
      return [fix(s * cA), fix(-s * sA * p.secZ), fix(s * sA), fix(s * cA * p.secZ)];
    }
    // world point -> screen (H, V) and scale M (8.8)
    project(x, y, z) {
      const p = this.p, sA = frac(p.sinA), cA = frac(p.cosA), sZ = frac(p.sinZ), cZ = frac(p.cosZ);
      const dx = x - p.gx, dy = y - p.gy, dz = z - p.gz;
      const depth = p.les - (dx * p.nx + dy * p.ny + dz * p.nz); // distance in front of the eye
      const k = p.les / (depth || 1e-9);
      const h = (dx * cA + dy * sA) * k;
      const v = (-dx * cZ * sA + dy * cZ * cA - dz * sZ) * k;
      return [fix(h), fix(v), fix(256 * k)];
    }
    // screen (H, V) -> ground point (X, Y)
    target(h, v) {
      const p = this.p, s = this.lineScale(v), sA = frac(p.sinA), cA = frac(p.cosA);
      const across = fix(s * h), along = fix(s * v * p.secZ);
      return [s16(p.cx + Math.trunc(across * cA) - Math.trunc(along * sA)), s16(p.cy + Math.trunc(across * sA) + Math.trunc(along * cA))];
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
        case 0x07: case 0x0f: case 0x17: case 0x27: case 0x37: return [1, () => [0x0000]]; // memory test: passed
        case 0x2f: return [1, () => [0x0100]];                                              // memory size
        // dump of the chip's constant memory: RetroPixels has no copy of it, so it reads as zeros
        case 0x1f: case 0x3f: return [1, () => new Array(1024).fill(0)];
      }
      return null;
    }
    saveState() { return { mat: this.mat.map((m) => Array.from(m)), p: Object.assign({}, this.p) }; }
    loadState(o) { o.mat.forEach((m, i) => this.mat[i].set(m)); this.reset(); o.mat.forEach((m, i) => this.mat[i].set(m)); Object.assign(this.p, o.p); if (!(this.p.secZ > -Infinity)) this.p.secZ = 1; }
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
    constructor(snes, kind, opts) {
      this.snes = snes; this.kind = kind;
      // A DSP-1 / DSP-2 program supplied by the player runs on the real
      // processor (snes_necdsp.js) instead of the high-level version.
      const N = global.RPNECDSP, fw = N ? N.findFirmware(snes, opts, kind === 2 ? "DSP-2" : "DSP-1") : null;
      if (fw) {
        this.core = { reset() {}, saveState() { return null; }, loadState() {} };
        this.port = new N.Clocked(snes, new N.NECDSP("upd7725", fw), 7600000);
      } else {
        this.core = kind === 2 ? new DSP2Core() : new DSP1Core();
        this.port = new DSPPort(this.core, kind !== 2);
      }
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
    { name: "DSP-2", detect: (c) => Kit.dspKind(c) === 2, create: (snes, opts) => new DSPChip(snes, 2, opts) },
    { name: "DSP-1", detect: (c) => Kit.dspKind(c) === 1, create: (snes, opts) => new DSPChip(snes, 1, opts) },
  );
  // exposed for RetroPixels' own tests
  global.RPSNESDSP = { DSP1Core, DSP2Core, DSPPort, dsin, dcos, inv };
})(typeof window !== "undefined" ? window : globalThis);
