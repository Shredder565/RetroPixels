// RetroPixels — Intel MCS-48 processor core (8035 / 8039 / 8048 family).
// Original code, written from Intel's MCS-48 user manual: the instruction set,
// the two register banks, the 8-level stack in internal RAM, the two program
// memory banks (SEL MB0/MB1), the timer/counter and its interrupt, the
// external interrupt, the T0/T1 test pins and the ports. The 8243 port
// expander (MOVD/ANLD/ORLD) is passed to the board if it wants it.
// Used as the Donkey Kong sound processor.
//
//   const c = new RPMCS48({
//     rom(addr)          program memory (12-bit address)
//     readX(a) / writeX(a, v)   MOVX external data (8-bit address; the board adds P2 bits if it pages)
//     outP(port, v)      port 1 / 2 / bus (0) written
//     inP(port)          port 1 / 2 / bus (0) read (the board ANDs the latch if it models the pins)
//     t0(), t1()         test pins (true = high)
//     intLine()          /INT pin: true = interrupt requested (pin low)
//   }, { ram: 64 })
//   c.step() -> machine cycles used (one machine cycle = 15 clock periods)
"use strict";

(function (global) {
  const CY = 0x80, AC = 0x40, F0 = 0x20, BS = 0x10;

  function RPMCS48(bus, opts) {
    const ramSize = (opts && opts.ram) || 64;
    const ram = new Uint8Array(ramSize);
    const RM = ramSize - 1;
    let a, pc, psw, f1, dbf, a11, t, tf, tirq, timerOn, counterOn, prescale, lastT1;
    let intEn, tcntEn, inIrq, p1, p2, busLatch;
    const rom = (ad) => bus.rom(ad & 0xfff) & 0xff;
    function fetch() { const v = rom(pc); pc = (pc & 0x800) | ((pc + 1) & 0x7ff); return v; }
    const regBase = () => (psw & BS) ? 24 : 0;
    const getR = (r) => ram[(regBase() + r) & RM];
    const setR = (r, v) => { ram[(regBase() + r) & RM] = v & 0xff; };
    const ind = (r) => getR(r) & RM;          // address held in R0 / R1
    function push() {
      const sp = psw & 7, ad = 8 + sp * 2;
      ram[ad & RM] = pc & 0xff;
      ram[(ad + 1) & RM] = ((pc >> 8) & 0x0f) | (psw & 0xf0);
      psw = (psw & 0xf8) | ((sp + 1) & 7);
    }
    function pop(restorePsw) {
      const sp = (psw - 1) & 7, ad = 8 + sp * 2;
      const lo = ram[ad & RM], hi = ram[(ad + 1) & RM];
      pc = ((hi & 0x0f) << 8) | lo;
      psw = (psw & 0xf8) | sp;
      if (restorePsw) psw = (hi & 0xf0) | sp;
    }
    function add(v, withCarry) {
      const c = withCarry && (psw & CY) ? 1 : 0;
      const r = a + v + c;
      psw = (psw & ~(CY | AC)) | (r > 0xff ? CY : 0) | (((a & 0xf) + (v & 0xf) + c) > 0xf ? AC : 0);
      a = r & 0xff;
    }
    // conditional jump: stays in the page of the address byte
    function jcc(cond) {
      const page = pc & 0xf00, lo = fetch();
      if (cond) pc = page | lo;
      return 2;
    }
    function outP(port, v) { if (bus.outP) bus.outP(port, v); }
    function inP(port) { return bus.inP ? bus.inP(port) & 0xff : (port === 1 ? p1 : port === 2 ? p2 : 0xff); }
    function expander(op, port, v) { return bus.expander ? bus.expander(op, port, v) & 0xf : 0xf; }

    function exec(op) {
      switch (op) {
        case 0x00: return 1;                                         // NOP
        case 0x02: busLatch = a; outP(0, a); return 2;               // OUTL BUS,A
        case 0x03: add(fetch(), false); return 2;                    // ADD A,#
        case 0x05: intEn = true; return 1;                           // EN I
        case 0x07: a = (a - 1) & 0xff; return 1;                     // DEC A
        case 0x08: a = inP(0); return 2;                             // INS A,BUS
        case 0x09: a = inP(1); return 2;                             // IN A,P1
        case 0x0a: a = inP(2); return 2;                             // IN A,P2
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: a = expander(0, op & 3, 0); return 2; // MOVD A,Pp
        case 0x10: case 0x11: { const ad = ind(op & 1); ram[ad] = (ram[ad] + 1) & 0xff; return 1; } // INC @Rr
        case 0x13: add(fetch(), true); return 2;                     // ADDC A,#
        case 0x15: intEn = false; return 1;                          // DIS I
        case 0x16: { const c = tf; tf = false; return jcc(c); }      // JTF
        case 0x17: a = (a + 1) & 0xff; return 1;                     // INC A
        case 0x20: case 0x21: { const ad = ind(op & 1), v = ram[ad]; ram[ad] = a; a = v; return 1; } // XCH A,@Rr
        case 0x23: a = fetch(); return 2;                            // MOV A,#
        case 0x25: tcntEn = true; return 1;                          // EN TCNTI
        case 0x26: return jcc(!bus.t0());                            // JNT0
        case 0x27: a = 0; return 1;                                  // CLR A
        case 0x30: case 0x31: { const ad = ind(op & 1), v = ram[ad]; ram[ad] = (v & 0xf0) | (a & 0x0f); a = (a & 0xf0) | (v & 0x0f); return 1; } // XCHD
        case 0x35: tcntEn = false; tirq = false; return 1;           // DIS TCNTI
        case 0x36: return jcc(bus.t0());                             // JT0
        case 0x37: a ^= 0xff; return 1;                              // CPL A
        case 0x39: p1 = a; outP(1, a); return 2;                     // OUTL P1,A
        case 0x3a: p2 = a; outP(2, a); return 2;                     // OUTL P2,A
        case 0x3c: case 0x3d: case 0x3e: case 0x3f: expander(1, op & 3, a); return 2; // MOVD Pp,A
        case 0x40: case 0x41: a |= ram[ind(op & 1)]; return 1;       // ORL A,@Rr
        case 0x42: a = t; return 1;                                  // MOV A,T
        case 0x43: a |= fetch(); return 2;                           // ORL A,#
        case 0x45: counterOn = true; timerOn = false; return 1;      // STRT CNT
        case 0x46: return jcc(!bus.t1());                            // JNT1
        case 0x47: a = ((a << 4) | (a >> 4)) & 0xff; return 1;       // SWAP A
        case 0x50: case 0x51: a &= ram[ind(op & 1)]; return 1;       // ANL A,@Rr
        case 0x53: a &= fetch(); return 2;                           // ANL A,#
        case 0x55: timerOn = true; counterOn = false; prescale = 0; return 1; // STRT T
        case 0x56: return jcc(bus.t1());                             // JT1
        case 0x57: {                                                 // DA A
          if ((a & 0x0f) > 9 || (psw & AC)) { if (a > 0xf9) psw |= CY; a = (a + 6) & 0xff; }
          if ((a & 0xf0) > 0x90 || (psw & CY)) { a = (a + 0x60) & 0xff; psw |= CY; }
          return 1;
        }
        case 0x60: case 0x61: add(ram[ind(op & 1)], false); return 1; // ADD A,@Rr
        case 0x62: t = a; return 1;                                  // MOV T,A
        case 0x65: timerOn = counterOn = false; return 1;            // STOP TCNT
        case 0x67: { const c = psw & CY; psw = (psw & ~CY) | ((a & 1) ? CY : 0); a = (a >> 1) | (c ? 0x80 : 0); return 1; } // RRC A
        case 0x70: case 0x71: add(ram[ind(op & 1)], true); return 1; // ADDC A,@Rr
        case 0x75: return 1;                                         // ENT0 CLK (clock output, not used)
        case 0x76: return jcc(f1);                                   // JF1
        case 0x77: a = ((a >> 1) | (a << 7)) & 0xff; return 1;       // RR A
        case 0x80: case 0x81: a = bus.readX ? bus.readX(getR(op & 1)) & 0xff : 0xff; return 2; // MOVX A,@Rr
        case 0x83: pop(false); return 2;                             // RET
        case 0x85: psw &= ~F0; return 1;                             // CLR F0
        case 0x86: return jcc(bus.intLine());                        // JNI (jump if /INT is low)
        case 0x88: busLatch |= fetch(); outP(0, busLatch); return 2; // ORL BUS,#
        case 0x89: p1 |= fetch(); outP(1, p1); return 2;             // ORL P1,#
        case 0x8a: p2 |= fetch(); outP(2, p2); return 2;             // ORL P2,#
        case 0x8c: case 0x8d: case 0x8e: case 0x8f: expander(3, op & 3, a); return 2; // ORLD Pp,A
        case 0x90: case 0x91: if (bus.writeX) bus.writeX(getR(op & 1), a); return 2;  // MOVX @Rr,A
        case 0x93: pop(true); inIrq = false; a11 = dbf; return 2;   // RETR
        case 0x95: psw ^= F0; return 1;                              // CPL F0
        case 0x96: return jcc(a !== 0);                              // JNZ
        case 0x97: psw &= ~CY; return 1;                             // CLR C
        case 0x98: busLatch &= fetch(); outP(0, busLatch); return 2; // ANL BUS,#
        case 0x99: p1 &= fetch(); outP(1, p1); return 2;             // ANL P1,#
        case 0x9a: p2 &= fetch(); outP(2, p2); return 2;             // ANL P2,#
        case 0x9c: case 0x9d: case 0x9e: case 0x9f: expander(2, op & 3, a); return 2; // ANLD Pp,A
        case 0xa0: case 0xa1: ram[ind(op & 1)] = a; return 1;        // MOV @Rr,A
        case 0xa3: a = rom((pc & 0xf00) | a); return 2;              // MOVP A,@A
        case 0xa5: f1 = false; return 1;                             // CLR F1
        case 0xa7: psw ^= CY; return 1;                              // CPL C
        case 0xb0: case 0xb1: { const ad = ind(op & 1); ram[ad] = fetch(); return 2; } // MOV @Rr,#
        case 0xb3: pc = (pc & 0xf00) | rom((pc & 0xf00) | a); return 2; // JMPP @A
        case 0xb5: f1 = !f1; return 1;                               // CPL F1
        case 0xb6: return jcc(!!(psw & F0));                         // JF0
        case 0xc5: psw &= ~BS; return 1;                             // SEL RB0
        case 0xc6: return jcc(a === 0);                              // JZ
        case 0xc7: a = psw | 0x08; return 1;                         // MOV A,PSW
        case 0xd0: case 0xd1: a ^= ram[ind(op & 1)]; return 1;       // XRL A,@Rr
        case 0xd3: a ^= fetch(); return 2;                           // XRL A,#
        case 0xd5: psw |= BS; return 1;                              // SEL RB1
        case 0xd7: psw = a & 0xf7; return 1;                         // MOV PSW,A
        case 0xe3: a = rom(0x300 | a); return 2;                     // MOVP3 A,@A
        case 0xe5: dbf = 0; if (!inIrq) a11 = 0; return 1;           // SEL MB0
        case 0xe6: return jcc(!(psw & CY));                          // JNC
        case 0xe7: a = ((a << 1) | (a >> 7)) & 0xff; return 1;       // RL A
        case 0xf0: case 0xf1: a = ram[ind(op & 1)]; return 1;        // MOV A,@Rr
        case 0xf5: dbf = 0x800; if (!inIrq) a11 = 0x800; return 1;   // SEL MB1
        case 0xf6: return jcc(!!(psw & CY));                         // JC
        case 0xf7: { const c = psw & CY; psw = (psw & ~CY) | ((a & 0x80) ? CY : 0); a = ((a << 1) | (c ? 1 : 0)) & 0xff; return 1; } // RLC A
        default: break;
      }
      const lo3 = op & 7, hi = op & 0xf8;
      if (op >= 0x08) switch (hi) {
        case 0x18: setR(lo3, getR(lo3) + 1); return 1;               // INC Rr
        case 0x28: { const v = getR(lo3); setR(lo3, a); a = v; return 1; } // XCH A,Rr
        case 0x48: a |= getR(lo3); return 1;                         // ORL A,Rr
        case 0x58: a &= getR(lo3); return 1;                         // ANL A,Rr
        case 0x68: add(getR(lo3), false); return 1;                  // ADD A,Rr
        case 0x78: add(getR(lo3), true); return 1;                   // ADDC A,Rr
        case 0xa8: setR(lo3, a); return 1;                           // MOV Rr,A
        case 0xb8: setR(lo3, fetch()); return 2;                     // MOV Rr,#
        case 0xc8: setR(lo3, getR(lo3) - 1); return 1;               // DEC Rr
        case 0xd8: a ^= getR(lo3); return 1;                         // XRL A,Rr
        case 0xe8: { const page = pc & 0xf00, lo = fetch(), v = (getR(lo3) - 1) & 0xff; setR(lo3, v); if (v) pc = page | lo; return 2; } // DJNZ
        case 0xf8: a = getR(lo3); return 1;                          // MOV A,Rr
        default: break;
      }
      switch (op & 0x1f) {
        case 0x04: { const lo = fetch(); pc = a11 | ((op & 0xe0) << 3) | lo; return 2; }           // JMP
        case 0x14: { const lo = fetch(); push(); pc = a11 | ((op & 0xe0) << 3) | lo; return 2; }   // CALL
        case 0x12: return jcc(!!(a & (1 << (op >> 5))));                                             // JBb
        default: return 1; // unused opcodes do nothing
      }
    }

    // timer: counts every 32 machine cycles; counter: counts falling edges of T1
    function tick(cycles) {
      if (timerOn) {
        prescale += cycles;
        while (prescale >= 32) { prescale -= 32; t = (t + 1) & 0xff; if (t === 0) { tf = true; if (tcntEn) tirq = true; } }
      } else if (counterOn) {
        const now = !!bus.t1();
        if (lastT1 && !now) { t = (t + 1) & 0xff; if (t === 0) { tf = true; if (tcntEn) tirq = true; } }
        lastT1 = now;
      }
    }

    function step() {
      let cycles;
      if (!inIrq && intEn && bus.intLine()) {
        push(); pc = 3; inIrq = true; a11 = 0; cycles = 2;
      } else if (!inIrq && tirq && tcntEn) {
        tirq = false; push(); pc = 7; inIrq = true; a11 = 0; cycles = 2;
      } else {
        cycles = exec(fetch());
      }
      tick(cycles);
      return cycles;
    }

    this.step = step;
    this.ram = ram;
    this.reset = function () {
      a = 0; pc = 0; psw = 0; f1 = false; dbf = 0; a11 = 0; t = 0; tf = false; tirq = false;
      timerOn = counterOn = false; prescale = 0; lastT1 = false;
      intEn = false; tcntEn = false; inIrq = false; p1 = p2 = 0xff; busLatch = 0xff;
    };
    this.getPC = () => pc;
    this.port = (n) => (n === 1 ? p1 : n === 2 ? p2 : busLatch);
    const KEYS = ["a", "pc", "psw", "f1", "dbf", "a11", "t", "tf", "tirq", "timerOn", "counterOn", "prescale", "lastT1",
      "intEn", "tcntEn", "inIrq", "p1", "p2", "bus"];
    this.getState = function () {
      return { r: [a, pc, psw, f1 ? 1 : 0, dbf, a11, t, tf ? 1 : 0, tirq ? 1 : 0, timerOn ? 1 : 0, counterOn ? 1 : 0,
        prescale, lastT1 ? 1 : 0, intEn ? 1 : 0, tcntEn ? 1 : 0, inIrq ? 1 : 0, p1, p2, busLatch], ram: ram.slice() };
    };
    this.setState = function (s) {
      const r = s.r;
      [a, pc, psw] = r; f1 = !!r[3]; dbf = r[4]; a11 = r[5]; t = r[6]; tf = !!r[7]; tirq = !!r[8];
      timerOn = !!r[9]; counterOn = !!r[10]; prescale = r[11]; lastT1 = !!r[12];
      intEn = !!r[13]; tcntEn = !!r[14]; inIrq = !!r[15]; p1 = r[16]; p2 = r[17]; busLatch = r[18];
      ram.set(s.ram);
    };
    this.regs = function () {
      const r = this.getState().r, o = {};
      KEYS.forEach((k, i) => { o[k] = r[i]; });
      return o;
    };
    this.reset();
  }
  global.RPMCS48 = RPMCS48;
})(typeof window !== "undefined" ? window : globalThis);
