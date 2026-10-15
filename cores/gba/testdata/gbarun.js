// Runs the web Game Boy Advance core (web/emu/arm7.js, gba_gpio.js, gba.js)
// headless for compare_test.go: the same ROM, battery save and scripted inputs
// as the Go port, writing per frame a hash of the picture, the rumble level
// and the audio samples, and the battery save at the end.
// usage: node gbarun.js <emuDir> <rom> <script.bin> <out.bin> <sram|-> [dumpFrame dumpFile]
// script.bin: per frame 32 bytes: u32 buttons, u32 flags (1 = reset first),
// f64 right stick x, f64 right stick y, f64 the computer's clock (ms).
// Run it with TZ=UTC (the Go side reads the clock in UTC).
"use strict";
const fs = require("fs"), path = require("path");
const [emuDir, romPath, scriptPath, outPath, sramPath, dumpS, dumpPath] = process.argv.slice(2);
const script = fs.readFileSync(scriptPath);
const frames = script.length / 32;
let wall = script.length ? script.readDoubleLE(24) : 0;
// the cartridge clock reads the computer's clock (new Date())
const RealDate = Date;
globalThis.Date = class extends RealDate {
  constructor(...a) { if (a.length) super(...a); else super(wall); }
  static now() { return wall; }
};
for (const f of ["arm7.js", "gba_gpio.js", "gba.js"]) (0, eval)(fs.readFileSync(path.join(emuDir, f), "utf8"));
const GBA = globalThis.RPCores.gba;
const rom = new Uint8Array(fs.readFileSync(romPath));
const sram = sramPath && sramPath !== "-" ? new Uint8Array(fs.readFileSync(sramPath)) : null;
let g;
try { g = new GBA(rom, { sampleRate: 48000, sram }); } catch (e) { fs.writeFileSync(outPath, "ERR " + e.message); process.exit(0); }
const dump = dumpS === undefined ? -1 : +dumpS;
const chunks = [];
const hashOf = (s) => {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) {
    const c = s[i], rgb = ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
    h = Math.imul(h ^ rgb, 16777619) >>> 0;
  }
  return h;
};
let error = "";
for (let f = 0; f < frames; f++) {
  const o = f * 32;
  const bits = script.readUInt32LE(o), flags = script.readUInt32LE(o + 4);
  wall = script.readDoubleLE(o + 24);
  let a;
  try {
    if (flags & 1) g.reset();
    g.setInput(0, bits);
    g.setAnalog(0, 0, 0, script.readDoubleLE(o + 8), script.readDoubleLE(o + 16));
    g.runFrame();
    a = g.takeAudio();
  } catch (e) { error = `frame ${f}: ${e.message}`; break; }
  const s = g.screen;
  if (f === dump) {
    const b = Buffer.alloc(s.length * 4);
    for (let i = 0; i < s.length; i++) { const c = s[i]; b.writeUInt32LE((((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255)) >>> 0, i * 4); }
    fs.writeFileSync(dumpPath, b);
  }
  const rum = g.rumble === undefined ? NaN : g.rumble;
  const hdr = Buffer.alloc(16); hdr.writeUInt32LE(hashOf(s), 0); hdr.writeUInt32LE(a.length, 4); hdr.writeDoubleLE(rum, 8);
  chunks.push(hdr, Buffer.from(a.buffer, a.byteOffset, a.length * 4));
}
const end = Buffer.alloc(16); end.writeUInt32LE(0xffffffff, 0); end.writeUInt32LE(0xffffffff, 4); chunks.push(end);
const out = g.getSRAM();
const tail = Buffer.alloc(4); tail.writeUInt32LE(out ? out.length : 0xffffffff, 0);
chunks.push(tail); if (out) chunks.push(Buffer.from(out));
const info = Buffer.from(JSON.stringify({ save: g.save.type, rtc: !!g.rtc, kind: g.gpio ? g.gpio.kind : null,
  buttons: g.buttons || GBA.info.buttons, stick: g.usesRightStick, note: g.controlsNote || "", error }));
const il = Buffer.alloc(4); il.writeUInt32LE(info.length, 0);
chunks.push(il, info);
fs.writeFileSync(outPath, Buffer.concat(chunks));
