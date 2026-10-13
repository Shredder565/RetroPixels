// RetroPixels — Game Boy Advance cartridge extras: the GPIO port and the
// sensors some carts carry (clock, sunlight, gyro, tilt, rumble). Original code.
//
// A few cartridges wire extra chips to four general-purpose pins that show up
// in the ROM area at 0x080000C4 (data), 0x080000C6 (pin direction, 1 = the
// game drives the pin) and 0x080000C8 (bit 0: the pins can be read back).
// What sits on those pins depends on the game, so it's picked by game code:
//   real-time clock     pin 0 clock, pin 1 data, pin 2 select (high)
//   sunlight sensor     pin 0 clock, pin 1 reset, pin 2 select (low), pin 3 "reached" flag
//   gyro sensor         pin 0 start a reading, pin 1 clock, pin 2 data, pin 3 rumble motor
//   rumble motor        pin 3
// The tilt sensor of Yoshi Topsy-Turvy / Koro Koro Puzzle is not on the pins:
// it answers in the save-memory area at 0x0E008000.
"use strict";

(function (global) {
  // game codes (header bytes 0xAC-0xAF) of the carts with extra hardware
  const GAMES = {
    U3IJ: "solar", U3IE: "solar", U3IP: "solar", // Boktai: The Sun Is in Your Hand
    U32J: "solar", U32E: "solar", U32P: "solar", // Boktai 2: Solar Boy Django
    U33J: "solar",                               // Shin Bokura no Taiyou (Boktai 3)
    RZWJ: "gyro", RZWE: "gyro", RZWP: "gyro",    // WarioWare: Twisted! / Mawaru Made in Wario
    V49J: "rumble", V49E: "rumble", V49P: "rumble", // Drill Dozer / Screw Breaker
    KYGJ: "tilt", KYGE: "tilt", KYGP: "tilt",    // Yoshi Topsy-Turvy / Yoshi's Universal Gravitation
    KHPJ: "tilt",                                // Koro Koro Puzzle Happy Panechu!
  };

  // ---- sunlight sensor (Boktai) ----
  // The game resets a counter, then clocks it until the sensor's comparator
  // says "reached". Bright light trips it early, darkness late.
  class Solar {
    constructor() { this.count = 0; this.base = 5; this.level = 0.5; this.clk = 0; }
    threshold() { return Math.round(0xe8 - this.level * (0xe8 - 0x50)); }
    pins(p) { // p = pin levels the game drives
      if (p & 4) { this.clk = p & 1; return; } // pin 2 high: the clock chip is selected instead
      if (p & 2) this.count = 0;
      else if ((p & 1) && !this.clk) this.count = (this.count + 1) & 0xff;
      this.clk = p & 1;
    }
    input() { return this.count >= this.threshold() ? 8 : 0; }
    saveState() { return [this.count, this.level, this.clk]; }
    loadState(o) { [this.count, this.level, this.clk] = o; }
  }

  // ---- gyro sensor (WarioWare: Twisted!) ----
  // Raising pin 0 samples the turning speed (12 bits, about 0x6C0 at rest);
  // each falling edge of the clock then puts the next of its 16 bits on pin 2,
  // top bit first.
  class Gyro {
    constructor() { this.rate = 0; this.word = 0; this.bit = 16; this.p = 0; }
    pins(p) {
      if ((p & 1) && !(this.p & 1)) { this.word = (0x6c0 - Math.round(this.rate * 0x300)) & 0xfff; this.bit = 16; }
      else if (!(p & 2) && (this.p & 2) && this.bit >= 0) this.bit--;
      this.p = p;
    }
    input() { return this.bit >= 0 && this.bit < 16 ? ((this.word >> this.bit) & 1) << 2 : 0; }
    saveState() { return [this.word, this.bit, this.p]; }
    loadState(o) { [this.word, this.bit, this.p] = o; }
  }

  // ---- tilt sensor (Yoshi Topsy-Turvy, Koro Koro Puzzle) ----
  // Write 0x55 to 0E008000 then 0xAA to 0E008100 to take a reading; the
  // 12-bit X / Y values then sit at 0E008200/0300 and 0E008400/0500
  // (bit 7 of 0300 says the reading is ready).
  class Tilt {
    constructor() { this.armed = false; this.x = 0x392; this.y = 0x3a0; this.ready = 0; }
    read8(a) {
      switch (a & 0xff00) {
        case 0x8200: return this.x & 0xff;
        case 0x8300: return ((this.x >> 8) & 15) | this.ready;
        case 0x8400: return this.y & 0xff;
        case 0x8500: return (this.y >> 8) & 15;
      }
      return 0;
    }
    write8(a, v, t) {
      if ((a & 0xff00) === 0x8000 && v === 0x55) this.armed = true;
      else if ((a & 0xff00) === 0x8100 && v === 0xaa && this.armed) {
        // tilting right / toward the player lowers the value, like the GB tilt carts
        this.x = 0x392 - Math.round(t.x * 0xa0); this.y = 0x3a0 - Math.round(t.y * 0xa0);
        this.ready = 0x80; this.armed = false;
      }
    }
    saveState() { return [this.armed ? 1 : 0, this.x, this.y, this.ready]; }
    loadState(o) { this.armed = !!o[0]; [, this.x, this.y, this.ready] = o; }
  }

  // ---- the port itself ----
  class Port {
    constructor(gba, kind, rtc) {
      this.gba = gba; this.kind = kind; this.rtc = rtc;
      this.dir = 0; this.ctrl = 0; this.out = 0;
      this.solar = kind === "solar" ? new Solar() : null;
      this.gyro = kind === "gyro" ? new Gyro() : null;
      this.tilt = kind === "tilt" ? new Tilt() : null;
      this.hasRumble = kind === "gyro" || kind === "rumble";
      this.onPins = !!(rtc || this.solar || this.gyro || this.hasRumble);
      // controls
      this.stick = { x: 0, y: 0 }; this.extra = 0; this.prevExtra = 0;
      this.motion = { x: 0, y: 0 };
      // rumble: share of the frame the motor was on
      this.motor = 0; this.motorSince = 0; this.motorOn = 0; this.rumbleLevel = 0;
    }
    // what the pins read: the game's own levels on its output pins, the chips' on the rest
    pinLevels() {
      let ins = 0;
      if (this.rtc) ins |= this.rtc.pins & 2;
      if (this.solar) ins |= this.solar.input();
      if (this.gyro) ins |= this.gyro.input();
      return ((this.out & this.dir) | (ins & ~this.dir)) & 15;
    }
    read16(o) {
      if (o === 0xc4) return this.pinLevels();
      if (o === 0xc6) return this.dir;
      return this.ctrl;
    }
    write16(o, v) {
      if (o === 0xc4) { this.out = v & 15; this.drive(); }
      else if (o === 0xc6) { this.dir = v & 15; this.drive(); }
      else if (o === 0xc8) this.ctrl = v & 1;
    }
    drive() {
      const p = this.out & this.dir;
      if (this.rtc) { this.rtc.dir = this.dir; this.rtc.write(this.out); }
      if (this.solar) this.solar.pins(p);
      if (this.gyro) this.gyro.pins(p);
      if (this.hasRumble) {
        const on = (p >> 3) & 1, now = this.gba.cpu.cycles;
        if (on !== this.motorOn) { if (this.motorOn) this.motor += now - this.motorSince; this.motorOn = on; this.motorSince = now; }
      }
    }
    // tilt sensor in the save area
    busRead8(a) { return this.tilt && (a & 0xffff) >= 0x8000 && (a & 0xffff) < 0x8600 ? this.tilt.read8(a) : -1; }
    busWrite8(a, v) { if (this.tilt && (a & 0xffff) >= 0x8000 && (a & 0xffff) < 0x8600) { this.tilt.write8(a, v, this.motion); return true; } return false; }

    // ---- controls ----
    setExtra(bits) { this.extra = bits; }
    frameStart() {
      const k = this.extra, s = this.stick, clamp = (v) => Math.max(-1, Math.min(1, v));
      if (this.solar) { // More Sun / Less Sun step the light by a tenth; pushing the right stick up adds sun on top
        const pressed = k & ~this.prevExtra;
        if (pressed & 1) this.solar.base = Math.min(10, this.solar.base + 1);
        if (pressed & 2) this.solar.base = Math.max(0, this.solar.base - 1);
        this.solar.level = Math.max(this.solar.base / 10, -s.y > 0.15 ? -s.y : 0);
      }
      if (this.gyro) this.gyro.rate = clamp(s.x + ((k & 2) ? 1 : 0) - ((k & 1) ? 1 : 0));
      if (this.tilt) { // Tilt Up / Down / Left / Right
        this.motion.x = clamp(s.x + ((k & 8) ? 1 : 0) - ((k & 4) ? 1 : 0));
        this.motion.y = clamp(s.y + ((k & 2) ? 1 : 0) - ((k & 1) ? 1 : 0));
      }
      this.prevExtra = k;
    }
    frameEnd(frameCycles) {
      if (!this.hasRumble) return;
      const now = this.gba.cpu.cycles;
      if (this.motorOn) { this.motor += now - this.motorSince; this.motorSince = now; }
      this.rumbleLevel = Math.min(1, this.motor / frameCycles);
      this.motor = 0;
    }
    get buttons() {
      return { solar: ["More Sun", "Less Sun"], gyro: ["Turn Left", "Turn Right"], tilt: ["Tilt Up", "Tilt Down", "Tilt Left", "Tilt Right"] }[this.kind] || null;
    }
    get note() {
      return {
        solar: "Sunlight: More Sun / Less Sun (I and K to start with) set the level; pushing the right stick up adds sun. Clicking the sticks (L3/R3) is used for save states.",
        gyro: "Turn the console with the right stick, or with Turn Left / Turn Right (J and L to start with). Clicking the sticks (L3/R3) is used for save states.",
        tilt: "Tilt the game with the right stick, or with the tilt keys (I J K L to start with). Clicking the sticks (L3/R3) is used for save states.",
      }[this.kind] || null;
    }

    saveState() {
      return {
        dir: this.dir, ctrl: this.ctrl, out: this.out,
        solar: this.solar ? this.solar.saveState() : null, sun: this.solar ? this.solar.base : null,
        gyro: this.gyro ? this.gyro.saveState() : null, tilt: this.tilt ? this.tilt.saveState() : null,
        motor: this.motorOn,
      };
    }
    loadState(o) {
      if (!o) return;
      this.dir = o.dir; this.ctrl = o.ctrl; this.out = o.out;
      if (this.solar && o.solar) { this.solar.loadState(o.solar); if (o.sun != null) this.solar.base = o.sun; }
      if (this.gyro && o.gyro) this.gyro.loadState(o.gyro);
      if (this.tilt && o.tilt) this.tilt.loadState(o.tilt);
      this.motorOn = o.motor || 0; this.motorSince = this.gba.cpu.cycles; this.motor = 0;
    }
  }

  // Build the port for this cartridge, or null when it has nothing on it.
  // `rtc` is the clock chip when the game's code asks for one.
  function create(gba, rom, RTC, wantsRtc) {
    const code = String.fromCharCode(rom[0xac], rom[0xad], rom[0xae], rom[0xaf]);
    const kind = GAMES[code] || null;
    const rtc = wantsRtc || kind === "solar" ? new RTC() : null;
    if (!kind && !rtc) return null;
    return new Port(gba, kind, rtc);
  }

  global.RPGBAGpio = { create, GAMES };
})(typeof window !== "undefined" ? window : globalThis);
