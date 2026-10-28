# RetroPixels SNES Debugger

A separate program, `RetroPixels-SNES-Debugger.exe`, that opens next to
RetroPixels when a SNES game starts. It saves what's in the SNES's video
memory at a moment you pick, finds where a background's tiles, tilemap and
palettes are in the ROM, and swaps a background for your own picture.

Source: `tools/snes-debugger` (Rust). RetroPixels' side: `cores/snes/trace.go`
and the `Tracer` in `web/emu/snes.js` (the two SNES emulators keep the same
records), `internal/snesdbg` (the shared folder), `snesdbg.go` and
`web/emu/snes_debug.js` (RetroPixels.exe), `cmd/retropixels-native/snesdbg.go`
(RetroPixels Native).

## Using it

1. Keep `RetroPixels-SNES-Debugger.exe` next to `RetroPixels.exe` /
   `RetroPixels-Native.exe` (the download has it there).
2. Start a SNES game. The debugger opens with it. (Turn this off in
   RetroPixels.exe below the game, or in RetroPixels Native's Options menu:
   "Open the SNES Debugger with SNES games".)
3. Play to the screen you want, the first level say, and press **Capture now**
   (or **Capture in 5 s**, then go back to the game).
4. Pick a layer (BG1-BG4). The debugger draws the whole tilemap as the game
   has it and lists where each part came from: "ROM file offset 0x08C000
   ($11:C000), copied by DMA #12 (channel 1, started by code at $00:8A3F)".
5. **Choose picture…** (a PNG), tick the options, **Make preview**, then
   **Save patched ROM…**. Open the patched ROM in RetroPixels to see it.

The same from a terminal:

    RetroPixels-SNES-Debugger.exe capture
    RetroPixels-SNES-Debugger.exe analyze <capture folder>
    RetroPixels-SNES-Debugger.exe export <capture folder> --bg 1 --out bg1.png
    RetroPixels-SNES-Debugger.exe swap <capture folder> --bg 1 --image new.png --out patched.sfc

## What a capture holds

RetroPixels' SNES emulators run a tracer from the game's first frame while
the debugger is on. For every byte of VRAM, CGRAM (palettes) and WRAM it notes
who wrote it last: a DMA transfer, HDMA, or the CPU (with the address of the
instruction), and for DMA the address the byte was read from. Each kind of DMA
transfer is kept once (repeats count up), with its channel, mode, B-bus
register, source address, size, where in VRAM / CGRAM / WRAM it started, and
the addresses of the instructions that wrote `$420B` and `$43x0`-`$43x6`.

The folder the debugger and RetroPixels share is
`%LOCALAPPDATA%\RetroPixels SNES Debugger`:

| file | |
|---|---|
| `active.json` | the game playing now (`session`: its folder, `""` when none) |
| `debugger.json` | the debugger's heartbeat; RetroPixels doesn't start a second one |
| `sessions/<time>-<game>/session.json` | the game's name, the ROM's SHA-1 |
| `sessions/.../rom.sfc` | the ROM as the emulator runs it (no copier header) |
| `sessions/.../requests/<id>.req` | the debugger asks for a capture |
| `sessions/.../captures/<id>/` | RetroPixels' answer (below) |

A capture folder:

| file | size | |
|---|---|---|
| `capture.json` | | frame, PPU registers (mode, `bgsc`, `bgnba`, scroll, `tm`, ...), CPU registers, DMA registers, the DMA records |
| `vram.bin` | 64 KB | VRAM (little-endian words) |
| `cgram.bin` | 512 B | CGRAM (256 BGR555 colours) |
| `oam.bin` | 544 B | sprite table |
| `wram.bin` | 128 KB | work RAM |
| `vram_src.bin`, `cgram_src.bin`, `wram_src.bin` | 4 bytes per byte | who wrote each byte: >= 0 a DMA record's id, -1 never written, -3 HDMA, -4 a DMA not kept, <= -16 the CPU (-16 - its address) |
| `vram_from.bin`, `cgram_from.bin`, `wram_from.bin` | 4 bytes per byte | the A-bus address a DMA read the byte from, or -1 |
| `screen.png` | | the picture at that moment |

## How the swap works

- **Finding the data.** The layer's tilemap, tile graphics and palettes come
  from the PPU registers. Each byte's origin follows the records: a DMA from
  ROM gives the file offset directly; a DMA from WRAM is followed to the DMA
  that filled WRAM. Bytes the CPU wrote (or decompressed) are looked for in
  the ROM as they are; a single match counts.
- **Colours.** The picture is scaled to the tilemap. With "new colours", the
  tiles are split into groups (as many as the layer has palettes that nothing
  else on screen uses), each group gets a palette (median cut, then k-means,
  in the SNES's 5-bit colour steps), and each tile takes the palette that
  draws it best. Colour 0 stays (transparent). Without it, the game's own
  palettes are used. Dithering is optional.
- **Tiles.** Repeated tiles (also flipped) are shared. They go in the tile
  slots whose bytes come from the same ROM data as the layer's own tiles,
  leaving alone VRAM other visible layers, their tilemaps and sprites use.
- **More tiles than room.** The transfer that loads the tiles moves to free
  ROM space (a run of `FF` or `00` padding inside one bank, or a new bank at
  the end of the ROM) with room for more, and is repointed: the source
  address, bank and size are changed where the game loads them as numbers
  before the stores to `$43x2`-`$43x5` (found from the instruction addresses
  in the capture), or in a table in the ROM that holds the 24-bit address
  (and the size next to it). When that isn't possible, similar tiles are
  merged until they fit.
- The ROM's checksum is updated; a copier header is kept.

## Limits

- Graphics stored compressed in the ROM can't be swapped in place (the
  debugger says so, and names the code that unpacks them).
- Scrolling levels build their tilemap from level data as they scroll, so
  their tilemap isn't in the ROM as VRAM has it; title screens and fixed
  backdrops are what can be swapped.
- Mode 7 layers are shown but not swapped yet.
- The same ROM data can be used by other screens of the game; they change too.
