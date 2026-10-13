// RetroPixels — Nintendo 64. Original code.
//
// The Nintendo 64 is too heavy to emulate inside the page, so RetroPixels.exe
// runs it (its own N64 emulator, written in Go) and this file is the page's
// side: it sends the game and the controllers over a WebSocket and shows the
// pictures and plays the sound that come back. Frames are asked for one at a
// time by the player's normal loop, so the picture is about one frame behind.
"use strict";

(function (global) {
  // Button order (bit i of setInput) and the matching N64 controller bits.
  const BUTTONS = ["A", "B", "Z", "Start", "Up", "Down", "Left", "Right", "L", "R",
    "C-Up", "C-Down", "C-Left", "C-Right", "Stick Up", "Stick Down", "Stick Left", "Stick Right"];
  const BITS = [0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001];
  const C_UP = 0x0008, C_DOWN = 0x0004, C_LEFT = 0x0002, C_RIGHT = 0x0001;
  const STICK = 80;       // how far a real N64 stick reaches
  const DEAD = 0.15;      // stick dead zone
  const MAX_AHEAD = 3;    // frames asked for but not back yet

  // Messages (see n64host.go)
  const START = 1, TICK = 2, RESET = 3;
  const READY = 0x81, FRAME = 0x82, SAVE = 0x83, ERROR = 0x84;

  const stickValue = (v) => {
    if (Math.abs(v) < DEAD) return 0;
    const s = (Math.abs(v) - DEAD) / (1 - DEAD);
    return Math.round(Math.sign(v) * Math.min(1, s) * STICK);
  };

  class N64 {
    constructor(rom, opts) {
      const desk = global.RETRO_DESKTOP;
      if (!desk || !desk.token) throw new Error("Nintendo 64 games play in the RetroPixels desktop app (RetroPixels.exe).");
      this.rate = opts.sampleRate || 48000;
      this.fps = 60; this.width = 320; this.height = 240;
      this.screen = new Uint32Array(320 * 240).fill(0xff000000);
      this.sram = opts.sram ? new Uint8Array(opts.sram) : null;
      this.sound = []; this.ahead = 0; this.error = null; this.session = null;
      this.buttons = [0, 0, 0, 0];
      this.sticks = [0, 1, 2, 3].map(() => [0, 0, 0, 0]); // left X, Y, right X, Y
      this.tick = new Uint8Array(22);
      this.ready = new Promise((resolve, reject) => { this.onReady = resolve; this.onFail = reject; });
      this.ready.catch(() => {});

      const ws = this.ws = new WebSocket(`ws://${location.host}/api/n64/ws?token=${encodeURIComponent(desk.token)}`);
      ws.binaryType = "arraybuffer";
      ws.onopen = () => {
        const save = this.sram || new Uint8Array(0);
        const head = new Uint8Array(9);
        const dv = new DataView(head.buffer);
        head[0] = START; dv.setUint32(1, this.rate, true); dv.setUint32(5, save.length, true);
        ws.send(new Blob([head, save, rom]));
      };
      ws.onmessage = (e) => this.receive(e.data);
      ws.onclose = () => { if (!this.closing) this.stop("Lost the connection to RetroPixels' Nintendo 64 emulator."); };
    }

    stop(text) {
      if (!this.error) this.error = new Error(text);
      this.onFail(this.error);
    }

    receive(buf) {
      const u8 = new Uint8Array(buf);
      switch (u8[0]) {
        case READY: {
          const hello = JSON.parse(new TextDecoder().decode(u8.subarray(1)));
          this.session = hello.session; this.fps = hello.fps || 60; this.title = hello.title;
          break;
        }
        case SAVE:
          this.sram = u8.slice(1);
          if (this.session) this.onReady(); // the save always follows "ready"
          break;
        case FRAME: {
          const dv = new DataView(buf);
          this.ahead = Math.max(0, this.ahead - u8[1]);
          const w = dv.getUint16(4, true), h = dv.getUint16(6, true), n = dv.getUint32(8, true);
          this.screen = new Uint32Array(buf, 12, w * h);
          this.width = w; this.height = h;
          if (n) {
            const pcm = new Int16Array(buf, 12 + w * h * 4, n * 2), out = new Float32Array(n * 2);
            for (let i = 0; i < out.length; i++) out[i] = pcm[i] / 32768;
            this.sound.push(out);
          }
          break;
        }
        case ERROR:
          this.stop(new TextDecoder().decode(u8.subarray(1)));
          break;
      }
    }

    setInput(port, bits) { this.buttons[port] = bits; }
    // Left stick = the analog stick, right stick = the C buttons.
    setAnalog(port, x, y, rx, ry) { this.sticks[port] = [x || 0, y || 0, rx || 0, ry || 0]; }

    runFrame() {
      if (this.error) throw this.error;
      if (this.ws.readyState !== 1 || !this.session || this.ahead >= MAX_AHEAD) return; // the emulator is busy: wait
      const t = this.tick;
      t[0] = TICK; t[1] = 1;
      const connected = [...(navigator.getGamepads ? navigator.getGamepads() : [])].filter(Boolean).length;
      for (let p = 0; p < 4; p++) {
        const bits = this.buttons[p], st = this.sticks[p];
        let n64 = 0;
        for (let i = 0; i < BITS.length; i++) if (bits & (1 << i)) n64 |= BITS[i];
        if (st[2] < -0.5) n64 |= C_LEFT;
        if (st[2] > 0.5) n64 |= C_RIGHT;
        if (st[3] < -0.5) n64 |= C_UP;
        if (st[3] > 0.5) n64 |= C_DOWN;
        let x = stickValue(st[0]), y = -stickValue(st[1]); // N64 up is +Y
        // Stick keys (arrows by default) push the stick all the way.
        if (bits & (1 << 16)) x = -STICK; else if (bits & (1 << 17)) x = STICK;
        if (bits & (1 << 14)) y = STICK; else if (bits & (1 << 15)) y = -STICK;
        const o = 2 + p * 5;
        t[o] = n64 & 0xff; t[o + 1] = n64 >> 8; t[o + 2] = x & 0xff; t[o + 3] = y & 0xff;
        t[o + 4] = p < connected ? 1 : 0;
      }
      this.ws.send(t);
      this.ahead++;
    }

    takeAudio() {
      if (!this.sound.length) return new Float32Array(0);
      if (this.sound.length === 1) return this.sound.pop();
      let n = 0; for (const s of this.sound) n += s.length;
      const out = new Float32Array(n);
      let o = 0; for (const s of this.sound) { out.set(s, o); o += s.length; }
      this.sound = [];
      return out;
    }

    reset() {
      if (this.ws.readyState === 1) this.ws.send(new Uint8Array([RESET]));
      this.sound = [];
    }

    getSRAM() { return this.sram; }

    // Save states are rare and asked for synchronously by the page, so these
    // use a synchronous request to RetroPixels, which asks the emulator.
    request(method, body) {
      if (this.error) throw this.error;
      if (!this.session) throw new Error("The game is still starting");
      const x = new XMLHttpRequest();
      x.open(method, "/api/n64/state?s=" + this.session, false);
      x.setRequestHeader("X-Retro-Token", global.RETRO_DESKTOP.token);
      x.overrideMimeType("text/plain; charset=x-user-defined"); // bytes come back one per character
      x.send(body || null);
      if (x.status !== 200) throw new Error((x.responseText || "No answer from the Nintendo 64 emulator").trim());
      return x.responseText;
    }
    saveState() {
      const text = this.request("GET");
      const out = new Uint8Array(text.length);
      for (let i = 0; i < text.length; i++) out[i] = text.charCodeAt(i) & 0xff;
      return { v: 1, n64: out };
    }
    loadState(s) {
      if (!s || !s.n64) throw new Error("That save state isn't for this emulator");
      this.request("POST", s.n64);
      this.sound = [];
    }

    close() { this.closing = true; try { this.ws.close(); } catch (e) { /* already closed */ } }
  }

  N64.info = {
    system: "n64", name: "Nintendo 64", width: 320, height: 240, aspect: 4 / 3, fps: 60, stereo: true, players: 4,
    buttons: BUTTONS,
    analogStick: true, // the left stick is the analog stick, not the D-pad
  };

  global.RPCores = global.RPCores || {};
  global.RPCores.n64 = N64;
})(typeof window !== "undefined" ? window : globalThis);
