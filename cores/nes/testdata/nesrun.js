// Runs the web NES core (web/emu) headless for compare_test.go: the same ROM
// and scripted inputs as the Go port, writing per frame a hash of the picture
// and the audio samples.
// usage: node nesrun.js <emuDir> <rom> <frames> <out.bin> [dumpFrame dumpFile]
"use strict";
const fs = require("fs"), path = require("path");
const [emuDir, romPath, framesS, outPath, dumpS, dumpPath] = process.argv.slice(2);
for (const f of ["cpu6502.js", "nes.js", "nes_audio.js", "nes_mappers.js"]) (0, eval)(fs.readFileSync(path.join(emuDir, f), "utf8"));
const NES = globalThis.RPCores.nes;
const rom = new Uint8Array(fs.readFileSync(romPath));
let nes;
try { nes = new NES(rom, { sampleRate: 48000 }); } catch (e) { fs.writeFileSync(outPath, "ERR " + e.message); process.exit(0); }
let seed = 0x1234567;
const rnd = () => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed; };
const frames = +framesS, dump = dumpS === undefined ? -1 : +dumpS;
const chunks = [];
let p1 = 0, p2 = 0;
for (let f = 0; f < frames; f++) {
  // Scripted input (same as scriptInput in compare_test.go).
  const r = rnd();
  if (f % 8 === 0) {
    p1 = r & 0xf7; if (((r >>> 8) & 15) === 0) p1 |= 8;
    p2 = (r >>> 16) & 0xff;
  }
  if (f < 120) p1 = (f >= 60 && f < 70) ? 8 : 0; // let the game boot, then press Start
  nes.setInput(0, p1); nes.setInput(1, p2);
  if (f === Math.floor(frames / 2)) nes.reset();
  nes.runFrame();
  const a = nes.takeAudio();
  const s = nes.screen;
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) {
    const c = s[i], rgb = ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
    h = Math.imul(h ^ rgb, 16777619) >>> 0;
  }
  if (f === dump) {
    const b = Buffer.alloc(s.length * 4);
    for (let i = 0; i < s.length; i++) { const c = s[i]; b.writeUInt32LE((((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255)) >>> 0, i * 4); }
    fs.writeFileSync(dumpPath, b);
  }
  const hdr = Buffer.alloc(8); hdr.writeUInt32LE(h, 0); hdr.writeUInt32LE(a.length, 4);
  chunks.push(hdr, Buffer.from(a.buffer, a.byteOffset, a.length * 4));
}
const sram = nes.getSRAM();
const tail = Buffer.alloc(4); tail.writeUInt32LE(sram ? sram.length : 0xffffffff, 0);
chunks.push(tail); if (sram) chunks.push(Buffer.from(sram));
fs.writeFileSync(outPath, Buffer.concat(chunks));
