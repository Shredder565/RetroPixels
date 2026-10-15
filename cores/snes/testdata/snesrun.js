// Runs the web SNES core (web/emu/snes.js and the scripts web/index.html
// loads with it) headless for compare_test.go: the same ROM, battery save,
// chip programs and scripted inputs as the Go port, writing per frame a hash
// of the picture, its size and the audio samples, then the battery save.
// usage: node snesrun.js <emuDir> <rom> <script.bin> <out.bin> <sram|-> <firmwareDir|-> [dumpFrame dumpFile]
// script.bin: per frame 24 bytes: u32 player 1 buttons, u32 player 2 buttons,
// u32 flags (1 = reset first), u32 0, f64 the computer's clock (ms).
"use strict";
const fs = require("fs"), path = require("path");
const [emuDir, romPath, scriptPath, outPath, sramPath, fwDir, dumpS, dumpPath] = process.argv.slice(2);
const script = fs.readFileSync(scriptPath);
const frames = script.length / 24;
let wall = script.length ? script.readDoubleLE(16) : 0;
Date.now = () => wall; // the cartridge clocks read the computer's clock
for (const f of ["w65816.js", "spc700.js", "sdsp.js", "snes_sa1.js", "snes_superfx.js", "snes.js", "snes_chips.js", "snes_necdsp.js", "snes_dsp.js", "snes_cx4.js", "snes_sdd1.js", "snes_spc7110.js", "snes_misc.js", "arm7.js", "snes_st018.js"])
  (0, eval)(fs.readFileSync(path.join(emuDir, f), "utf8"));
const SNES = globalThis.RPCores.snes;
const rom = new Uint8Array(fs.readFileSync(romPath));
const sram = sramPath && sramPath !== "-" ? new Uint8Array(fs.readFileSync(sramPath)) : null;
const firmware = {};
if (fwDir && fwDir !== "-") for (const f of fs.readdirSync(fwDir)) firmware[f] = new Uint8Array(fs.readFileSync(path.join(fwDir, f)));
let snes;
try { snes = new SNES(rom, { sampleRate: 48000, sram, firmware }); } catch (e) {
  fs.writeFileSync(outPath, "ERR " + (e.needsFirmware ? "FW " + e.needsFirmware.chip + " " : "") + e.message); process.exit(0);
}
const dump = dumpS === undefined ? -1 : +dumpS;
const chunks = [];
const conv = (c) => (((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255)) >>> 0;
for (let f = 0; f < frames; f++) {
  const o = f * 24;
  const b0 = script.readUInt32LE(o), b1 = script.readUInt32LE(o + 4), flags = script.readUInt32LE(o + 8);
  wall = script.readDoubleLE(o + 16);
  if (flags & 1) snes.reset();
  snes.setInput(0, b0); snes.setInput(1, b1);
  snes.runFrame();
  const a = snes.takeAudio();
  const s = snes.screen, n = snes.width * snes.height;
  let h = 2166136261;
  for (let i = 0; i < n; i++) h = Math.imul(h ^ conv(s[i]), 16777619) >>> 0;
  if (f === dump) {
    const b = Buffer.alloc(n * 4);
    for (let i = 0; i < n; i++) b.writeUInt32LE(conv(s[i]), i * 4);
    fs.writeFileSync(dumpPath, b);
  }
  const hdr = Buffer.alloc(16); hdr.writeUInt32LE(h, 0); hdr.writeUInt32LE(a.length, 4); hdr.writeUInt32LE(snes.width, 8); hdr.writeUInt32LE(snes.height, 12);
  chunks.push(hdr, Buffer.from(a.buffer, a.byteOffset, a.length * 4));
}
const out = snes.getSRAM();
const tail = Buffer.alloc(4); tail.writeUInt32LE(out ? out.length : 0xffffffff, 0);
chunks.push(tail); if (out) chunks.push(Buffer.from(out));
const info = Buffer.from(JSON.stringify({ chip: snes.chip || "", coproc: snes.coproc ? snes.coproc.constructor.name : "", map: snes.cart.map, pal: !!snes.pal, title: snes.cart.title, sramSize: snes.cart.sramSize }));
const il = Buffer.alloc(4); il.writeUInt32LE(info.length, 0);
chunks.push(il, info);
fs.writeFileSync(outPath, Buffer.concat(chunks));
