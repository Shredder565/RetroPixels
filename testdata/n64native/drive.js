// Runs web/emu/n64.js (the web player's Nintendo 64) in Node against a
// running RetroPixels server, with a script of controls, and writes what it
// gets back: for each frame the picture's size and SHA-256, and all the sound.
// Used by n64native_test.go to check the native core plays the same.
//
// node drive.js <host:port> <token> <n64.js> <rom> <script.json> <out dir>
//
// script.json: {sram: base64 | null, frames: [{conn, reset, pads: [[bits, x, y, rx, ry] x4]}],
//               stateAt: frame to fetch a save state before, loadAt: frame to load it back before}
"use strict";
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const [addr, token, n64js, romPath, scriptPath, outDir] = process.argv.slice(2);
const script = JSON.parse(fs.readFileSync(scriptPath, "utf8"));

let connected = 1;
globalThis.location = { host: addr };
globalThis.RETRO_DESKTOP = { token };
Object.defineProperty(globalThis, "navigator", {
  value: { getGamepads: () => Array.from({ length: connected }, () => ({})) }, configurable: true, writable: true,
});
(0, eval)(fs.readFileSync(n64js, "utf8"));
const N64 = globalThis.RPCores.n64;

async function main() {
  const rom = new Uint8Array(fs.readFileSync(romPath));
  const sram = script.sram ? new Uint8Array(Buffer.from(script.sram, "base64")) : null;
  const core = new N64(rom, { sampleRate: 48000, sram });
  let gotFrame = null;
  const recv = core.receive.bind(core);
  core.receive = (buf) => {
    recv(buf);
    if (new Uint8Array(buf)[0] === 0x82 && gotFrame) { const f = gotFrame; gotFrame = null; f(); }
  };
  await core.ready;

  const http = async (method, body) => {
    const r = await fetch(`http://${addr}/api/n64/state?s=${core.session}`, {
      method, body, headers: { "X-Retro-Token": token },
    });
    if (!r.ok) throw new Error(`state ${method}: ${r.status} ${await r.text()}`);
    return new Uint8Array(await r.arrayBuffer());
  };

  const frames = [];
  const audio = [];
  let state = null;
  for (let i = 0; i < script.frames.length; i++) {
    const f = script.frames[i];
    if (i === script.stateAt) {
      state = await http("GET");
      fs.writeFileSync(path.join(outDir, "state.bin"), state);
    }
    if (i === script.loadAt) await http("POST", state);
    connected = f.conn;
    f.pads.forEach((p, n) => { core.setInput(n, p[0]); core.setAnalog(n, p[1], p[2], p[3], p[4]); });
    if (f.reset) core.reset();
    const done = new Promise((res) => { gotFrame = res; });
    core.runFrame();
    if (core.ahead !== 1) throw new Error("the frame wasn't asked for");
    await done;
    const px = Buffer.from(core.screen.buffer, core.screen.byteOffset, core.screen.byteLength);
    frames.push(`${core.width} ${core.height} ${crypto.createHash("sha256").update(px).digest("hex")}`);
    if (i === script.frames.length - 1) fs.writeFileSync(path.join(outDir, "last.rgba"), px);
    const a = core.takeAudio();
    audio.push(Buffer.from(a.buffer, a.byteOffset, a.byteLength));
  }
  fs.writeFileSync(path.join(outDir, "frames.txt"), frames.join("\n") + "\n");
  fs.writeFileSync(path.join(outDir, "audio.f32"), Buffer.concat(audio));
  fs.writeFileSync(path.join(outDir, "sram.bin"), core.getSRAM() || new Uint8Array(0));
  core.close();
}

main().then(() => process.exit(0), (e) => { console.error(e && e.stack || e); process.exit(1); });
