// Prints the web core's scheme detection for each file listed in <list>
// (lines "path\tname"): the scheme, or "ERR:" and the message.
// usage: node detect.js <web/emu dir> <list>
"use strict";
const fs = require("fs"), path = require("path");
const [emu, list] = process.argv.slice(2);
for (const f of ["arm7.js", "atari2600_carts.js", "atari2600_dpcplus.js"]) require(path.join(emu, f));
const C = globalThis.RPAtariCarts;
const UNSUPPORTED = ["4A50", "CDF", "CDFJ", "CDFJ+", "AR6K"];
const out = [];
for (const line of fs.readFileSync(list, "utf8").split("\n")) {
  if (!line) continue;
  const [file, name] = line.split("\t");
  const rom = new Uint8Array(fs.readFileSync(file));
  try {
    const t = C.detectType(rom, name);
    if (UNSUPPORTED.includes(t)) { try { C.create(rom, { name }); out.push("ERR?" + t); } catch (e) { out.push("ERR:" + e.message); } }
    else out.push(t);
  } catch (e) { out.push("ERR:" + e.message); }
}
process.stdout.write(out.join("\n") + "\n");
