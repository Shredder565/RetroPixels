// Runs the web Genesis core (web/emu) in Node with scripted inputs and writes
// every frame's picture and sound to stdout, for jsref_test.go to compare
// with the Go core.
//
// usage: node jsref.js <web/emu dir> <rom> <inputs file> <frames> <sample rate> [sram file]
// inputs file: per frame 20 bytes: u32 bits for pads 1-4, u32 action
// (1 = the player's Restart button, before the frame).
// output: u32 0, u32 length, a description of the cartridge (region, mapper,
// EEPROM, J-Cart, 3-button); then per frame: u32 status (0 ok, 1 error: then
// u32 length + message and the run ends), u32 width, u32 height,
// width*height u32 pixels 0x00RRGGBB, u32 sample count, float32 samples; at
// the end u32 2, u32 length (0xffffffff: none) + the battery save.
"use strict";
const fs = require("fs"), path = require("path");
const [emu, romPath, inputsPath, framesArg, rateArg, sramPath] = process.argv.slice(2);
for (const f of ["m68k.js", "z80.js", "ym2612.js", "sn76489.js", "genesis_cart.js", "genesis.js"]) require(path.join(emu, f));

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
const sram = sramPath ? new Uint8Array(fs.readFileSync(sramPath)) : null;
let core;
try { core = new globalThis.RPCores.segaMD(rom, { sampleRate: +rateArg, sram, name: path.basename(romPath) }); } catch (e) { fail(e); }
{
  const c = core.cart;
  const ee = c.eeprom ? `ee${c.eeprom.mem.length}/${c.eeprom.mode}` : "noee";
  const t = Buffer.from(`${core.region} ${c.mapper || "none"} ${ee} jcart=${c.jcart ? 1 : 0} three=${c.threeButton ? 1 : 0} sram=${c.sramStart.toString(16)}-${c.sramEnd.toString(16)}`);
  u32(0); u32(t.length); write(t);
}

for (let f = 0; f < frames; f++) {
  const o = f * 20;
  if (inputs.readUInt32LE(o + 16) === 1) core.reset();
  for (let p = 0; p < 4; p++) core.setInput(p, inputs.readUInt32LE(o + p * 4));
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
