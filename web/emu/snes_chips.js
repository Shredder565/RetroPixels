// RetroPixels — SNES cartridge chip registry. Original code.
//
// The SNES core (snes.js) asks this list which coprocessor a cartridge has.
// Each entry: { name, detect(cart) -> bool, create(snes, opts) -> chip } where
// `create` is missing for chips RetroPixels can't run yet (the core then shows
// its friendly "not supported yet" message with `name`).
//
// A chip object answers bus accesses on the 4 KB pages it claims:
//   read(bank, off) -> byte, or -1 to let the normal cartridge mapping answer
//   write(bank, off, v) -> true when it took the write
//   reset(), saveState(), loadState(o)
//   optional getSRAM() (battery RAM incl. clock data) and
//   optional dmaFeed(channel, address, count) -> function giving DMA bytes, or null.
"use strict";

(function (global) {
  const list = global.RPSNESChips = global.RPSNESChips || [];

  const Kit = global.RPSNESChipKit = {
    s16: (x) => (x << 16) >> 16,
    s8: (x) => (x << 24) >> 24,
    // Claim banks b0..b1 (and their $80 mirrors when `mirror`) at offsets o0..o1.
    claim(snes, b0, b1, o0, o1, mirror) {
      for (let b = b0; b <= b1; b++) {
        for (let p = o0 >> 12; p <= o1 >> 12; p++) {
          snes.chipPages[(b << 4) | p] = 1;
          if (mirror) snes.chipPages[((b | 0x80) << 4) | p] = 1;
        }
      }
    },
    // Raw header title bytes (the cart's own title has non-ASCII blanked out).
    titleBytes(cart) { return cart.rom.subarray(cart.headerAt, cart.headerAt + 21); },
    titleHas(cart, text) { return cart.title.toUpperCase().includes(text); },
    mapMode(cart) { return cart.rom[cart.headerAt + 0x15]; },
    // Battery RAM with a few extra bytes on the end (clock chips keep their time there).
    joinSRAM(sram, extra) { const o = new Uint8Array(sram.length + extra.length); o.set(sram); o.set(extra, sram.length); return o; },
  };

  // The NEC DSP family shares chip byte $03-$05 (ROM + DSP, + RAM, + battery); the
  // firmware inside is told apart by the game.
  Kit.isDSP = (cart) => cart.chip >= 0x03 && cart.chip <= 0x05;
  Kit.dspKind = (cart) => {
    if (!Kit.isDSP(cart)) return 0;
    if (Kit.titleHas(cart, "DUNGEON MASTER")) return 2;
    const t = Kit.titleBytes(cart);
    if (t[0] === 0x53 && t[1] === 0x44 && Kit.titleHas(cart, "GX")) return 3; // SD Gundam GX
    if (Kit.titleHas(cart, "TOP GEAR 3000") || Kit.titleHas(cart, "PLANETS CHAMP")) return 4;
    return 1;
  };

  // Chips that still need work (listed first so they win over the general checks).
  // DSP-3, DSP-4, ST010 and ST011 run in snes_necdsp.js, which puts its own
  // entries ahead of these; these names only show if that file isn't loaded.
  list.push(
    { name: "DSP-3", detect: (c) => Kit.dspKind(c) === 3 },
    { name: "DSP-4", detect: (c) => Kit.dspKind(c) === 4 },
    { name: "ST010", detect: (c) => c.chip === 0xf6 && Kit.titleHas(c, "F1 ROC") },
    { name: "ST011", detect: (c) => c.chip === 0xf6 },
  );
})(typeof window !== "undefined" ? window : globalThis);
