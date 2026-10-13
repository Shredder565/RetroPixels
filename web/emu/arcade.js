// RetroPixels — Arcade: the framework that runs classic arcade boards. Original code.
//
// An arcade game comes as a .zip "ROM set": one file per chip on the game's
// circuit board (program ROMs, graphics ROMs, colour PROMs...). This file
// finds which game a set is (by the files' names, with their sizes as a check),
// builds the right board, turns its picture upright for vertical-monitor
// games, and gives the RetroPixels player (host.js) the same core interface the
// consoles use. The boards themselves live in emu/arcade_<board>.js.
//
// ===========================================================================
// BOARD INTERFACE (what an emu/arcade_<board>.js file provides)
// ===========================================================================
// Start the file with
//     global.RPArcade = global.RPArcade || { boards: [] };
// and end it with
//     global.RPArcade.boards.push(MyBoard);
// so it works whether it loads before or after this file.
//
// Static fields of the board class:
//   id        "pacman"            short name of the board family
//   title     "Namco Pac-Man"     shown to the player
//   width, height                 the board's picture in pixels, as the board draws it
//                                 (the monitor's own scan direction, before rotation)
//   rotate    0 | 90 | 180 | 270  how far to turn the picture clockwise so it shows
//                                 upright (vertical-monitor games are usually 90)
//   aspect    optional, the shape of the monitor for the unrotated picture (default 4/3)
//   fps       frames per second of the real board
//   buttons   the controls, in order, using names from RPArcade.BUTTON_NAMES
//             (the directions plus e.g. "Fire", "Coin", "Start (1P)", "Start (2P)").
//             Always include "Coin", "Start (1P)" and "Start (2P)".
//   dips      DIP switch settings: [{ id, label, options: [{ label, value }], default }]
//             (value: any number/string the board understands; default: one of the values)
//   games     the games this board runs:
//             [{ id: "pacman", title: "Pac-Man", year: 1980, maker: "Namco (Midway license)",
//                parent: "puckman" (optional: the set it's a clone of, for messages),
//                files: [{ name: "pacman.6e", size: 4096, role: "cpu", offset: 0,
//                          alt: ["other-name.6e"] (optional other names for the same chip),
//                          optional: true (optional: the board can run without it) }, ...],
//                rotate / width / height / fps / dips / buttons (optional per-game overrides) }]
//             Files are found by NAME (case doesn't matter, folders are ignored) and
//             checked by SIZE. Write these lists yourself; no checksum databases.
//
// Instance (new MyBoard(game, files, opts)):
//   game      the entry from `games` that was picked
//   files     Map: lower-case file name -> Uint8Array (every file in the zip).
//             RPArcade.region(game, files, "cpu", 0x4000) gathers all files of one
//             role into one array at their offsets (alt names handled).
//   opts      { sampleRate, dips, sram }
//             dips: { id: value } for every setting. The framework changes values in this
//                   same object when the player changes a setting, so read it when the
//                   game reads its switches (or on reset).
//             sram: the bytes getSRAM() returned last time (or null).
//   screen    Uint32Array(width * height), filled by runFrame(); pixels as
//             RPArcade.rgb(r, g, b) makes them (0xAABBGGRR, like every RetroPixels core).
//   runFrame()        run one video frame and draw it into `screen`
//   setInput(p1, p2)  p1 / p2: { buttonName: true/false } for the first and second
//                     controller (p2 is all false with one controller). Upright cabinets
//                     where the players take turns usually only read p1.
//   takeAudio()       mono Float32Array of the samples made since the last call, at
//                     opts.sampleRate (RPArcade.Resampler helps with this)
//   reset()           the board's reset (the player's Restart button)
//   saveState() / loadState(o)  plain object with numbers and typed arrays
//   getSRAM()         optional: battery-backed RAM / high-score NVRAM, or null
//
// Helpers in RPArcade: rgb(r, g, b), region(...), Resampler, rotateInto(...).
//
// ===========================================================================
// FOR THE PAGE
// ===========================================================================
//   RPArcade.identify(files, zipName) -> { Board, game } or null
//       files: Map (or plain object) name -> Uint8Array. A zip named after a game
//       (pacman.zip) picks that game even if files are missing, so the error can say
//       which ones; otherwise the game whose files are all there is picked.
//   RPArcade.supportedText() -> "Pac-Man (pacman.zip), Galaxian (galaxian.zip), ..."
//   new RPCores.arcade(zipBytes, { files, zipName, dips, sampleRate, sram })
//       core.dips -> [{ id, label, options, default, value }]; core.setDip(id, value)
//       core.gameId / core.gameTitle
"use strict";

(function (global) {
  const RPArcade = global.RPArcade = global.RPArcade || { boards: [] };
  if (!RPArcade.boards) RPArcade.boards = [];

  // Control names boards may use (the player's controller/keyboard defaults are set up for these).
  RPArcade.BUTTON_NAMES = ["Up", "Down", "Left", "Right", "Fire", "Jump", "Button 1", "Button 2", "Button 3", "Button 4",
    "Coin", "Coin 2", "Start (1P)", "Start (2P)", "Service"];

  RPArcade.rgb = (r, g, b) => (0xff000000 | ((b & 255) << 16) | ((g & 255) << 8) | (r & 255)) >>> 0;

  const baseName = (n) => String(n).split(/[\\/]/).pop().toLowerCase();

  // Map of lower-case base names -> bytes, from a Map, a plain object or an array of { name, data }.
  function normFiles(files) {
    const out = new Map();
    if (!files) return out;
    const put = (k, v) => { const b = baseName(k); if (b && !out.has(b)) out.set(b, v); };
    if (files instanceof Map) for (const [k, v] of files) put(k, v);
    else if (Array.isArray(files)) for (const f of files) put(f.name, f.data);
    else for (const k of Object.keys(files)) put(k, files[k]);
    return out;
  }
  RPArcade.normFiles = normFiles;

  // the file in the set for one entry of a game's list (its own name or one of its other names)
  function findFile(entry, files) {
    for (const n of [entry.name, ...(entry.alt || [])]) { const d = files.get(n.toLowerCase()); if (d) return d; }
    return null;
  }
  RPArcade.findFile = findFile;

  // What's missing or the wrong size for one game.
  function check(game, files) {
    const missing = [], wrongSize = [];
    let found = 0;
    for (const f of game.files) {
      const d = findFile(f, files);
      if (!d) { if (!f.optional) missing.push(f.name); continue; }
      if (d.length !== f.size) { if (!f.optional) wrongSize.push({ name: f.name, size: f.size, actual: d.length }); continue; }
      found++;
    }
    return { missing, wrongSize, found, ok: !missing.length && !wrongSize.length };
  }
  RPArcade.check = check;

  // All files of one role, placed at their offsets (in list order when no offset is given).
  RPArcade.region = function (game, files, role, size) {
    files = files instanceof Map ? files : normFiles(files);
    const parts = game.files.filter((f) => f.role === role);
    let end = 0, pos = 0;
    const placed = parts.map((f) => { const at = f.offset !== undefined ? f.offset : pos; pos = at + f.size; end = Math.max(end, pos); return at; });
    const out = new Uint8Array(Math.max(size || 0, end)).fill(0xff);
    parts.forEach((f, i) => { const d = findFile(f, files); if (d) out.set(d.subarray(0, Math.min(d.length, out.length - placed[i])), placed[i]); });
    return out;
  };

  function allGames() {
    const list = [];
    for (const Board of RPArcade.boards) for (const game of Board.games || []) list.push({ Board, game });
    return list;
  }
  RPArcade.allGames = allGames;

  RPArcade.identify = function (files, zipName) {
    files = normFiles(files);
    const zip = baseName(zipName || "").replace(/\.[^.]*$/, "");
    const games = allGames();
    if (zip) { const hit = games.find((g) => g.game.id === zip); if (hit) return hit; }
    let best = null, bestFound = 0;
    for (const g of games) {
      const c = check(g.game, files);
      if (c.ok && c.found > bestFound) { best = g; bestFound = c.found; }
    }
    return best;
  };

  RPArcade.supportedText = function () {
    return allGames().map((g) => `${g.game.title} (${g.game.id}.zip)`).join(", ") || "none yet";
  };

  // The friendly error when a set can't be used.
  function setError(game, files, zipName) {
    const c = check(game, files);
    const zip = zipName ? baseName(zipName) : game.id + ".zip";
    const parts = [];
    if (c.missing.length) parts.push(`${zip} is missing ${c.missing.length === 1 ? "this chip file" : "these chip files"} for ${game.title}: ${c.missing.join(", ")}.`);
    if (c.wrongSize.length) parts.push(`${c.wrongSize.length === 1 ? "This file is" : "These files are"} the wrong size: ` +
      c.wrongSize.map((w) => `${w.name} (${w.actual.toLocaleString()} bytes, should be ${w.size.toLocaleString()})`).join(", ") + ".");
    if (game.parent) parts.push(`${game.title} is a version of the game in ${game.parent}.zip, so some of its chips may be in that set: use a full (non-merged) ROM set, or add the missing files into ${zip}.`);
    else parts.push("Use a complete ROM set for this game.");
    return new Error(parts.join(" "));
  }

  // ===================================================================
  // Rotation
  // ===================================================================
  // Copy a w*h picture into dst turned clockwise by rot degrees.
  RPArcade.rotateInto = function (src, w, h, rot, dst) {
    if (rot === 90) {
      for (let y = 0; y < h; y++) { const row = y * w, col = h - 1 - y; for (let x = 0; x < w; x++) dst[x * h + col] = src[row + x]; }
    } else if (rot === 270) {
      for (let y = 0; y < h; y++) { const row = y * w; for (let x = 0; x < w; x++) dst[(w - 1 - x) * h + y] = src[row + x]; }
    } else if (rot === 180) {
      const n = w * h; for (let i = 0; i < n; i++) dst[n - 1 - i] = src[i];
    } else dst.set(src.subarray(0, w * h));
  };

  // ===================================================================
  // Sound helper: native-rate samples in, the player's rate out
  // ===================================================================
  // add(v) one sample at the board's own rate; take() the samples at the output rate,
  // averaged down (or stretched up), with the steady (DC) part removed.
  class Resampler {
    constructor(inRate, outRate) {
      this.ratio = inRate / outRate; this.buf = new Float32Array(4096); this.n = 0;
      this.acc = 0; this.cnt = 0; this.pos = 0; this.last = 0; this.hp = 0; this.hpPrev = 0;
    }
    push(v) {
      // remove DC so silence sits at zero
      this.hp = 0.9985 * (this.hp + v - this.hpPrev); this.hpPrev = v;
      if (this.n >= this.buf.length) { const b = new Float32Array(this.buf.length * 2); b.set(this.buf); this.buf = b; }
      this.buf[this.n++] = this.hp;
    }
    add(v) {
      if (this.ratio >= 1) {
        this.acc += v; this.cnt++; this.pos += 1;
        if (this.pos >= this.ratio) { this.push(this.acc / this.cnt); this.pos -= this.ratio; this.acc = 0; this.cnt = 0; }
      } else {
        // more output samples than input ones: step between the last two inputs
        while (this.pos < 1) { this.push(this.last + (v - this.last) * this.pos); this.pos += this.ratio; }
        this.pos -= 1; this.last = v;
      }
    }
    take() { const out = this.buf.slice(0, this.n); this.n = 0; return out; }
    clear() { this.n = 0; }
  }
  RPArcade.Resampler = Resampler;

  // ===================================================================
  // The core the player runs
  // ===================================================================
  const DEFAULT_BUTTONS = ["Up", "Down", "Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)"];

  class Arcade {
    // rom: the zip's bytes (unused when opts.files is given); opts.files: Map name -> bytes
    constructor(rom, opts) {
      opts = opts || {};
      const files = normFiles(opts.files);
      const zipName = opts.zipName || (opts.name ? opts.name + ".zip" : "");
      const hit = RPArcade.identify(files, zipName);
      if (!hit) throw new Error(`RetroPixels doesn't know this arcade game yet. The games it can play are: ${RPArcade.supportedText()}. Load the game's .zip ROM set (keep it zipped).`);
      const { Board, game } = hit;
      if (!check(game, files).ok) throw setError(game, files, zipName);
      this.Board = Board; this.game = game;
      this.gameId = game.id; this.gameTitle = game.title;

      // DIP switches: defaults, then the player's own choices
      const dipList = game.dips || Board.dips || [];
      this.dipValues = {};
      for (const d of dipList) {
        let v = d.default;
        if (opts.dips && opts.dips[d.id] !== undefined && d.options.some((o) => o.value === opts.dips[d.id])) v = opts.dips[d.id];
        this.dipValues[d.id] = v;
      }
      this.dipList = dipList;

      this.board = new Board(game, files, { sampleRate: opts.sampleRate || 48000, dips: this.dipValues, sram: opts.sram || null });
      this.bw = game.width || Board.width; this.bh = game.height || Board.height;
      this.rot = ((game.rotate !== undefined ? game.rotate : Board.rotate) || 0) % 360;
      const side = this.rot === 90 || this.rot === 270;
      this.width = side ? this.bh : this.bw; this.height = side ? this.bw : this.bh;
      const a = game.aspect || Board.aspect || 4 / 3;
      this.aspect = side ? 1 / a : a;
      this.fps = game.fps || Board.fps || 60;
      this.buttons = (game.buttons || Board.buttons || DEFAULT_BUTTONS).slice();
      this.screen = this.rot ? new Uint32Array(this.width * this.height) : this.board.screen;
      this.controlsNote = "Put a coin in with Coin (View button, or 5 on the keyboard), then press Start (Menu button, or 1). " +
        "The left stick moves too. Clicking the sticks (L3/R3) and the right stick are used for save states.";
      this.inputs = [{}, {}];
      for (const p of this.inputs) for (const b of this.buttons) p[b] = false;
      this.updateScreen();
    }

    get dips() { return this.dipList.map((d) => Object.assign({}, d, { value: this.dipValues[d.id] })); }
    setDip(id, value) {
      const d = this.dipList.find((x) => x.id === id);
      if (!d || !d.options.some((o) => o.value === value)) return false;
      this.dipValues[id] = value;
      return true;
    }

    updateScreen() {
      if (this.rot) RPArcade.rotateInto(this.board.screen, this.bw, this.bh, this.rot, this.screen);
      else if (this.screen !== this.board.screen) this.screen = this.board.screen;
    }
    runFrame() { this.board.runFrame(); this.updateScreen(); }
    takeAudio() { return this.board.takeAudio ? this.board.takeAudio() : new Float32Array(0); }
    setInput(p, bits) {
      if (p > 1) return;
      const st = this.inputs[p];
      this.buttons.forEach((b, i) => { st[b] = !!(bits & (1 << i)); });
      this.board.setInput(this.inputs[0], this.inputs[1]);
    }
    reset() { this.board.reset(); }
    getSRAM() { return this.board.getSRAM ? this.board.getSRAM() : null; }
    saveState() { return { v: 1, game: this.game.id, board: this.board.saveState() }; }
    loadState(s) {
      if (!s || s.game !== this.game.id) throw new Error("That save state is for a different arcade game");
      this.board.loadState(s.board);
      this.updateScreen();
    }
  }

  Arcade.info = { system: "arcade", name: "Arcade", width: 224, height: 288, players: 2, buttons: DEFAULT_BUTTONS };

  global.RPCores = global.RPCores || {};
  global.RPCores.arcade = Arcade;
})(typeof window !== "undefined" ? window : globalThis);
