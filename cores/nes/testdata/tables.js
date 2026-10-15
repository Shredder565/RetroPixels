// Prints the web NES core's computed tables (palette, sound chip tables) as
// JSON, for TestTablesMatchJS. usage: node tables.js <emuDir>
"use strict";
const fs = require("fs"), path = require("path");
const emu = process.argv[2];
const load = (f, patch) => { let s = fs.readFileSync(path.join(emu, f), "utf8"); for (const [a, b] of patch) { if (!s.includes(a)) throw new Error("missing " + a); s = s.replace(a, b); } (0, eval)(s); };
load("nes.js", [["const PALETTE = buildPalette();", "const PALETTE = buildPalette(); globalThis.__T = { palette: Array.from(PALETTE, (c) => ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255)) };"]]);
load("nes_audio.js", [["global.RPNesAudio = {", "Object.assign(globalThis.__T, { vol5b: Array.from(VOL5B), ksl: KSL_DB, sin: Array.from(SIN), dbToAmp: Array.from(DB_TO_AMP), decay: Array.from(DECAY), attack: Array.from(ATTACK) }); global.RPNesAudio = {"]]);
// Exact bits of the doubles, so nothing is lost in printing.
const bits = (a) => a.map((x) => { const b = Buffer.alloc(8); b.writeDoubleLE(x); return b.toString("hex"); });
const T = globalThis.__T;
process.stdout.write(JSON.stringify({ palette: T.palette, vol5b: bits(T.vol5b), ksl: bits(T.ksl), sin: bits(T.sin), dbToAmp: bits(T.dbToAmp), decay: bits(T.decay), attack: bits(T.attack) }));
