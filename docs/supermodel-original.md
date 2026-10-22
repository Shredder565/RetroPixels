# Supermodel: replacing Supermodel's code with original code

RetroPixels' Model 3 engine (`Supermodel\supermodel.exe`, used for Star Wars
Trilogy Arcade) is built from Supermodel (The Supermodel Team, GPL-3.0-or-later)
at commit 24d2ffc. The goal is the same as for PixelU (`docs/pixelu-original.md`)
and PixelU-Wii (`docs/pixelu-wii-original.md`): replace Supermodel's code with
RetroPixels' own one piece at a time, so every step leaves an emulator that
builds, runs and keeps every feature, and at the end none of Supermodel's code
is left.

## Layout

- `supermodel/Src/` is the code being rewritten. It started (step S0) as
  upstream's `Src/` at 24d2ffc with RetroPixels' changes already applied (the
  old patches 0001 and 0002: the `-retropixels-home` folders, desktop-size
  fullscreen, the flight-stick curve, dead zone and smoothing). It is kept
  byte for byte, line endings included (`.gitattributes`: `-text`).
- `supermodel/build.sh fetch` gets the pinned upstream tree for the rest
  (Makefiles, Config/Games.xml, Assets, Docs), applies
  `patches/0001-retropixels.patch` (now only the Makefile line for
  `OSD/RetroPixels.cpp` and `Docs/RetroPixels.txt`), then replaces upstream's
  `Src/` with ours.
- Licence: the program stays GPL-3.0 and ships its exact source
  (`Supermodel-source.tar.gz`) while any Supermodel code is left.

## Method

The rules are PixelU-Wii's:

1. Read the Supermodel file **only for its interface**: the names other files
   call, the config and `Games.xml` names users' files depend on, and what a
   game sees (memory maps, register layouts, IRQ bits, the Real3D display
   list format, SCSP registers, save-state layout).
2. Write the file again from our own knowledge of the hardware and its public
   documentation (Sega Model 3 hardware notes, the PowerPC 603e manuals, the
   Motorola 68000 manual, the Yamaha SCSP documentation). Do not copy
   Supermodel's code, comments, structure or internal names; keep only the
   interface.
3. Behaviour stays the same from a game's point of view. Check each rewrite
   against the old file with an old-vs-new harness (same inputs, compare
   outputs), and that Star Wars Trilogy Arcade still runs (CI starts the
   Linux build headless).
4. Start the file with this header and no Supermodel copyright line:

   ```cpp
   // Original code for RetroPixels (Supermodel). Not derived from Supermodel.
   ```

5. Keep file names so the Makefile lists don't change. Run
   `python3 tools/supermodel-original.py` to update the table below;
   `--list Model3` lists one module's files.
6. Never add ROMs, BIOS images or games.

Third-party code (`Pkgs/`: GLEW, Dear ImGui, minizip, minimp3, TinyXML-2; and
Musashi, the 68000 core) is neither Supermodel's nor ours. Musashi can later
be replaced by RetroPixels' own 68000 core.

## Status

<!-- table:start -->
Totals (lines in supermodel/Src, h/hpp/c/cpp/inc/glsl files):

| Kind | Lines | Share |
|---|---:|---:|
| Original (RetroPixels) | 3091 | 1.3% |
| Supermodel (with or without RetroPixels changes) | 81931 | 35.6% |
| Third-party | 145172 | 63.1% |
| **All** | **230194** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| (top level) | 1671 | 542 | mixed (32% original) |
| CPU (top level) | 110 | 0 | Supermodel |
| CPU/68K | 19997 | 0 | Supermodel, 19151 lines third-party |
| CPU/PowerPC | 6264 | 0 | Supermodel |
| CPU/Z80 | 4353 | 0 | Supermodel |
| Debugger (top level) | 10744 | 0 | Supermodel |
| Debugger/CPU | 4748 | 0 | Supermodel |
| Graphics (top level) | 1486 | 0 | Supermodel |
| Graphics/Legacy3D | 5553 | 0 | Supermodel |
| Graphics/New3D | 6505 | 0 | Supermodel |
| Inputs | 6344 | 0 | Supermodel |
| Model3 (top level) | 11601 | 2549 | mixed (22% original) |
| Model3/DriveBoard | 3534 | 0 | Supermodel |
| Network | 2861 | 0 | Supermodel |
| OSD (top level) | 2410 | 0 | Supermodel |
| OSD/SDL | 6734 | 0 | Supermodel |
| OSD/Unix | 166 | 0 | Supermodel |
| OSD/Windows | 2930 | 0 | Supermodel |
| Pkgs | 126021 | 0 | third-party |
| Sound (top level) | 2986 | 0 | Supermodel |
| Sound/MPEG | 414 | 0 | Supermodel |
| Util | 2762 | 0 | Supermodel |
<!-- table:end -->

## Log

- S0: `supermodel/Src` added as upstream's `Src/` with RetroPixels' changes,
  without `CPU/68K/Turbo68K` (an unused 68000 generator) and `OSD/OSX` (macOS only);
  `patches/0002-flight-stick.patch` folded into it; `build.sh` builds from it.
- SB (Model 3 board devices and the support code they use): `BlockFile`,
  `ROMSet`, `Game.h`, `Supermodel.h`, `Version.h`, and in `Model3/` the IRQ
  controller, PCI bus, MPC105/106 bridge, RTC72421, 93C46 EEPROM, 53C810 SCSI
  (with its disassembler), JTAG/TAP, the 315-5881 decryption (`Crypto`) and
  `IEmulator.h`, written again. Checked old against new with harnesses: the
  devices with random register/bus traffic and save/load (10 seeds, 449k
  trace lines identical), the decryption with 20,000 random keys/streams and
  save/load (2.0M output words identical in every case the old code handled
  without undefined behaviour); Windows and Linux builds start headless.
