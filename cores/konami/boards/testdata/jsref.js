// Runs the web arcade core (web/emu) with the Konami boards in Node with
// scripted inputs and writes every frame's picture and sound to stdout, for
// jsref_test.go to compare with the Go boards.
//
// usage: node jsref.js <web/emu dir> <zip> <inputs file> <frames> <sample rate> [sram file]
// inputs file: per frame 20 bytes: u32 bits for players 1-4, u32 action
// (1 = the player's Restart button, before the frame).
// output: u32 0 (or 1 + message: refused), u32 length, "<game id> <players> <width>x<height>";
// then per frame: u32 status (0 ok, 1 error: then u32 length + message and the run
// ends), u32 width, u32 height, width*height u32 pixels 0x00RRGGBB, u32 sample count,
// float32 samples; at the end u32 2, u32 length (0xffffffff: none) + the battery save.
"use strict";
const fs = require("fs"), path = require("path"), zlib = require("zlib");
const [emu, zipPath, inputsPath, framesArg, rateArg, sramPath] = process.argv.slice(2);
for (const f of ["m68k.js", "z80.js", "konami2.js", "ym2151.js", "k007232.js", "k053260.js", "upd7759.js", "eeprom93c46.js",
  "k052109.js", "k051960.js", "k053245.js", "k053251.js", "arcade.js", "arcade_tmnt.js", "arcade_tmnt2.js", "arcade_simpsons.js"]) require(path.join(emu, f));

const out = fs.openSync("/dev/stdout", "w");
function write(buf) { let off = 0; while (off < buf.length) off += fs.writeSync(out, buf, off, buf.length - off); }
function u32(v) { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); write(b); }
function fail(e, first) {
  const msg = Buffer.from(String((e && e.message) || e));
  u32(1); u32(msg.length); write(msg);
  process.exit(0);
}

// a small .zip reader (stored and deflated entries)
function readZip(buf) {
  let eocd = buf.length - 22;
  while (eocd >= 0 && buf.readUInt32LE(eocd) !== 0x06054b50) eocd--;
  const n = buf.readUInt16LE(eocd + 10);
  let p = buf.readUInt32LE(eocd + 16);
  const files = new Map();
  for (let i = 0; i < n; i++) {
    const method = buf.readUInt16LE(p + 10), csize = buf.readUInt32LE(p + 20);
    const nl = buf.readUInt16LE(p + 28), xl = buf.readUInt16LE(p + 30), cl = buf.readUInt16LE(p + 32), lho = buf.readUInt32LE(p + 42);
    const name = buf.toString("utf8", p + 46, p + 46 + nl);
    p += 46 + nl + xl + cl;
    if (name.endsWith("/")) continue;
    const start = lho + 30 + buf.readUInt16LE(lho + 26) + buf.readUInt16LE(lho + 28);
    const raw = buf.subarray(start, start + csize);
    files.set(name, new Uint8Array(method === 8 ? zlib.inflateRawSync(raw) : raw));
  }
  return files;
}

const files = readZip(fs.readFileSync(zipPath));
const inputs = fs.readFileSync(inputsPath);
const frames = +framesArg;
const sram = sramPath ? new Uint8Array(fs.readFileSync(sramPath)) : null;
const name = path.basename(zipPath).replace(/\.[^.]*$/, "");
let core;
try { core = new globalThis.RPCores.arcade(null, { files, name, sampleRate: +rateArg, sram }); } catch (e) { fail(e); }
{
  const t = Buffer.from(`${core.gameId} ${core.players} ${core.width}x${core.height}`);
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
