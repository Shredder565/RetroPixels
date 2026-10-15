// Prints sdsp.js's interpolation kernel (taken from its source) as JSON.
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
const a = src.indexOf("const KERN"), b = src.indexOf("KERN[f * 4 + 1] += 2048 - acc;");
const end = src.indexOf("}", b) + 1;
const KERN = new Function(src.slice(a, end) + "; return KERN;")();
process.stdout.write(JSON.stringify(Array.from(KERN)));
