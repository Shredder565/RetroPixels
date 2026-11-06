# PixelDS

PixelDS is RetroPixels' own Nintendo DS emulator, and the start of one
emulator for both the DS and the 3DS. It is new code written for RetroPixels:
no code from melonDS, Azahar, DeSmuME or any other emulator is in it. Public
hardware documentation (GBATEK and the ARM Architecture Reference Manual) and
the behaviour of other emulators were used as references only.

## Where things are

- `cores/pixelds/arm`: the ARM interpreter (ARMv4T for the ARM7, ARMv5TE for
  the ARM9, and the start of ARMv6K for the 3DS's ARM11).
  `arm946_test.go` runs 20,000 instruction cases generated with
  `tools/pixelds/armcases.py` against Unicorn's ARM946 model.
- `cores/pixelds/nds`: the DS itself: memory and VRAM banks, timers, DMA,
  IPC, interrupts, the cartridge and its save chip, SPI (firmware, touch
  screen, power), the clock, both 2D engines, the sound chip, a built-in BIOS
  (`bios.go`, plus small ARM stubs from `tools/pixelds/biosstub.py`) and save
  states. `gpu3d.go` is the 3D geometry engine (matrices, lighting,
  clipping, polygon lists), `render3d.go` the 3D rasterizer (textures,
  depth, translucency, toon and highlight, shadows, edge marking, fog) and
  `capture.go` display capture.
- `cores/pixelds/ctr`: the 3DS. Memory in 4 KB pages over the 3DS's FCRAM,
  VRAM and DSP RAM (`mem.go`); a high-level kernel with threads,
  priorities, handles, events, mutexes, semaphores, timers, the address
  arbiter and memory blocks (`kernel.go`, `svc.go` for the system calls);
  the system services reached over IPC (`services.go`: srv, `apt.go`,
  `gsp.go` for the screens and the GX command queue, `hid.go`, `fs.go`,
  `cfg.go`); the .3dsx loader (`load3dsx.go`); and the start of the
  PICA200: `gpu.go` (registers, command lists, vertex loading),
  `shader.go` (the vertex shader processor), `raster.go` (clipping,
  rasterizer, textures, colour combiners, per-pixel tests and blending) and
  `lighting.go`; `loadncch.go` loads games, `font.go` builds the system
  font from `sysfont.bin` (from `tools/pixelds/sysfont`), `dsp.go` is the
  sound processor's service. `ctr_test.go` runs `tools/pixelds/ctrtest`, our own
  3DS program that calls the kernel and services directly and checks each
  answer.
- `cores/pixelds/arm`: also the ARM11's ARMv6 media instructions
  (`media.go`) and its VFPv2 floating-point unit (`vfp.go`), checked by
  `arm11_test.go` against 20,000 cases from Unicorn's ARM11 MPCore model
  (`tools/pixelds/arm11cases.py`).
- `cores/pixelds/pixelds.go`: the front shared by DS and 3DS games.
- `cmd/pixelds`: PixelDS.exe, the window, keyboard, controller and sound.
- `tools/pixelds/selftest`: a small homebrew DS program (our own code, built
  with `gcc-arm-none-eabi`) that the tests boot to check the CPU, the BIOS,
  the IPC, the timers, DMA, the sound and the screens.

## Command line

    PixelDS.exe [-f] game.nds              play (fullscreen with -f)
    PixelDS.exe [-f] program.3dsx          a 3DS homebrew program
    PixelDS.exe -w -frames 600 -shot a.png game.nds   run without a window

Saves are `game.sav` next to the game, or under
`%APPDATA%\RetroPixels\PixelDS\saves` when that folder can't be written.
Save states (DS and 3DS games, ten slots each) are in
`%APPDATA%\RetroPixels\PixelDS\states`; the window's menu bar has them
(Save states) and Pause (Game; P or a controller's left-stick click). A
3DS state is the whole machine, saved by `cores/pixelds/snap`, which
walks the machine's Go values and loads them into a newly opened copy of
the game (the game's own files aren't stored in it). The 3DS's SD
card and 3DS save data are in `%APPDATA%\RetroPixels\PixelDS\3DS`.

3DS keys: arrows (or a controller's left stick) are the circle pad,
I / J / K / L the D-pad, E = ZL, R = ZR (LT / RT on a controller); the
others are as for DS games, and the mouse is the stylus.

## Texture packs and the 3D resolution (DS games)

- `nds/texdecode.go` decodes a texture the way the 3D engine reads it and
  names it by a hash of its format, size and decoded texels (FNV-1a, 16 hex
  digits). `nds/texpack.go` looks each texture up in the loaded pack (cached
  until texture VRAM is remapped), dumps textures, and samples a pack's
  picture in place of the texture (any size; its alpha, or the texture's
  own see-through texels when the picture has none).
- `nds/hires.go` and `render3d.go`: the 3D engine draws at 1-8 times the
  DS's 256×192. The 2D engine records how each pixel used the 3D layer
  (plain, blended by 3D alpha, by EVA/EVB, brightened or darkened), and the
  big picture applies the same to the detailed 3D pixels; 2D layers are the
  DS's pixels enlarged. Display capture and the 2D engine still see 256×192.
- `cores/pixelds/texpack`: makes a pack from the game file: the DS file
  system, LZ77, NARC archives, and the TEX0 (textures, palettes) and MDL0
  (which palette each material uses) blocks of BMD0 / BTX0 files. Each
  texture is enlarged with Catmull-Rom filtering; the characters' (by model
  file name, "mario" by default) are restyled first (`modern`). Textures
  dumped while playing that are a character's picture with other colours
  (a power-up's palette) are restyled too.

Packs live in `%APPDATA%\RetroPixels\PixelDS\textures\<game code>`, the
automatic one in its `auto` folder (other pictures win over it), dumps in
`dump`. Keys: F2 3D resolution, F3 dump on / off, F9 make the automatic
pack, F10 pack on / off. Command line: `-scale N`, `-textures DIR|off`,
`-dump-textures DIR`, and `-make-texpack [-style modern|hd] [-chars
mario,luigi] [-pack-scale 4] [-only-chars] [-out DIR] game.nds`.

## Plan

1. 2.29.0: DS games with 2D graphics, sound, touch, saves (done).
2. 2.30.0: DS 3D graphics (geometry engine and rasterizer) and display
   capture (done). Anti-aliasing is still to do.
3. 2.31.0: the 3DS's ARM11 and system software (high-level kernel and
   services); 3DS homebrew that draws straight to the screen runs (done).
4. 2.32.0: the PICA200 GPU (shaders, textures, combiners, lighting,
   fog), loading decrypted games (.3ds, .cci, .cxi, .cia), a system font
   built from DejaVu Sans, the DSP's start-up, and the choice to play 3DS
   games in PixelDS from the tab (done).
5. 2.33.0: code modules (ldr:ro), sound (the DSP's voices, filters and
   mixers), geometry shaders, cube, shadow, projected and procedural
   textures (done).
6. 2.34.0: PixelDS is the default for 3DS games; games are recorded
   (-record / -ffmpeg, given by RetroPixels); the Xbox controller's left
   stick is only the circle pad on the 3DS, and LT is L on the DS (done).
7. 2.35.0: DS texture packs (dump, load, an automatic pack made from the
   game) and DS 3D at up to 4× the resolution (done).
8. 2.36.0: save states for 3DS games; a menu bar with Pause and the save
   states (done).
9. Fixes from real games.
