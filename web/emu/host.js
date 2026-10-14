// RetroPixels — player for RetroPixels' own emulators. Original code.
//
// Draws the emulator's picture, plays its sound, reads the Xbox controller and
// keyboard, keeps battery saves, and exposes save states / screenshots in the
// same shape the rest of the page already uses.
"use strict";

(function (global) {
  // ===================================================================
  // Save-state format (RetroPixels' own): "RPS1" + JSON + raw arrays
  // ===================================================================
  const TYPES = ["Uint8Array", "Uint16Array", "Uint32Array", "Int8Array", "Int16Array", "Int32Array", "Float32Array", "Float64Array", "Uint8ClampedArray"];
  const RPState = {
    encode(obj) {
      const blobs = []; let total = 0;
      const json = JSON.stringify(obj, function (k, v) {
        if (ArrayBuffer.isView(v)) {
          const bytes = new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice();
          const ref = { "\u0000ta": [TYPES.indexOf(v.constructor.name), total, bytes.length] };
          blobs.push(bytes); total += bytes.length; return ref;
        }
        if (v === undefined || typeof v === "function") return undefined;
        return v;
      });
      const jb = new TextEncoder().encode(json);
      const out = new Uint8Array(8 + jb.length + total);
      out.set([0x52, 0x50, 0x53, 0x31], 0); // "RPS1"
      new DataView(out.buffer).setUint32(4, jb.length, true);
      out.set(jb, 8);
      let o = 8 + jb.length; for (const b of blobs) { out.set(b, o); o += b.length; }
      return out;
    },
    decode(u8) {
      if (u8[0] !== 0x52 || u8[1] !== 0x50 || u8[2] !== 0x53 || u8[3] !== 0x31) throw new Error("That isn't a RetroPixels save state");
      const jl = new DataView(u8.buffer, u8.byteOffset).getUint32(4, true);
      const base = 8 + jl;
      const json = new TextDecoder().decode(u8.subarray(8, base));
      return JSON.parse(json, function (k, v) {
        if (v && typeof v === "object" && v["\u0000ta"]) {
          const [t, off, len] = v["\u0000ta"];
          const bytes = u8.slice(base + off, base + off + len);
          const C = global[TYPES[t]];
          return new C(bytes.buffer, 0, len / C.BYTES_PER_ELEMENT);
        }
        return v;
      });
    },
  };

  // ===================================================================
  // Controls
  // ===================================================================
  // Standard gamepad layout (what Xbox controllers report in the browser):
  // 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 View, 9 Menu, 10 L3, 11 R3, 12-15 D-pad.
  // L3 / R3 and the right stick are kept free for RetroPixels' save-state shortcuts.
  const PAD_NAMES = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "L3", "R3", "D-pad ↑", "D-pad ↓", "D-pad ←", "D-pad →"];
  const DIRS = [["Up", 12, 1, -1], ["Down", 13, 1, 1], ["Left", 14, 0, -1], ["Right", 15, 0, 1]];
  // keys for games with motion / light sensors (only listed for those games; the right stick works too)
  const TILT_KEYS = { "Tilt Up": { pad: [], keys: ["i"] }, "Tilt Down": { pad: [], keys: ["k"] }, "Tilt Left": { pad: [], keys: ["j"] }, "Tilt Right": { pad: [], keys: ["l"] } };
  const DEFAULT_MAPS = {
    // Nintendo-style systems: Xbox A (bottom) = B, Xbox B (right) = A, like the original pads.
    nes: { A: { pad: [1], keys: ["x"] }, B: { pad: [0], keys: ["z"] }, Select: { pad: [8], keys: ["shift"] }, Start: { pad: [9], keys: ["enter"] } },
    gb: { A: { pad: [1], keys: ["x"] }, B: { pad: [0], keys: ["z"] }, Select: { pad: [8], keys: ["shift"] }, Start: { pad: [9], keys: ["enter"] },
      ...TILT_KEYS },
    atari2600: { Fire: { pad: [0, 1], keys: ["z", "x", " "] }, Reset: { pad: [9], keys: ["enter"] }, Select: { pad: [8], keys: ["shift"] },
      "Left difficulty": { pad: [4], keys: ["1"], toggle: true }, "Right difficulty": { pad: [5], keys: ["2"], toggle: true },
      "Color / B&W": { pad: [3], keys: ["3"], toggle: true } },
    atari7800: { "Button 1": { pad: [0], keys: ["z"] }, "Button 2": { pad: [1], keys: ["x"] }, Pause: { pad: [9], keys: ["enter"] },
      Select: { pad: [8], keys: ["shift"] }, Reset: { pad: [3], keys: ["r"] },
      "Left difficulty": { pad: [4], keys: ["1"], toggle: true }, "Right difficulty": { pad: [5], keys: ["2"], toggle: true } },
    // Genesis: Xbox X / A / B = A / B / C (same row as the Genesis pad), LB / Y / RB = X / Y / Z.
    segaMD: { A: { pad: [2], keys: ["a"] }, B: { pad: [0], keys: ["s"] }, C: { pad: [1], keys: ["d"] },
      X: { pad: [4], keys: ["q"] }, Y: { pad: [3], keys: ["w"] }, Z: { pad: [5], keys: ["e"] },
      Start: { pad: [9], keys: ["enter"] }, Mode: { pad: [8], keys: ["shift"] } },
    // Master System: Xbox A / B = buttons 1 / 2 (the same spots as on the SMS pad); Menu = the console's Pause button.
    sms: { "1": { pad: [0], keys: ["z"] }, "2": { pad: [1], keys: ["x"] }, Pause: { pad: [9], keys: ["enter"] }, Reset: { pad: [], keys: ["r"] } },
    gba: { A: { pad: [1], keys: ["x"] }, B: { pad: [0], keys: ["z"] }, L: { pad: [4], keys: ["q"] }, R: { pad: [5], keys: ["w"] },
      Select: { pad: [8], keys: ["shift"] }, Start: { pad: [9], keys: ["enter"] },
      ...TILT_KEYS, "Turn Left": { pad: [], keys: ["j"] }, "Turn Right": { pad: [], keys: ["l"] },
      "More Sun": { pad: [], keys: ["i"] }, "Less Sun": { pad: [], keys: ["k"] } },
    // SNES: same button positions as the Xbox pad's diamond (Xbox B = A, A = B, Y = X, X = Y)
    snes: { A: { pad: [1], keys: ["x"] }, B: { pad: [0], keys: ["z"] }, X: { pad: [3], keys: ["s"] }, Y: { pad: [2], keys: ["a"] },
      L: { pad: [4], keys: ["q"] }, R: { pad: [5], keys: ["w"] }, Select: { pad: [8], keys: ["shift"] }, Start: { pad: [9], keys: ["enter"] } },
    // Nintendo 64: Xbox A / X = A / B (same spots as the N64's big buttons), LT = Z. The left stick is the
    // analog stick and the right stick the C buttons; on the keyboard the arrows move the stick.
    n64: { A: { pad: [0], keys: ["x"] }, B: { pad: [2], keys: ["z"] }, Z: { pad: [6], keys: ["a"] }, Start: { pad: [9], keys: ["enter"] },
      Up: { pad: [], keys: ["t"] }, Down: { pad: [], keys: ["g"] }, Left: { pad: [], keys: ["f"] }, Right: { pad: [], keys: ["h"] },
      L: { pad: [4], keys: ["q"] }, R: { pad: [5], keys: ["w"] },
      "C-Up": { pad: [], keys: ["i"] }, "C-Down": { pad: [], keys: ["k"] }, "C-Left": { pad: [], keys: ["j"] }, "C-Right": { pad: [], keys: ["l"] },
      "Stick Up": { pad: [], keys: ["arrowup"] }, "Stick Down": { pad: [], keys: ["arrowdown"] },
      "Stick Left": { pad: [], keys: ["arrowleft"] }, "Stick Right": { pad: [], keys: ["arrowright"] } },
    // Arcade: Coin = View / 5, Start = Menu / 1 (the keys arcade players know), first button = Xbox A.
    arcade: { Coin: { pad: [8], keys: ["5", "c"] }, "Coin 2": { pad: [], keys: ["6"] }, "Start (1P)": { pad: [9], keys: ["1", "enter"] },
      "Start (2P)": { pad: [], keys: ["2"] }, Fire: { pad: [0], keys: ["z"] }, Jump: { pad: [0], keys: ["z"] },
      "Button 1": { pad: [0], keys: ["z"] }, "Button 2": { pad: [1], keys: ["x"] }, "Button 3": { pad: [2], keys: ["a"] },
      "Button 4": { pad: [3], keys: ["s"] }, Service: { pad: [], keys: ["9"] } },
  };
  const ARROWS = { Up: "arrowup", Down: "arrowdown", Left: "arrowleft", Right: "arrowright" };

  function loadMap(system) {
    const base = JSON.parse(JSON.stringify(DEFAULT_MAPS[system] || {}));
    for (const d of ["Up", "Down", "Left", "Right"]) base[d] = base[d] || { pad: [], keys: [ARROWS[d]] };
    try {
      const saved = JSON.parse(localStorage.getItem("rp.map." + system) || "null");
      if (saved) for (const k in saved) if (base[k]) base[k] = saved[k];
    } catch (e) { /* ignore */ }
    return base;
  }
  function saveMap(system, map) { try { localStorage.setItem("rp.map." + system, JSON.stringify(map)); } catch (e) { /* ignore */ } }

  // ===================================================================
  // Sound: an AudioWorklet playing from a ring buffer
  // ===================================================================
  const WORKLET = `
    class RPOut extends AudioWorkletProcessor {
      constructor() {
        super();
        this.buf = new Float32Array(48000 * 2 * 2); this.r = 0; this.w = 0; this.n = 0; this.consumed = 0; this.tick = 0;
        this.port.onmessage = (e) => {
          if (e.data === "clear") { this.r = this.w = this.n = 0; return; }
          const d = e.data, len = this.buf.length;
          for (let i = 0; i < d.length; i++) {
            if (this.n >= len) { this.r = (this.r + 1) % len; this.n--; } // overflow: drop oldest
            this.buf[this.w] = d[i]; this.w = (this.w + 1) % len; this.n++;
          }
        };
      }
      process(inputs, outputs) {
        const L = outputs[0][0], R = outputs[0][1] || outputs[0][0], len = this.buf.length;
        for (let i = 0; i < L.length; i++) {
          if (this.n >= 2) {
            L[i] = this.buf[this.r]; R[i] = this.buf[(this.r + 1) % len];
            this.r = (this.r + 2) % len; this.n -= 2; this.consumed++;
          } else { L[i] = 0; R[i] = 0; }
        }
        if (++this.tick % 8 === 0) this.port.postMessage(this.n >> 1);
        return true;
      }
    }
    registerProcessor("rp-out", RPOut);`;

  class Audio {
    constructor() {
      this.ctx = null; this.node = null; this.gain = null; this.queued = 0; this.volume = 0.8; this.muted = false; this.ready = false;
    }
    async start() {
      const AC = global.AudioContext || global.webkitAudioContext;
      if (!AC) return;
      this.ctx = new AC({ sampleRate: 48000, latencyHint: "interactive" });
      this.rate = this.ctx.sampleRate;
      const url = URL.createObjectURL(new Blob([WORKLET], { type: "application/javascript" }));
      await this.ctx.audioWorklet.addModule(url);
      this.node = new AudioWorkletNode(this.ctx, "rp-out", { outputChannelCount: [2] });
      this.node.port.onmessage = (e) => { this.queued = e.data; };
      this.gain = this.ctx.createGain();
      // Recordings tap the sound before this volume node, so volume/mute only affect the speakers.
      this.gain._rpRecordFrom = this.node;
      this.node.connect(this.gain); this.gain.connect(this.ctx.destination);
      this.applyVolume();
      if (this.ctx.state === "suspended") { try { await this.ctx.resume(); } catch (e) { /* needs a click */ } }
      this.ready = true;
    }
    applyVolume() { if (this.gain) this.gain.gain.value = this.muted ? 0 : this.volume; }
    // samples: Float32Array; stereo=false means mono
    push(samples, stereo) {
      if (!this.ready || !samples.length) return;
      let out = samples;
      if (!stereo) { out = new Float32Array(samples.length * 2); for (let i = 0; i < samples.length; i++) out[2 * i] = out[2 * i + 1] = samples[i]; }
      this.node.port.postMessage(out, [out.buffer]);
      this.queued += out.length >> 1;
    }
    clear() { if (this.node) this.node.port.postMessage("clear"); this.queued = 0; }
    secondsQueued() { return this.ready ? this.queued / this.rate : 0.05; }
    close() { if (this.ctx) this.ctx.close(); }
  }

  // ===================================================================
  // The player
  // ===================================================================
  const CSS = `
    .rp-player{position:relative;width:100%;height:100%;background:#000;overflow:hidden;user-select:none;outline:none}
    .rp-player canvas.rp-screen{position:absolute;inset:0;width:100%;height:100%;image-rendering:pixelated}
    .rp-bar{position:absolute;left:0;right:0;bottom:0;display:flex;gap:6px;align-items:center;padding:8px 10px;
      background:linear-gradient(transparent,#000c 40%);opacity:0;transition:opacity .2s;font:13px system-ui,sans-serif;color:#eee}
    .rp-player:hover .rp-bar,.rp-player.rp-show .rp-bar{opacity:1}
    .rp-bar button{background:#0008;border:1px solid #fff3;color:#eee;border-radius:7px;padding:5px 9px;font:inherit;cursor:pointer}
    .rp-bar button:hover{background:#fff2}.rp-bar button[aria-pressed=true]{background:#107c10;border-color:#107c10}
    .rp-bar input[type=range]{width:90px;accent-color:#3dd13d}
    .rp-bar .rp-sp{flex:1}
    .rp-msg{position:absolute;left:50%;top:14px;transform:translateX(-50%);background:#000c;color:#fff;padding:6px 12px;border-radius:8px;font:13px system-ui;opacity:0;transition:opacity .3s;pointer-events:none}
    .rp-msg.on{opacity:1}
    .rp-modal{position:absolute;inset:0;background:#000d;display:flex;align-items:center;justify-content:center;font:14px system-ui;color:#eee}
    .rp-modal .rp-box{background:#171920;border:1px solid #2c303b;border-radius:12px;padding:16px;max-height:90%;overflow:auto;min-width:320px}
    .rp-modal table{border-collapse:collapse;width:100%} .rp-modal td{padding:5px 8px;border-bottom:1px solid #2c303b}
    .rp-modal button{background:#1f222b;border:1px solid #2c303b;color:#eee;border-radius:7px;padding:4px 8px;cursor:pointer;font:inherit}
    .rp-modal button.wait{background:#107c10}
    .rp-paused{position:absolute;inset:0;display:none;align-items:center;justify-content:center;color:#fff;font:600 22px system-ui;background:#0006;pointer-events:none}
    .rp-player.rp-is-paused .rp-paused{display:flex}`;

  class Player {
    constructor(opts) {
      this.opts = opts;
      const Core = global.RPCores[opts.system];
      if (!Core) throw new Error("No emulator for " + opts.system);
      this.Core = Core; this.info = Core.info; this.system = opts.system;
      this.audio = new Audio();
      this.paused = false; this.fast = false; this.holdFast = false; this.destroyed = false;
      this.map = loadMap(this.info.inputMap || opts.system);
      this.toggles = {}; this.prevPressed = {};
      this.keys = new Set();
      this.buildDom();
    }

    async start(rom) {
      this.rom = rom;
      await this.audio.start().catch(() => {});
      // firmware: chip programs the player supplied ({ "DSP-3": bytes, ... }), for cartridges with such chips
      // coreOpts: anything else a core needs from the page (the arcade core gets the zip's files)
      this.core = new this.Core(rom, { sampleRate: this.audio.rate || 48000, sram: this.opts.sram || null, name: this.opts.name, firmware: this.opts.firmware || null, ...(this.opts.coreOpts || {}) });
      // Cores that run elsewhere (Nintendo 64 runs inside RetroPixels.exe) say when they're ready.
      if (this.core.ready) await this.core.ready;
      // Some games bring extra controls (tilt or light sensor keys) — the core lists them.
      if (this.core.buttons) this.info = Object.assign({}, this.info, { buttons: this.core.buttons });
      // ...and some pick the screen's shape per game (arcade games on an upright monitor)
      if (this.core.aspect) this.info = Object.assign({}, this.info, { aspect: this.core.aspect });
      // ...and how many controllers it reads (arcade games for 3 or 4 players)
      if (this.core.players > (this.info.players || 1)) this.info = Object.assign({}, this.info, { players: this.core.players });
      // Games for 3 or 4 players get a button that picks which player the keyboard and
      // first controller play as (the other controllers move round with it).
      this.seat = 0;
      if ((this.info.players || 1) >= 3) {
        this.root.querySelector('[data-a="seat"]').hidden = false;
        let saved = 0;
        try { saved = parseInt(localStorage.getItem(this.seatKey()), 10) || 0; } catch (e) { /* no storage */ }
        this.setSeat(saved, true);
      }
      this.lastSram = this.sramSnapshot();
      this.resize();
      this.frameTime = 1000 / this.core.fps;
      this.acc = 0; this.last = performance.now();
      this.loop = this.loop.bind(this);
      requestAnimationFrame(this.loop);
      this.sramTimer = setInterval(() => this.checkSram(), 2000);
      if (this.opts.onReady) this.opts.onReady(this);
    }

    buildDom() {
      if (!document.getElementById("rp-player-css")) {
        const st = document.createElement("style"); st.id = "rp-player-css"; st.textContent = CSS; document.head.appendChild(st);
      }
      const root = this.root = document.createElement("div");
      root.className = "rp-player"; root.tabIndex = 0;
      root.innerHTML = `<canvas class="rp-screen"></canvas><div class="rp-paused">Paused</div><div class="rp-msg"></div>
        <div class="rp-bar">
          <button data-a="pause" title="Pause (P)">⏸ Pause</button>
          <button data-a="reset" title="Restart the game">↺ Restart</button>
          <button data-a="fast" aria-pressed="false" title="Fast-forward (hold Space)">⏩ Fast</button>
          <span>🔊</span><input type="range" min="0" max="1" step="0.05" data-a="vol" aria-label="Volume">
          <button data-a="mute" title="Mute (M)">Mute</button>
          <span class="rp-sp"></span>
          <button data-a="seat" title="Which player you control (games for 3 or 4 players)" hidden>👤 Player 1</button>
          <button data-a="controls" title="Change buttons">🎮 Controls</button>
          <button data-a="shot" title="Save a screenshot">📷</button>
        </div>`;
      this.canvas = root.querySelector("canvas");
      this.ctx2d = this.canvas.getContext("2d", { alpha: false });
      this.msgEl = root.querySelector(".rp-msg");
      this.opts.container.innerHTML = "";
      this.opts.container.appendChild(root);
      const vol = root.querySelector('[data-a="vol"]');
      try { const v = parseFloat(localStorage.getItem("rp.volume")); if (v >= 0 && v <= 1) this.audio.volume = v; } catch (e) { /* ignore */ }
      vol.value = this.audio.volume;
      vol.addEventListener("input", () => { this.audio.volume = +vol.value; this.audio.applyVolume(); try { localStorage.setItem("rp.volume", vol.value); } catch (e) { /* */ } });
      root.querySelector(".rp-bar").addEventListener("click", (e) => {
        const b = e.target.closest("button"); if (!b) return;
        const a = b.dataset.a;
        if (a === "pause") this.setPaused(!this.paused);
        if (a === "reset") { this.core.reset(); this.flash("Restarted"); }
        if (a === "fast") { this.fast = !this.fast; b.setAttribute("aria-pressed", String(this.fast)); }
        if (a === "mute") { this.audio.muted = !this.audio.muted; this.audio.applyVolume(); b.textContent = this.audio.muted ? "Unmute" : "Mute"; }
        if (a === "controls") this.showControls();
        if (a === "seat") this.setSeat((this.seat + 1) % (this.info.players || 1));
        if (a === "shot") this.downloadScreenshot();
        root.focus();
      });
      // keyboard (only while the game has focus, so typing elsewhere isn't captured)
      this.onKey = (e) => {
        if (!this.root.isConnected) return;
        const tag = (e.target && e.target.tagName) || "";
        if (/INPUT|TEXTAREA|SELECT/.test(tag)) return;
        const k = e.key.toLowerCase();
        if (e.type === "keydown") {
          if (k === "p" && !e.repeat) { this.setPaused(!this.paused); return; }
          if (k === "m" && !e.repeat) { this.audio.muted = !this.audio.muted; this.audio.applyVolume(); return; }
          if (k === " ") { this.holdFast = true; }
          this.keys.add(k);
        } else { this.keys.delete(k); if (k === " ") this.holdFast = false; }
        if (this.usesKey(k) || k === " ") e.preventDefault();
      };
      window.addEventListener("keydown", this.onKey);
      window.addEventListener("keyup", this.onKey);
      this.onBlur = () => { this.keys.clear(); this.holdFast = false; };
      window.addEventListener("blur", this.onBlur);
      this.ro = new ResizeObserver(() => this.resize());
      this.ro.observe(this.root);
      setTimeout(() => root.focus(), 50);
    }

    usesKey(k) { for (const n in this.map) if ((this.map[n].keys || []).includes(k)) return true; return false; }

    resize() {
      const r = this.root.getBoundingClientRect(), dpr = global.devicePixelRatio || 1;
      this.canvas.width = Math.max(1, Math.round(r.width * dpr));
      this.canvas.height = Math.max(1, Math.round(r.height * dpr));
      this.ctx2d.imageSmoothingEnabled = false;
      this.draw();
    }

    setPaused(p) {
      this.paused = p; this.root.classList.toggle("rp-is-paused", p);
      this.root.querySelector('[data-a="pause"]').textContent = p ? "▶ Play" : "⏸ Pause";
      if (p) { this.audio.clear(); if (this.core && this.core.rumble !== undefined) this.rumble(0); }
    }

    flash(text) {
      this.msgEl.textContent = text; this.msgEl.classList.add("on");
      clearTimeout(this.msgT); this.msgT = setTimeout(() => this.msgEl.classList.remove("on"), 1500);
    }

    seatKey() { return "rp.seat." + (this.core && this.core.gameId || this.system); }
    setSeat(n, quiet) {
      const players = this.info.players || 1;
      this.seat = n >= 0 && n < players ? n : 0;
      this.root.querySelector('[data-a="seat"]').textContent = "👤 Player " + (this.seat + 1);
      try { localStorage.setItem(this.seatKey(), String(this.seat)); } catch (e) { /* no storage */ }
      if (!quiet) this.flash("You are player " + (this.seat + 1));
    }

    // ---- input ----
    readInput() {
      const names = this.info.buttons;
      const pads = [...(navigator.getGamepads ? navigator.getGamepads() : [])].filter(Boolean);
      const players = this.info.players || 1;
      for (let p = 0; p < players; p++) {
        const gp = pads[p];
        let bits = 0;
        names.forEach((name, i) => {
          const m = this.map[name] || { pad: [], keys: [] };
          let on = false;
          if (gp) for (const b of m.pad || []) if (gp.buttons[b] && (gp.buttons[b].pressed || gp.buttons[b].value > 0.5)) on = true;
          if (p === 0) for (const k of m.keys || []) if (this.keys.has(k)) on = true;
          // D-pad directions also follow the left stick (unless the core reads the stick itself)
          const dir = DIRS.find(d => d[0] === name);
          if (dir && gp) {
            if (gp.buttons[dir[1]] && gp.buttons[dir[1]].pressed) on = true;
            const ax = gp.axes[dir[2]] || 0;
            if (!this.info.analogStick && (dir[3] < 0 ? ax < -0.5 : ax > 0.5)) on = true;
          }
          // switches that flip each press (Atari console switches)
          if (m.toggle) {
            const key = p + ":" + name;
            if (on && !this.prevPressed[key]) this.toggles[key] = !this.toggles[key];
            this.prevPressed[key] = on;
            on = !!this.toggles[key];
          }
          if (on) bits |= 1 << i;
        });
        // controller p plays as player p + seat (going round), so the keyboard can be any player
        const who = (p + (this.seat || 0)) % players;
        this.core.setInput(who, bits);
        // analog sticks for cores that use them (Atari paddles: left X; N64: both sticks)
        if (this.core.setAnalog) {
          const ax = gp ? gp.axes : [];
          this.core.setAnalog(who, ax[0] || 0, ax[1] || 0, ax[2] || 0, ax[3] || 0);
        }
      }
    }

    // ---- main loop ----
    loop(now) {
      if (this.destroyed) return;
      requestAnimationFrame(this.loop);
      const dt = Math.min(100, now - this.last); this.last = now;
      if (this.paused || this.modalOpen) return;
      this.readInput();
      const fast = this.fast || this.holdFast;
      let frames = 0;
      if (fast) frames = 4;
      else {
        this.acc += dt;
        // Keep the sound buffer around 50–120 ms: run a frame early or late as needed.
        const q = this.audio.secondsQueued();
        if (q < 0.035) this.acc += this.frameTime * 0.5;
        else if (q > 0.14) this.acc -= this.frameTime * 0.5;
        while (this.acc >= this.frameTime && frames < 4) { frames++; this.acc -= this.frameTime; }
        if (this.acc > this.frameTime * 4) this.acc = 0;
      }
      for (let i = 0; i < frames; i++) {
        try { this.core.runFrame(); } catch (e) { this.fail(e); return; }
        const a = this.core.takeAudio();
        if (!fast || i === 0) this.audio.push(a, !!this.info.stereo);
      }
      if (frames) this.draw();
      if (this.core.rumble !== undefined) this.rumble(fast ? 0 : this.core.rumble);
    }

    // Cartridges with a rumble motor shake the controller (where the browser allows it).
    rumble(level) {
      const gp = [...(navigator.getGamepads ? navigator.getGamepads() : [])].find(Boolean);
      const act = gp && gp.vibrationActuator;
      if (!act || !act.playEffect) return;
      const now = performance.now();
      if (level > 0.02) {
        if (now - (this.rumbleAt || 0) > 80 || Math.abs(level - (this.rumbleLevel || 0)) > 0.25) {
          act.playEffect("dual-rumble", { duration: 120, strongMagnitude: Math.min(1, level), weakMagnitude: Math.min(1, level * 0.6) }).catch(() => {});
          this.rumbleAt = now; this.rumbleLevel = level;
        }
      } else if (this.rumbleLevel) {
        this.rumbleLevel = 0;
        if (act.reset) act.reset().catch(() => {}); else act.playEffect("dual-rumble", { duration: 0, strongMagnitude: 0, weakMagnitude: 0 }).catch(() => {});
      }
    }

    fail(e) {
      this.destroyed = true;
      console.error(e);
      this.flash("The game stopped: " + e.message);
    }

    draw() {
      if (!this.core) return;
      const w = this.core.width, h = this.core.height;
      if (!this.img || this.img.width !== w || this.img.height !== h) {
        this.img = new ImageData(w, h);
        this.off = document.createElement("canvas"); this.off.width = w; this.off.height = h;
        this.offCtx = this.off.getContext("2d");
        this.img32 = new Uint32Array(this.img.data.buffer);
      }
      this.img32.set(this.core.screen.subarray(0, w * h));
      this.offCtx.putImageData(this.img, 0, 0);
      const cw = this.canvas.width, ch = this.canvas.height, aspect = this.info.aspect || w / h;
      let dw = cw, dh = Math.round(cw / aspect);
      if (dh > ch) { dh = ch; dw = Math.round(ch * aspect); }
      const dx = (cw - dw) >> 1, dy = (ch - dh) >> 1;
      const c = this.ctx2d;
      c.fillStyle = "#000"; c.fillRect(0, 0, cw, ch);
      c.imageSmoothingEnabled = false;
      c.drawImage(this.off, 0, 0, w, h, dx, dy, dw, dh);
      if (this.recCanvas) this.drawRec();
    }

    // ---- recording ----
    // A separate canvas holding only the game picture (no bars, no buttons or
    // messages), at the game's own shape, for the page's video recorder.
    recordingCanvas() {
      if (!this.core) return null;
      if (!this.recCanvas) {
        const w = this.core.width, h = this.core.height, aspect = this.info.aspect || w / h;
        const scale = Math.max(1, Math.round(720 / h));
        const rh = (h * scale) & ~1, rw = Math.round(rh * aspect) & ~1;
        this.recCanvas = document.createElement("canvas");
        this.recCanvas.width = rw; this.recCanvas.height = rh;
        this.recCtx = this.recCanvas.getContext("2d");
        this.drawRec();
      }
      return this.recCanvas;
    }
    stopRecordingCanvas() { this.recCanvas = null; this.recCtx = null; }
    drawRec() {
      if (!this.off) return;
      const c = this.recCtx, rw = this.recCanvas.width, rh = this.recCanvas.height;
      c.imageSmoothingEnabled = false;
      // If the game changes resolution mid-recording, it still fills the same video size.
      c.drawImage(this.off, 0, 0, this.off.width, this.off.height, 0, 0, rw, rh);
    }

    // ---- saves ----
    sramSnapshot() { const s = this.core && this.core.getSRAM ? this.core.getSRAM() : null; return s ? s.slice() : null; }
    checkSram() {
      const s = this.core && this.core.getSRAM ? this.core.getSRAM() : null;
      if (!s || !this.opts.onSram) return;
      const prev = this.lastSram;
      let same = prev && prev.length === s.length;
      if (same) for (let i = 0; i < s.length; i++) if (s[i] !== prev[i]) { same = false; break; }
      if (!same) { this.lastSram = s.slice(); this.opts.onSram(this.lastSram); }
    }
    saveState() { return RPState.encode({ system: this.system, state: this.core.saveState() }); }
    loadState(bytes) {
      const o = RPState.decode(bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes));
      if (o.system !== this.system) throw new Error("That save state is for a different system");
      this.core.loadState(o.state);
      this.audio.clear();
      this.draw();
    }
    screenshotPNG() {
      return new Promise((resolve) => this.canvas.toBlob(async (b) => resolve(new Uint8Array(await b.arrayBuffer())), "image/png"));
    }
    async downloadScreenshot() {
      const png = await this.screenshotPNG();
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([png], { type: "image/png" }));
      a.download = (this.opts.name || "screenshot") + " " + new Date().toISOString().slice(0, 19).replace(/[T:]/g, "-") + ".png";
      a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 5000);
      this.flash("Screenshot saved");
    }

    // ---- controls screen ----
    showControls() {
      this.modalOpen = true; this.audio.clear();
      const names = this.info.buttons;
      const modal = document.createElement("div");
      modal.className = "rp-modal";
      const render = () => {
        modal.innerHTML = `<div class="rp-box"><h3 style="margin:0 0 10px">Controls — ${this.info.name}</h3>
          <table>${names.map(n => {
            const m = this.map[n] || { pad: [], keys: [] };
            return `<tr><td>${n}</td><td>${(m.pad || []).map(b => PAD_NAMES[b]).join(", ") || "—"}</td><td>${(m.keys || []).map(k => k === " " ? "Space" : k.replace("arrow", "")).join(", ") || "—"}</td>
              <td><button data-set="${n}">Change</button></td></tr>`;
          }).join("")}</table>
          <p style="color:#8b90a0;font-size:12px;margin:10px 0">Click Change, then press a controller button or a key. Esc cancels. ${this.core && this.core.controlsNote ? this.core.controlsNote : this.info.analogStick
            ? "The left stick is the analog stick and the right stick works the C buttons. Clicking the sticks (L3/R3) is used for save states."
            : "The left stick always moves too. Clicking the sticks (L3/R3) and the right stick are used for save states."}</p>
          <div style="display:flex;gap:8px;justify-content:flex-end"><button data-reset>Reset to default</button><button data-close>Done</button></div></div>`;
      };
      render();
      let waiting = null, poll = null;
      const stopWait = () => { waiting = null; clearInterval(poll); window.removeEventListener("keydown", onKey, true); };
      const onKey = (e) => {
        e.preventDefault(); e.stopPropagation();
        if (e.key === "Escape") { stopWait(); render(); return; }
        this.map[waiting] = { pad: this.map[waiting].pad || [], keys: [e.key.toLowerCase()], toggle: this.map[waiting].toggle };
        saveMap(this.system, this.map); stopWait(); render();
      };
      modal.addEventListener("click", (e) => {
        const t = e.target;
        if (t.dataset.close !== undefined) { stopWait(); modal.remove(); this.modalOpen = false; this.root.focus(); return; }
        if (t.dataset.reset !== undefined) { try { localStorage.removeItem("rp.map." + this.system); } catch (x) { /* */ } this.map = loadMap(this.system); render(); return; }
        if (t.dataset.set) {
          stopWait(); waiting = t.dataset.set; t.textContent = "Press…"; t.classList.add("wait");
          window.addEventListener("keydown", onKey, true);
          const before = new Set();
          poll = setInterval(() => {
            const gp = [...(navigator.getGamepads ? navigator.getGamepads() : [])].find(Boolean);
            if (!gp) return;
            gp.buttons.forEach((b, i) => {
              const down = b.pressed || b.value > 0.5;
              if (down && !before.has(i) && i !== 10 && i !== 11 && waiting) {
                this.map[waiting] = { pad: [i], keys: this.map[waiting].keys || [], toggle: this.map[waiting].toggle };
                saveMap(this.system, this.map); stopWait(); render();
              }
              if (down) before.add(i); else before.delete(i);
            });
          }, 30);
        }
      });
      this.root.appendChild(modal);
    }

    destroy() {
      this.destroyed = true;
      if (this.core && this.core.rumble !== undefined) this.rumble(0);
      this.checkSram();
      clearInterval(this.sramTimer);
      window.removeEventListener("keydown", this.onKey);
      window.removeEventListener("keyup", this.onKey);
      window.removeEventListener("blur", this.onBlur);
      if (this.ro) this.ro.disconnect();
      if (this.core && this.core.close) this.core.close();
      this.audio.close();
    }
  }

  global.RPPlayer = Player;
  global.RPState = RPState;
})(window);
