// RetroPixels — extra NES / Famicom cartridge boards (mappers). Original code.
//
// nes.js holds the most common boards; this file adds the rest of the licensed
// library (Konami VRC, Namco, Bandai, Jaleco, Irem, Taito, Sunsoft...) and the
// popular unlicensed boards and multicarts. Each board is written from its
// documented register behaviour. The sound chips some of them carry live in
// nes_audio.js.
"use strict";

(function (global) {
  const { Mapper, MIRROR, MAPPERS, M1, M4 } = global.RPNes;
  const A = () => global.RPNesAudio;
  // Common 2-bit mirroring encodings.
  const VH1 = [MIRROR.V, MIRROR.H, MIRROR.ONE_LO, MIRROR.ONE_HI];
  const HV1 = [MIRROR.H, MIRROR.V, MIRROR.ONE_LO, MIRROR.ONE_HI];

  // The value last seen on the CPU data bus (for registers that only drive some bits).
  function openBus(m) { return m.cart.nes ? m.cart.nes.openBus : 0; }

  // ===================================================================
  // Konami
  // ===================================================================

  // The IRQ counter shared by the VRC4, VRC6 and VRC7: an 8-bit up-counter that
  // fires on overflow, clocked either every CPU cycle or once per scanline
  // (a prescaler that counts 341 PPU dots, i.e. 113.67 CPU cycles).
  class VrcIrq {
    constructor() { this.latch = 0; this.counter = 0; this.prescaler = 341; this.enabled = false; this.enableAfterAck = false; this.cycleMode = false; this.pending = false; }
    control(v) {
      this.enableAfterAck = !!(v & 1); this.enabled = !!(v & 2); this.cycleMode = !!(v & 4);
      if (this.enabled) { this.counter = this.latch; this.prescaler = 341; }
      this.pending = false;
    }
    ack() { this.pending = false; this.enabled = this.enableAfterAck; }
    clock() {
      if (!this.enabled) return;
      if (!this.cycleMode) { this.prescaler -= 3; if (this.prescaler > 0) return; this.prescaler += 341; }
      if (this.counter === 0xff) { this.counter = this.latch; this.pending = true; } else this.counter++;
    }
  }

  // 21 / 22 / 23 / 25 — VRC2 and VRC4 (Gradius II, Contra Japan, Ganbare Goemon, Wai Wai World 2...)
  // The boards differ in which CPU address lines reach the chip's two register-select pins.
  class VRC24 extends Mapper {
    constructor(cart, no) {
      super(cart);
      const sub = cart.submapper;
      let lines; this.vrc4 = true; this.chrShift = 0;
      switch (no) {
        case 21: lines = sub === 1 ? [[2, 4]] : sub === 2 ? [[0x40, 0x80]] : [[2, 4], [0x40, 0x80]]; break;
        case 22: lines = [[2, 1]]; this.vrc4 = false; this.chrShift = 1; break;
        case 23: lines = sub === 1 ? [[1, 2]] : sub === 2 ? [[4, 8]] : sub === 3 ? [[1, 2]] : [[1, 2], [4, 8]]; this.vrc4 = sub !== 3; break;
        default: lines = sub === 1 ? [[2, 1]] : sub === 2 ? [[8, 4]] : sub === 3 ? [[2, 1]] : [[2, 1], [8, 4]]; this.vrc4 = sub !== 3; break;
      }
      this.lines = lines;
      this.prg0 = 0; this.prg1 = 0; this.swap = 0; this.chrLo = new Uint8Array(8); this.chrHi = new Uint8Array(8);
      this.vrc = new VrcIrq();
      this.update();
    }
    reg(a) {
      let r = 0;
      for (const [lo, hi] of this.lines) { if (a & lo) r |= 1; if (a & hi) r |= 2; }
      return r;
    }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      const r = this.reg(a);
      switch (a & 0xf000) {
        case 0x8000: this.prg0 = v & 0x1f; break;
        case 0x9000:
          if (!this.vrc4) this.mirror = v & 1 ? MIRROR.H : MIRROR.V;
          else if (r < 2) this.mirror = VH1[v & 3];
          else this.swap = (v >> 1) & 1;
          break;
        case 0xa000: this.prg1 = v & 0x1f; break;
        case 0xf000:
          if (!this.vrc4) break;
          if (r === 0) this.vrc.latch = (this.vrc.latch & 0xf0) | (v & 0x0f);
          else if (r === 1) this.vrc.latch = (this.vrc.latch & 0x0f) | ((v & 0x0f) << 4);
          else if (r === 2) this.vrc.control(v); else this.vrc.ack();
          return;
        default: {
          const i = (((a >> 12) - 0xb) << 1) | (r >> 1);
          if (r & 1) this.chrHi[i] = v & 0x1f; else this.chrLo[i] = v & 0x0f;
        }
      }
      this.update();
    }
    update() {
      if (this.swap) { this.setPrg8(0, -2); this.setPrg8(2, this.prg0); } else { this.setPrg8(0, this.prg0); this.setPrg8(2, -2); }
      this.setPrg8(1, this.prg1); this.setPrg8(3, -1);
      for (let i = 0; i < 8; i++) this.setChr1(i, (this.chrLo[i] | (this.chrHi[i] << 4)) >> this.chrShift);
    }
    cpuClock() { if (this.vrc4) { this.vrc.clock(); this.irq = this.vrc.pending; } }
    afterLoad() { this.update(); }
  }

  // 73 — VRC3 (Salamander): 16-bit IRQ counter
  class M73 extends Mapper {
    constructor(cart) { super(cart); this.latch = 0; this.counter = 0; this.ctl = 0; this.setPrg16(1, -1); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      switch (a & 0xf000) {
        case 0x8000: case 0x9000: case 0xa000: case 0xb000: {
          const sh = ((a >> 12) - 8) * 4;
          this.latch = (this.latch & ~(0xf << sh)) | ((v & 0xf) << sh); break;
        }
        case 0xc000: this.ctl = v & 7; if (v & 2) this.counter = this.latch; this.irq = false; break;
        case 0xd000: this.irq = false; this.ctl = (this.ctl & ~2) | ((this.ctl & 1) << 1); break;
        case 0xf000: this.setPrg16(0, v & 7); break;
      }
    }
    cpuClock() {
      if (!(this.ctl & 2)) return;
      if (this.ctl & 4) { // 8-bit mode: only the low byte counts
        const lo = (this.counter + 1) & 0xff;
        if (lo === 0) { this.counter = (this.counter & 0xff00) | (this.latch & 0xff); this.irq = true; } else this.counter = (this.counter & 0xff00) | lo;
      } else if (this.counter === 0xffff) { this.counter = this.latch; this.irq = true; } else this.counter++;
    }
  }

  // 75 — VRC1 (Ganbare Goemon!, Exciting Boxing)
  class M75 extends Mapper {
    constructor(cart) { super(cart); this.c = [0, 0]; this.ch = 0; this.setPrg8(3, -1); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      switch (a & 0xf000) {
        case 0x8000: this.setPrg8(0, v & 0x0f); break;
        case 0x9000: if (this.mirror !== MIRROR.FOUR) this.mirror = v & 1 ? MIRROR.H : MIRROR.V; this.ch = v; break;
        case 0xa000: this.setPrg8(1, v & 0x0f); break;
        case 0xc000: this.setPrg8(2, v & 0x0f); break;
        case 0xe000: this.c[0] = v & 0x0f; break;
        case 0xf000: this.c[1] = v & 0x0f; break;
      }
      this.setChr4(0, this.c[0] | ((this.ch & 2) << 3)); this.setChr4(1, this.c[1] | ((this.ch & 4) << 2));
    }
  }

  // 24 / 26 — VRC6 (Akumajou Densetsu, Madara, Esper Dream 2) with its two pulse channels and sawtooth
  class M24 extends Mapper {
    constructor(cart, swapLines) {
      super(cart);
      this.swapLines = swapLines; this.prg16 = 0; this.prg8 = 0; this.r = new Uint8Array(8); this.ctl = 0x20;
      this.vrc = new VrcIrq();
      this.audio = new (A().VRC6)();
      this.update();
    }
    cpuRead(a) {
      if (a >= 0x6000 && a < 0x8000 && !(this.ctl & 0x80)) return -1;
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a < 0x8000) { if (this.ctl & 0x80) super.cpuWrite(a, v); return; }
      let r = a & 3; if (this.swapLines) r = ((r & 1) << 1) | (r >> 1);
      switch (a & 0xf000) {
        case 0x8000: this.prg16 = v & 0x0f; break;
        case 0x9000: case 0xa000: case 0xb000:
          if ((a & 0xf000) === 0xb000 && r === 3) { this.ctl = v; break; }
          this.audio.write(((a >> 12) - 9), r, v); return;
        case 0xc000: this.prg8 = v & 0x1f; break;
        case 0xd000: this.r[r] = v; break;
        case 0xe000: this.r[4 + r] = v; break;
        case 0xf000:
          if (r === 0) this.vrc.latch = v; else if (r === 1) this.vrc.control(v); else if (r === 2) this.vrc.ack();
          return;
      }
      this.update();
    }
    update() {
      this.setPrg16(0, this.prg16); this.setPrg8(2, this.prg8); this.setPrg8(3, -1);
      const r = this.r;
      switch (this.ctl & 3) {
        case 0: for (let i = 0; i < 8; i++) this.setChr1(i, r[i]); break;
        case 1: for (let i = 0; i < 8; i++) this.setChr1(i, (r[i >> 1] & 0xfe) | (i & 1)); break;
        default:
          for (let i = 0; i < 4; i++) this.setChr1(i, r[i]);
          for (let i = 4; i < 8; i++) this.setChr1(i, (r[4 + ((i - 4) >> 1)] & 0xfe) | (i & 1));
      }
      if (this.mirror !== MIRROR.FOUR) this.mirror = VH1[(this.ctl >> 2) & 3];
    }
    cpuClock() { this.vrc.clock(); this.irq = this.vrc.pending; this.audio.clock(); }
    afterLoad() { this.update(); }
  }

  // 85 — VRC7 (Lagrange Point, Tiny Toon Adventures 2) with its FM synthesizer
  class M85 extends Mapper {
    constructor(cart) {
      super(cart);
      const sub = cart.submapper;
      this.hiLine = sub === 1 ? 0x08 : sub === 2 ? 0x10 : 0x18; // which address line picks the second register
      this.p = [0, 0, 0]; this.c = new Uint8Array(8); this.ctl = 0; this.vrc = new VrcIrq();
      this.audio = new (A().VRC7)();
      this.update();
    }
    cpuRead(a) { if (a >= 0x6000 && a < 0x8000 && !(this.ctl & 0x80)) return -1; return super.cpuRead(a); }
    cpuWrite(a, v) {
      if (a < 0x8000) { if (this.ctl & 0x80) super.cpuWrite(a, v); return; }
      const hi = (a & this.hiLine) ? 1 : 0;
      switch (a & 0xf000) {
        case 0x8000: this.p[hi] = v & 0x3f; break;
        case 0x9000:
          if (a & 0x10) { if (a & 0x20) this.audio.writeData(v); else this.audio.writeAddr(v); return; }
          if (!hi) this.p[2] = v & 0x3f;
          break;
        case 0xa000: case 0xb000: case 0xc000: case 0xd000: this.c[((a >> 12) - 0xa) * 2 + hi] = v; break;
        case 0xe000:
          if (!hi) { this.ctl = v; this.audio.mute = !!(v & 0x40); }
          else this.vrc.latch = v;
          break;
        case 0xf000: if (!hi) this.vrc.control(v); else this.vrc.ack(); return;
      }
      this.update();
    }
    update() {
      this.setPrg8(0, this.p[0]); this.setPrg8(1, this.p[1]); this.setPrg8(2, this.p[2]); this.setPrg8(3, -1);
      for (let i = 0; i < 8; i++) this.setChr1(i, this.c[i]);
      this.mirror = VH1[this.ctl & 3];
    }
    cpuClock() { this.vrc.clock(); this.irq = this.vrc.pending; this.audio.clock(); }
    afterLoad() { this.update(); }
  }

  // ===================================================================
  // Bandai
  // ===================================================================

  // Serial EEPROMs (I2C) used for saves on Bandai boards: the 24C02 (256 bytes,
  // device address + word address, MSB first) and the older X24C01 (128 bytes,
  // address sent directly, LSB first).
  class Eeprom {
    constructor(size, x24c01) {
      this.data = new Uint8Array(size); this.x01 = x24c01;
      this.scl = 1; this.sda = 1; this.state = 0; this.bits = 0; this.byte = 0; this.addr = 0; this.out = 1;
      this.next = 0; this.ackIn = 1; this.purpose = 0;
    }
    // states: 0 idle, 1 receiving a byte, 2 sending our ACK, 3 sending a byte, 4 waiting for the master's ACK
    set(scl, sda) {
      if (this.scl && scl) {
        if (this.sda && !sda) this.start();
        else if (!this.sda && sda) { this.state = 0; this.out = 1; }
      } else if (!this.scl && scl) this.rise(sda);
      else if (this.scl && !scl) this.fall();
      this.scl = scl; this.sda = sda;
    }
    start() { this.state = 1; this.bits = 0; this.byte = 0; this.purpose = 0; this.out = 1; }
    rise(sda) {
      if (this.state === 1) {
        if (this.x01) this.byte |= sda << this.bits; else this.byte = (this.byte << 1) | sda;
        this.bits++;
      } else if (this.state === 3) this.bits++;
      else if (this.state === 4) this.ackIn = sda;
    }
    loadByte() { this.byte = this.data[this.addr]; this.addr = (this.addr + 1) % this.data.length; this.bits = 0; this.state = 3; this.out = this.bitOut(0); }
    bitOut(i) { return this.x01 ? (this.byte >> i) & 1 : (this.byte >> (7 - i)) & 1; }
    fall() {
      switch (this.state) {
        case 1:
          if (this.bits < 8) return;
          if (this.x01) {
            if (this.purpose === 0) { this.addr = this.byte & 0x7f; this.next = this.byte & 0x80 ? 3 : 1; this.purpose = 2; }
            else { this.data[this.addr] = this.byte; this.addr = (this.addr & 0x7c) | ((this.addr + 1) & 3); this.next = 1; }
          } else if (this.purpose === 0) {
            if ((this.byte >> 4) !== 0xa) { this.state = 0; this.out = 1; return; }
            this.next = this.byte & 1 ? 3 : 1; this.purpose = 1;
          } else if (this.purpose === 1) { this.addr = this.byte; this.next = 1; this.purpose = 2; }
          else { this.data[this.addr] = this.byte; this.addr = (this.addr & 0xf0) | ((this.addr + 1) & 0x0f); this.next = 1; }
          this.state = 2; this.out = 0; break;
        case 2:
          if (this.next === 3) this.loadByte();
          else { this.state = 1; this.bits = 0; this.byte = 0; this.out = 1; }
          break;
        case 3:
          if (this.bits < 8) this.out = this.bitOut(this.bits);
          else { this.state = 4; this.out = 1; this.ackIn = 1; }
          break;
        case 4:
          if (this.ackIn === 0) this.loadByte(); else { this.state = 0; this.out = 1; }
          break;
      }
    }
  }

  // 16 / 153 / 159 — Bandai FCG-1/2, LZ93D50 (Dragon Ball, SD Gundam, Famicom Jump II)
  class M16 extends Mapper {
    constructor(cart, kind) {
      super(cart);
      const sub = cart.submapper;
      this.kind = kind;
      this.lowRegs = kind === 16 && sub !== 5;    // FCG-1/2 registers at $6000
      this.highRegs = !(kind === 16 && sub === 4); // LZ93D50 registers at $8000
      this.copyLatch = !(kind === 16 && sub === 4);
      this.c = new Uint8Array(8); this.prgReg = 0; this.irqOn = false; this.counter = 0; this.latch = 0; this.ramOn = false; this.eepCtl = 0;
      this.eeprom = kind === 159 ? new Eeprom(128, true) : (kind === 16 && sub !== 4 ? new Eeprom(256, false) : null);
      if (this.eeprom) { this.batteryData = () => this.eeprom.data; this.loadBattery = (d) => this.eeprom.data.set(d.subarray(0, this.eeprom.data.length)); }
      this.update();
    }
    cpuRead(a) {
      if (a >= 0x6000 && a < 0x8000) {
        if (this.kind === 153) return this.ramOn ? this.prgRam[a & 0x1fff] : -1;
        const bit = this.eeprom && (this.eepCtl & 0x80) ? this.eeprom.out : 0;
        return (openBus(this) & 0xef) | (bit << 4);
      }
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x8000) {
        if (this.kind === 153) { if (this.ramOn) this.prgRam[a & 0x1fff] = v; return; }
        if (!this.lowRegs) return;
      } else if (a < 0x6000 || !this.highRegs) return;
      switch (a & 0x0f) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: this.c[a & 7] = v; break;
        case 8: this.prgReg = v; break;
        case 9: this.mirror = VH1[v & 3]; break;
        case 0xa: this.irqOn = !!(v & 1); if (this.copyLatch) this.counter = this.latch; this.irq = false; break;
        case 0xb: this.latch = (this.latch & 0xff00) | v; if (!this.copyLatch || this.lowRegs) this.counter = (this.counter & 0xff00) | v; break;
        case 0xc: this.latch = (this.latch & 0xff) | (v << 8); if (!this.copyLatch || this.lowRegs) this.counter = (this.counter & 0xff) | (v << 8); break;
        case 0xd:
          if (this.kind === 153) this.ramOn = !!(v & 0x20);
          else { this.eepCtl = v; if (this.eeprom) this.eeprom.set((v >> 5) & 1, (v >> 6) & 1); }
          break;
      }
      this.update();
    }
    update() {
      if (this.kind === 153) {
        const outer = (this.c[0] | this.c[1] | this.c[2] | this.c[3]) & 1 ? 0x10 : 0;
        this.setPrg16(0, outer | (this.prgReg & 0x0f)); this.setPrg16(1, outer | 0x0f);
      } else {
        this.setPrg16(0, this.prgReg & 0x0f); this.setPrg16(1, -1);
        for (let i = 0; i < 8; i++) this.setChr1(i, this.c[i]);
      }
    }
    cpuClock() {
      if (!this.irqOn) return;
      this.counter = (this.counter - 1) & 0xffff;
      if (this.counter === 0) this.irq = true;
    }
    afterLoad() { this.update(); }
  }

  // 70 — Bandai 74161 (Kamen Rider Club); 152 — the same with one-screen mirroring control
  class M70 extends Mapper {
    constructor(cart, oneScreen) { super(cart); this.oneScreen = oneScreen; this.setPrg16(1, -1); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      v &= this.cpuRead(a);
      this.setPrg16(0, (v >> 4) & (this.oneScreen ? 7 : 15)); this.setChr8(v & 15);
      if (this.oneScreen) this.mirror = v & 0x80 ? MIRROR.ONE_HI : MIRROR.ONE_LO;
    }
  }

  // ===================================================================
  // Namco
  // ===================================================================

  // 19 — Namco 163 (Megami Tensei II, King of Kings, Final Lap) with its wavetable sound;
  // 210 — Namco 175 / 340 (Famista, Wagyan Land 2): the same banking without the extras.
  class M19 extends Mapper {
    constructor(cart, kind) {
      super(cart);
      this.kind = kind; // 163, 175 or 340 (0 = not known yet)
      this.ciram = cart.vram;
      this.c = new Uint8Array(12).fill(0xff); // 8 pattern windows + 4 nametables
      for (let i = 8; i < 12; i++) this.c[i] = 0xe0 | (cart.mirror === MIRROR.V ? i & 1 : (i >> 1) & 1);
      this.p = new Uint8Array(3); this.ramOff = [0, 0]; this.counter = 0;
      if (kind === 163) this.audio = new (A().N163)();
      this.ram175 = false;
      this.update();
    }
    update() { this.setPrg8(0, this.p[0]); this.setPrg8(1, this.p[1]); this.setPrg8(2, this.p[2]); this.setPrg8(3, -1); }
    cpuRead(a) {
      if (this.kind === 163) {
        if (a >= 0x4800 && a < 0x5000) return this.audio.readData();
        if (a >= 0x5000 && a < 0x5800) return this.counter & 0xff;
        if (a >= 0x5800 && a < 0x6000) return this.counter >> 8;
      }
      if (a >= 0x6000 && a < 0x8000 && this.kind === 175 && !this.ram175) return -1;
      if (a >= 0x6000 && a < 0x8000 && this.kind === 340) return -1;
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a < 0x6000) {
        if (this.kind !== 163) return;
        if (a >= 0x4800 && a < 0x5000) this.audio.writeData(v);
        else if (a >= 0x5000 && a < 0x5800) { this.counter = (this.counter & 0xff00) | v; this.irq = false; }
        else if (a >= 0x5800) { this.counter = (this.counter & 0xff) | (v << 8); this.irq = false; }
        return;
      }
      if (a < 0x8000) { if (this.kind !== 175 || this.ram175) super.cpuWrite(a, v); return; }
      if (a < 0xc000) { this.c[(a - 0x8000) >> 11] = v; return; }
      if (a < 0xe000) {
        if (this.kind === 163) this.c[8 + ((a - 0xc000) >> 11)] = v;
        else if (this.kind === 175 && a < 0xc800) this.ram175 = !!(v & 1);
        return;
      }
      if (a < 0xe800) {
        this.p[0] = v & 0x3f;
        if (this.kind === 163) this.audio.disabled = !!(v & 0x40);
        if (this.kind === 0 && (v & 0xc0)) this.kind = 340; // a game that sets mirroring bits is on a 340
        if (this.kind === 340) this.mirror = [MIRROR.ONE_LO, MIRROR.V, MIRROR.ONE_HI, MIRROR.H][v >> 6];
      } else if (a < 0xf000) { this.p[1] = v & 0x3f; this.ramOff = [(v >> 6) & 1, (v >> 7) & 1]; }
      else if (a < 0xf800) this.p[2] = v & 0x3f;
      else if (this.kind === 163) this.audio.addr = v;
      this.update();
    }
    window(slot, a) { // returns [isCiram, index]
      const r = this.c[slot];
      if (this.kind === 163 && r >= 0xe0 && (slot >= 8 || !this.ramOff[slot >> 2])) return ((r & 1) << 10) | (a & 0x3ff) | 0x100000;
      return (r % (this.chr.length >> 10)) * 0x400 + (a & 0x3ff);
    }
    ppuRead(a) { const i = this.window(a >> 10, a); return i & 0x100000 ? this.ciram[i & 0x7ff] : this.chr[i]; }
    ppuWrite(a, v) { const i = this.window(a >> 10, a); if (i & 0x100000) this.ciram[i & 0x7ff] = v; else if (this.chrRam) this.chr[i] = v; }
    ntRead(a) { if (this.kind !== 163) return this.ciram[this.ntIndex(a)]; const i = this.window(8 + ((a >> 10) & 3), a); return i & 0x100000 ? this.ciram[i & 0x7ff] : this.chr[i]; }
    ntWrite(a, v) { if (this.kind !== 163) { this.ciram[this.ntIndex(a)] = v; return; } const i = this.window(8 + ((a >> 10) & 3), a); if (i & 0x100000) this.ciram[i & 0x7ff] = v; }
    cpuClock() {
      if (this.kind === 163) {
        if ((this.counter & 0x8000) && (this.counter & 0x7fff) !== 0x7fff) {
          this.counter++;
          if ((this.counter & 0x7fff) === 0x7fff) this.irq = true;
        }
        this.audio.clock();
      }
    }
    afterLoad() { this.update(); }
  }

  // 76 — Namco 3446 (Digital Devil Story): Namco 108 with 2 KB CHR banks
  class M76 extends Mapper {
    constructor(cart) { super(cart); this.sel = 0; this.r = new Uint8Array(8); this.update(); }
    cpuWrite(a, v) {
      if (a < 0x8000 || a >= 0xa000) return super.cpuWrite(a, v);
      if (!(a & 1)) this.sel = v & 7; else this.r[this.sel] = v;
      this.update();
    }
    update() {
      this.setPrg8(0, this.r[6]); this.setPrg8(1, this.r[7]); this.setPrg8(2, -2); this.setPrg8(3, -1);
      for (let i = 0; i < 4; i++) this.setChr2(i, this.r[2 + i]);
    }
    afterLoad() { this.update(); }
  }

  // 88 / 154 — Namco 108 with the two halves of CHR split by A12; 154 adds one-screen mirroring.
  // 95 — Namco 108 where CHR bank bit 5 also picks the nametable (Dragon Buster).
  class N108 extends Mapper {
    constructor(cart, variant) { super(cart); this.variant = variant; this.sel = 0; this.r = new Uint8Array([0, 2, 4, 5, 6, 7, 0, 1]); this.update(); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      if (this.variant === 154) this.mirror = v & 0x40 ? MIRROR.ONE_HI : MIRROR.ONE_LO;
      if (a >= 0xa000) return;
      if (!(a & 1)) this.sel = v & 7; else this.r[this.sel] = v & 0x3f;
      this.update();
    }
    update() {
      const r = this.r;
      this.setPrg8(0, r[6] & 0x0f); this.setPrg8(1, r[7] & 0x0f); this.setPrg8(2, -2); this.setPrg8(3, -1);
      const split = this.variant === 88 || this.variant === 154;
      const lo = (x) => split ? x & 0x3f & ~0x40 : x, hi = (x) => split ? x | 0x40 : x;
      this.setChr2(0, lo(r[0]) >> 1); this.setChr2(1, lo(r[1]) >> 1);
      for (let i = 0; i < 4; i++) this.setChr1(4 + i, hi(r[2 + i]));
      if (this.variant === 95) this.setNt((r[0] >> 5) & 1, (r[0] >> 5) & 1, (r[1] >> 5) & 1, (r[1] >> 5) & 1);
    }
    afterLoad() { this.update(); }
  }

  // ===================================================================
  // Jaleco
  // ===================================================================

  // 18 — Jaleco SS88006 (Ninja Jajamaru, Pizza Pop!, Magic John)
  class M18 extends Mapper {
    constructor(cart) {
      super(cart); this.p = new Uint8Array(3); this.c = new Uint8Array(8); this.latch = 0; this.counter = 0; this.ctl = 0; this.ramOn = 0;
      this.update();
    }
    cpuRead(a) { if (a >= 0x6000 && a < 0x8000 && !(this.ramOn & 1)) return -1; return super.cpuRead(a); }
    cpuWrite(a, v) {
      if (a < 0x8000) { if ((this.ramOn & 3) === 3) super.cpuWrite(a, v); return; }
      const r = ((a - 0x8000) >> 12) * 4 + (a & 3), n = v & 0x0f;
      if (r < 6) { const i = r >> 1; this.p[i] = r & 1 ? (this.p[i] & 0x0f) | (n << 4) : (this.p[i] & 0xf0) | n; }
      else if (r === 6) this.ramOn = v & 3;
      else if (r >= 8 && r < 24) { const i = (r - 8) >> 1; this.c[i] = r & 1 ? (this.c[i] & 0x0f) | (n << 4) : (this.c[i] & 0xf0) | n; }
      else if (r >= 24 && r < 28) { const sh = (r - 24) * 4; this.latch = (this.latch & ~(0xf << sh)) | (n << sh); }
      else if (r === 28) { this.counter = this.latch; this.irq = false; }
      else if (r === 29) { this.ctl = v & 0x0f; this.irq = false; }
      else if (r === 30) this.mirror = HV1[v & 3];
      this.update();
    }
    update() {
      for (let i = 0; i < 3; i++) this.setPrg8(i, this.p[i]);
      this.setPrg8(3, -1);
      for (let i = 0; i < 8; i++) this.setChr1(i, this.c[i]);
    }
    cpuClock() {
      if (!(this.ctl & 1)) return;
      const c = this.ctl, mask = c & 8 ? 0xf : c & 4 ? 0xff : c & 2 ? 0xfff : 0xffff;
      const low = this.counter & mask;
      if (low === 0) this.irq = true;
      this.counter = (this.counter & ~mask) | ((low - 1) & mask);
    }
    afterLoad() { this.update(); }
  }

  // 72 / 92 — Jaleco JF-17 / JF-19: bit 7 latches the PRG bank, bit 6 the CHR bank
  class M72 extends Mapper {
    constructor(cart, upper) { super(cart); this.upper = upper; if (upper) this.setPrg16(0, 0); else this.setPrg16(1, -1); this.last = 0; }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      v &= this.cpuRead(a);
      if ((v & 0x80) && !(this.last & 0x80)) this.setPrg16(this.upper ? 1 : 0, v & 0x0f);
      if ((v & 0x40) && !(this.last & 0x40)) this.setChr8(v & 0x0f);
      this.last = v;
    }
  }

  // 86 — Jaleco JF-13 (Moero!! Pro Yakyuu); 87 — Jaleco/Konami CNROM-like at $6000
  class M86 extends Mapper {
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x7000) { this.setPrg32((v >> 4) & 3); this.setChr8((v & 3) | ((v >> 4) & 4)); }
    }
  }
  class M87 extends Mapper {
    cpuWrite(a, v) { if (a >= 0x6000 && a < 0x8000) this.setChr8(((v & 1) << 1) | ((v >> 1) & 1)); }
  }

  // 78 — Irem 74HC161 (Holy Diver) / Jaleco JF-16 (Uchuusen Cosmo Carrier)
  class M78 extends Mapper {
    constructor(cart) {
      super(cart); this.setPrg16(1, -1);
      const sub = cart.submapper;
      this.hv = sub === 3 || (sub !== 1 && (cart.mirror === MIRROR.V || cart.mirror === MIRROR.FOUR));
      this.mirror = this.hv ? MIRROR.H : MIRROR.ONE_LO;
    }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      v &= this.cpuRead(a);
      this.setPrg16(0, v & 7); this.setChr8(v >> 4);
      this.mirror = this.hv ? (v & 8 ? MIRROR.V : MIRROR.H) : (v & 8 ? MIRROR.ONE_HI : MIRROR.ONE_LO);
    }
  }

  // ===================================================================
  // Irem
  // ===================================================================

  // 32 — Irem G-101 (Image Fight, Major League, Kaiketsu Yanchamaru 2)
  class M32 extends Mapper {
    constructor(cart) { super(cart); this.p0 = 0; this.p1 = 0; this.mode = 0; this.major = cart.submapper === 1; if (this.major) this.mirror = MIRROR.ONE_LO; this.update(); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      switch (a & 0xf000) {
        case 0x8000: this.p0 = v & 0x1f; break;
        case 0x9000: if (!this.major) { this.mirror = v & 1 ? MIRROR.H : MIRROR.V; this.mode = (v >> 1) & 1; } break;
        case 0xa000: this.p1 = v & 0x1f; break;
        case 0xb000: this.setChr1(a & 7, v); return;
      }
      this.update();
    }
    update() {
      if (this.mode) { this.setPrg8(0, -2); this.setPrg8(2, this.p0); } else { this.setPrg8(0, this.p0); this.setPrg8(2, -2); }
      this.setPrg8(1, this.p1); this.setPrg8(3, -1);
    }
    afterLoad() { this.update(); }
  }

  // 65 — Irem H3001 (Daiku no Gen-san 2, Spartan X 2)
  class M65 extends Mapper {
    constructor(cart) {
      super(cart); this.p = [0, 1, 0xfe]; this.mode = 0; this.irqOn = false; this.counter = 0; this.latch = 0;
      this.update();
    }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      switch (a & 0xf000) {
        case 0x8000: this.p[0] = v; break;
        case 0x9000:
          switch (a & 7) {
            case 0: this.mode = v >> 7; break;
            case 1: this.mirror = v & 0x80 ? MIRROR.H : MIRROR.V; break;
            case 3: this.irqOn = !!(v & 0x80); this.irq = false; break;
            case 4: this.counter = this.latch; this.irq = false; break;
            case 5: this.latch = (this.latch & 0xff) | (v << 8); break;
            case 6: this.latch = (this.latch & 0xff00) | v; break;
          }
          break;
        case 0xa000: this.p[1] = v; break;
        case 0xb000: this.setChr1(a & 7, v); return;
        case 0xc000: this.p[2] = v; break;
      }
      this.update();
    }
    update() {
      if (this.mode) { this.setPrg8(0, this.p[2]); this.setPrg8(2, this.p[0]); } else { this.setPrg8(0, this.p[0]); this.setPrg8(2, this.p[2]); }
      this.setPrg8(1, this.p[1]); this.setPrg8(3, -1);
    }
    cpuClock() {
      if (this.irqOn && this.counter > 0) { if (--this.counter === 0) { this.irq = true; this.irqOn = false; } }
    }
    afterLoad() { this.update(); }
  }

  // 77 — Irem (Napoleon Senki): 2 KB of switchable CHR ROM, 6 KB CHR RAM and four-screen nametables
  class M77 extends Mapper {
    constructor(cart) { super(cart); this.ram = new Uint8Array(0x2000); this.mirror = MIRROR.FOUR; this.setPrg32(0); this.cb = 0; }
    cpuWrite(a, v) { if (a >= 0x8000) { v &= this.cpuRead(a); this.setPrg32(v & 0x0f); this.cb = v >> 4; } else super.cpuWrite(a, v); }
    ppuRead(a) { return a < 0x800 ? this.chr[((this.cb * 0x800) + a) % this.chr.length] : this.ram[a]; }
    ppuWrite(a, v) { if (a >= 0x800) this.ram[a] = v; }
  }

  // 97 — Irem TAM-S1 (Kaiketsu Yanchamaru): fixed $8000, switchable $C000
  class M97 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(0, -1); this.setPrg16(1, 0); }
    cpuWrite(a, v) {
      if (a < 0x8000 || a >= 0xc000) return super.cpuWrite(a, v);
      this.setPrg16(1, v & 0x1f);
      this.mirror = [MIRROR.ONE_LO, MIRROR.H, MIRROR.V, MIRROR.ONE_HI][v >> 6];
    }
  }

  // ===================================================================
  // Taito
  // ===================================================================

  // 33 — Taito TC0190 (Akira, Don Doko Don); 48 — TC0690 (Bubble Bobble 2, The Jetsons) adds an MMC3-style IRQ
  class M33 extends M4 {
    constructor(cart, tc0690) { super(cart); this.tc0690 = tc0690; this.p = [0, 1]; this.c = new Uint8Array(6); this.update(); }
    cpuWrite(a, v) {
      if (a < 0x8000) return Mapper.prototype.cpuWrite.call(this, a, v);
      switch (a & 0xe003) {
        case 0x8000: this.p[0] = v & 0x3f; if (!this.tc0690) this.mirror = v & 0x40 ? MIRROR.H : MIRROR.V; break;
        case 0x8001: this.p[1] = v & 0x3f; break;
        case 0x8002: this.c[0] = v; break;
        case 0x8003: this.c[1] = v; break;
        case 0xa000: case 0xa001: case 0xa002: case 0xa003: this.c[2 + (a & 3)] = v; break;
        case 0xc000: if (this.tc0690) this.irqLatch = v ^ 0xff; break;
        case 0xc001: if (this.tc0690) { this.irqCounter = 0; this.irqReload = true; } break;
        case 0xc002: if (this.tc0690) this.irqEnable = true; break;
        case 0xc003: if (this.tc0690) { this.irqEnable = false; this.irq = false; } break;
        case 0xe000: if (this.tc0690) this.mirror = v & 0x40 ? MIRROR.H : MIRROR.V; break;
      }
      this.update();
    }
    update() {
      if (!this.c) return; // called by the MMC3 constructor before our registers exist
      this.setPrg8(0, this.p[0]); this.setPrg8(1, this.p[1]); this.setPrg8(2, -2); this.setPrg8(3, -1);
      this.setChr2(0, this.c[0]); this.setChr2(1, this.c[1]);
      for (let i = 0; i < 4; i++) this.setChr1(4 + i, this.c[2 + i]);
    }
    ppuRead(a) { if (this.tc0690) this.ppuAddr(a); return Mapper.prototype.ppuRead.call(this, a); }
    regs() { return null; }
    afterLoad() { this.update(); }
  }

  // 80 — Taito X1-005 (Minelvaton Saga, Kyonshiizu 2); 207 — the same board with
  // nametables picked by CHR bank bit 7 (Fudou Myouou Den)
  class M80 extends Mapper {
    constructor(cart, alt) {
      super(cart); this.alt = alt; this.ram = new Uint8Array(128); this.ramOn = false; this.c = new Uint8Array(6); this.p = [0, 1, 0xfe];
      this.batteryData = () => this.ram; this.loadBattery = (d) => this.ram.set(d.subarray(0, 128));
      this.update();
    }
    cpuRead(a) {
      if (a >= 0x7f00 && a < 0x8000) return this.ramOn ? this.ram[a & 0x7f] : -1;
      if (a < 0x8000) return -1;
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a >= 0x7f00 && a < 0x8000) { if (this.ramOn) this.ram[a & 0x7f] = v; return; }
      if (a < 0x7ef0 || a > 0x7eff) return;
      const r = a & 0x0f;
      if (r < 6) this.c[r] = v;
      else if (r < 8) { if (!this.alt) this.mirror = v & 1 ? MIRROR.V : MIRROR.H; }
      else if (r < 10) this.ramOn = v === 0xa3;
      else this.p[(r - 10) >> 1] = v;
      this.update();
    }
    update() {
      const c = this.c;
      this.setChr2(0, (c[0] & 0x7f) >> 1); this.setChr2(1, (c[1] & 0x7f) >> 1);
      for (let i = 0; i < 4; i++) this.setChr1(4 + i, c[2 + i]);
      this.setPrg8(0, this.p[0]); this.setPrg8(1, this.p[1]); this.setPrg8(2, this.p[2]); this.setPrg8(3, -1);
      if (this.alt) this.setNt(c[0] >> 7, c[0] >> 7, c[1] >> 7, c[1] >> 7);
    }
    afterLoad() { this.update(); }
  }

  // 82 — Taito X1-017 (SD Keiji Blader, Kyuukyoku Harikiri Stadium)
  class M82 extends Mapper {
    constructor(cart) {
      super(cart); this.c = new Uint8Array(6); this.p = [0, 1, 2]; this.inv = 0; this.ramOn = [0, 0, 0];
      this.update();
    }
    ramSlot(a) { return a < 0x6800 ? 0 : a < 0x7000 ? 1 : a < 0x7400 ? 2 : -1; }
    cpuRead(a) {
      if (a >= 0x6000 && a < 0x8000) { const s = this.ramSlot(a); return s >= 0 && this.ramOn[s] ? this.prgRam[a - 0x6000] : -1; }
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x7ef0) { const s = this.ramSlot(a); if (s >= 0 && this.ramOn[s]) this.prgRam[a - 0x6000] = v; return; }
      if (a < 0x7ef0 || a > 0x7eff) return;
      const r = a & 0x0f;
      if (r < 6) this.c[r] = v;
      else if (r === 6) { this.mirror = v & 1 ? MIRROR.V : MIRROR.H; this.inv = (v >> 1) & 1; }
      else if (r === 7) this.ramOn[0] = v === 0xca ? 1 : 0;
      else if (r === 8) this.ramOn[1] = v === 0x69 ? 1 : 0;
      else if (r === 9) this.ramOn[2] = v === 0x84 ? 1 : 0;
      else if (r < 13) this.p[r - 10] = v >> 2;
      this.update();
    }
    update() {
      const c = this.c, x = this.inv ? 4 : 0;
      this.setChr1(0 ^ x, c[0] & 0xfe); this.setChr1(1 ^ x, c[0] | 1); this.setChr1(2 ^ x, c[1] & 0xfe); this.setChr1(3 ^ x, c[1] | 1);
      for (let i = 0; i < 4; i++) this.setChr1((4 + i) ^ x, c[2 + i]);
      this.setPrg8(0, this.p[0]); this.setPrg8(1, this.p[1]); this.setPrg8(2, this.p[2]); this.setPrg8(3, -1);
    }
    afterLoad() { this.update(); }
  }

  // ===================================================================
  // Sunsoft
  // ===================================================================

  // 67 — Sunsoft-3 (Fantasy Zone II, Mito Koumon)
  class M67 extends Mapper {
    constructor(cart) { super(cart); this.counter = 0; this.irqOn = false; this.toggle = 0; this.setPrg16(1, -1); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      switch (a & 0xf800) {
        case 0x8800: this.setChr2(0, v); break;
        case 0x9800: this.setChr2(1, v); break;
        case 0xa800: this.setChr2(2, v); break;
        case 0xb800: this.setChr2(3, v); break;
        case 0xc800: if (this.toggle ^= 1) this.counter = (this.counter & 0xff) | (v << 8); else this.counter = (this.counter & 0xff00) | v; break;
        case 0xd800: this.irqOn = !!(v & 0x10); this.toggle = 0; this.irq = false; break;
        case 0xe800: this.mirror = VH1[v & 3]; break;
        case 0xf800: this.setPrg16(0, v); break;
      }
    }
    cpuClock() {
      if (!this.irqOn) return;
      if (this.counter === 0) { this.counter = 0xffff; this.irq = true; this.irqOn = false; } else this.counter--;
    }
  }

  // 68 — Sunsoft-4 (After Burner, Maharaja): nametables can come from CHR ROM
  class M68 extends Mapper {
    constructor(cart) {
      super(cart); this.ciram = cart.vram; this.nt = [0x80, 0x80]; this.ntRom = false; this.ramOn = false;
      this.setPrg16(1, -1);
    }
    cpuRead(a) { if (a >= 0x6000 && a < 0x8000 && !this.ramOn) return -1; return super.cpuRead(a); }
    cpuWrite(a, v) {
      if (a < 0x8000) { if (this.ramOn) super.cpuWrite(a, v); return; }
      switch (a & 0xf000) {
        case 0x8000: this.setChr2(0, v); break;
        case 0x9000: this.setChr2(1, v); break;
        case 0xa000: this.setChr2(2, v); break;
        case 0xb000: this.setChr2(3, v); break;
        case 0xc000: this.nt[0] = v | 0x80; break;
        case 0xd000: this.nt[1] = v | 0x80; break;
        case 0xe000: this.mirror = VH1[v & 3]; this.ntRom = !!(v & 0x10); break;
        case 0xf000: this.setPrg16(0, v & 0x0f); this.ramOn = !!(v & 0x10); break;
      }
    }
    ntRead(a) {
      const i = this.ntIndex(a);
      if (!this.ntRom) return this.ciram[i];
      return this.chr[((this.nt[i >> 10] * 0x400) + (a & 0x3ff)) % this.chr.length];
    }
    ntWrite(a, v) { if (!this.ntRom) this.ciram[this.ntIndex(a)] = v; }
  }

  // 89 — Sunsoft-2 on Sunsoft-3 board (Tenka no Goikenban); 93 — Sunsoft-2 (Fantasy Zone, Shanghai)
  class M89 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(1, -1); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      v &= this.cpuRead(a);
      this.setPrg16(0, (v >> 4) & 7); this.setChr8((v & 7) | ((v & 0x80) >> 4));
      this.mirror = v & 8 ? MIRROR.ONE_HI : MIRROR.ONE_LO;
    }
  }
  class M93 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(1, -1); this.chrOn = true; }
    cpuWrite(a, v) { if (a < 0x8000) return super.cpuWrite(a, v); v &= this.cpuRead(a); this.setPrg16(0, (v >> 4) & 7); this.chrOn = !!(v & 1); }
    ppuRead(a) { return this.chrOn ? super.ppuRead(a) : 0; }
    ppuWrite(a, v) { if (this.chrOn) super.ppuWrite(a, v); }
  }

  // 184 — Sunsoft-1 (Atlantis no Nazo, Wing of Madoola)
  class M184 extends Mapper {
    cpuWrite(a, v) { if (a >= 0x6000 && a < 0x8000) { this.setChr4(0, v & 7); this.setChr4(1, ((v >> 4) & 7) | 4); } }
  }

  // ===================================================================
  // Nintendo boards built on the MMC1 / MMC3
  // ===================================================================

  // 105 — NES-EVENT (Nintendo World Championships 1990): MMC1 plus a countdown timer
  class M105 extends M1 {
    constructor(cart) { super(cart); this.started = 0; this.timer = 0; this.limit = 0x20000000 | (4 << 25); this.update(); }
    update() {
      if (this.started === undefined) return super.update();
      // The CHR register's I bit holds the timer in reset; the game toggles it once to unlock banking.
      const ibit = this.chr0 & 0x10;
      if (this.started === 0 && ibit) this.started = 1;
      else if (this.started === 1 && !ibit) this.started = 2;
      this.mirror = [MIRROR.ONE_LO, MIRROR.ONE_HI, MIRROR.V, MIRROR.H][this.ctrl & 3];
      if (this.started < 2) this.setPrg32(0);
      else if (!(this.chr0 & 8)) this.setPrg32((this.chr0 >> 1) & 3);
      else {
        const p = this.prgReg & 7;
        switch ((this.ctrl >> 2) & 3) {
          case 0: case 1: this.setPrg32(4 + (p >> 1)); break;
          case 2: this.setPrg16(0, 8); this.setPrg16(1, 8 | p); break;
          case 3: this.setPrg16(0, 8 | p); this.setPrg16(1, 15); break;
        }
      }
      this.setChr8(0);
    }
    cpuClock() {
      if (this.chr0 & 0x10) { this.timer = 0; this.irq = false; }
      else if (this.started === 2 && ++this.timer === this.limit) this.irq = true;
    }
  }

  // 155 — MMC1A (its RAM can't be switched off; same as MMC1 here)
  // 47 — NES-QJ (Super Spike V'Ball + Nintendo World Cup); 37 — PAL-ZZ (Super Mario Bros. + Tetris + Nintendo World Cup)
  class M47 extends M4 {
    constructor(cart, zz) { super(cart); this.zz = zz; this.outer = 0; this.update(); }
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x8000) { this.outer = v & 7; this.update(); return; }
      super.cpuWrite(a, v);
    }
    setPrg8(slot, bank) {
      if (this.outer === undefined) return super.setPrg8(slot, bank);
      if (!this.zz) return super.setPrg8(slot, (bank & 0x0f) | ((this.outer & 1) << 4));
      const q = this.outer;
      const base = (q & 4 ? 0x10 : 0) | ((q & 3) === 3 ? 0x08 : 0);
      super.setPrg8(slot, base | (bank & 7));
    }
    setChr1(slot, bank) {
      if (this.outer === undefined) return super.setChr1(slot, bank);
      super.setChr1(slot, (bank & 0x7f) | ((this.zz ? (this.outer >> 2) & 1 : this.outer & 1) << 7));
    }
  }

  // 118 — TxSROM (Armadillo, NES Play Action Football): CHR bank bit 7 picks each nametable
  class M118 extends M4 {
    update() {
      super.update();
      const r = this.r, inv = this.bankSel & 0x80;
      // The bank register that covers each of the first four 1 KB CHR windows decides that nametable.
      const reg = inv ? [r[2], r[3], r[4], r[5]] : [r[0], r[0], r[1], r[1]];
      this.setNt(reg[0] >> 7, reg[1] >> 7, reg[2] >> 7, reg[3] >> 7);
    }
    cpuWrite(a, v) { if ((a & 0xe001) === 0xa000) return; super.cpuWrite(a, v); }
  }

  // 119 — TQROM (Pinbot, High Speed): CHR ROM plus 8 KB CHR RAM, picked by bank bit 6
  class M119 extends M4 {
    constructor(cart) { super(cart); this.vram8 = new Uint8Array(0x2000); this.ramSlot = new Uint8Array(8); this.update(); }
    setChr1(slot, bank) {
      if (this.ramSlot && (bank & 0x40)) { this.ramSlot[slot] = 1; this.chrMap[slot] = (bank & 7) * 0x400; return; }
      if (this.ramSlot) this.ramSlot[slot] = 0;
      super.setChr1(slot, bank);
    }
    ppuRead(a) {
      this.ppuAddr(a);
      const s = a >> 10;
      return this.ramSlot[s] ? this.vram8[this.chrMap[s] + (a & 0x3ff)] : this.chr[this.chrMap[s] + (a & 0x3ff)];
    }
    ppuWrite(a, v) { const s = a >> 10; if (this.ramSlot[s]) this.vram8[this.chrMap[s] + (a & 0x3ff)] = v; }
  }

  // 64 — Tengen RAMBO-1 (Klax, Skull & Crossbones, Shinobi)
  class M64 extends M4 {
    constructor(cart) {
      super(cart); this.r16 = new Uint8Array(16); this.r16.set([0, 2, 4, 5, 6, 7, 0, 1, 1, 3, 0, 0, 0, 0, 0, 2]);
      this.cycleMode = 0; this.prescaler = 0; this.update();
    }
    cpuWrite(a, v) {
      if (a < 0x8000) return Mapper.prototype.cpuWrite.call(this, a, v);
      const even = !(a & 1);
      switch (a & 0xe000) {
        case 0x8000: if (even) this.bankSel = v; else this.r16[this.bankSel & 15] = v; this.update(); break;
        case 0xa000: if (even && this.mirror !== MIRROR.FOUR) this.mirror = v & 1 ? MIRROR.H : MIRROR.V; break;
        case 0xc000:
          if (even) this.irqLatch = v;
          else { this.cycleMode = v & 1; this.irqReload = true; this.irqCounter = 0; this.prescaler = 0; }
          break;
        case 0xe000: if (even) { this.irqEnable = false; this.irq = false; } else this.irqEnable = true; break;
      }
    }
    update() {
      const r = this.r16; if (!r) return;
      const s = this.bankSel;
      if (s & 0x40) { this.setPrg8(0, r[15]); this.setPrg8(1, r[6]); this.setPrg8(2, r[7]); }
      else { this.setPrg8(0, r[6]); this.setPrg8(1, r[7]); this.setPrg8(2, r[15]); }
      this.setPrg8(3, -1);
      const x = s & 0x80 ? 4 : 0;
      if (s & 0x20) { this.setChr1(0 ^ x, r[0]); this.setChr1(1 ^ x, r[8]); this.setChr1(2 ^ x, r[1]); this.setChr1(3 ^ x, r[9]); }
      else { this.setChr1(0 ^ x, r[0] & 0xfe); this.setChr1(1 ^ x, r[0] | 1); this.setChr1(2 ^ x, r[1] & 0xfe); this.setChr1(3 ^ x, r[1] | 1); }
      for (let i = 0; i < 4; i++) this.setChr1((4 + i) ^ x, r[2 + i]);
    }
    // A forced reload ($C001) loads one more than the latch, so the IRQ comes a line later than on the MMC3.
    clockIrq() {
      if (this.irqReload) { this.irqCounter = this.irqLatch + (this.irqLatch ? 1 : 0); this.irqReload = false; }
      else if (this.irqCounter === 0) this.irqCounter = this.irqLatch;
      else this.irqCounter--;
      if (this.irqCounter === 0 && this.irqEnable) this.irq = true;
    }
    ppuRead(a) { if (!this.cycleMode) this.ppuAddr(a); return Mapper.prototype.ppuRead.call(this, a); }
    cpuClock() { if (this.cycleMode && ++this.prescaler >= 4) { this.prescaler = 0; this.clockIrq(); } }
    regs() { return null; }
    afterLoad() { this.update(); }
  }

  // 245 — Waixing MMC3 with an outer PRG bit in CHR register 0 and CHR RAM
  class M245 extends M4 {
    update() {
      if (this.chrRam) this.setChr8(0); else super.update();
      const outer = (this.r[0] & 2) << 5;
      const r = this.r;
      if (this.bankSel & 0x40) { this.setPrg8(0, outer | 0x3e); this.setPrg8(2, outer | (r[6] & 0x3f)); }
      else { this.setPrg8(0, outer | (r[6] & 0x3f)); this.setPrg8(2, outer | 0x3e); }
      this.setPrg8(1, outer | (r[7] & 0x3f)); this.setPrg8(3, outer | 0x3f);
    }
  }

  // ===================================================================
  // Small discrete-logic boards (licensed and unlicensed)
  // ===================================================================

  // 15 — K-1029 / K-1030P 100-in-1 Contra Function 16
  class M15 extends Mapper {
    constructor(cart) { super(cart); this.write(0x8000, 0); }
    cpuWrite(a, v) { if (a >= 0x8000) this.write(a, v); else super.cpuWrite(a, v); }
    write(a, v) {
      const b = v & 0x3f, s = v >> 7;
      this.mirror = v & 0x40 ? MIRROR.H : MIRROR.V;
      switch (a & 3) {
        case 0: this.setPrg16(0, b & ~1); this.setPrg16(1, b | 1); break;
        case 1: this.setPrg16(0, b); this.setPrg16(1, b | 7); break;
        case 2: for (let i = 0; i < 4; i++) this.setPrg8(i, b * 2 + s); break;
        case 3: this.setPrg16(0, b); this.setPrg16(1, b); break;
      }
      this.chrProtect = (a & 3) === 0 || (a & 3) === 3;
    }
    ppuWrite(a, v) { if (!this.chrProtect) super.ppuWrite(a, v); }
  }

  // 28 — Action 53 (homebrew multicart)
  class M28 extends Mapper {
    constructor(cart) { super(cart); this.sel = 0; this.chrReg = 0; this.inner = 0; this.mode = 0; this.outer = 0xff; this.update(); }
    cpuWrite(a, v) {
      if (a >= 0x5000 && a < 0x6000) { this.sel = ((v >> 6) & 2) | (v & 1); return; }
      if (a < 0x8000) return super.cpuWrite(a, v);
      switch (this.sel) {
        case 0: this.chrReg = v & 3; if (!(this.mode & 2)) this.mode = (this.mode & ~1) | ((v >> 4) & 1); break;
        case 1: this.inner = v & 0x0f; if (!(this.mode & 2)) this.mode = (this.mode & ~1) | ((v >> 4) & 1); break;
        case 2: this.mode = v & 0x3f; break;
        case 3: this.outer = v; break;
      }
      this.update();
    }
    bankFor(half) {
      const pmode = (this.mode >> 2) & 3, size = (this.mode >> 4) & 3, mask = (2 << size) - 1;
      const outer = this.outer << 1;
      if (pmode < 2) return (outer & ~mask) | (((this.inner << 1) | half) & mask);
      const fixedHalf = pmode === 2 ? 0 : 1;
      if (half === fixedHalf) return outer | half;
      return (outer & ~mask) | (this.inner & mask);
    }
    update() {
      this.setPrg16(0, this.bankFor(0)); this.setPrg16(1, this.bankFor(1));
      this.setChr8(this.chrReg);
      this.mirror = [MIRROR.ONE_LO, MIRROR.ONE_HI, MIRROR.V, MIRROR.H][this.mode & 3];
    }
    afterLoad() { this.update(); }
  }

  // 30 — UNROM 512 (homebrew: Black Box Challenge, Mega Man fan games)
  class M30 extends Mapper {
    constructor(cart) {
      super(cart);
      const f = cart.flags6 & 9;
      this.oneScreen = f === 8; if (f === 9 || f === 8) this.mirror = f === 9 ? MIRROR.FOUR : MIRROR.ONE_LO;
      this.setPrg16(1, -1);
    }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      if (!this.cart.battery) v &= this.cpuRead(a);
      this.setPrg16(0, v & 0x1f); this.setChr8((v >> 5) & 3);
      if (this.oneScreen) this.mirror = v & 0x80 ? MIRROR.ONE_HI : MIRROR.ONE_LO;
    }
  }

  // 38 — Bit Corp (Crime Busters)
  class M38 extends Mapper { cpuWrite(a, v) { if (a >= 0x7000 && a < 0x8000) { this.setPrg32(v & 3); this.setChr8((v >> 2) & 3); } } }

  // 41 — Caltron 6-in-1
  class M41 extends Mapper {
    constructor(cart) { super(cart); this.outer = 0; this.inner = 0; }
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x6800) {
        this.outer = a & 0x3f; this.setPrg32(a & 7); this.mirror = a & 0x20 ? MIRROR.H : MIRROR.V;
      } else if (a >= 0x8000 && (this.outer & 4)) this.inner = v & 3 & this.cpuRead(a);
      else return;
      this.setChr8(((this.outer >> 1) & 0x0c) | this.inner);
    }
  }

  // 79 / 146 — AVE NINA-03/06 and Sachen 3015; 113 — the multicart version
  class M79 extends Mapper {
    constructor(cart, multi) { super(cart); this.multi = multi; }
    cpuWrite(a, v) {
      if ((a & 0xe100) !== 0x4100) return super.cpuWrite(a, v);
      if (this.multi) { this.setPrg32((v >> 3) & 7); this.setChr8((v & 7) | ((v >> 3) & 8)); this.mirror = v & 0x80 ? MIRROR.V : MIRROR.H; }
      else { this.setPrg32((v >> 3) & 1); this.setChr8(v & 7); }
    }
  }

  // 94 — UN1ROM (Senjou no Ookami)
  class M94 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(1, -1); }
    cpuWrite(a, v) { if (a >= 0x8000) this.setPrg16(0, ((v & this.cpuRead(a)) >> 2) & 7); else super.cpuWrite(a, v); }
  }

  // 107 — Magic Dragon
  class M107 extends Mapper { cpuWrite(a, v) { if (a >= 0x8000) { this.setPrg32(v >> 1); this.setChr8(v); } else super.cpuWrite(a, v); } }

  // 111 — GTROM / Cheapocabra (homebrew): CHR RAM and nametable RAM banks on the cartridge
  class M111 extends Mapper {
    constructor(cart) {
      super(cart); this.ntRam = new Uint8Array(0x2000); this.ntPage = 0;
      if (this.chr.length < 0x4000) { this.chr = new Uint8Array(0x4000); this.chrRam = true; }
      this.setPrg32(0); this.setChr8(0);
    }
    cpuWrite(a, v) {
      if ((a >= 0x5000 && a < 0x6000) || (a >= 0x7000 && a < 0x8000)) { this.setPrg32(v & 0x0f); this.setChr8((v >> 4) & 1); this.ntPage = (v >> 5) & 1; }
    }
    ntRead(a) { return this.ntRam[(this.ntPage << 12) | (a & 0xfff)]; }
    ntWrite(a, v) { this.ntRam[(this.ntPage << 12) | (a & 0xfff)] = v; }
  }

  // 112 — Asder / NTDEC (Huang Di, Master Shooter)
  class M112 extends Mapper {
    constructor(cart) { super(cart); this.sel = 0; this.r = new Uint8Array(8); this.update(); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      switch (a & 0xe000) {
        case 0x8000: this.sel = v & 7; break;
        case 0xa000: this.r[this.sel] = v; break;
        case 0xe000: this.mirror = v & 1 ? MIRROR.H : MIRROR.V; break;
      }
      this.update();
    }
    update() {
      const r = this.r;
      this.setPrg8(0, r[0]); this.setPrg8(1, r[1]); this.setPrg8(2, -2); this.setPrg8(3, -1);
      this.setChr2(0, r[2] >> 1); this.setChr2(1, r[3] >> 1);
      for (let i = 0; i < 4; i++) this.setChr1(4 + i, r[4 + i]);
    }
    afterLoad() { this.update(); }
  }

  // 132 — TXC 22211A
  class M132 extends Mapper {
    constructor(cart) { super(cart); this.r = new Uint8Array(4); }
    cpuRead(a) {
      if ((a & 0xe103) === 0x4100) return (openBus(this) & 0xf0) | ((this.r[1] ^ this.r[2]) & 0x0f) | 0x40;
      return super.cpuRead(a);
    }
    cpuWrite(a, v) {
      if (a >= 0x4100 && a < 0x6000) { if ((a & 0xe100) === 0x4100) this.r[a & 3] = v; return; }
      if (a >= 0x8000) { this.setPrg32((this.r[2] >> 2) & 1); this.setChr8(this.r[2] & 3); return; }
      super.cpuWrite(a, v);
    }
  }

  // 133 — Sachen 3009 / SA-72008; 145 — Sachen SA-72007; 147 — Sachen 3018; 148 — Sachen SA-0037; 149 — Sachen SA-0036
  class M133 extends Mapper { cpuWrite(a, v) { if (a >= 0x4100 && a < 0x6000 && (a & 0x4100) === 0x4100) { this.setPrg32((v >> 2) & 1); this.setChr8(v & 3); } else super.cpuWrite(a, v); } }
  class M145 extends Mapper { cpuWrite(a, v) { if (a >= 0x4100 && a < 0x6000 && (a & 0x4100) === 0x4100) this.setChr8(v >> 7); else super.cpuWrite(a, v); } }
  class M147 extends Mapper {
    cpuWrite(a, v) {
      if (a >= 0x4100 && a < 0x6000 && (a & 0x4103) === 0x4102) { this.setPrg32(((v >> 2) & 1) | ((v >> 6) & 2)); this.setChr8((v >> 3) & 0x0f); }
      else if (a >= 0x8000) { this.setPrg32(((v >> 2) & 1) | ((v >> 6) & 2)); this.setChr8((v >> 3) & 0x0f); }
      else super.cpuWrite(a, v);
    }
  }
  class M148 extends Mapper { cpuWrite(a, v) { if (a >= 0x8000) { v &= this.cpuRead(a); this.setPrg32((v >> 3) & 1); this.setChr8(v & 7); } else super.cpuWrite(a, v); } }
  class M149 extends Mapper { cpuWrite(a, v) { if (a >= 0x8000) this.setChr8((v & this.cpuRead(a)) >> 7); else super.cpuWrite(a, v); } }

  // 143 — Sachen NROM with a copy-protection read at $4100
  class M143 extends Mapper {
    cpuRead(a) { if (a >= 0x4100 && a < 0x6000 && (a & 0x4100) === 0x4100) return (~a & 0x3f) | 0x40; return super.cpuRead(a); }
  }

  // 137-139, 141 — Sachen 8259 A/B/C/D and 8259D
  class M8259 extends Mapper {
    constructor(cart, kind) { super(cart); this.kind = kind; this.sel = 0; this.r = new Uint8Array(8); this.update(); }
    cpuWrite(a, v) {
      if (a >= 0x4100 && a < 0x6000 && (a & 0x4101) === 0x4100) { this.sel = v & 7; return; }
      if (a >= 0x4100 && a < 0x6000 && (a & 0x4101) === 0x4101) { this.r[this.sel] = v & 7; this.update(); return; }
      super.cpuWrite(a, v);
    }
    update() {
      const r = this.r, simple = r[7] & 1;
      this.setPrg32(r[5] & 7);
      if (this.chrRam) this.setChr8(0);
      else if (this.kind === 137) { // 1 KB windows at $0000, last 4 KB fixed at $1000
        this.setChr1(0, r[0]); this.setChr1(1, r[1] | ((r[4] & 1) << 4)); this.setChr1(2, r[2] | ((r[4] & 2) << 3));
        this.setChr1(3, r[3] | ((r[4] & 4) << 2) | ((r[6] & 1) << 3));
        for (let i = 4; i < 8; i++) this.setChr1(i, i - 8);
      } else {
        for (let i = 0; i < 4; i++) {
          const b = ((simple ? r[0] : r[i]) & 7) | ((r[4] & 7) << 3);
          this.setChr2(i, this.kind === 141 ? (b << 1) | (i & 1) : this.kind === 139 ? (b << 2) | (i & 3) : b);
        }
      }
      if (simple) this.mirror = MIRROR.V;
      else { const m = (r[7] >> 1) & 3; if (m === 2) this.setNt(0, 1, 1, 1); else this.mirror = [MIRROR.V, MIRROR.H, 0, MIRROR.ONE_LO][m]; }
    }
    afterLoad() { this.update(); }
  }

  // 168 — Racermate (64 KB CHR RAM)
  class M168 extends Mapper {
    constructor(cart) {
      super(cart); if (this.chr.length < 0x10000) { this.chr = new Uint8Array(0x10000); this.chrRam = true; }
      this.setPrg16(1, -1); this.setChr4(0, 0); this.setChr4(1, 0);
    }
    cpuWrite(a, v) { if (a >= 0x8000) { this.setPrg16(0, v >> 6); this.setChr4(1, v & 0x0f); } else super.cpuWrite(a, v); }
  }

  // 171 — Kaiser KS-7058 (Tui Do Woo Ma Jeung)
  class M171 extends Mapper {
    cpuWrite(a, v) { if ((a & 0xf080) === 0xf000) this.setChr4(0, v); else if ((a & 0xf080) === 0xf080) this.setChr4(1, v); else super.cpuWrite(a, v); }
  }

  // 185 — CNROM with copy protection: the wrong value disables CHR ROM
  class M185 extends Mapper {
    constructor(cart) { super(cart); this.on = true; this.sub = cart.submapper; }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      v &= this.cpuRead(a);
      if (this.sub >= 4) this.on = (v & 3) === (this.sub & 3);
      else this.on = (v & 0x0f) !== 0 && v !== 0x13;
    }
    ppuRead(a) { return this.on ? super.ppuRead(a) : 0xff; }
  }

  // 190 — Magic Kid GooGoo
  class M190 extends Mapper {
    constructor(cart) { super(cart); this.setPrg16(0, 0); this.setPrg16(1, 0); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      if (a < 0xa000) this.setPrg16(0, v & 7);
      else if (a < 0xc000) this.setChr2(a & 3, v);
      else if (a < 0xe000) this.setPrg16(0, (v & 7) | 8);
    }
  }

  // 193 — NTDEC TC-112 (Fighting Hero)
  class M193 extends Mapper {
    constructor(cart) { super(cart); this.setPrg8(1, -3); this.setPrg8(2, -2); this.setPrg8(3, -1); }
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x8000) {
        switch (a & 3) {
          case 0: this.setChr4(0, v >> 2); break;
          case 1: this.setChr2(2, v >> 1); break;
          case 2: this.setChr2(3, v >> 1); break;
          case 3: this.setPrg8(0, v); break;
        }
      }
    }
  }

  // 156 — Daou 306 (Metal Force, Buzz & Waldog)
  class M156 extends Mapper {
    constructor(cart) { super(cart); this.lo = new Uint8Array(8); this.hi = new Uint8Array(8); this.mirror = MIRROR.ONE_LO; this.setPrg16(1, -1); }
    cpuWrite(a, v) {
      if (a < 0xc000) return super.cpuWrite(a, v);
      if (a >= 0xc000 && a <= 0xc00f) {
        const r = a & 0x0f, slot = (r & 3) | (r & 8 ? 4 : 0);
        if (r & 4) this.hi[slot] = v; else this.lo[slot] = v;
        this.setChr1(slot, this.lo[slot] | (this.hi[slot] << 8));
      } else if (a === 0xc010) this.setPrg16(0, v);
      else if (a === 0xc014) this.mirror = v & 1 ? MIRROR.H : MIRROR.V;
    }
  }

  // 166 / 167 — Subor educational computer cartridges
  class M166 extends Mapper {
    constructor(cart, alt) { super(cart); this.alt = alt; this.r = new Uint8Array(4); this.update(); }
    cpuWrite(a, v) { if (a >= 0x8000) { this.r[(a >> 13) & 3] = v; this.update(); } else super.cpuWrite(a, v); }
    update() {
      const r = this.r, base = ((r[0] ^ r[1]) & 0x10) << 1, bank = (r[2] ^ r[3]) & 0x1f;
      if (r[1] & 8) {
        const b = (base + bank) & ~1;
        if (this.alt) { this.setPrg16(0, b | 1); this.setPrg16(1, b); } else { this.setPrg16(0, b); this.setPrg16(1, b | 1); }
      } else if (r[1] & 4) { this.setPrg16(0, 0x1f); this.setPrg16(1, base + bank); }
      else { this.setPrg16(0, base + bank); this.setPrg16(1, this.alt ? 0x20 : 0x07); }
    }
    afterLoad() { this.update(); }
  }

  // ===================================================================
  // Multicarts
  // ===================================================================

  // 200 — 1200-in-1 style: address lines pick a mirrored 16 KB bank
  class M200 extends Mapper {
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      this.setPrg16(0, a & 7); this.setPrg16(1, a & 7); this.setChr8(a & 7); this.mirror = a & 8 ? MIRROR.H : MIRROR.V;
    }
  }
  // 201 — 21-in-1
  class M201 extends Mapper { cpuWrite(a, v) { if (a >= 0x8000) { this.setPrg32(a & 0xff); this.setChr8(a & 0xff); } else super.cpuWrite(a, v); } }
  // 202 — 150-in-1
  class M202 extends Mapper {
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      const b = (a >> 1) & 7;
      if ((a & 9) === 9) this.setPrg32(b >> 1); else { this.setPrg16(0, b); this.setPrg16(1, b); }
      this.setChr8(b); this.mirror = a & 1 ? MIRROR.H : MIRROR.V;
    }
  }
  // 203 — 35-in-1
  class M203 extends Mapper {
    cpuWrite(a, v) { if (a < 0x8000) return super.cpuWrite(a, v); this.setPrg16(0, v >> 2); this.setPrg16(1, v >> 2); this.setChr8(v & 3); }
  }

  // 225 / 255 — 52-in-1, 64-in-1, 110-in-1
  class M225 extends Mapper {
    constructor(cart) { super(cart); this.nib = new Uint8Array(4); this.setPrg32(0); }
    cpuRead(a) { if (a >= 0x5800 && a < 0x6000) return (openBus(this) & 0xf0) | this.nib[a & 3]; return super.cpuRead(a); }
    cpuWrite(a, v) {
      if (a >= 0x5800 && a < 0x6000) { this.nib[a & 3] = v & 0x0f; return; }
      if (a < 0x8000) return;
      const hi = (a >> 8) & 0x40, p = ((a >> 6) & 0x3f) | hi;
      if (a & 0x1000) { this.setPrg16(0, p); this.setPrg16(1, p); } else this.setPrg32(p >> 1);
      this.setChr8((a & 0x3f) | hi); this.mirror = a & 0x2000 ? MIRROR.H : MIRROR.V;
    }
  }

  // 226 — 76-in-1, Super 42-in-1
  class M226 extends Mapper {
    constructor(cart) { super(cart); this.r = [0, 0]; this.update(); }
    cpuWrite(a, v) { if (a >= 0x8000) { this.r[a & 1] = v; this.update(); } else super.cpuWrite(a, v); }
    update() {
      const r = this.r, p = (r[0] & 0x1f) | ((r[0] & 0x80) >> 2) | ((r[1] & 1) << 6);
      if (r[0] & 0x40) { this.setPrg16(0, p); this.setPrg16(1, p); } else this.setPrg32(p >> 1);
      this.mirror = r[0] & 0x40 ? (r[0] & 0x20 ? MIRROR.V : MIRROR.H) : (r[0] & 0x20 ? MIRROR.V : MIRROR.H);
    }
    afterLoad() { this.update(); }
  }

  // 227 — 1200-in-1
  class M227 extends Mapper {
    constructor(cart) { super(cart); this.write(0x8000); }
    cpuWrite(a, v) { if (a >= 0x8000) this.write(a); else super.cpuWrite(a, v); }
    write(a) {
      const p = ((a >> 2) & 0x1f) | ((a >> 3) & 0x20), s = a & 1, last = a & 0x80, l = a & 0x200;
      if (last) { if (s) this.setPrg32(p >> 1); else { this.setPrg16(0, p); this.setPrg16(1, p); } }
      else {
        this.setPrg16(0, s ? p & ~1 : p);
        this.setPrg16(1, l ? p | 7 : p & 0x38);
      }
      this.mirror = a & 2 ? MIRROR.H : MIRROR.V;
    }
  }

  // 228 — Active Enterprises (Action 52, Cheetahmen II)
  class M228 extends Mapper {
    constructor(cart) { super(cart); this.nib = new Uint8Array(4); this.write(0x8000, 0); }
    cpuRead(a) { if (a >= 0x5ff0 && a < 0x6000) return this.nib[a & 3]; return super.cpuRead(a); }
    cpuWrite(a, v) {
      if (a >= 0x5ff0 && a < 0x6000) { this.nib[a & 3] = v & 0x0f; return; }
      if (a >= 0x8000) this.write(a, v);
    }
    write(a, v) {
      let chip = (a >> 11) & 3; if (chip === 3) chip = 2;
      const p = (chip << 5) | ((a >> 6) & 0x1f);
      if (a & 0x20) { this.setPrg16(0, p); this.setPrg16(1, p); } else this.setPrg32(p >> 1);
      this.setChr8(((a & 0x0f) << 2) | (v & 3)); this.mirror = a & 0x2000 ? MIRROR.H : MIRROR.V;
    }
  }

  // 229 — 31-in-1
  class M229 extends Mapper {
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      if ((a & 0x1e) === 0) this.setPrg32(0); else { this.setPrg16(0, a & 0x1f); this.setPrg16(1, a & 0x1f); }
      this.setChr8(a & 0xff); this.mirror = a & 0x20 ? MIRROR.H : MIRROR.V;
    }
  }

  // 230 — 22-in-1 (Contra on one side of a reset switch, a menu of games on the other)
  class M230 extends Mapper {
    constructor(cart) { super(cart); this.multi = 0; this.reg = 0; this.update(); }
    reset() { this.multi ^= 1; this.reg = 0; this.update(); }
    cpuWrite(a, v) { if (a >= 0x8000) { this.reg = v; this.update(); } else super.cpuWrite(a, v); }
    update() {
      const v = this.reg;
      if (!this.multi) { this.setPrg16(0, v & 7); this.setPrg16(1, 7); this.mirror = MIRROR.V; }
      else {
        const p = (v & 0x1f) + 8;
        if (v & 0x20) { this.setPrg16(0, p); this.setPrg16(1, p); } else { this.setPrg16(0, p & ~1); this.setPrg16(1, p | 1); }
        this.mirror = v & 0x40 ? MIRROR.V : MIRROR.H;
      }
    }
    afterLoad() { this.update(); }
  }

  // 231 — 20-in-1
  class M231 extends Mapper {
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      const b = (a & 0x1e) | ((a >> 5) & 1);
      this.setPrg16(0, a & 0x1e); this.setPrg16(1, b); this.mirror = a & 0x80 ? MIRROR.H : MIRROR.V;
    }
  }

  // 232 — Camerica / Codemasters Quattro carts
  class M232 extends Mapper {
    constructor(cart) { super(cart); this.outer = 0; this.inner = 0; this.aladdin = cart.submapper === 1; this.update(); }
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      if (a < 0xc000) this.outer = this.aladdin ? ((v >> 4) & 1) | ((v >> 2) & 2) : (v >> 3) & 3;
      else this.inner = v & 3;
      this.update();
    }
    update() { this.setPrg16(0, (this.outer << 2) | this.inner); this.setPrg16(1, (this.outer << 2) | 3); }
    afterLoad() { this.update(); }
  }

  // 233 — 42-in-1 with reset switch
  class M233 extends Mapper {
    constructor(cart) { super(cart); this.outer = 0; this.reg = 0; this.update(); }
    reset() { this.outer ^= 0x20; this.update(); }
    cpuWrite(a, v) { if (a >= 0x8000) { this.reg = v; this.update(); } else super.cpuWrite(a, v); }
    update() {
      const v = this.reg, p = (v & 0x1f) | this.outer;
      if (v & 0x20) { this.setPrg16(0, p); this.setPrg16(1, p); } else this.setPrg32(p >> 1);
      this.mirror = [MIRROR.ONE_LO, MIRROR.V, MIRROR.H, MIRROR.ONE_HI][v >> 6];
    }
    afterLoad() { this.update(); }
  }

  // 234 — Maxi 15 (AVE D-1012): registers in ROM space, triggered by reads as well as writes
  class M234 extends Mapper {
    constructor(cart) { super(cart); this.r0 = 0; this.r1 = 0; this.update(); }
    touch(a, v) {
      if (a >= 0xff80 && a <= 0xff9f) { if (!(this.r0 & 0x3f)) { this.r0 = v; this.update(); } }
      else if (a >= 0xffe8 && a <= 0xfff7) { this.r1 = v; this.update(); }
    }
    cpuRead(a) { const v = super.cpuRead(a); if (a >= 0xff80) this.touch(a, v); return v; }
    cpuWrite(a, v) { if (a >= 0xff80) this.touch(a, v & super.cpuRead(a)); else super.cpuWrite(a, v); }
    update() {
      const r0 = this.r0, r1 = this.r1;
      if (r0 & 0x40) { this.setPrg32((r0 & 0x0e) | (r1 & 1)); this.setChr8(((r0 & 0x0e) << 2) | ((r1 >> 4) & 7)); }
      else { this.setPrg32(r0 & 0x0f); this.setChr8(((r0 & 0x0f) << 2) | ((r1 >> 4) & 3)); }
      this.mirror = r0 & 0x80 ? MIRROR.H : MIRROR.V;
    }
    afterLoad() { this.update(); }
  }

  // 235 — 150-in-1, 260-in-1
  class M235 extends Mapper {
    cpuWrite(a, v) {
      if (a < 0x8000) return super.cpuWrite(a, v);
      const p = (a & 0x1f) | ((a >> 3) & 0x60);
      if (a & 0x800) { const b = (p << 1) | ((a >> 12) & 1); this.setPrg16(0, b); this.setPrg16(1, b); } else this.setPrg32(p);
      this.mirror = a & 0x400 ? MIRROR.ONE_LO : (a & 0x2000 ? MIRROR.H : MIRROR.V);
    }
  }

  // 240 — C&E (Jing Ke Xin Zhuan); 241 — BNROM-like with RAM; 242 — Wai Xing Zhan Shi
  class M240 extends Mapper {
    cpuWrite(a, v) { if (a >= 0x4020 && a < 0x6000) { this.setPrg32(v >> 4); this.setChr8(v & 0x0f); } else super.cpuWrite(a, v); }
  }
  class M241 extends Mapper { cpuWrite(a, v) { if (a >= 0x8000) this.setPrg32(v); else super.cpuWrite(a, v); } }
  class M242 extends Mapper {
    cpuWrite(a, v) { if (a >= 0x8000) { this.setPrg32((a >> 3) & 0x0f); this.mirror = a & 2 ? MIRROR.H : MIRROR.V; } else super.cpuWrite(a, v); }
  }

  // 243 — Sachen SA-020A (Poker III)
  class M243 extends Mapper {
    constructor(cart) { super(cart); this.sel = 0; this.r = new Uint8Array(8); }
    cpuWrite(a, v) {
      if (a >= 0x4100 && a < 0x6000) {
        if ((a & 0xc101) === 0x4100) this.sel = v & 7;
        else if ((a & 0xc101) === 0x4101) {
          this.r[this.sel] = v & 7;
          const r = this.r;
          this.setPrg32(r[5] & 3);
          this.setChr8((r[2] & 1) | ((r[4] & 1) << 1) | ((r[6] & 3) << 2));
          this.mirror = [MIRROR.H, MIRROR.V, MIRROR.H, MIRROR.ONE_HI][(r[7] >> 1) & 3];
        }
        return;
      }
      super.cpuWrite(a, v);
    }
  }

  // 244 — C&E Decathlon: bank numbers are scrambled through fixed tables
  const P244 = [[0, 1, 2, 3], [3, 2, 1, 0], [0, 2, 1, 3], [3, 1, 2, 0]];
  const C244 = [[0, 1, 2, 3, 4, 5, 6, 7], [0, 2, 1, 3, 4, 6, 5, 7], [0, 1, 4, 5, 2, 3, 6, 7], [0, 4, 1, 5, 2, 6, 3, 7],
    [0, 4, 2, 6, 1, 5, 3, 7], [0, 2, 4, 6, 1, 3, 5, 7], [7, 6, 5, 4, 3, 2, 1, 0], [7, 6, 5, 4, 3, 2, 1, 0]];
  class M244 extends Mapper {
    cpuWrite(a, v) {
      if (a >= 0x8065 && a <= 0x80a4) { const o = a - 0x8065; this.setPrg32(P244[(o >> 4) & 3][o & 3]); }
      else if (a >= 0x80a5 && a <= 0x80e4) { const o = a - 0x80a5; this.setChr8(C244[(o >> 3) & 7][o & 7]); }
      else super.cpuWrite(a, v);
    }
  }

  // 246 — C&E (Fong Shen Bang)
  class M246 extends Mapper {
    constructor(cart) { super(cart); for (let i = 0; i < 4; i++) this.setPrg8(i, i - 4); }
    cpuRead(a) { if (a >= 0x6000 && a < 0x6800) return -1; return super.cpuRead(a); }
    cpuWrite(a, v) {
      if (a >= 0x6000 && a < 0x6008) { if (a & 4) this.setChr2(a & 3, v); else this.setPrg8(a & 3, v); return; }
      if (a >= 0x6800 && a < 0x8000) super.cpuWrite(a, v);
    }
  }

  const reg = {
    15: M15, 16: (c) => new M16(c, 16), 153: (c) => new M16(c, 153), 159: (c) => new M16(c, 159),
    18: M18, 19: (c) => new M19(c, 163), 210: (c) => new M19(c, c.submapper === 1 ? 175 : c.submapper === 2 ? 340 : 0),
    21: (c) => new VRC24(c, 21), 22: (c) => new VRC24(c, 22), 23: (c) => new VRC24(c, 23), 25: (c) => new VRC24(c, 25),
    24: (c) => new M24(c, false), 26: (c) => new M24(c, true), 85: M85, 73: M73, 75: M75,
    28: M28, 30: M30, 32: M32, 33: (c) => new M33(c, false), 48: (c) => new M33(c, true),
    37: (c) => new M47(c, true), 47: (c) => new M47(c, false), 38: M38, 41: M41, 64: M64, 65: M65, 67: M67, 68: M68,
    70: (c) => new M70(c, false), 152: (c) => new M70(c, true), 72: (c) => new M72(c, false), 92: (c) => new M72(c, true),
    76: M76, 77: M77, 78: M78, 79: (c) => new M79(c, false), 146: (c) => new M79(c, false), 113: (c) => new M79(c, true),
    80: (c) => new M80(c, false), 207: (c) => new M80(c, true), 82: M82, 86: M86, 87: M87,
    88: (c) => new N108(c, 88), 154: (c) => new N108(c, 154), 95: (c) => new N108(c, 95),
    89: M89, 93: M93, 94: M94, 97: M97, 105: M105, 107: M107, 111: M111, 112: M112, 118: M118, 119: M119,
    132: M132, 133: M133, 137: (c) => new M8259(c, 137), 138: (c) => new M8259(c, 138), 139: (c) => new M8259(c, 139), 141: (c) => new M8259(c, 141),
    143: M143, 145: M145, 147: M147, 148: M148, 149: M149, 155: M1, 156: M156,
    166: (c) => new M166(c, false), 167: (c) => new M166(c, true), 168: M168, 171: M171, 184: M184, 185: M185, 190: M190, 193: M193,
    200: M200, 201: M201, 202: M202, 203: M203, 225: M225, 255: M225, 226: M226, 227: M227, 228: M228, 229: M229,
    230: M230, 231: M231, 232: M232, 233: M233, 234: M234, 235: M235, 240: M240, 241: M241, 242: M242, 243: M243, 244: M244, 245: M245, 246: M246,
  };
  Object.assign(MAPPERS, reg);
  global.RPNesMappers = { VrcIrq, Eeprom };
})(typeof window !== "undefined" ? window : globalThis);
