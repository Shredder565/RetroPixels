# Licensing: what is closed, what is third-party, what is left to do

This page explains RetroPixels' licensing in plain English, for the owner
(Shredder565) and anyone preparing a paid release. It is not legal advice:
before selling, have a lawyer read it together with `LICENSE.txt` and
`THIRD-PARTY-NOTICES.txt`.

## The short version

- **RetroPixels' own code is closed (proprietary).** `LICENSE.txt` says
  "Copyright Shredder565, all rights reserved": buyers get a personal-use
  licence, nobody may copy, redistribute or modify it without permission,
  and there is no warranty.
- **Third-party parts keep their own licences.** `THIRD-PARTY-NOTICES.txt`
  lists every one with the notice it needs. The Windows and Linux downloads
  and the Android app carry both files.
- **Things still standing in the way of a fully closed product:**
  1. the 19 Cemu-derived files in `wiiu/` (MPL-2.0), compiled into
     RetroPixels.exe, RetroPixels-Native.exe and the Android engine. They are
     being rewritten; until then their source must stay available;
  2. the Model 3 engine (`Supermodel\supermodel.exe`) still contains GPL
     code (the Musashi 68000 core and two small Windows resource files), so
     it has to be shipped under the GPL with its source;
  3. PixelU-Wii (`PixelU\wii\PixelU-Wii.exe`) is linked with GPL libraries
     (FFmpeg as configured, LZO, FreeSurround, a GPL header), so it has to be
     shipped under the GPL with its source;
  4. smaller obligations: statically linked LGPL libraries (libusb in PixelU;
     libusb, libiconv and the Bochs disassembler in PixelU-Wii), Cemu's
     MPL-2.0 game profiles and translations, and on Linux the GPL BlueZ
     library next to PixelU.

  None of these stops RetroPixels itself from being sold closed-source:
  the engines are separate programs that RetroPixels starts, and the GPL
  and MPL allow them to be sold alongside it ("aggregation"). What they
  require is that those parts are offered **with their source code and
  under their own licence**, and that buyers are told so. Items 2 and 3
  also mean RetroPixels' own code inside those two engines reaches buyers
  under the GPL for as long as they are built this way.

## What belongs to whom

| Part | Who | Licence | Closed sale? |
|---|---|---|---|
| RetroPixels Go programs: `main.go` and `*.go` in the root, `cmd/`, `core/`, `cores/`, `n64/`, `internal/`, `sevenzip/`, `web/`, `tools/` | Shredder565 | Proprietary (`LICENSE.txt`) | Yes |
| `wiiu/` (Go Wii U emulator), except the 19 files below | Shredder565 | Proprietary | Yes |
| 19 files in `wiiu/` with an MPL header (`ax_voice.go`, `coreinit_alarm.go`, `coreinit_coroutine_cemu.go`, `coreinit_fg.go`, `coreinit_sync.go`, `coreinit_sysinfo.go`, `coreinit_threads.go`, `gx2_command.go`, `gx2r_resource.go`, `latte_cmd.go`, `mem_blockheap.go`, `mem_expheap.go`, `mem_frmheap.go`, `mem_heap.go`, `mem_unitheap.go`, `nn_ac.go`, `nn_fp.go`, `padscore.go`, `vpad_touch.go`) | Go ports of Cemu code | MPL-2.0 | **Being replaced.** Until then: allowed, but these files stay MPL and their source must be made available to buyers (MPL 3.2) |
| Android app (`android/`) | Shredder565 | Proprietary | Yes (JUnit is test-only; the Gradle wrapper is a build tool). APKs built with Supermodel for Android (CI's are) also pack the Model 3 engine (`libsupermodel.so`, GPL as shipped, below) and SDL 2 (`libSDL2.so` and its Java code, zlib licence), with their licences in `assets/supermodel` |
| PixelU (`pixelu/src`, CMake files, art) | Shredder565 (every Cemu source file replaced) | Proprietary | Yes, see the PixelU notes below |
| PixelU-Wii (`pixelu/wii/Source`, data, art) | Shredder565 (every Dolphin source file replaced) | Proprietary code, but the program as shipped is GPL | Only under the GPL until the GPL libraries go (below) |
| Model 3 engine (`supermodel/Src`, except `Pkgs/` and `CPU/68K/Musashi`) | Shredder565 | Proprietary code, but the program as shipped is GPL | Only under the GPL until the GPL parts go (below) |
| MAME (`MAME\` in the download) | MAME team | GPL-2.0-or-later | Separate program; may be bundled if its source is offered (it is, with each release) |

Go modules: `go.mod` has no dependencies; RetroPixels uses only Go's
standard library (BSD-3-Clause, notice in `THIRD-PARTY-NOTICES.txt`). Two
small third-party pieces are inside RetroPixels' Go code: fdlibm's maths
routines (Sun's "freely granted, keep this notice" licence) in
`cores/gba/jsmath.go` and `cores/snes/jsmath.go`, and a bitmap drawn from
DejaVu Sans Mono (Bitstream Vera licence) in `wiiu/font.go`. Both allow a
closed sale with the notice.

## Every third-party component that ships

Licences that allow a closed-source sale with only a notice are marked
"notice". Anything with more obligations is spelled out.

### RetroPixels.exe, RetroPixels-Native.exe, Linux RetroPixels, Android engine

| Component | Licence | Obligation |
|---|---|---|
| Go runtime and standard library | BSD-3-Clause | notice |
| fdlibm | Sun permission notice | notice |
| DejaVu / Bitstream Vera glyphs | Bitstream Vera | notice |
| Cemu code in 19 `wiiu/` files | MPL-2.0 | Those files stay MPL; tell buyers where to get their source and provide it (repository or on request) at no more than cost; include the MPL text (`LICENSE-MPL-2.0.txt`). Being replaced. |

### PixelU (Wii U engine)

| Component | Licence | Obligation |
|---|---|---|
| Cemu game profiles (`gameProfiles/default/*.ini`) and translations (`resources/*/cemu.mo`) | MPL-2.0 | Separate data files: keep them MPL, keep their source available (they are the source / the Cemu `.po` files). Replace them with RetroPixels' own to be rid of MPL entirely. |
| TinyXML-2, Font Awesome 5 table | zlib | notice |
| robin_hood, date.h, fast_float, Dear ImGui, pugixml, RapidJSON, GLM, fmt, ZArchive (MIT-0) | MIT | notice |
| ska::flat_hash_map, Boost | BSL-1.0 | none for binaries; notice kept anyway |
| cubeb | ISC | notice |
| libavc (ih264d), Vulkan-Headers, OpenSSL 3, xbyak_aarch64 | Apache-2.0 | notice + licence text + NOTICE file |
| GameMode client, libzip, Zstandard (BSD choice) | BSD-3-Clause | notice |
| SDL 3, zlib | zlib | notice |
| curl | curl | notice |
| libpng, libtiff, libjpeg-turbo | their own permissive licences | notice |
| glslang | BSD-3/MIT/Apache-2.0 (+ Bison exception) | notice |
| wxWidgets 3.3 | wxWindows Library Licence | binaries may be distributed on any terms; notice |
| **libusb** (static, vcpkg `x64-windows-static`) | **LGPL-2.1+** | Static linking: buyers must be able to relink PixelU with a modified libusb. Either ship libusb as a DLL (a vcpkg overlay triplet with `VCPKG_LIBRARY_LINKAGE dynamic` for `libusb`), or provide PixelU's object files on request (the notice offers this). Plus LGPL text and libusb's source location. |
| D-Bus (Linux) | AFL-2.1 or GPL-2.0+ | Use it under AFL-2.1: notice |
| **libbluetooth.so.3** (Linux download only) | **GPL-2.0+** | PixelU for Linux links it, which makes the Linux PixelU arguably a GPL derivative. Blocker for a closed Linux PixelU: talk to BlueZ over D-Bus or hidraw instead, or load it only if the user installed it. |
| Lato, Noto Sans CJK, Font Awesome 4.7 | SIL OFL 1.1 | ship the OFL text (in the notices); never sell the fonts on their own |
| Icons8 icons | Icons8 licence | Free use requires a visible link/credit to icons8.com, or a paid licence. Check the About dialog credits Icons8, or buy a licence, or redraw them. |
| "More Perfect DOS VGA" bitmap font | freeware, no licence text | **Unclear.** Replace with a font that has a clear licence before selling. |
| XAudio2 / DirectX SDK headers | Microsoft DirectX SDK terms | Headers only; generally fine, low risk. |

PixelU's About dialog (`pixelu/src/gui/wxgui/MainWindow.cpp`) still says
"built from Cemu's source under the Mozilla Public License 2.0". That text is
no longer accurate for its code; change it in a PixelU batch (it was left
alone here so the program's behaviour does not change).

The PixelU package did not carry its libraries' licences before; the
`pixelu.yml` build now copies vcpkg's copyright file of each library into
`PixelU\Licenses\vcpkg\` and the submodules' licence files into
`PixelU\Licenses\`.

### PixelU-Wii (GameCube / Wii engine)

`pixelu/wii/COPYING` is already a complete notice. The parts that matter for
a closed sale:

| Component | Licence | Effect |
|---|---|---|
| **FFmpeg**, built with `--enable-gpl --enable-version3`, static | **GPL-3.0+** | Makes PixelU-Wii.exe GPL-3.0 as a whole. Fix: build FFmpeg without those two flags (no GPL parts are needed for its video dumping) as DLLs, which makes it LGPL with no relinking issue. |
| **LZO** (miniLZO) | **GPL-2.0+** | GPL. Replace with RetroPixels' own LZO-compatible code (decompression format is documented) or another compressor. |
| **FreeSurround** | **GPL-2.0+** | GPL. Replace with own surround upmixing or drop the feature. |
| **TAP-Windows header** (`Source/Core/Core/HW/EXI/BBA/TAP_Win32.h`) | **GPL-2.0** | Compiled in. Rewrite the few ioctl constants from public documentation, or drop the TAP network adapter backend. |
| libusb, libiconv, Bochs disassembler (static) | LGPL-2.1+ | Static linking needs relinkable objects or DLLs (as libusb above). |
| Qt 6 (DLLs) | LGPL-3.0 | Fine as shipped (replaceable DLLs); keep the LGPL text and Qt source location. |
| mGBA | MPL-2.0 | Fine; keep mGBA's files MPL and their source available. |
| OpenAL headers | LGPL-2.0+ | Headers only; low risk. |
| Dolphin data: `Sys\GameSettings`, `ApprovedInis.json`, `Languages`, `dsp_rom.bin`/`dsp_coef.bin` | GPL-2.0+ | Separate data files: allowed alongside a closed program with their source; replace to be rid of GPL entirely. |
| `EnumMap.natvis`, `qt6.natvis` | GPL | Debugger visualisers only, not in the exe. |
| Everything else in `Externals/` | permissive (MIT, BSD, zlib, ISC, Apache-2.0, curl, bzip2, 0BSD) | notice (copied into `Licenses\Externals` at build time) |

### Model 3 engine (Supermodel folder)

The emulator code in `supermodel/Src` is RetroPixels' own except `Pkgs/`
and `CPU/68K/Musashi`. But `build.sh` still fetches upstream Supermodel at
commit 24d2ffc, applies `patches/0001-retropixels.patch` and builds with
upstream's Makefiles. Exactly what upstream (GPL-3.0) material ends up in or
next to the built program:

**Compiled into `supermodel.exe`:**

1. `Src/CPU/68K/Musashi/` (8 files: `m68k.h`, `m68k_in.c`, `m68kconf.h`,
   `m68kcpu.c`, `m68kcpu.h`, `m68kctx.h`, `m68kdasm.c`, `m68kmake.c`, plus
   the `m68kops.c/h` that `m68kmake` generates from `m68k_in.c`). This is
   Supermodel's GPL-3.0 fork of Musashi 3.3, with Supermodel Team changes.
   Musashi 3.3's own notice allows only non-commercial use; Supermodel uses
   it under the GPL by permission. **This is the main blocker.** Not
   counted as "Supermodel" by `tools/supermodel-original.py` (it is listed
   as third-party), but its licence is GPL.
2. `Src/OSD/Windows/SupermodelResources.rc` (2 lines) and
   `Src/OSD/Windows/SupermodelManifest.xml` (8 lines, a DPI-awareness
   manifest), byte for byte upstream's from step S0, compiled in by
   `windres` on Windows. Trivial, but upstream's.
3. The version string: upstream's `Makefiles/Rules.inc` defines
   `SUPERMODEL_VERSION="0.3a-<date>-git-<hash>"`; and `OSD/SDL/Main.cpp`
   still prints "Copyright 2003-2026 by The Supermodel Team" in its banner.
   Not code, but it states upstream copyright in the binary.

**Used to build it (not in the binary):** `Makefiles/Makefile.Win32`,
`Makefile.UNIX`, `Options.inc`, `Rules.inc` (GPL build scripts; the patch
adds `OSD/RetroPixels.cpp` to `Rules.inc`).

**Shipped next to it:** `Config/Games.xml` and `Config/Music.xml` (GPL
data by the Supermodel Team), `Assets/p1crosshair.bmp` and
`p2crosshair.bmp` (upstream art), `LICENSE.txt` (upstream `Docs/LICENSE.txt`,
GPL-3.0), `README.txt` (upstream's manual), `RetroPixels.txt` (from the
patch; says the program is GPL), `VERSION.txt` (says GPL) and
`Supermodel-source.tar.gz` (the whole upstream tree plus our `Src/`).

**To make `supermodel.exe` contain no GPL code:**

1. Replace Musashi with RetroPixels' own 68000 core in C++ (RetroPixels
   already has its own 68000 core in Go, `cores/m68k`, to work from) behind
   the existing `CPU/68K/68K.cpp` interface. (Upstream Musashi 4.x is MIT,
   but Supermodel's fork has GPL changes such as `m68kctx.h`; using MIT
   Musashi would mean redoing those changes independently.)
2. Write RetroPixels' own `SupermodelResources.rc` and manifest.
3. Change the banner in `OSD/SDL/Main.cpp` and set the version string
   from RetroPixels' build.
4. Replace upstream's Makefiles with RetroPixels' own build (a Makefile or
   CMake list of `supermodel/Src`), and stop fetching upstream in
   `build.sh` (and delete `patches/` and `pins.sh`'s `SUPERMODEL_*`).
5. Ship RetroPixels' own game list instead of upstream's `Games.xml` and
   `Music.xml` (only Star Wars Trilogy Arcade's entries are used; the ROM
   names and checksums are facts, but write the file afresh), and own
   crosshair pictures (or none).
6. Package RetroPixels' `LICENSE.txt` and notices instead of
   `Docs/LICENSE.txt`, `README.txt`, `RetroPixels.txt`,
   `Supermodel-source.tar.gz` and the GPL line in `VERSION.txt`.
7. Optionally rename the engine: "Supermodel" is the upstream project's
   name.

Until then it must keep shipping as GPL-3.0 with its source, which
`build.sh` already does correctly. The third-party code it keeps after
that (GLEW, Khronos headers, Dear ImGui, minizip, minimp3, TinyXML-2, SDL2,
SDL2_net, zlib, MinGW-w64's winpthreads, Mesa GLU on Linux) is all
permissive and needs only the notices.

### MAME, FFmpeg, Dolphin

- **MAME** is bundled unchanged in the Windows and Linux downloads. It is a
  separate GPL-2.0+ program that RetroPixels starts, so bundling it is
  "mere aggregation": allowed with a closed RetroPixels, as long as MAME's
  licence goes with it (its own `COPYING`/`docs\legal`) and its source is
  offered (the release attaches `MAME-<version>-source.tar.gz`; keep doing
  that, or a written offer, for every paid download).
- **FFmpeg** is not bundled with RetroPixels: it is downloaded on request
  from BtbN's GPL builds and run as a separate program. Fine.
- **Dolphin** is only used from the user's own install. Fine.

## Remaining obligations, whatever happens

1. Keep `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt` and `LICENSE-MPL-2.0.txt`
   in every download, and the Android app (it now packs the first two as
   assets). Consider showing them in an "About / Licences" screen.
2. Offer the source of every GPL/MPL/LGPL part you ship: the Model 3 engine
   and PixelU-Wii (whole programs, while GPL), the 19 `wiiu/` files, Cemu's
   data files, MAME, and relinkable objects (or DLLs) for the LGPL
   libraries. If the repository becomes private, publish these sources
   separately (for example a "third-party sources" zip on each release)
   instead of the whole RetroPixels source.
3. The Release workflow currently attaches `RetroPixels-source-<v>.zip`,
   the full source, to every public GitHub release, and the README and app
   texts still describe the engines as MPL/GPL in places
   (`web/index.html` Wii U, GameCube/Wii and Supermodel pages; PixelU's
   About dialog). Decide whether the source stays public ("source
   available" but not open source is possible with `LICENSE.txt`), and
   update those texts when the engines change. They were not changed here
   to keep the app's behaviour the same.
4. Versions already published under the MPL or GPL stay available under
   those licences to whoever received them; the closed licence applies from
   now on.
5. Ownership: almost all commits were written by an AI assistant for the
   owner. In several countries (including the US) copyright protects only
   human authorship, so the strength of the "all rights reserved" claim
   depends on the owner's own creative input; trademark ("RetroPixels"),
   contract (the licence buyers accept) and Play's terms still apply. Also
   make sure everyone else who contributed has assigned their rights.
6. The rewrites read upstream files "for their interface"; keep running
   `tools/verbatim-audit.py` and the rewrite rules so no expression is
   carried over.

## Before selling on Google Play

The Android app is the Go engine and, in APKs built with Supermodel for
Android (CI's are), the Model 3 engine for Star Wars Trilogy Arcade (no
PixelU, PixelU-Wii or MAME). Its licensing is simple once the 19 `wiiu/` MPL
files are replaced (until then, offer their source) and the Model 3 engine
has no GPL parts left (until then an APK with it is shipped under the GPL
for that library: offer its source, as for `supermodel.exe`, or build the
Play APK without it). Other things to settle first:

- **Trademarks in the app and store listing.** The app's text names
  Nintendo, Game Boy, SNES, Wii, GameCube, Sega, Genesis, Atari, Xbox and
  Pac-Man (about 60 places in `android/app/src/main`), and the engine's
  messages a few more. Naming systems to describe compatibility is
  generally allowed, but Play removes apps that look affiliated: keep these
  names out of the app title, icon and short description, never use their
  logos or console pictures, add the "not affiliated" line (already in
  `LICENSE.txt` section 4) to the listing, and prefer generic labels
  ("16-bit home console") in prominent places. "PlayStation" does not
  appear anywhere now; keep it that way unless a PlayStation engine is
  added.
- **Play's policy on emulators.** Emulators are allowed, but the app and
  listing must not include, link to or advertise games, ROMs or BIOS files,
  and screenshots must not show commercial games (the README's screenshot,
  a homebrew game, is the right kind). Arcade game names in the library
  (Pac-Man, Galaga and others) are fine as a compatibility list inside the
  app but should not be in the listing.
- **Signing key.** Release builds are signed with the owner's key only when
  the `RP_KEYSTORE_*` secrets are set; otherwise with a debug key, which
  Play rejects. Create the upload key, enrol in Play App Signing, keep the
  key backed up (it can't be recovered), and never commit it.
- **App Bundle.** New Play apps must be uploaded as an `.aab`
  (`./gradlew bundleRelease`), not the APK the Release workflow builds now.
- **Native engine checks.** The engine is a Go program run from the app's
  native library folder (`useLegacyPackaging = true`); that is the
  allowed way to run a bundled program. Verify 16 KB page alignment of
  every `librpengine.so` (x86_64 is linked with `-R 0x4000`; check arm64
  and the others) before Play's 16 KB requirement blocks the upload.
- **Store paperwork.** Paid-app merchant account, a privacy policy URL, the
  Data safety form (the app asks for no internet permission today), content
  rating, and the licence buyers accept (Play's terms plus `LICENSE.txt`).
- **Package name** `io.github.shredder565.retropixels` is permanent once
  published.
