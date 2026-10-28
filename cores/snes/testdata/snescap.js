// Runs the web SNES core with the debug tracer on, for trace_test.go: the ROM
// for some frames, then writes the capture (capture.json and the files) to a
// folder, as web/emu/snes.js's debugCapture makes it.
// usage: node snescap.js <emuDir> <rom> <frames> <outDir>
"use strict";
const fs = require("fs"), path = require("path");
const [emuDir, romPath, framesS, outDir] = process.argv.slice(2);
for (const f of ["w65816.js", "spc700.js", "sdsp.js", "snes_sa1.js", "snes_superfx.js", "snes.js", "snes_chips.js", "snes_necdsp.js", "snes_dsp.js", "snes_cx4.js", "snes_sdd1.js", "snes_spc7110.js", "snes_misc.js", "arm7.js", "snes_st018.js"])
  (0, eval)(fs.readFileSync(path.join(emuDir, f), "utf8"));
const snes = new globalThis.RPCores.snes(new Uint8Array(fs.readFileSync(romPath)), { sampleRate: 48000 });
snes.enableTrace();
for (let f = 0; f < +framesS; f++) { snes.runFrame(); snes.takeAudio(); }
const cap = snes.debugCapture();
fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, "capture.json"), JSON.stringify(cap.meta));
for (const [n, b] of Object.entries(cap.files)) fs.writeFileSync(path.join(outDir, n), b);
