// RetroPixels — 6502 processor (used by the NES, Atari 2600 and Atari 7800 emulators).
//
// Every memory access goes through bus.read / bus.write and costs one cycle,
// including the "dummy" accesses real hardware makes, so devices that count
// cycles (video chips, timers) see exactly the same timing as on the console.
// Written from the public behaviour of the chip; original code.
"use strict";

(function (global) {
  const C = 0x01, Z = 0x02, I = 0x04, D = 0x08, B = 0x10, U = 0x20, V = 0x40, N = 0x80;

  class CPU6502 {
    // bus: { read(addr), write(addr, value) } — each call is one cycle.
    // opts.decimal: whether the D flag enables decimal arithmetic (off on the NES).
    constructor(bus, opts = {}) {
      this.bus = bus;
      this.decimal = !!opts.decimal;
      this.machinePolls = !!opts.machinePolls;
      this.a = 0; this.x = 0; this.y = 0; this.s = 0xfd; this.p = I | U; this.pc = 0;
      this.cycles = 0;          // total cycles run
      this.nmiPending = false;  // edge-triggered, set by the machine
      this.irqLine = false;     // level-triggered, set by the machine
      this.jammed = false;
      this.prevIrq = false;     // interrupt polling happens before the last cycle
      this.prevNmi = false;
      this.buildTable();
    }

    // ---- bus helpers (each costs a cycle) ----
    // Before each cycle, note the interrupt lines as they were at the end of the
    // previous cycle: after an instruction, that's the "second-to-last cycle"
    // state the real chip uses to decide whether to take an interrupt next.
    // (A machine can sample at a finer point within the cycle by setting
    // opts.machinePolls and calling poll() itself.)
    poll() { this.pollNmi = this.nmiPending; this.pollIrq = this.irqLine && !(this.p & I); }
    rd(a) { if (!this.machinePolls) this.poll(); this.cycles++; return this.bus.read(a & 0xffff); }
    wr(a, v) { if (!this.machinePolls) this.poll(); this.cycles++; this.bus.write(a & 0xffff, v & 0xff); }
    push(v) { this.wr(0x100 | this.s, v); this.s = (this.s - 1) & 0xff; }
    pull() { this.s = (this.s + 1) & 0xff; return this.rd(0x100 | this.s); }
    fetch() { const v = this.rd(this.pc); this.pc = (this.pc + 1) & 0xffff; return v; }
    fetch16() { const lo = this.fetch(); return lo | (this.fetch() << 8); }

    setZN(v) { this.p = (this.p & ~(Z | N)) | (v & 0xff ? 0 : Z) | (v & N); return v & 0xff; }

    reset() {
      this.rd(this.pc); this.rd(this.pc); // internal cycles
      this.rd(0x100 | this.s); this.s = (this.s - 1) & 0xff;
      this.rd(0x100 | this.s); this.s = (this.s - 1) & 0xff;
      this.rd(0x100 | this.s); this.s = (this.s - 1) & 0xff;
      this.p |= I;
      this.pc = this.rd(0xfffc) | (this.rd(0xfffd) << 8);
      this.jammed = false;
    }

    interrupt(vector, brk) {
      this.push(this.pc >> 8); this.push(this.pc & 0xff);
      this.push(brk ? (this.p | U | B) : ((this.p | U) & ~B));
      this.p |= I;
      // An NMI arriving during a BRK/IRQ takes over its vector.
      // (With machine polling, only an NMI already sampled by this point counts.)
      if (vector === 0xfffe && (this.machinePolls ? this.pollNmi : this.nmiPending)) { vector = 0xfffa; this.nmiPending = false; }
      this.pc = this.rd(vector) | (this.rd(vector + 1) << 8);
    }

    // Run one instruction (or service an interrupt). Returns cycles used.
    step() {
      const start = this.cycles;
      if (this.jammed) { this.rd(this.pc); return this.cycles - start; }
      if (this.prevNmi || this.prevIrq) {
        this.rd(this.pc); this.rd(this.pc);
        if (this.prevNmi) { this.nmiPending = false; this.interrupt(0xfffa, false); }
        else this.interrupt(0xfffe, false);
        // The handler's first instruction always runs before another interrupt.
        this.prevNmi = false; this.prevIrq = false;
        return this.cycles - start;
      }
      this.branchPoll = null;
      const op = this.fetch();
      this.ops[op].call(this);
      if (this.branchPoll) { this.pollNmi = this.branchPoll[0]; this.pollIrq = this.branchPoll[1]; }
      if (op === 0x00) { this.prevNmi = false; this.prevIrq = false; } // like any interrupt, BRK's handler runs one instruction first
      else { this.prevNmi = this.pollNmi; this.prevIrq = this.pollIrq; }
      return this.cycles - start;
    }

    // ---- addressing (return effective address; cycle-exact dummy reads) ----
    zp() { return this.fetch(); }
    zpx() { const a = this.fetch(); this.rd(a); return (a + this.x) & 0xff; }
    zpy() { const a = this.fetch(); this.rd(a); return (a + this.y) & 0xff; }
    abs() { return this.fetch16(); }
    // read=true: only take the extra cycle when a page is crossed.
    absIdx(idx, read) {
      const base = this.fetch16(), ea = (base + idx) & 0xffff;
      if (!read || (base ^ ea) & 0xff00) this.rd((base & 0xff00) | (ea & 0xff));
      return ea;
    }
    izx() { const z = this.fetch(); this.rd(z); const p = (z + this.x) & 0xff; return this.rd(p) | (this.rd((p + 1) & 0xff) << 8); }
    izy(read) {
      const z = this.fetch(), base = this.rd(z) | (this.rd((z + 1) & 0xff) << 8), ea = (base + this.y) & 0xffff;
      if (!read || (base ^ ea) & 0xff00) this.rd((base & 0xff00) | (ea & 0xff));
      return ea;
    }

    // ---- arithmetic ----
    adc(v) {
      const c = this.p & C;
      if (this.decimal && (this.p & D)) {
        let lo = (this.a & 0x0f) + (v & 0x0f) + c;
        let hi = (this.a & 0xf0) + (v & 0xf0);
        if (lo > 9) { lo += 6; hi += 0x10; }
        const zTest = (this.a + v + c) & 0xff;
        this.p &= ~(Z | N | V | C);
        if (!zTest) this.p |= Z;
        if (hi & 0x80) this.p |= N;
        if (~(this.a ^ v) & (this.a ^ hi) & 0x80) this.p |= V;
        if (hi > 0x90) hi += 0x60;
        if (hi > 0xff) this.p |= C;
        this.a = (hi & 0xf0) | (lo & 0x0f);
        return;
      }
      const r = this.a + v + c;
      this.p &= ~(C | V);
      if (r > 0xff) this.p |= C;
      if (~(this.a ^ v) & (this.a ^ r) & 0x80) this.p |= V;
      this.a = this.setZN(r);
    }
    sbc(v) {
      if (this.decimal && (this.p & D)) {
        const c = this.p & C ? 0 : 1;
        const bin = this.a - v - c;
        let lo = (this.a & 0x0f) - (v & 0x0f) - c;
        let hi = (this.a & 0xf0) - (v & 0xf0);
        if (lo & 0x10) { lo -= 6; hi -= 0x10; }
        if (hi & 0x100) hi -= 0x60;
        this.p &= ~(C | V | Z | N);
        if (bin >= 0) this.p |= C;
        if ((this.a ^ v) & (this.a ^ bin) & 0x80) this.p |= V;
        if (!(bin & 0xff)) this.p |= Z;
        if (bin & 0x80) this.p |= N;
        this.a = (hi & 0xf0) | (lo & 0x0f);
        return;
      }
      this.adcBinary(v ^ 0xff);
    }
    adcBinary(v) {
      const r = this.a + v + (this.p & C);
      this.p &= ~(C | V);
      if (r > 0xff) this.p |= C;
      if (~(this.a ^ v) & (this.a ^ r) & 0x80) this.p |= V;
      this.a = this.setZN(r);
    }
    cmp(r, v) { const t = r - v; this.p = (this.p & ~C) | (t >= 0 ? C : 0); this.setZN(t); }
    asl(v) { this.p = (this.p & ~C) | (v >> 7); return this.setZN(v << 1); }
    lsr(v) { this.p = (this.p & ~C) | (v & 1); return this.setZN(v >> 1); }
    rol(v) { const c = this.p & C; this.p = (this.p & ~C) | (v >> 7); return this.setZN((v << 1) | c); }
    ror(v) { const c = this.p & C; this.p = (this.p & ~C) | (v & 1); return this.setZN((v >> 1) | (c << 7)); }

    branch(cond) {
      const off = this.fetch();
      if (!cond) return;
      const early = [this.pollNmi, this.pollIrq]; // captured just before the operand cycle
      // A taken branch delays interrupt polling unless it crosses a page.
      this.rd(this.pc);
      const target = (this.pc + ((off << 24) >> 24)) & 0xffff;
      // A taken branch that stays on the same page doesn't look at interrupts
      // on its last cycle, so the earlier check counts.
      if ((target ^ this.pc) & 0xff00) this.rd((this.pc & 0xff00) | (target & 0xff));
      else this.branchPoll = early;
      this.pc = target;
    }

    // Read-modify-write helper: read, dummy write of old value, write new value.
    rmw(ea, fn) { const v = this.rd(ea); this.wr(ea, v); const r = fn.call(this, v); this.wr(ea, r); return r; }

    buildTable() {
      const t = new Array(256);
      const self = this;
      const R = (mode) => {
        switch (mode) {
          case "imm": return function () { return this.fetch(); };
          case "zp": return function () { return this.rd(this.zp()); };
          case "zpx": return function () { return this.rd(this.zpx()); };
          case "zpy": return function () { return this.rd(this.zpy()); };
          case "abs": return function () { return this.rd(this.abs()); };
          case "abx": return function () { return this.rd(this.absIdx(this.x, true)); };
          case "aby": return function () { return this.rd(this.absIdx(this.y, true)); };
          case "izx": return function () { return this.rd(this.izx()); };
          case "izy": return function () { return this.rd(this.izy(true)); };
        }
      };
      const A = (mode) => { // address for writes / RMW
        switch (mode) {
          case "zp": return function () { return this.zp(); };
          case "zpx": return function () { return this.zpx(); };
          case "zpy": return function () { return this.zpy(); };
          case "abs": return function () { return this.abs(); };
          case "abx": return function () { return this.absIdx(this.x, false); };
          case "aby": return function () { return this.absIdx(this.y, false); };
          case "izx": return function () { return this.izx(); };
          case "izy": return function () { return this.izy(false); };
        }
      };
      const read = (op, mode, fn) => { const get = R(mode); t[op] = function () { fn.call(this, get.call(this)); }; };
      const store = (op, mode, fn) => { const ea = A(mode); t[op] = function () { this.wr(ea.call(this), fn.call(this)); }; };
      const modify = (op, mode, fn) => { const ea = A(mode); t[op] = function () { this.rmw(ea.call(this), fn); }; };
      const implied = (op, fn) => { t[op] = function () { this.rd(this.pc); fn.call(this); }; };

      const modesR = { imm: 0x09, zp: 0x05, zpx: 0x15, abs: 0x0d, abx: 0x1d, aby: 0x19, izx: 0x01, izy: 0x11 };
      const group1 = [
        [0x00, function (v) { this.a = this.setZN(this.a | v); }],   // ORA
        [0x20, function (v) { this.a = this.setZN(this.a & v); }],   // AND
        [0x40, function (v) { this.a = this.setZN(this.a ^ v); }],   // EOR
        [0x60, function (v) { this.adc(v); }],                        // ADC
        [0xa0, function (v) { this.a = this.setZN(v); }],            // LDA
        [0xc0, function (v) { this.cmp(this.a, v); }],                // CMP
        [0xe0, function (v) { this.sbc(v); }],                        // SBC
      ];
      for (const [base, fn] of group1) for (const m in modesR) read(base + modesR[m], m, fn);
      for (const m in modesR) if (m !== "imm") store(0x80 + modesR[m], m, function () { return this.a; }); // STA

      // shifts / inc / dec
      const sh = [[0x00, "asl"], [0x20, "rol"], [0x40, "lsr"], [0x60, "ror"]];
      for (const [base, name] of sh) {
        const fn = CPU6502.prototype[name];
        implied(base + 0x0a, function () { this.a = fn.call(this, this.a); });
        modify(base + 0x06, "zp", fn); modify(base + 0x16, "zpx", fn);
        modify(base + 0x0e, "abs", fn); modify(base + 0x1e, "abx", fn);
      }
      const inc = function (v) { return this.setZN(v + 1); }, dec = function (v) { return this.setZN(v - 1); };
      modify(0xe6, "zp", inc); modify(0xf6, "zpx", inc); modify(0xee, "abs", inc); modify(0xfe, "abx", inc);
      modify(0xc6, "zp", dec); modify(0xd6, "zpx", dec); modify(0xce, "abs", dec); modify(0xde, "abx", dec);

      // loads / stores for X and Y
      const ldx = function (v) { this.x = this.setZN(v); }, ldy = function (v) { this.y = this.setZN(v); };
      read(0xa2, "imm", ldx); read(0xa6, "zp", ldx); read(0xb6, "zpy", ldx); read(0xae, "abs", ldx); read(0xbe, "aby", ldx);
      read(0xa0, "imm", ldy); read(0xa4, "zp", ldy); read(0xb4, "zpx", ldy); read(0xac, "abs", ldy); read(0xbc, "abx", ldy);
      store(0x86, "zp", function () { return this.x; }); store(0x96, "zpy", function () { return this.x; }); store(0x8e, "abs", function () { return this.x; });
      store(0x84, "zp", function () { return this.y; }); store(0x94, "zpx", function () { return this.y; }); store(0x8c, "abs", function () { return this.y; });
      const cpx = function (v) { this.cmp(this.x, v); }, cpy = function (v) { this.cmp(this.y, v); };
      read(0xe0, "imm", cpx); read(0xe4, "zp", cpx); read(0xec, "abs", cpx);
      read(0xc0, "imm", cpy); read(0xc4, "zp", cpy); read(0xcc, "abs", cpy);
      const bit = function (v) { this.p = (this.p & ~(Z | V | N)) | (this.a & v ? 0 : Z) | (v & (V | N)); };
      read(0x24, "zp", bit); read(0x2c, "abs", bit);

      // transfers, flags, inc/dec registers
      implied(0xaa, function () { this.x = this.setZN(this.a); });
      implied(0xa8, function () { this.y = this.setZN(this.a); });
      implied(0x8a, function () { this.a = this.setZN(this.x); });
      implied(0x98, function () { this.a = this.setZN(this.y); });
      implied(0xba, function () { this.x = this.setZN(this.s); });
      implied(0x9a, function () { this.s = this.x; });
      implied(0xe8, function () { this.x = this.setZN(this.x + 1); });
      implied(0xc8, function () { this.y = this.setZN(this.y + 1); });
      implied(0xca, function () { this.x = this.setZN(this.x - 1); });
      implied(0x88, function () { this.y = this.setZN(this.y - 1); });
      implied(0x18, function () { this.p &= ~C; }); implied(0x38, function () { this.p |= C; });
      implied(0x58, function () { this.p &= ~I; }); implied(0x78, function () { this.p |= I; });
      implied(0xb8, function () { this.p &= ~V; });
      implied(0xd8, function () { this.p &= ~D; }); implied(0xf8, function () { this.p |= D; });
      implied(0xea, function () {});

      // stack
      t[0x48] = function () { this.rd(this.pc); this.push(this.a); };
      t[0x08] = function () { this.rd(this.pc); this.push(this.p | B | U); };
      t[0x68] = function () { this.rd(this.pc); this.rd(0x100 | this.s); this.a = this.setZN(this.pull()); };
      t[0x28] = function () { this.rd(this.pc); this.rd(0x100 | this.s); this.p = (this.pull() & ~B) | U; };

      // jumps & calls
      t[0x4c] = function () { this.pc = this.fetch16(); };
      t[0x6c] = function () { // indirect, with the page-wrap quirk
        const p = this.fetch16();
        this.pc = this.rd(p) | (this.rd((p & 0xff00) | ((p + 1) & 0xff)) << 8);
      };
      t[0x20] = function () {
        const lo = this.fetch();
        this.rd(0x100 | this.s);
        this.push(this.pc >> 8); this.push(this.pc & 0xff);
        this.pc = lo | (this.fetch() << 8);
      };
      t[0x60] = function () {
        this.rd(this.pc); this.rd(0x100 | this.s);
        const lo = this.pull(); this.pc = lo | (this.pull() << 8);
        this.rd(this.pc); this.pc = (this.pc + 1) & 0xffff;
      };
      t[0x40] = function () {
        this.rd(this.pc); this.rd(0x100 | this.s);
        this.p = (this.pull() & ~B) | U;
        const lo = this.pull(); this.pc = lo | (this.pull() << 8);
      };
      t[0x00] = function () { this.fetch(); this.interrupt(0xfffe, true); };

      // branches
      t[0x10] = function () { this.branch(!(this.p & N)); };
      t[0x30] = function () { this.branch(this.p & N); };
      t[0x50] = function () { this.branch(!(this.p & V)); };
      t[0x70] = function () { this.branch(this.p & V); };
      t[0x90] = function () { this.branch(!(this.p & C)); };
      t[0xb0] = function () { this.branch(this.p & C); };
      t[0xd0] = function () { this.branch(!(this.p & Z)); };
      t[0xf0] = function () { this.branch(this.p & Z); };

      // ---- unofficial opcodes (several games rely on these) ----
      const nopR = (op, mode) => read(op, mode, function () {});
      for (const op of [0x1a, 0x3a, 0x5a, 0x7a, 0xda, 0xfa]) implied(op, function () {});
      for (const op of [0x80, 0x82, 0x89, 0xc2, 0xe2]) nopR(op, "imm");
      for (const op of [0x04, 0x44, 0x64]) nopR(op, "zp");
      for (const op of [0x14, 0x34, 0x54, 0x74, 0xd4, 0xf4]) nopR(op, "zpx");
      nopR(0x0c, "abs");
      for (const op of [0x1c, 0x3c, 0x5c, 0x7c, 0xdc, 0xfc]) nopR(op, "abx");
      const lax = function (v) { this.a = this.x = this.setZN(v); };
      read(0xa7, "zp", lax); read(0xb7, "zpy", lax); read(0xaf, "abs", lax); read(0xbf, "aby", lax);
      read(0xa3, "izx", lax); read(0xb3, "izy", lax); read(0xab, "imm", function (v) { this.a = this.x = this.setZN((this.a | 0xff) & v); });
      const sax = function () { return this.a & this.x; };
      store(0x87, "zp", sax); store(0x97, "zpy", sax); store(0x8f, "abs", sax); store(0x83, "izx", sax);
      read(0xeb, "imm", function (v) { this.sbc(v); });
      const combo = (base, rmwFn, after) => {
        const f = function (v) { const r = rmwFn.call(this, v); after.call(this, r); return r; };
        modify(base + 0x07, "zp", f); modify(base + 0x17, "zpx", f); modify(base + 0x0f, "abs", f);
        modify(base + 0x1f, "abx", f); modify(base + 0x1b, "aby", f); modify(base + 0x03, "izx", f); modify(base + 0x13, "izy", f);
      };
      combo(0x00, CPU6502.prototype.asl, function (r) { this.a = this.setZN(this.a | r); });   // SLO
      combo(0x20, CPU6502.prototype.rol, function (r) { this.a = this.setZN(this.a & r); });   // RLA
      combo(0x40, CPU6502.prototype.lsr, function (r) { this.a = this.setZN(this.a ^ r); });   // SRE
      combo(0x60, CPU6502.prototype.ror, function (r) { this.adc(r); });                       // RRA
      combo(0xc0, function (v) { return (v - 1) & 0xff; }, function (r) { this.cmp(this.a, r); }); // DCP
      combo(0xe0, function (v) { return (v + 1) & 0xff; }, function (r) { this.sbc(r); });         // ISC
      read(0x0b, "imm", function (v) { this.a = this.setZN(this.a & v); this.p = (this.p & ~C) | (this.a >> 7); }); // ANC
      t[0x2b] = t[0x0b];
      read(0x4b, "imm", function (v) { this.a = this.lsr(this.a & v); }); // ALR
      read(0x6b, "imm", function (v) { // ARR
        const r = ((this.a & v) >> 1) | ((this.p & C) << 7);
        this.a = this.setZN(r);
        this.p = (this.p & ~(C | V)) | ((r >> 6) & 1) | (((r >> 6) ^ (r >> 5)) & 1 ? V : 0);
      });
      read(0xcb, "imm", function (v) { // AXS
        const t2 = (this.a & this.x) - v;
        this.p = (this.p & ~C) | (t2 >= 0 ? C : 0);
        this.x = this.setZN(t2);
      });
      read(0xbb, "aby", function (v) { this.a = this.x = this.s = this.setZN(v & this.s); }); // LAS
      // Unstable stores: high-byte AND quirks.
      const hiStore = (op, mode, regFn) => {
        t[op] = function () {
          const base = this.fetch16(), idx = mode === "aby" ? this.y : this.x, ea = (base + idx) & 0xffff;
          this.rd((base & 0xff00) | (ea & 0xff));
          const val = regFn.call(this) & (((base >> 8) + 1) & 0xff);
          const addr = (base ^ ea) & 0xff00 ? ((val << 8) | (ea & 0xff)) : ea;
          this.wr(addr, val);
        };
      };
      hiStore(0x9c, "abx", function () { return this.y; });            // SHY
      hiStore(0x9e, "aby", function () { return this.x; });            // SHX
      hiStore(0x9f, "aby", function () { return this.a & this.x; });   // SHA
      t[0x9b] = function () { // TAS
        const base = this.fetch16(), ea = (base + this.y) & 0xffff;
        this.rd((base & 0xff00) | (ea & 0xff));
        this.s = this.a & this.x;
        this.wr(ea, this.s & (((base >> 8) + 1) & 0xff));
      };
      t[0x93] = function () { // SHA (zp),y
        const z = this.fetch(), base = this.rd(z) | (this.rd((z + 1) & 0xff) << 8), ea = (base + this.y) & 0xffff;
        this.rd((base & 0xff00) | (ea & 0xff));
        this.wr(ea, this.a & this.x & (((base >> 8) + 1) & 0xff));
      };
      read(0x8b, "imm", function (v) { this.a = this.setZN((this.a | 0xee) & this.x & v); }); // XAA
      // JAM opcodes stop the processor.
      for (const op of [0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xb2, 0xd2, 0xf2]) t[op] = function () { this.jammed = true; };

      for (let i = 0; i < 256; i++) if (!t[i]) t[i] = function () {}; // should not happen
      this.ops = t;
      void self;
    }

    saveState() {
      return { a: this.a, x: this.x, y: this.y, s: this.s, p: this.p, pc: this.pc, cycles: this.cycles,
        nmiPending: this.nmiPending, irqLine: this.irqLine, jammed: this.jammed, prevIrq: this.prevIrq, prevNmi: this.prevNmi,
        pollNmi: this.pollNmi, pollIrq: this.pollIrq };
    }
    loadState(o) { Object.assign(this, o); }
  }

  CPU6502.FLAGS = { C, Z, I, D, B, U, V, N };
  global.RPCPU6502 = CPU6502;
})(typeof window !== "undefined" ? window : globalThis);
