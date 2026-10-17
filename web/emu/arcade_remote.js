// RetroPixels — Arcade boards that run inside RetroPixels.exe. Original code.
//
// Some arcade boards are written only in Go (cores/ in the source). In the
// desktop app they run inside RetroPixels.exe, like the Nintendo 64: this file
// asks RetroPixels for the list of those boards and adds each one to RPArcade
// as a board class, so zips are recognised exactly like the page's own boards.
// A game then sends its files over a WebSocket (see arcadehost.go) and shows
// the pictures and plays the sound that come back. RetroPixels sends the
// picture already turned upright, so every one of these games looks like a
// horizontal game to arcade.js. Frames are asked for one at a time by the
// player's normal loop, so the picture is about one frame behind.
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };
  if (!RPArcade.boards) RPArcade.boards = [];
  const desk = global.RETRO_DESKTOP;
  if (!desk || !desk.token || typeof XMLHttpRequest === "undefined") return; // only in RetroPixels.exe

  // Messages (see arcadehost.go)
  const START = 1, TICK = 2, RESET = 3, DIPS = 4;
  const READY = 0x81, FRAME = 0x82, SAVE = 0x83, ERROR = 0x84;
  const MAX_AHEAD = 3; // frames asked for but not back yet

  let list = [];
  try {
    const x = new XMLHttpRequest();
    x.open("GET", "/api/arcade/boards", false);
    x.setRequestHeader("X-Retro-Token", desk.token);
    x.send(null);
    if (x.status === 200) list = JSON.parse(x.responseText) || [];
  } catch (e) { list = []; }

  const enc = new TextEncoder();
  const u32 = (v) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v >>> 0, true); return b; };

  function makeBoard(def) {
    class RemoteBoard {
      constructor(game, files, opts) {
        this.game = game;
        this.buttons = game.buttons || def.buttons;
        this.width = game.width || def.width; this.height = game.height || def.height;
        this.screen = new Uint32Array(this.width * this.height).fill(0xff000000);
        this.rate = opts.sampleRate || 48000;
        this.dips = opts.dips; this.sentDips = JSON.stringify(opts.dips || {});
        this.sram = opts.sram ? new Uint8Array(opts.sram) : null;
        this.sound = []; this.ahead = 0; this.error = null; this.session = null;
        this.pads = [0, 0, 0, 0];
        this.ready = new Promise((resolve, reject) => { this.onReady = resolve; this.onFail = reject; });
        this.ready.catch(() => {});

        const ws = this.ws = new WebSocket(`ws://${location.host}/api/arcade/ws?token=${encodeURIComponent(desk.token)}`);
        ws.binaryType = "arraybuffer";
        ws.onopen = () => {
          const head = enc.encode(JSON.stringify({ game: game.id, zip: opts.zipName || game.id + ".zip", dips: opts.dips || {} }));
          const save = this.sram || new Uint8Array(0);
          const parts = [new Uint8Array([START]), u32(this.rate), u32(head.length), head, u32(save.length), save];
          for (const [name, data] of files) {
            const n = enc.encode(name);
            const nl = new Uint8Array(2); nl[0] = n.length & 255; nl[1] = n.length >> 8;
            parts.push(nl, n, u32(data.length), data);
          }
          ws.send(new Blob(parts));
        };
        ws.onmessage = (e) => this.receive(e.data);
        ws.onclose = () => { if (!this.closing) this.stop("Lost the connection to RetroPixels' arcade emulator."); };
      }

      stop(text) {
        if (!this.error) this.error = new Error(text);
        this.onFail(this.error);
      }

      receive(buf) {
        const u8 = new Uint8Array(buf);
        switch (u8[0]) {
          case READY:
            this.session = JSON.parse(new TextDecoder().decode(u8.subarray(1))).session;
            this.onReady();
            break;
          case SAVE:
            this.sram = u8.slice(1);
            break;
          case FRAME: {
            const dv = new DataView(buf);
            this.ahead = Math.max(0, this.ahead - u8[1]);
            const w = dv.getUint16(4, true), h = dv.getUint16(6, true), n = dv.getUint32(8, true);
            if (w === this.width && h === this.height) this.screen.set(new Uint32Array(buf, 12, w * h));
            if (n) {
              const pcm = new Int16Array(buf, 12 + w * h * 4, n), out = new Float32Array(n);
              for (let i = 0; i < n; i++) out[i] = pcm[i] / 32768;
              this.sound.push(out);
            }
            break;
          }
          case ERROR:
            this.stop(new TextDecoder().decode(u8.subarray(1)));
            break;
        }
      }

      setInput(...pads) {
        for (let p = 0; p < 4; p++) {
          const st = pads[p];
          let bits = 0;
          if (st) this.buttons.forEach((b, i) => { if (st[b]) bits |= 1 << i; });
          this.pads[p] = bits >>> 0;
        }
      }

      runFrame() {
        if (this.error) throw this.error;
        if (this.ws.readyState !== 1 || !this.session || this.ahead >= MAX_AHEAD) return; // the emulator is busy: wait
        const d = JSON.stringify(this.dips || {});
        if (d !== this.sentDips) { // the player changed a machine setting
          this.sentDips = d;
          this.ws.send(new Blob([new Uint8Array([DIPS]), enc.encode(d)]));
        }
        const t = new Uint8Array(3 + 16), dv = new DataView(t.buffer);
        t[0] = TICK; t[1] = 1; t[2] = 4;
        for (let p = 0; p < 4; p++) dv.setUint32(3 + p * 4, this.pads[p], true);
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

      // Save states are rare and asked for synchronously by the page.
      request(method, body) {
        if (this.error) throw this.error;
        if (!this.session) throw new Error("The game is still starting");
        const x = new XMLHttpRequest();
        x.open(method, "/api/arcade/state?s=" + this.session, false);
        x.setRequestHeader("X-Retro-Token", desk.token);
        x.overrideMimeType("text/plain; charset=x-user-defined"); // bytes come back one per character
        x.send(body || null);
        if (x.status !== 200) throw new Error((x.responseText || "No answer from the arcade emulator").trim());
        return x.responseText;
      }
      saveState() {
        const text = this.request("GET");
        const out = new Uint8Array(text.length);
        for (let i = 0; i < text.length; i++) out[i] = text.charCodeAt(i) & 0xff;
        return { remote: out };
      }
      loadState(s) {
        if (!s || !s.remote) throw new Error("That save state isn't for this game");
        this.request("POST", s.remote);
        this.sound = [];
      }

      close() { this.closing = true; try { this.ws.close(); } catch (e) { /* already closed */ } }
    }
    Object.assign(RemoteBoard, {
      id: def.id, title: def.title, width: def.width, height: def.height, rotate: 0, fps: def.fps,
      buttons: def.buttons, dips: def.dips, players: def.players, games: def.games, remote: true,
    });
    return RemoteBoard;
  }

  for (const def of list) {
    if (RPArcade.boards.some((b) => b.id === def.id)) continue;
    RPArcade.boards.push(makeBoard(def));
  }
})(typeof window !== "undefined" ? window : globalThis);
