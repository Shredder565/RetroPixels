// Runs the web Atari 7800 core (web/emu) in Node with scripted inputs and
// writes every frame's picture and sound to stdout, for jsref_test.go to
// compare with the Go core.
//
// usage: node jsref.js <web/emu dir> <rom> <inputs file> <frames> <sample rate>
// inputs file: per frame 12 bytes: u32 p0 bits, u32 p1 bits, u32 action
// (1 = power reset before the frame).
// output: u32 0, u32 length, the board ("flat", "supergame", ...); then per
// frame: u32 status (0 ok, 1 error: then u32 length + message and the run
// ends), u32 width, u32 height, width*height u32 pixels 0x00RRGGBB, u32 sample
// count, float32 samples; at the end u32 2, u32 length (0xffffffff: none) +
// the battery save.
"use strict";
const fs = require("fs"), path = require("path");
const [emu, romPath, inputsPath, framesArg, rateArg] = process.argv.slice(2);
for (const f of ["cpu6502.js", "atari2600.js", "atari7800.js"]) require(path.join(emu, f));

const out = fs.openSync("/dev/stdout", "w");
function write(buf) { let off = 0; while (off < buf.length) off += fs.writeSync(out, buf, off, buf.length - off); }
function u32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); write(b); }
function fail(e) {
  const msg = Buffer.from(String((e && e.message) || e));
  u32(1); u32(msg.length); write(msg);
  process.exit(0);
}

const rom = new Uint8Array(fs.readFileSync(romPath));
const inputs = fs.readFileSync(inputsPath);
const frames = +framesArg;
let core;
try { core = new globalThis.RPCores.atari7800(rom, { sampleRate: +rateArg }); } catch (e) { fail(e); }
{ const t = Buffer.from(core.cart.kind); u32(0); u32(t.length); write(t); }

for (let f = 0; f < frames; f++) {
  const o = f * 12;
  if (inputs.readUInt32LE(o + 8) === 1) core.reset();
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
const sram = core.getSRAM();
u32(2);
if (!sram) u32(0xffffffff); else { u32(sram.length); write(Buffer.from(sram)); }
