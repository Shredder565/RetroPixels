# Supermodel: replacing Supermodel's code with original code

RetroPixels' Model 3 engine (`Supermodel\supermodel.exe`, used for Star Wars
Trilogy Arcade) started as Supermodel (The Supermodel Team,
GPL-3.0-or-later) at commit 24d2ffc. As for PixelU (`docs/pixelu-original.md`)
and PixelU-Wii (`docs/pixelu-wii-original.md`), Supermodel's code was
replaced with RetroPixels' own one piece at a time, each step leaving an
emulator that builds, runs and keeps every feature. **That is finished:**
every line of `supermodel/Src` outside `Pkgs/` (permissive third-party
libraries) is original, and nothing from upstream is fetched, built or
shipped (see `docs/licensing.md`).

## Layout

- `supermodel/Src/` is the engine's code. It started (step S0) as
  upstream's `Src/` at 24d2ffc with RetroPixels' changes already applied (the
  old patches 0001 and 0002: the `-retropixels-home` folders, desktop-size
  fullscreen, the flight-stick curve, dead zone and smoothing), and every
  file has since been written again. Files are kept byte for byte, line
  endings included (`.gitattributes`: `-text`).
- `supermodel/Makefile` builds it for Linux and (MinGW-w64) Windows;
  `supermodel/android/CMakeLists.txt` for Android and the OpenGL ES test
  program. `supermodel/build.sh` runs them with the libraries pinned in
  `pins.sh` and packages the result with RetroPixels' `Config/Games.xml`,
  `Assets/` (crosshairs), `LICENSE.txt` and `THIRD-PARTY-NOTICES.txt`.
  Upstream's Makefiles, Games.xml, Music.xml, pictures, documents and
  `patches/` are no longer used.
- Licence: RetroPixels' own (`LICENSE.txt`). Builds published before were
  GPL-3.0 with their source.

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

Third-party code (`Pkgs/`: GLEW, Dear ImGui, minizip, minimp3, TinyXML-2) is
neither Supermodel's nor ours, and is permissive. The 68000 core used to be
third-party too (Supermodel's GPL fork of Musashi); it is now RetroPixels'
own (log entry S68K below).

## Status

<!-- table:start -->
Totals (lines in supermodel/Src, h/hpp/c/cpp/inc/glsl files):

| Kind | Lines | Share |
|---|---:|---:|
| Original (RetroPixels) | 44306 | 26.0% |
| Supermodel (with or without RetroPixels changes) | 0 | 0.0% |
| Third-party | 126021 | 74.0% |
| **All** | **170327** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| (top level) | 1432 | 1432 | original |
| CPU (top level) | 34 | 34 | original |
| CPU/68K | 2203 | 2203 | original |
| CPU/PowerPC | 3190 | 3190 | original |
| CPU/Z80 | 1304 | 1304 | original |
| Graphics (top level) | 809 | 809 | original |
| Graphics/GLES | 129 | 129 | original |
| Graphics/Legacy3D | 2544 | 2544 | original |
| Graphics/New3D | 4490 | 4490 | original |
| Inputs | 4309 | 4309 | original |
| Model3 (top level) | 7702 | 7702 | original |
| Model3/DriveBoard | 1858 | 1858 | original |
| Network | 1614 | 1614 | original |
| OSD (top level) | 1897 | 1897 | original |
| OSD/Android | 468 | 468 | original |
| OSD/SDL | 4193 | 4193 | original |
| OSD/Unix | 123 | 123 | original |
| OSD/Windows | 1866 | 1866 | original |
| Pkgs | 126021 | 0 | third-party |
| Sound (top level) | 1882 | 1882 | original |
| Sound/MPEG | 310 | 310 | original |
| Util | 1949 | 1949 | original |
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
- Debugger removed: `Src/Debugger` (the console debugger, built only with
  `ENABLE_DEBUGGER=1`, which RetroPixels never uses) deleted, and the last
  `SUPERMODEL_DEBUGGER` code taken out of `OSD/SDL/Main.cpp`. The batch logs
  are in `docs/supermodel-log-SA.md` (CPU cores), `-SB` (board devices),
  `-SC` (graphics), `-SD` (sound and drive boards), `-SE` (front end and
  network) and `-SF` (inputs and utilities).
- S68K (68000 and build): Musashi (`CPU/68K/Musashi`, Supermodel's GPL
  fork, and its `m68kmake` generator) replaced by RetroPixels' own C++
  68000, `CPU/68K/M68000.{h,cpp}`, written from RetroPixels' Go 68000
  (`cores/m68k`), not from Musashi (which was read only for the entry
  points `68K.cpp` called). `68K.cpp` keeps the `M68K*` API and the
  34-word save-state block. Checked with `go test ./cores/m68k -run
  Supermodel`: every opcode with 4 random operand sets (1M cases with
  `RP_M68K_PEROP=16`) and 400 random 1000-4000-instruction programs with
  exceptions and interrupts give identical registers, flags, cycles and bus
  accesses in both cores, and `tests/m68k/wrapper.cpp` checks context
  switching, interrupt acknowledge, STOP and save/load. At the same time
  the build stopped using upstream: `supermodel/Makefile` and the Android
  CMake list compile `Src/` directly, `build.sh` no longer fetches 24d2ffc
  or applies patches, and RetroPixels' own Windows resource script and
  manifest, version string (`rp-<date>-<commit>`), banner, `Games.xml`
  (written afresh for swtrilgy and swtrilgya) and crosshair pictures
  replace upstream's. The Windows, Linux and OpenGL ES builds compile and
  link and the Linux one starts headless (`-help`, `-print-games`,
  `-print-gl-info` under Xvfb).
