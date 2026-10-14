// RetroPixels — Konami K053251 priority / colour mixer. Original code, written
// from the chip's documented behaviour.
//
// The K053251 sits after the tilemap and sprite chips. It takes up to five
// colour inputs (CI0-CI4), each a pen number from one layer, and for every pixel
// passes on the one with the best (lowest) priority value that isn't see-through
// (pen 0 of its colour). It also adds a per-input palette offset, so the same
// tile chip can use different parts of the palette, and it carries the sprite
// chip's shadow signal through: a shadow pixel darkens whatever ends up in front
// of nothing better than the shadow itself.
//
// Registers (16 of 6 bits, written by the game):
//   0-4   priority of CI0..CI4 (lower = in front). Boards whose sprite chip sends its
//         own per-pixel priority (TMNT2, The Simpsons) give that instead for the sprite input.
//   5-7   shadow priorities (the shadow only darkens layers whose priority is at least this)
//   9     palette offsets of CI0, CI1, CI2: bits 0-1, 2-3, 4-5 (in steps of 32 colours)
//   10    palette offsets of CI3, CI4: bits 0-2, 3-5 (in steps of 16 colours)
//   12    input enables / mode bits (kept, not used by the boards here)
//
// Interface:
//   const m = new RPK053251()
//   m.write(reg, v) / m.read(reg)       reg 0..15
//   m.priority(ci)                      register priority of input ci (0..4)
//   m.paletteBase(ci)                   the input's palette offset in colours (multiply by 16 for pens)
//   m.shadowPriority                    register 5's value
//   m.mix(out, shade, n, layers, bgPen)
//       layers: [{ ci, pens: Uint16Array (0 or pen & 15 == 0: see-through), pri: number or Uint8Array,
//                  shadow: optional Uint8Array (1 = shadow pixel, pens ignored) }]
//       For each of the n pixels writes the winning pen to out (Uint16Array; bgPen when every input
//       is see-through) and 1 to shade (Uint8Array) when a shadow covers it. Ties go to the
//       earlier entry of layers.
//   m.getState() / m.setState(o)
"use strict";

(function (global) {
  class RPK053251 {
    constructor() { this.regs = new Uint8Array(16); }
    reset() { this.regs.fill(0); }
    write(reg, v) { this.regs[reg & 15] = v & 0x3f; }
    read(reg) { return this.regs[reg & 15]; }
    priority(ci) { return this.regs[ci] & 0x3f; }
    paletteBase(ci) {
      const r9 = this.regs[9], r10 = this.regs[10];
      switch (ci) {
        case 0: return 32 * (r9 & 3);
        case 1: return 32 * ((r9 >> 2) & 3);
        case 2: return 32 * ((r9 >> 4) & 3);
        case 3: return 16 * (r10 & 7);
        default: return 16 * ((r10 >> 3) & 7);
      }
    }
    get shadowPriority() { return this.regs[5] & 0x3f; }

    mix(out, shade, n, layers, bgPen) {
      const L = layers.length;
      const pens = layers.map((l) => l.pens), fixed = layers.map((l) => (typeof l.pri === "number" ? l.pri : -1));
      const pris = layers.map((l) => (typeof l.pri === "number" ? null : l.pri)), shadows = layers.map((l) => l.shadow || null);
      for (let i = 0; i < n; i++) {
        let best = 255, pen = bgPen, sh = 255;
        for (let k = 0; k < L; k++) {
          const p = fixed[k] >= 0 ? fixed[k] : pris[k][i];
          const s = shadows[k];
          if (s && s[i]) { if (p < sh) sh = p; continue; }
          const v = pens[k][i];
          if (!(v & 15)) continue;
          if (p < best) { best = p; pen = v; }
        }
        out[i] = pen;
        shade[i] = sh <= best ? 1 : 0;
      }
    }

    getState() { return { regs: this.regs.slice() }; }
    setState(o) { this.regs.set(o.regs); }
  }

  global.RPK053251 = RPK053251;
})(typeof window !== "undefined" ? window : globalThis);
