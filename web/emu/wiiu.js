// RetroPixels — Wii U. Original code.
//
// The Wii U runs inside RetroPixels.exe (its own Wii U emulator, written in
// Go, as the Nintendo 64 does) and this file is the page's side: it sends the
// program and the GamePad over a WebSocket and shows the pictures and plays
// the sound that come back. See wiiuhost.go for the messages.
"use strict";

(function (global) {
  // Button order (bit i of setInput): the same as cores/wiiu in Go.
  const BUTTONS = ["A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "Home",
    "Up", "Down", "Left", "Right", "L Stick Click", "R Stick Click",
    "Stick Up", "Stick Down", "Stick Left", "Stick Right", "TV / GamePad"];
  const MAX_AHEAD = 3; // frames asked for but not back yet

  const START = 1, TICK = 2, RESET = 3;
  const READY = 0x81, FRAME = 0x82, ERROR = 0x84;

  const axis = v => Math.max(-127, Math.min(127, Math.round((v || 0) * 127)));

  class WiiU {
    constructor(rpx, opts) {
      const desk = global.RETRO_DESKTOP;
      if (!desk || !desk.token) throw new Error("Wii U programs play in the RetroPixels desktop app (RetroPixels.exe).");
      this.rate = opts.sampleRate || 48000;
      this.fps = 59.94; this.width = 1280; this.height = 720;
      this.screen = new Uint32Array(1280 * 720).fill(0xff000000);
      this.sound = []; this.ahead = 0; this.error = null; this.session = null;
      this.buttons = 0; this.sticks = [0, 0, 0, 0]; this.rumble = 0;
      this.tick = new Uint8Array(10);
      this.ready = new Promise((resolve, reject) => { this.onReady = resolve; this.onFail = reject; });
      this.ready.catch(() => {});

      const ws = this.ws = new WebSocket(`ws://${location.host}/api/wiiu/ws?token=${encodeURIComponent(desk.token)}`);
      ws.binaryType = "arraybuffer";
      ws.onopen = () => {
        const name = new TextEncoder().encode((opts.name || "program") + ".rpx");
        const head = new Uint8Array(9);
        const dv = new DataView(head.buffer);
        head[0] = START; dv.setUint32(1, this.rate, true); dv.setUint32(5, name.length, true);
        ws.send(new Blob([head, name, rpx]));
      };
      ws.onmessage = e => this.receive(e.data);
      ws.onclose = () => { if (!this.closing) this.stop("Lost the connection to RetroPixels' Wii U emulator."); };
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
          this.session = hello.session; this.fps = hello.fps || 59.94;
          this.onReady();
          break;
        }
        case FRAME: {
          const dv = new DataView(buf);
          this.ahead = Math.max(0, this.ahead - u8[1]);
          this.rumble = u8[3] / 255;
          const w = dv.getUint16(4, true), h = dv.getUint16(6, true), n = dv.getUint32(8, true);
          let o = 12;
          if (u8[2] === 0) { // all of the picture
            if (this.screen.length !== w * h) this.screen = new Uint32Array(w * h);
            this.screen.set(new Uint32Array(buf, 12, w * h));
            this.width = w; this.height = h;
            o += w * h * 4;
          } else if (u8[2] === 2) { // some rows changed
            const y0 = dv.getUint16(12, true), rows = dv.getUint16(14, true);
            this.screen.set(new Uint32Array(buf, 16, rows * w), y0 * w);
            o += 4 + rows * w * 4;
          }
          if (n) {
            const pcm = new Int16Array(buf, o, n * 2), out = new Float32Array(n * 2);
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

    setInput(port, bits) { if (port === 0) this.buttons = bits; }
    // Both sticks are the GamePad's.
    setAnalog(port, x, y, rx, ry) { if (port === 0) this.sticks = [x || 0, y || 0, rx || 0, ry || 0]; }

    runFrame() {
      if (this.error) throw this.error;
      if (this.ws.readyState !== 1 || !this.session || this.ahead >= MAX_AHEAD) return; // the emulator is busy: wait
      const t = this.tick, dv = new DataView(t.buffer);
      t[0] = TICK; t[1] = 1;
      dv.setUint32(2, this.buttons >>> 0, true);
      for (let i = 0; i < 4; i++) t[6 + i] = axis(this.sticks[i]) & 0xff;
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

    getSRAM() { return null; }
    saveState() { throw new Error("Save states aren't available for the Wii U yet"); }
    loadState() { throw new Error("Save states aren't available for the Wii U yet"); }

    close() { this.closing = true; try { this.ws.close(); } catch (e) { /* already closed */ } }
  }
  WiiU.prototype.usesRightStick = true; // the right stick is the GamePad's, not the save-state shortcuts
  WiiU.prototype.controlsNote = "Left stick and right stick = the GamePad's sticks. TV / GamePad switches which screen you see. " +
    "Wii U homebrew (.rpx) runs now; games come in later versions.";

  WiiU.info = {
    system: "wiiu", name: "Wii U", width: 1280, height: 720, aspect: 16 / 9, fps: 59.94, stereo: true, players: 1,
    buttons: BUTTONS,
    analogStick: true, // the left stick is the GamePad's stick, not the D-pad
  };

  global.RPCores = global.RPCores || {};
  global.RPCores.wiiu = WiiU;
})(typeof window !== "undefined" ? window : globalThis);
