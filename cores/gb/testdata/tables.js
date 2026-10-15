// Prints the web Game Boy core's Game Boy Color colour table (CGB_RGB in
// gb.js) as JSON, 0xRRGGBB values, for TestColorTableMatchesJS.
// usage: node tables.js <emuDir>
"use strict";
const fs = require("fs"), path = require("path");
let s = fs.readFileSync(path.join(process.argv[2], "gb.js"), "utf8");
const at = "  DMG_PALETTES.green =";
if (!s.includes(at)) throw new Error("missing " + at);
s = s.replace(at, "  globalThis.__CGB = CGB_RGB;\n" + at);
(0, eval)(s);
process.stdout.write(JSON.stringify(Array.from(globalThis.__CGB, (c) => ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255))));
