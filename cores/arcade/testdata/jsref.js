// Runs the web arcade core (web/emu/arcade.js and its boards) in Node with
// scripted inputs and writes every frame's picture and sound to stdout, for
// compare_test.go to compare with the Go core.
//
// usage: node jsref.js <web/emu dir> <set dir> <zip name> <inputs file> <frames> <sample rate> [dips json]
// The set dir holds the set's files plus "order.txt", their names in zip order.
// inputs file: per frame 16 bytes: u32 p0 bits, u32 p1 bits, u32 action
// (bit 0: reset before the frame; bit 1: change a setting: arg = dip index << 8 | option index),
// u32 arg.
// output: u32 status (0 ok; 1 error: u32 length + message, and the run ends),
// then u32 length + "gameId width height"; per frame: u32 0, u32 width,
// u32 height, the pixels as 0x00RRGGBB, u32 sample count, float32 samples;
// at the end u32 2, u32 length (0xffffffff: none) + the battery save.
"use strict";
const fs = require("fs"), path = require("path");
const [emu, setDir, zipName, inputsPath, framesArg, rateArg, dipsArg] = process.argv.slice(2);
for (const f of ["z80.js", "i8080.js", "mcs48.js", "arcade.js", "arcade_pacman.js", "arcade_galaxian.js", "arcade_invaders.js", "arcade_dkong.js"])
  require(path.join(emu, f));

const out = fs.openSync("/dev/stdout", "w");
function write(buf) { let off = 0; while (off < buf.length) off += fs.writeSync(out, buf, off, buf.length - off); }
function u32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); write(b); }
function str(s) { const b = Buffer.from(s); u32(b.length); write(b); }
function fail(e) { u32(1); str(String((e && e.message) || e)); process.exit(0); }

const files = new Map();
for (const n of fs.readFileSync(path.join(setDir, "order.txt"), "utf8").split("\n").filter(Boolean))
  files.set(n, new Uint8Array(fs.readFileSync(path.join(setDir, n.replace(/[\\/]/g, "__")))));
const inputs = inputsPath === "-" ? Buffer.alloc(0) : fs.readFileSync(inputsPath);
const frames = +framesArg;
let core;
try {
  core = new globalThis.RPCores.arcade(null, { files, zipName, sampleRate: +rateArg, dips: dipsArg ? JSON.parse(dipsArg) : undefined });
} catch (e) { fail(e); }
u32(0); str(`${core.gameId} ${core.width} ${core.height}`);

for (let f = 0; f < frames; f++) {
  const o = f * 16, act = inputs.readUInt32LE(o + 8), arg = inputs.readUInt32LE(o + 12);
  if (act & 1) core.reset();
  if (act & 2) { const d = core.dips[arg >> 8]; if (d) core.setDip(d.id, d.options[(arg & 255) % d.options.length].value); }
  core.setInput(0, inputs.readUInt32LE(o));
  core.setInput(1, inputs.readUInt32LE(o + 4));
  try { core.runFrame(); } catch (e) { fail(e); }
  const w = core.width, h = core.height, scr = core.screen;
  const px = new Uint32Array(w * h);
  for (let i = 0; i < px.length; i++) { const c = scr[i]; px[i] = ((c & 0xff) << 16) | (c & 0xff00) | ((c >>> 16) & 0xff); }
  const aud = core.takeAudio();
  const head = Buffer.alloc(12); head.writeUInt32LE(0, 0); head.writeUInt32LE(w, 4); head.writeUInt32LE(h, 8);
  write(head);
  write(Buffer.from(px.buffer));
  u32(aud.length);
  write(Buffer.from(new Float32Array(aud).buffer));
}
const save = core.getSRAM();
u32(2);
if (!save) u32(0xffffffff); else { u32(save.length); write(Buffer.from(save)); }
