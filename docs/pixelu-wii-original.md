# PixelU-Wii: replacing Dolphin code with original code

PixelU-Wii (PixelU's GameCube and Wii engine, `PixelU\wii\PixelU-Wii.exe`)
is built from Dolphin 2609a (GPL-2.0-or-later). The goal is the same as for
PixelU's Wii U part (`docs/pixelu-original.md`): replace Dolphin's code with
RetroPixels' own, one piece at a time, so that every step leaves an emulator
that builds, runs and keeps every feature, and at the end nothing of
Dolphin's code is left.

## Layout

- `pixelu/wii/Source/` is the code being rewritten. It started (step W0) as
  Dolphin's `Source/` at commit 409881c (tag 2609a) with the PixelU changes
  of the old `pixelu/wii/patches` 0001, 0003 and 0004 already applied
  (branding, the `PixelU-Wii.exe` name, the render window hidden until it is
  docked). Directories the Windows build never configures were left out:
  `Android`, `UnitTests` (`ENABLE_TESTS=OFF`), `DSPSpy`, `DSPTool`
  (`DSPTOOL=OFF`), `Core/MacUpdater` (Apple only), `Core/WinUpdater`
  (`ENABLE_AUTOUPDATE=OFF`) and `Core/DolphinNoGUI` (`ENABLE_NOGUI=OFF`).
- `pixelu/wii/dolphin` (the submodule, still at 409881c) only provides the
  rest of the build tree: the top-level `CMakeLists.txt`, `CMake/`,
  `CMakePresets.json`, `Externals/`, `Data/` and `Languages/`.
- `pixelu/wii/patches` now holds only changes to files outside `Source/`:
  `0002-no-autoupdate-no-analytics.patch` (the top-level `CMakeLists.txt`
  defaults for `ENABLE_AUTOUPDATE` and `ENABLE_ANALYTICS`).
- `pixelu/wii/COPYING` and `pixelu/wii/LICENSES/` are Dolphin's licence
  files. They stay as long as any Dolphin code (or Dolphin's bundled MIT,
  BSD and CC0 pieces) is left.
- CI (`.github/workflows/pixelu-wii.yml`) checks out the submodule and its
  Externals, deletes the submodule's `Source/`, copies `pixelu/wii/Source`
  in its place, applies the patches, commits and tags the result
  `2609a-pixelu` (so the version string is unchanged) and builds
  `dolphin-emu` with MSVC as before.

## Method

The GPL covers a derived work as a whole, not file by file the way MPL-2.0
does. A file written from scratch is still new code, but the program is GPL
as long as any Dolphin code is linked in; it only stops being Dolphin's work
once every Dolphin file is gone. So the rules are strict, and the same as
PixelU's:

1. Read the Dolphin file **only for its interface**: the names other files
   call (its header: classes, functions, enums, config keys), the
   `DoState`/config/INI names that must stay compatible with users'
   existing files, and what a game sees (MMIO register layouts, IOS ioctl
   numbers and return values, DSP ucode behaviour, GX command formats).
2. Write the file again from our own knowledge of the hardware and its
   public documentation (for example YAGCD, WiiBrew, the GameCube/Wii SDK
   behaviour as documented publicly, libogc's headers as an API reference).
   Do not copy Dolphin's code, comments, structure or internal names; keep
   only the interface other files and games depend on.
3. Behaviour stays the same from a game's point of view: same register
   values, same timings where games depend on them, same IOS replies, same
   data written to memory. Fixing an obvious gap is fine; changing what
   working games see is not.
4. Start the file with this header and **no** Dolphin copyright or SPDX line
   (no licence has been chosen for original code yet):

   ```cpp
   // Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
   ```

5. Keep the same file names so the CMake lists do not change. A
   directory's `CMakeLists.txt` is Dolphin's too: rewrite it in the batch
   that finishes the directory. Check locally as far as possible (see
   below), let the Windows CI build (`.github/workflows/pixelu-wii.yml`)
   confirm it, and run `python3 tools/pixelu-wii-original.py` to update the
   tables below. `python3 tools/pixelu-wii-original.py --list W7` lists the
   files of a batch with their line counts.
6. Never add Nintendo keys, IPL/BIOS images, DSP ROM dumps, firmware or
   games.

Local check (Linux, configure only; Qt is not needed): with the submodule
and its Externals fetched and `pixelu/wii/Source` copied over its `Source/`,
`cmake -S <tree> -B build -G Ninja -DENABLE_QT=OFF -DENABLE_TESTS=OFF
-DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DUSE_SYSTEM_LIBS=OFF
-DENABLE_ALSA=OFF -DENABLE_PULSEAUDIO=OFF -DENABLE_EVDEV=OFF -DENABLE_HWDB=OFF
-DENABLE_LLVM=OFF -DENCODE_FRAMEDUMPS=OFF -DENABLE_X11=OFF -DENABLE_EGL=OFF`.
In W0 this configured with our `Source/` and produced the same
`build.ninja` as with Dolphin's own `Source/`. After it, `ninja common`,
`ninja core` and so on compile one library at a time. Since W1 removed
the Unix memory arena, console listener and other Linux sources, a Linux
tree still configures but no longer links; compiling single files and
libraries that do not need them still works.

## Status

Status values: **original** = rewritten from scratch; **Dolphin** =
Dolphin's file unchanged; **Dolphin + PixelU change** = Dolphin's file with
our edits (still Dolphin code); **mixed** = some files of the module
rewritten; **third-party** = bundled code that is neither Dolphin's nor ours
(the Khronos OpenGL headers in `Common/GL/GLExtensions` and OpenVPN's
`TAP_Win32.h`). Dolphin's own CC0/MIT/BSD-licensed files count as Dolphin
code.

<!-- table:start -->
Totals (lines in pixelu/wii/Source, h/hpp/c/cpp/mm/m/inc/inl/S files):

| Kind | Lines | Share |
|---|---:|---:|
| Original (RetroPixels) | 211201 | 51.0% |
| Dolphin, untouched | 192628 | 46.5% |
| Dolphin, with PixelU changes | 4224 | 1.0% |
| PixelU-added files (no original mark) | 0 | 0.0% |
| Third-party | 5763 | 1.4% |
| **All** | **413816** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| Core/AudioCommon | 3219 | 0 | Dolphin |
| Core/Common (top level) | 25007 | 25007 | original |
| Core/Common/Assembler | 3757 | 3757 | original |
| Core/Common/Config | 915 | 915 | original |
| Core/Common/Crypto | 1202 | 1202 | original |
| Core/Common/Debug | 436 | 436 | original |
| Core/Common/GL | 9096 | 0 | Dolphin, 5672 lines third-party |
| Core/Common/Logging | 581 | 581 | original |
| Core/Core (top level) | 25031 | 892 | mixed (4% original) |
| Core/Core/Boot | 2838 | 2838 | original |
| Core/Core/Config | 2398 | 2398 | original |
| Core/Core/ConfigLoaders | 1245 | 1245 | original |
| Core/Core/Debugger | 3384 | 3384 | original |
| Core/Core/DSP (top level) | 4938 | 4938 | original |
| Core/Core/DSP/Interpreter | 2883 | 2883 | original |
| Core/Core/DSP/Jit | 3625 | 3625 | original |
| Core/Core/FifoPlayer | 1943 | 1943 | original |
| Core/Core/HLE | 1429 | 1429 | original |
| Core/Core/HW (top level) | 9961 | 9961 | original |
| Core/Core/HW/DSPHLE | 6575 | 6575 | original |
| Core/Core/HW/DSPLLE | 571 | 571 | original |
| Core/Core/HW/DVD | 4942 | 2244 | mixed (45% original) |
| Core/Core/HW/EXI | 7965 | 6494 | mixed (82% original), 91 lines third-party |
| Core/Core/HW/GCMemcard | 3145 | 3145 | original |
| Core/Core/HW/HSP | 964 | 964 | original |
| Core/Core/HW/MagCard | 1368 | 0 | Dolphin |
| Core/Core/HW/SI | 3159 | 2504 | mixed (79% original) |
| Core/Core/HW/Triforce | 3604 | 124 | mixed (3% original) |
| Core/Core/HW/WiimoteCommon | 1006 | 1006 | original |
| Core/Core/HW/WiimoteEmu | 7553 | 7553 | original |
| Core/Core/HW/WiimoteReal | 2362 | 2362 | original |
| Core/Core/IOS (top level) | 3655 | 3655 | original |
| Core/Core/IOS/Crypto | 275 | 275 | original |
| Core/Core/IOS/DI | 915 | 915 | original |
| Core/Core/IOS/ES | 5115 | 5115 | original |
| Core/Core/IOS/FS | 2744 | 2744 | original |
| Core/Core/IOS/Network | 7642 | 7642 | original |
| Core/Core/IOS/SDIO | 698 | 698 | original |
| Core/Core/IOS/STM | 199 | 199 | original |
| Core/Core/IOS/USB (top level) | 2583 | 2583 | original |
| Core/Core/IOS/USB/Bluetooth | 8741 | 0 | Dolphin |
| Core/Core/IOS/USB/Emulated | 5232 | 0 | Dolphin |
| Core/Core/IOS/USB/OH0 | 616 | 616 | original |
| Core/Core/IOS/USB/USB_HID | 585 | 585 | original |
| Core/Core/IOS/USB/USB_VEN | 204 | 204 | original |
| Core/Core/IOS/WFS | 1237 | 1237 | original |
| Core/Core/PowerPC (top level) | 9982 | 9982 | original |
| Core/Core/PowerPC/CachedInterpreter | 1083 | 1083 | original |
| Core/Core/PowerPC/Interpreter | 4391 | 4391 | original |
| Core/Core/PowerPC/Jit64 | 10119 | 10119 | original |
| Core/Core/PowerPC/Jit64Common | 2509 | 2509 | original |
| Core/Core/PowerPC/JitCommon | 2164 | 2164 | original |
| Core/Core/PowerPC/SignatureDB | 787 | 787 | original |
| Core/DiscIO | 15966 | 15966 | original |
| Core/DolphinQt (top level) | 15076 | 0 | Dolphin + PixelU change |
| Core/DolphinQt/Achievements | 1273 | 0 | Dolphin |
| Core/DolphinQt/Config | 18978 | 0 | Dolphin |
| Core/DolphinQt/Debugger | 14683 | 0 | Dolphin |
| Core/DolphinQt/EmulatedUSB | 367 | 0 | Dolphin |
| Core/DolphinQt/FIFO | 1371 | 0 | Dolphin |
| Core/DolphinQt/GameList | 2605 | 0 | Dolphin |
| Core/DolphinQt/InfinityBase | 403 | 0 | Dolphin |
| Core/DolphinQt/NetPlay | 3201 | 0 | Dolphin |
| Core/DolphinQt/QtUtils | 1405 | 0 | Dolphin |
| Core/DolphinQt/Settings | 4742 | 0 | Dolphin |
| Core/DolphinQt/SkylanderPortal | 1499 | 0 | Dolphin |
| Core/DolphinQt/TAS | 1979 | 0 | Dolphin |
| Core/DolphinTool | 1341 | 0 | Dolphin |
| Core/InputCommon (top level) | 2259 | 0 | Dolphin |
| Core/InputCommon/ControllerEmu | 3592 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface (top level) | 1863 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/DInput | 1112 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/DualShockUDPClient | 978 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/ForceFeedback | 337 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/SDL | 1311 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/SteamDeck | 335 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/WGInput | 807 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Wiimote | 2020 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Win32 | 139 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/XInput | 350 | 0 | Dolphin |
| Core/InputCommon/ControlReference | 2333 | 0 | Dolphin |
| Core/InputCommon/DynamicInputTextures | 477 | 0 | Dolphin |
| Core/UICommon | 4121 | 0 | Dolphin |
| Core/UpdaterCommon | 903 | 0 | Dolphin |
| Core/VideoBackends/D3D | 3633 | 0 | Dolphin |
| Core/VideoBackends/D3D12 | 5320 | 0 | Dolphin |
| Core/VideoBackends/D3DCommon | 1094 | 0 | Dolphin |
| Core/VideoBackends/Null | 552 | 552 | original |
| Core/VideoBackends/OGL | 5742 | 0 | Dolphin |
| Core/VideoBackends/Software | 4785 | 4785 | original |
| Core/VideoBackends/Vulkan | 10213 | 0 | Dolphin |
| Core/VideoCommon (top level) | 46443 | 31419 | mixed (68% original) |
| Core/VideoCommon/Assets | 4615 | 0 | Dolphin |
| Core/VideoCommon/GraphicsModSystem | 2853 | 0 | Dolphin |
| Core/VideoCommon/Resources | 2053 | 0 | Dolphin |
| PCH | 109 | 0 | Dolphin |
<!-- table:end -->

## Batch plan

Batches of about 5-15k lines, grouped by subsystem, from the bottom of the
dependency tree up: Common first, then DiscIO, the PowerPC core, the
hardware, the DSP, IOS, the rest of Core, VideoCommon, the video backends,
InputCommon, AudioCommon/UICommon, DolphinQt last. Lines are Dolphin lines
left in each batch (third-party files are not counted); the exact file
lists are in `tools/pixelu-wii-original.py` (`BATCHES`).

W1 was different: it **deleted** code no Windows x64 build compiles (the
ARM64 JIT and emitter, Metal, and the Android, macOS, Linux, BSD and Haiku
platform backends) together with their entries in the CMake lists, instead
of rewriting it (see the W1 log below). PixelU-Wii now builds only for
Windows x64, which is all RetroPixels ships. Each removal was checked
against the CMake conditions (for example `IOhidapi` and `SteamDeck` are
built on Windows too, so they stayed). Shared files that still have
`__APPLE__`, `ANDROID` or `_M_ARM_64` branches keep them until their own
batch rewrites them.

<!-- batches:start -->
| Batch | Scope | Lines | Original lines | Status |
|---|---|---:|---:|---|
| W1 | Prune: delete code no Windows x64 build compiles (ARM64, Metal, macOS, Android, Linux/BSD) | 0 | 0 | done (no files left) |
| W2 | Common: types, bits, containers, strings, files, threads, config, logging | 13455 | 13455 | original |
| W3 | Common: platform, memory, network, crypto, file formats | 9026 | 9026 | original |
| W4 | Common: x64 emitter, disassemblers, PowerPC assembler | 9417 | 9417 | original |
| W5 | DiscIO: blob formats and compression | 7637 | 7637 | original |
| W6 | DiscIO: volumes, filesystems, verifier, Riivolution, NAND import | 8329 | 8329 | original |
| W7 | PowerPC: CPU state, MMU, caches, analyst, tables, symbols, breakpoints, GDB stub | 10769 | 10769 | original |
| W8 | PowerPC: interpreter and cached interpreter | 5474 | 5474 | original |
| W9 | PowerPC: JIT common and Jit64 | 14792 | 14792 | original |
| W10 | HW: memory map, MMIO, CPU/PI/MI/VI/AI/DSP interfaces, timers, IPC, CoreTiming | 8543 | 8543 | original |
| W11 | HW: DVD and GameCube memory cards | 8087 | 5389 | mixed (67% original) |
| W12 | HW: EXI devices (memory card slots, BBA, mic, modem, IPL) | 7874 | 6494 | mixed (82% original) |
| W13 | HW: SI, pads, keyboard, GBA link, HSP, Triforce, MagCard | 10990 | 5487 | mixed (50% original) |
| W14 | HW: Wii Remote (common, emulated, real) | 11267 | 11267 | original |
| W15 | DSP: core, interpreter, LLE glue | 8461 | 8461 | original |
| W16 | DSP: x64 JIT | 3625 | 3625 | original |
| W17 | DSP: HLE (AX, Zelda, GBA, CARD, ROM ucodes) | 6575 | 6575 | original |
| W18 | IOS: kernel, ES, FS, DI, SDIO, STM, WFS, crypto | 14838 | 14838 | original |
| W19 | IOS: network (SO, KD/WC24, NCD, WD, SSL) and USB host devices | 11630 | 11630 | original |
| W20 | IOS: Bluetooth and emulated USB devices (Skylanders, Infinity, keyboard, mic) | 13973 | 0 | Dolphin |
| W21 | Core: boot, config, config loaders, HLE, debugger, FIFO player | 13237 | 13237 | original |
| W22 | Core: Core.cpp, System, state saves, movies, Wii root, title DB, hotkeys | 10095 | 0 | Dolphin + PixelU change |
| W23 | Core: cheats, patches, NetPlay, achievements, analytics, USB utils | 14044 | 0 | Dolphin |
| W24 | VideoCommon: GX state and command processing (BP/CP/XF, FIFO, PE, CPU cull) | 11051 | 11051 | original |
| W25 | VideoCommon: vertex loaders and vertex manager | 4192 | 4192 | original |
| W26 | VideoCommon: texture cache and decoders | 7527 | 7527 | original |
| W27 | VideoCommon: shader generators and shader managers | 8649 | 8649 | original |
| W28 | VideoCommon: renderer abstraction, framebuffers, shader cache, present, post-processing | 11210 | 0 | Dolphin |
| W29 | VideoCommon: OSD, frame dumping, performance, assets, graphics mods, resources | 13335 | 0 | Dolphin |
| W30 | Video backends: Software and Null | 5337 | 5337 | original |
| W31 | Video backends: OpenGL (with Common/GL) | 9166 | 0 | Dolphin |
| W32 | Video backends: Vulkan | 10213 | 0 | Dolphin |
| W33 | Video backends: D3D11, D3D12, D3DCommon | 10047 | 0 | Dolphin |
| W34 | InputCommon: controller emulation, control references, dynamic input textures | 8661 | 0 | Dolphin |
| W35 | InputCommon: ControllerInterface (DInput, XInput, WGInput, SDL, DSU, Wii Remote, Steam Deck) | 9252 | 0 | Dolphin |
| W36 | AudioCommon, UICommon, UpdaterCommon, DolphinTool, PCH | 9693 | 0 | Dolphin |
| W37 | DolphinQt: main window, menus, host, settings, render widget, hotkeys, Qt utils | 10960 | 0 | Dolphin + PixelU change |
| W38 | DolphinQt: game list and the other top-level dialogs (cheats, memcards, GBA, convert, NAND) | 8126 | 0 | Dolphin |
| W39 | DolphinQt: config dialogs (general, graphics, controls, tooltips, SDL hints) | 12387 | 0 | Dolphin |
| W40 | DolphinQt: controller mapping and settings panes | 11333 | 0 | Dolphin |
| W41 | DolphinQt: debugger | 14683 | 0 | Dolphin |
| W42 | DolphinQt: NetPlay, TAS, FIFO, achievements, portals, emulated USB | 10093 | 0 | Dolphin |
<!-- batches:end -->

After the last batch, Dolphin's files outside `Source/` are what is left:
the top-level `CMakeLists.txt`, `CMake/*.cmake`, `CMakePresets.json`,
`patches/0002`, `Data/Sys` (shaders, game INIs, themes, resources, title
databases, the Gecko code handler), `Languages/` (Dolphin's translations)
and the CMake glue in `Externals/`. Those need their own steps (an original
top-level build in `pixelu/wii`, original or dropped data files, fetching
the third-party Externals directly) before the submodule can go.

Decided 2026-10-20 (project owner, decision card): remove Nintendo's keys
from PixelU-Wii (Wii common, Korean, SD and vWii ancast keys, and the
built-in console key and signature). Wii discs, WADs and SD saves then use a
keys file dumped from the owner's own Wii (BootMii keys.bin / otp.bin),
chosen once in RetroPixels. GameCube is unaffected. Done as its own step
after W18 (IOSC) merges.
The keys step offers "Find keys on this PC" (scan drives for keys.bin /
otp.bin / keys.txt the owner already has) and a short guide to dumping them
from the owner's own consoles (BootMii for Wii, dumpling for Wii U). It does
not download keys or firmware from the internet (owner asked; declined,
since that would distribute Nintendo's files).

Queued after the rewrite and before Linux (project owner, 2026-10-20):
analyse and improve Supermodel (Sega Model 3; the owner's upload of
supermodel-0.3a-20260726-git-24d2ffc-windows.zip) and add it to RetroPixels
under its own tab with the same launch rules as the other emulators, so that
clicking the Supermodel icon starts Star Wars Trilogy Arcade playable. The
game ROM is the owner's own (/mnt/project-files/swtrilgya.zip) and is never
bundled: RetroPixels finds the owner's copy once and launches it from then on.

Then, once both engines and the app are 100% original: a Linux version of
RetroPixels (asked for by the project owner on 2026-10-20). That needs
Linux back ends for the Go app's window, sound, controllers and menus, and
for PixelU and PixelU-Wii, whose non-Windows code was pruned for the rewrite
(display via OpenGL/Vulkan, audio, input).

## Log

- W0: Dolphin 2609a's `Source/` (with the old Source patches) vendored into
  `pixelu/wii/Source`; CI builds with it; the tracker and this plan added.
- W1 log: deleted 107 source files (33206 lines) that no Windows x64 build
  compiles, plus 6 other files and about 470 lines of CMake.
  Source now has 444993 lines (from 478199).
  - ARM64 and other non-x86 CPUs (19957 lines): `PowerPC/JitArm64`,
    `PowerPC/JitArmCommon`, Common's `Arm64Emitter`, `ArmCommon.h`,
    `ArmCPUDetect.cpp`, `ArmFPURoundMode.cpp`, `GenericCPUDetect.cpp`,
    `GenericFPURoundMode.cpp`, VideoCommon's `VertexLoaderARM64` and
    `TextureDecoder_Generic.cpp`. Also removed: the `elf_aux_info` check
    and the Windows ARM64 Qt target selection.
  - Metal (4618 lines): `VideoBackends/Metal` and its CMake list.
  - macOS (1039 lines): `CommonFuncsObjC.mm`, `MemArenaDarwin.cpp`, the AGL
    GL interface, the Quartz input backend, `TAP_Apple.cpp`; also the app
    bundle files (`Info.plist.in`, entitlements, the mac `qt.conf`,
    `Core/VersionInfo.plist.in`). Also removed: the MacUpdater
    subdirectory, the `Dolphin.app` bundle target and DolphinQt's bundle
    block. `UpdaterCommon` is now added on Windows only.
  - Android (2210 lines): `MemArenaAndroid.cpp`, `ConsoleListenerDroid.cpp`,
    the EGLAndroid GL interface, `IOAndroid.cpp`, the Android and Touch
    input backends, `OpenSLESStream.cpp`. Also removed: the `Android/jni`
    subdirectory, the `androidcommon` links, and Vulkan's adrenotools
    link and include path.
  - Linux, BSD and Haiku (5267 lines): `MemArenaUnix.cpp`,
    `ConsoleListenerNix.cpp`, `UnixUtil.h`, the EGL, EGLX11, GLX11Window
    and BGL GL interfaces, `TraversalServer.cpp` (the Unix-only
    `traversal_server` tool), `TAP_Unix.cpp`, `IOLinux.cpp` (BlueZ),
    `MemoryWatcher`, the evdev, Xlib and Pipes input backends,
    `AlsaSoundStream.cpp`, `PulseAudioStream.cpp`, UICommon's `X11Utils`
    and `DBusUtils`, DolphinQt's `SignalDaemon`. Also removed: Source's
    `ENABLE_BLUEZ` option. `ENABLE_ALSA`, `ENABLE_PULSEAUDIO`,
    `ENABLE_X11`, `ENABLE_EGL`, `ENABLE_EVDEV` and `ENABLE_HWDB` are
    options of the submodule's top-level CMake, so they stay. They now
    turn on nothing in `Source/`, and on Windows they never did.
  - Not built by any CMake list (115 lines): `IOS/Network/ICMPWin.cpp`
    (every platform builds the `ICMPLin.cpp` stubs) and DolphinQt's
    `pch_qt.h`/`.cpp`.
  - Kept, because Windows-compiled files include them for their
    non-platform stand-ins: `IOAndroid.h` and `IOLinux.h` (from
    `WiimoteReal.cpp`), and `AlsaSoundStream.h`, `PulseAudioStream.h` and
    `OpenSLESStream.h` (from `AudioCommon.cpp`). They moved to W14 and
    W36. Includes of deleted headers are left as they are when they sit
    inside non-Windows `#if` branches of shared files (`GLContext.cpp`,
    `ControllerInterface.cpp`, `JitInterface.cpp`, `VertexLoaderBase.cpp`,
    `VideoBackendBase.cpp`, `Core.cpp`, `AMMediaboard.cpp`,
    `MainWindow.cpp`, `UICommon.cpp`), so those files are still unchanged
    Dolphin files. The same goes for the Apple/Haiku link libraries
    that shared code uses.
  - Checked: the Linux configure-only run (flags above) succeeds. Its
    `build.ninja` differs from W0's only by the Unix-only sources removed
    (`ConsoleListenerNix`, `MemArenaUnix`, `TraversalServer`, `TAP_Unix`,
    `MemoryWatcher`, `Pipes`). Every file named in a `Source/` CMake list
    exists, except the generated `AchievementApprovedHash.h`. No deleted
    header is included outside a non-Windows `#if`.

- W3 log: rewrote all 63 files of Common's platform, memory, network,
  crypto and file-format code (8998 lines now, from 9782). Every file starts
  with the original-code header and has no Dolphin copyright or SPDX line.
  File names, public declarations, macros, enum values, file formats and
  wire layouts are unchanged.
  - Crypto (`Crypto/AES`, `SHA1`, `HMAC`, `bn`, `ec`): AES-128-CBC through
    mbedtls, with an AES-NI path (own key expansion, 8-block decryption).
    SHA-1 through mbedtls, with a SHA-NI path. HMAC-SHA1 written directly
    on SHA1 (RFC 2104). Big-endian bignum mod arithmetic. sect233r1 over
    GF(2^233) with 64-bit limbs: Itoh-Tsujii inversion, affine
    double/add, ECDSA sign/verify, ECDH. The quirks callers depend on are
    kept exactly (multiplication of unreduced inputs, point at infinity as
    (0,0), 240-bit scalars, the same bn operand order in signing).
  - Debug (`MemoryPatches`, `Threads.h`, `Watches`), `CWDemangler`
    (CodeWarrior symbol demangler, new parser with the same output),
    `SymbolDB`, `NandPaths` (one-pass escape/unescape), `Network` (Ethernet,
    ARP, IPv4, TCP, UDP and DHCP headers, checksums, packet parsing and
    building, MAC/BT/IP-range parsing).
  - Memory: `MemArena.h` and `MemArenaWin.cpp` (fastmem). The placeholder
    path (VirtualAlloc2FromApp / MapViewOfFile3FromApp / UnmapViewOfFileEx,
    split with MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER, merged with
    MEM_COALESCE_PLACEHOLDERS) and the legacy MapViewOfFileEx fallback make
    the same Win32 calls as before. Also `LazyMemoryRegion` (8 MiB blocks
    over one shared read-only zero section) and `MemoryUtil`.
  - Platform: `DynamicLibrary`, `HRWrap`, `LdrWatcher`, `WindowsRegistry`,
    `WindowsDevice`.
  - Network: `ENet`, `SFMLHelper`, `SocketContext`, `QoSSession`, `UPnP`,
    `TraversalProto.h` (same packed layout, now static_asserted: 37-byte
    packet, 19-byte address), `TraversalClient`, `HttpRequest`.
  - Files: `SDCardUtil` (blank FAT32 image), `FatFsUtil` (SD folder <->
    image sync through FatFs), `PcapFile`, `Image` (spng),
    `FilesystemWatcher`, `MinizipUtil.h`.
  - Strings kept on purpose because users' data or other programs see
    them: the SD volume label `DOLPHINSD`, the hole-punch payload
    `Hello from Dolphin Netplay...` and the UPnP mapping description
    `dolphin-emu UDP on <ip>`.
  - Deliberate differences (bug fixes, nothing a game sees):
    - `StringToIPv4PortRange`: CIDR prefixes that are not a multiple of 8
      now give the right range (the old mask was built in host order and
      applied to network-order bytes).
    - `PacketView` rejects an IPv4 IHL or TCP data offset below 5 (was
      out-of-bounds reads). TCP/UDP packet building uses offsets instead
      of raw pointers into the buffer.
    - `SymbolDB::GetSymbolFromHash` returns null for a hash whose set is
      empty (was undefined behaviour). `SHA1::StringToDigest` cannot read
      past its input. `DecodeNetworkError` falls back to "Unknown error N".
    - `CryptOFB` initialises and frees its mbedtls context. HMAC no longer
      goes through mbedtls' md layer (same output).
    - `Watches::LoadFromStrings`: a blank line gives a watch at address 0
      (the address was uninitialised).
    - `Common::RootUserPath` no longer pops the back of an empty user
      path.
    - `LazyMemoryRegion`: `EnsureMemoryPagesWritable` also covers the last
      block when the range does not start on a block boundary; a failed
      `Create` releases every block (it could leak mapped views and
      address space); making a block writable creates the new section
      before unmapping the zero view, and maps the zero view back if the
      new mapping fails (the block was left unmapped). The legacy fastmem
      mapping passes the high DWORD of the offset too.
    - `DynamicLibrary::Open` frees the library it held (after loading the
      new one, so reopening the same DLL does not unload it).
    - `LdrWatcher` matches module names exactly (ignoring case). It used
      to accept any loaded module whose name was a prefix of a watched
      name, so a module with an empty name matched everything.
    - `HRWrap`: `winrt::hresult` prints as unsigned hex like `HRWrap`
      (was `-0x7fffbffb` style). The unused second `GetHResultMessage`
      overload is gone.
    - `WindowsDevice`: a zero-size property no longer underflows the
      string length. `WindowsRegistry`: `RtlGetVersion` is null-checked.
    - `ENet::SendPacket` frees a packet that `enet_peer_send` refused
      (leaked). `QoSSession`: move-assignment releases the flow it already
      held, and the Windows DSCP value is `tos >> 2` (EF = 46, the same
      marking `IP_TOS` gives elsewhere) instead of `tos >> 3` (23).
    - `TraversalClient`: checks `m_Client` before `OnTtlDetermined`, copies
      server packets out of the receive buffer before reading them, and
      the intercept hook copes with no client. The TTL-test reply filter
      never had any effect (it compared an address with a port), so the
      new code simply counts any datagram, which is what happened before.
    - `HttpRequest`: curl options get `long`/`curl_off_t` values, a failed
      `curl_easy_escape` returns an empty string (was a null
      `std::string`), and the handle no longer points at the freed header
      list after a request.
    - `SDCardCreate` closes the file before deleting it on failure (on
      Windows an open file cannot be deleted). `FatFsUtil`: a failed
      `LocalTime` gives a zero timestamp (was a null dereference).
      `ReadFileFromZip` fails when an entry ends early (looped forever).
    - Dropped: MemoryUtil's and MemArena's macOS/BSD/Haiku branches
      (Windows-only build; POSIX kept for tooling).
  - Checked:
    - Bit-exactness, old vs new code in the same harness (ASan+UBSan):
      AES (3000 random CBC encrypt/decrypt cases on the mbedtls and AES-NI
      paths, including bad lengths, zero IV and in-place use, plus 500 OFB
      cases), SHA-1 (4000 random inputs on both paths, string round
      trips) and 1500 HMAC cases, bn (20000 random add/sub/mul/exp/inv/
      compare cases including edge values), ec (120 key pairs: public
      keys, signatures, verification of good and corrupted signatures,
      shared secrets): all identical. Network (2.1 million
      lines of output from parsing, checksums, building and fuzzed
      packets), NandPaths, CWDemangler (real symbol corpus plus token
      fuzzing, all option sets), SymbolDB, MemoryPatches and Watches
      operation sequences: identical, apart from the CIDR and blank-line
      fixes above.
    - MemArenaWin against a model of the Win32 placeholder API that
      rejects illegal calls and injects failures: identical call traces
      for 400 random map/unmap/protect/view sequences (placeholder and
      legacy modes, API-set missing, libraries missing). LazyMemoryRegion
      ends in the same state. With failures injected into section
      creation, splitting and mapping (unmapping kept reliable), the new
      code leaves nothing behind and makes no illegal call in 3000 runs;
      the old code leaked in 242.
    - SD images from `SDCardCreate`, 7 MiB to 32 GiB on every cluster-size
      boundary: byte-identical with the volume serial masked. FatFs packs
      in deterministic mode (auto and configured size): byte-identical
      images; unpacking either image reproduces the source tree; cancelled
      and too-small runs leave no files; a failed unpack restores the old
      folder. pcap output identical apart from timestamps.
    - Syntax: every W3 .cpp, and the 94 files outside W3 that include a
      W3 header, compiled with g++ (Linux) and clang
      `-target x86_64-w64-windows-gnu` and compared with the untouched
      tree: no new failures. The files that fail in both trees fail only
      because of this machine's headers (mingw lacks qos2.h, winrt, the
      Windows 10 cfgmgr32 calls and Dolphin's patched ENet; SFML 2.6
      instead of 3). `WindowsDevice`, `QoSSession` and `TraversalClient`
      were also checked with -Wall against shims of the SDK declarations
      and `HRWrap` against a winrt stub.

- W2 log: rewrote all 100 W2 files in `Core/Common` (types, bits,
  containers, strings, files, threads, config, logging) as original code.
  They now have 13402 lines (from 15729). Each starts with the RetroPixels
  original-code line and has no Dolphin copyright or SPDX header.
  - Kept unchanged: file names, public declarations (names, signatures,
    namespaces, enum values, macros), config and INI key names, and
    every file format and on-disk layout. That covers the savestate
    (`PointerWrap`) layout, the `LinearDiskCache` header ("DCAC", key and
    value sizes, 40-byte revision) and entries, the `setting.txt`
    encoding, the analytics wire format, the texture hashes (CRC32 and
    Murmur paths), the `CommonPaths` strings, INI parse and save rules,
    the user-path rebuild rules, translated strings and the "Dolphin"
    name and version strings.
  - Dropped: the Apple, Android and BSD code paths. Their declarations
    stay in the headers (`GetMacOSVersion`, `GetBundleDirectory`,
    `SetSysDirectory` and so on). Windows code is complete; Linux/POSIX
    keeps minimal fallbacks so the tree still syntax-checks there.
  - Deliberate differences (genuine bugs, harmless fixes):
    - `TryParse` for enums with a u64 underlying type rejects negative
      input.
    - `OnConfigChanged` goes by index and copies each callback, so a
      callback can add or remove callbacks safely.
    - `GetSystemName`/`GetLayerName` return an empty string for unknown
      values instead of throwing.
    - LogManager: the source-path cut-off is guarded, mixed separators
      are handled, and a code-page error still shows the message text.
    - `StringToPath`/`PathToString` test `_WIN32`.
    - `TStrToUTF8` builds without UNICODE.
    - `StringFromFormatV` uses `va_copy`.
    - `UTF16BEToUTF8`'s iconv path is fixed (non-Windows only).
    - `ExtractBits` is correct for u8 and u16.
    - A moved-from `Buffer` is null.
    - `HookableEvent`: a nested `Trigger` no longer erases hooks before
      the outer `Trigger` is done.
    - `ScopeGuard` works with lambdas that can't be assigned.
    - Random uses the OS CSPRNG (BCryptGenRandom, getrandom) instead of
      an mbedtls HMAC-DRBG.
    - `GetLocalTimeSinceJan1970` comes from `LocalTime()` on every
      platform.
    - TimeUtil uses `localtime_s` on all `_WIN32` builds (fixes mingw).
    - The Matrix44 inverse and determinant are computed in double
      precision. Results differ from the old ones only around the 4th
      significant digit.
    - Analytics: the s64 magnitude has no undefined behaviour for
      INT64_MIN, and the reporter thread starts only after every member
      exists.
    - `MoveWithOverwrite` returns false on failure; it always returned
      true, and callers ignore the value.
    - `ScanDirectoryTree` doesn't throw on broken symlinks.
    - `CreateTempDir` on Windows no longer makes a double slash.
    - `SetUserPath(D_DUMPDEBUG_IDX)` also updates its BranchWatch and
      JitBlocks subdirectories (nothing calls it).
    - DirectIOFile's Windows read at end of file fails instead of
      looping forever.
    - `FileUtil.h` always includes `StringUtil.h`.
    - CompatPatches: on non-MSVC compilers it runs from a static
      initialiser, and it scans in 64 KiB steps (same result).
    - CPUDetect, Thread, IOFile, FileUtil, TimeUtil and CompatPatches
      now compile with mingw.
    - Thread names use `SetThreadDescription`; the exception trick
      runs only under MSVC.
    - Timer links `winmm.lib` and Random links `bcrypt.lib` with
      `#pragma comment` (MSVC).
    - Dropped: unused `Common.h` macros, and ConsoleListener's
      non-Windows `m_use_color`. Assert wording changed.
      `Projection.h` gained `#pragma once`.
  - Checked: harnesses ran the old and new code on the same inputs.
    Outputs are identical for:
    - threading (the events, BlockingLoop, WorkQueueThread);
    - Version, including an empty and a set commits-ahead count;
    - JSON;
    - Hash (CRC32 and Murmur paths), SettingsHandler, Timer and
      Profiler;
    - CPUDetect;
    - FileUtil and IOFile;
    - DirectIOFile and FileSearch;
    - IniFile (1500 random files, parse and save);
    - LinearDiskCache, including corrupt and truncated files;
    - ChunkFile, with random states, truncation and corruption;
    - Analytics (the wire bytes);
    - StringUtil, BitField and ranges;
    - Matrix, except the Matrix44 inverse noted above.
  - Checked: every changed `.cpp` passes a syntax check (fmt 12, C++23)
    with g++ on Linux and with mingw for Windows (NOMINMAX, UNICODE,
    WIN32_LEAN_AND_MEAN).
  - Checked: the whole-tree checks have no new failures against the W2
    base. The Linux check covered 712 files. The mingw check covered
    Common's `.cpp` files and the 141 files that fail on Linux only.
    The files still failing are missing a third-party header (enet,
    SFML, curl, spng, ...) or are outside W2. Six Common files that
    failed on mingw before now pass: CompatPatches, FileUtil, IOFile,
    Thread, TimeUtil and x64CPUDetect.

- W5 log: rewrote DiscIO's blob readers, converters and their helpers as
  original code: `Blob`, `FileBlob`, `CISOBlob`, `TGCBlob`, `SplitFileBlob`,
  `WbfsBlob`, `NFSBlob`, `CompressedBlob` (GCZ), `WIABlob` and
  `WIACompression` (WIA/RVZ), `LaggedFibonacciGenerator`,
  `WiiEncryptionCache`, `DiscScrubber`, `ScrubbedBlob`, `CachedBlob`,
  `VolumeFileBlobReader`, `DirectoryBlob` (each `.cpp` and `.h`) and
  `MultithreadedCompressor.h`. 35 files, 8943 lines before, 7584 after. Each
  starts with the "Original code for RetroPixels (PixelU-Wii)" line and has
  no Dolphin copyright or SPDX header. Dolphin's code was read only for
  interfaces and the behaviour other code relies on.
  - Kept: file names, every declaration other files use (including
    `FSTBuilderNode`, the `Content*` sources, `WIAFileReader` and
    `RVZFileReader`, `ConversionResultCode`, the `Convert*` functions and
    `GetAllowedCompressionLevels`), and every on-disk format. GCZ, WIA and
    RVZ files written by the new code are byte-identical to the old code's,
    and every reader returns the same bytes, except where noted below.
    `WIABlob.h` and `DirectoryBlob.h` still include what they used to, since
    other files (`DolphinTool/ConvertCommand.cpp`, for one) got headers such
    as `VolumeDisc.h` through them.
  - Changed inside DiscIO only: `SectorReader` (the base of the CISO, GCZ,
    TGC and WBFS readers) is now a plain 32-entry LRU block cache, and
    `SetChunkSize` is gone (no reader used it). `DiscContent`,
    `DiscContentContainer` and `DirectoryBlobPartition` moved into
    `DirectoryBlob.cpp`. `WIACompression.h` is a new internal decoder and
    encoder interface used only by `WIABlob.cpp`. `MultithreadedCompressor`
    keeps its API but now uses a job queue and a results map. Output still
    goes out in submission order, at most twice as many jobs as threads are
    in flight, and `Shutdown` can be called more than once.
  - Old-code bugs fixed (these are the only intended behaviour changes):
    - WIA/RVZ reading: within one `Read`, hash exceptions piled up across Wii
      groups, and the check that skips an exception list already applied
      dropped lists for chunks of 4 MiB or more. Reads of such files returned
      wrong data (on the synthetic test discs, whole-disc reads of 4 MiB and
      6 MiB WIA files didn't match the source). Exceptions are now cleared
      once a group's are applied. Exceptions now apply up to and including
      the last valid offset (the old `>` check could never be reached).
    - WIA writing: a WIA (not RVZ) chunk over 2 MiB copied past the end of
      the buffer for a last partial chunk. Only the in-bounds part is copied
      now, so output is unchanged.
    - WIA writing: chunks reused because their data is one repeated byte were
      keyed on the first byte of the whole Wii group, so two chunks with
      different bytes could share storage. The key now also includes the
      chunk's own byte. Output is the same unless the old code would have
      stored the wrong data.
    - WIA/RVZ: a chunk size of 0 in the header is rejected.
    - WBFS: `CopyReader` crashed for images split over several files. It now
      copies every part.
    - NFS: a block in a missing part file now fails the read; the old code
      indexed past the end of its file list.
    - GCZ: a block size of 0 is rejected, and the size check on the
      compressed data can't overflow.
    - CachedBlob: a read is served from the cache only if every cluster it
      touches is loaded; the old code checked only the first cluster of an
      unaligned read. Progress reporting can't divide by zero.
    - VolumeFileBlobReader: it keeps its own copy of the partition (the old
      one kept a reference that could dangle), and `CopyReader` works (the
      old one asserted and returned null).
    - DiscScrubber: a scrubber set up a second time starts from an empty
      table (the old one kept the old marks).
    - DirectoryBlob: `ShouldHideFromGameList` looped forever on a relative
      path that starts with "files" (e.g. `files/sys/main.dol`). It stops at
      the start of the path now. A `sys/main.dol` at the file system root
      no longer makes it take a substring past the end of the path.
    - MultithreadedCompressor: after an error or a cancel, no more output is
      written.
  - Checked with a test harness, built from both the old and the new
    sources with ASan and UBSan, on synthetic discs only. No game images,
    keys or firmware were used: test discs, tickets and partitions are
    generated, and Wii partitions are encrypted with a made-up key. It
    covers:
    - the junk generator (seed recovery and output);
    - plain, CISO, TGC, split, WBFS (one and several files) and NFS images;
    - GCZ and every WIA/RVZ compression type, level and chunk size, as
      conversion, whole and random reads, decrypted Wii reads, copies, and
      corrupt files;
    - scrubbing, the cached and volume-file readers;
    - extracted GameCube and Wii discs (with update, channel, `P-XXXX` and
      `Pn` partitions, missing and short files), `ShouldHideFromGameList`,
      and rebuilt volumes with Riivolution-style callbacks.
    Every log is identical between old and new. For WIA/RVZ reads the
    baseline is the old code with only the exception fix applied; the old
    code itself differs there only in read results.
  - Also checked: every changed `.cpp`, and the files that include these
    headers (DiscIO's volumes, verifier and Riivolution patcher,
    DolphinTool, UICommon's game files, `WiiUtils`, `DVDInterface`,
    `AchievementManager`), compile with g++ 13 (-Wall -Wextra, no new
    warnings). Where an external was missing (minizip, cpp-optparse, SFML,
    enet, picojson), the check used stub headers, and the only errors left
    came from those stubs. Every changed `.cpp` also compiles with
    x86_64-w64-mingw32-g++ 13 (`_WIN32` paths, including DirectoryBlob's
    `\` separators).

- W4 log: rewrote all 25 files of Common's x64 code generation, host and
  Gekko disassemblers and the Gekko assembler (9479 lines now, from
  13374). Every file starts with the original-code header and has no
  Dolphin copyright or SPDX line. File names and every public declaration
  other code uses are unchanged.
  - Emitter (`x64Emitter`, `x64Reg.h`, `x64ABI`, `CodeBlock.h`): encoders
    written again from the Intel SDM and AMD APM encodings (ModRM/SIB/REX/
    VEX building, operand classes, the SSE/AVX/BMI/FMA tables). The ABI
    helpers keep the same push/pop order, shadow space and stack alignment
    for the Windows x64 and System V conventions.
  - `JitRegister` (perf map output, same format) and `HostDisassembler`
    (Bochs and LLVM back ends and the hex-dump fallback, same text).
  - `GekkoDisassembler`: table-driven decoder that keeps its state per call
    instead of in static buffers (so it is thread-safe now). Same
    `Disassemble`, `GetGPRName` and `GetFPRName`.
  - Assembler (`Assembler/*`): lexer with lazy lookahead and the same three
    identifier rules; recursive-descent parser with precedence climbing and
    the same plugin callbacks in the same order (the syntax highlighter
    depends on them); IR builder that evaluates directives at once and
    instruction operands after the last label is known; encoder driven by
    a table of instruction layouts plus rewrites for the simplified
    mnemonics. Same error messages and positions. The GekkoMnemonic and
    ExtendedGekkoMnemonic enums were internal and are gone.
  - Deliberate differences (bug fixes, nothing a game sees):
    - Emitter: `MOVQ_xmm(m64, xmm)` (66 0F D6) put REX before the 0x66
      prefix, where the CPU ignores it; now 0x66 then REX. BT/BTS/BTR/BTC
      with a register bit index and a RIP-relative operand, and
      PEXTRW/PINSRW/PINSRD with a RIP-relative operand, encoded the wrong
      displacement because they did not count the trailing byte.
    - Assembler: an error on the first token of a line reported the
      previous line as `error_line` (the debugger's error display could
      then throw on `substr`). It is now the token's own line.
    - Assembler: out-of-range `.float`/`.double` literals (e.g. 1e50) threw
      `std::out_of_range` from stof/stod; they now give inf/0 like strtof/
      strtod.
    - Assembler: division by zero in an expression crashed with SIGFPE
      (including when the divisor was an unresolved symbol); it is now the
      error "Division by zero", reported at the token after the
      expression.
    - Assembler: `b<cond>lr`/`b<cond>ctr` (and their l/+/- forms) rejected
      the optional CR field operand (`beqlr cr1` gave "Unexpected token
      ... where line should have ended"); they now accept it and encode
      the same word as the matching `bclr`/`bcctr`.
    - Assembler: `Lexer::LookaheadTagRef` could loop forever or read past
      its buffer when asked for a token not yet lexed; it now lexes ahead
      as needed. `AssemblerError::FormatError` no longer calls `back()` on
      an empty line.
  - Checked:
    - Emitter: old and new emitters linked into one harness, every public
      method called with generated operands: 110,265,978 calls with all CPU
      features on and all off, small-buffer runs (write failures) and
      Windows-ABI runs. Bytes, alerts and write-failed flags identical,
      apart from the three fixes above (34,304 MOVQ cases, exactly the
      reordered prefix; 1,152 BT* cases; 864 PEXTRW/PINSRW/PINSRD cases).
      `ABI_CallLambdaPC` differs only by build-specific addresses.
      CodeBlock tests identical.
    - JitRegister and HostDisassembler (Bochs real, LLVM mocked): identical
      output.
    - GekkoDisassembler: all 2^32 instruction words, random addresses,
      both byte orders: identical text.
    - Assembler: 3 million generated sources (valid programs for every
      mnemonic and suffix, directives, labels, numeric labels, @ha/@l,
      expressions, strings with escapes, floats, comments, CRLF, NUL bytes,
      mutated and random text) through old and new: the assembled blocks,
      errors (message, line, col, len, error line, FormatError), a full
      trace of every parser callback with lexer positions, and lexer token
      dumps under seven match-rule sequences. 450,112 sources assembled
      without error. Every difference is one of the fixes above:
      previous-line `error_line` (old) vs own line (new), old stof/stod
      exceptions, old "where line should have ended" right after a
      `b<cond>lr/ctr` mnemonic, and 25 old SIGFPE crashes (new: an error
      each time). Targeted tests
      check `b<cond>lr/ctr[l] crN` for all 12 conditions and 8 fields
      against `bclr`/`bcctr`, float overflow results and div-by-zero
      errors.
    - Syntax: every W4 .cpp with -Wall -Wextra, and the files outside W4
      that use W4 headers (Jit64, Jit64Common, JitCommon, DSP JIT,
      CachedInterpreter, Interpreter, VertexLoaderX64, PPCDebugInterface,
      and DolphinQt's GekkoSyntaxHighlight against a Qt stub plus the
      assembler/disassembler calls of the other DolphinQt users), compiled
      with g++ (Linux) and clang `-target x86_64-w64-windows-gnu` and
      compared with the untouched tree: no new failures, and no warnings
      from W4 code (MainSettings.cpp fails in both trees for lack of
      mbedtls headers here).
  - Not in W4: `x64CPUDetect.cpp` and `CPUDetect.h` belong to W2.

- W6 log: rewrote all 32 W6 files in `Core/DiscIO` (volumes, file
  systems, verifier, Riivolution, NAND import, banners) as original
  code. They now have 8290 lines (from 8676). Each starts with the
  RetroPixels original-code line and has no Dolphin copyright or SPDX
  header. The files are DiscExtractor, DiscUtils, Enums,
  FileSystemGCWii, Filesystem, GameModDescriptor, NANDImporter,
  RiivolutionParser, RiivolutionPatcher, Volume, VolumeDisc, VolumeGC,
  VolumeVerifier, VolumeWad, VolumeWii and WiiSaveBanner (.cpp and .h).
  - Kept unchanged: file names, and every public declaration (names,
    signatures, namespaces, enum values, constants, packed structs).
    Everything that affects output also stays the same:
    - game IDs, maker IDs, revisions, regions, countries, languages,
      titles and descriptions;
    - GC/Wii banners and the WAD and save banner layout;
    - the partition table, ticket/TMD/cert reads, title-key decryption
      (IOSC common-key handles 4 and 11, IV = title ID), the hash tree
      (H0/H1/H2/H3), block encryption and decryption, and `HashGroup`
      and `EncryptGroup` output;
    - FST parsing and lookups;
    - the extraction layout (sys files, apploader, DOL, FST, folders,
      `_sys`);
    - the verifier's checks, problem order, severities, hash results,
      Redump matching and cache file, and every report string;
    - Riivolution XML parsing, placeholder replacement (builtins first,
      then params), the HostFS path rules (including the `//` quirk), and
      file, folder, memory, search and ocarina patch application;
    - the GameModDescriptor JSON;
    - the NAND import tree and its keys file;
    - every GetStringT, FmtFormatT, PanicAlertFmtT and _trans string,
      and every log string.
  - Kept quirks: Riivolution `<macro>` elements are still a no-op.
    `IsValidForGame`/`GeneratePatches` still use `substr`, so they throw
    on IDs that are too short. Numbers are still parsed with `stoi`.
    WAD offset sums still wrap as u32. VolumeWii's `GetLongNames` still
    treats the number of bytes read as a number of characters, so a
    short read keeps more of the names than were read.
  - Deliberate differences (genuine bugs, harmless fixes):
    - `RegionCodeToRegion` returns `Region::Unknown` for codes above 4.
      It used to return out-of-range enum values.
    - VolumeWii drops its cached block when a read fails, so the next
      read doesn't return stale data.
    - The `ExportDirectory` progress callback gets the full path of each
      child (`Data/a.bin`, not just `a.bin`). It also adds a `/` to the
      export folder only when one is missing, so nested folders no
      longer get a double slash (`out//Sub/`). The files land in the same
      place.
    - FileSystemGCWii checks the FST entry count in u64, so a huge count
      can't overflow the size check. The result is unchanged; only the
      log message differs.
    - Copying a default-constructed `FileInfo` const_iterator no longer
      dereferences null.
    - `HashGroup` zeroes every padding field of the hash blocks. It used
      to leave stale bytes in part of the H1/H2 padding.
    - The fmt formatter for NAND FST entries stops at the end of the
      12-byte name field (`strnlen`), so it never reads past an
      unterminated name.
    - The verifier's hex parse for Redump hashes is a standard hex parse.
    - `VolumeVerifier::Finish` waits for the Redump lookup before reading
      its status. This fixes a race.
    - Riivolution search patches stop at the last whole step, so the
      search no longer hangs when the length is smaller than the
      alignment.
    - Riivolution HostFS `GetFileContents` rejects directories instead
      of trying to read one as a file.
    - Enums uses a single `_trans("Unknown")` instead of three copies.
      The string is the same.
  - Checked: a harness ran the old and new code on synthetic inputs only:
    fake GC and Wii headers, a fake FST, fake WADs, fake partitions
    encrypted with a made-up test key injected through an IOSC/ES stub,
    and fake sparse BootMii NAND images with a test OTP. No Nintendo
    keys, images or NAND dumps were used. Outputs are identical for:
    - the Enums tables (176558 lines);
    - WAD volumes and WiiSaveBanner;
    - all Riivolution parsing, patch generation, HostFS, a file-patch
      fuzz (400 runs) and memory patches;
    - NAND import (4 image variants, extracted tree and keys);
    - the GC and Wii volumes, extraction, `HashGroup`/`EncryptGroup`,
      the verifier, Redump matching (13 scenarios) and a 250-run header
      fuzz, apart from the deliberate differences above.
    - String check: every translatable and log string is the same, apart
      from the `_trans("Unknown")` change.
    - Compile check: every W6 .cpp builds with g++ (Linux) and clang
      `-target x86_64-w64-windows-gnu`. Under -Wall -Wextra they give the
      same warnings as the old code. Both compilers build the 249 files
      that include W6 headers with the same pass/fail status as the
      untouched tree (Linux: 124 ok, 125 fail; Windows: 114 ok, 135
      fail). The failures come from missing Qt, SFML, cubeb, libusb and
      similar.

- W7 log: rewrote all 32 files of the PowerPC core outside the CPU
  backends (10740 lines now, from 11905): `BreakPoints`, `CPUCoreBase.h`,
  `ConditionRegister`, `Expression`, `GDBStub`, `Gekko.h`, `JitInterface`,
  `MMU`, `PPCAnalyst`, `PPCCache`, `PPCSymbolDB`, `PPCTables`, `PowerPC`,
  and `SignatureDB/{CSV,DSY,MEGA,}SignatureDB`. Every file starts with the
  original-code header and has no Dolphin copyright or SPDX line. File
  names, public declarations, register/struct layouts (`PowerPCState`
  offsets the JIT uses, checked on Linux and MSVC-style layout), savestate
  `DoState` order, symbol map / signature file formats and the GDB wire
  protocol are unchanged.
  - CPU state: `Gekko.h` (SPR numbers, register bitfields, exception bits),
    `ConditionRegister` (the 64-bit-per-field CR form: SO bit 59, LT bit 62,
    EQ when the low word is zero, GT when the value is positive),
    `CPUCoreBase.h`, `PowerPC` (state reset, exceptions and their priority,
    decrementer/timebase, MSR/MMCR updates, single step, breakpoint and
    memcheck checks), `PPCCache` (instruction cache with the same pseudo-LRU
    tree, valid bits and DoState).
  - `MMU`: effective-to-physical translation (BATs, page table with the
    same PTEG search order and R/C bit updates, TLB with the same
    replacement), host read/write helpers, DSI/ISI generation, dcbz/dcbi
    helpers and the fastmem BAT/page mappings.
  - `PPCTables`: one flat instruction list (form, extended opcode, name,
    class, cycles, flags) built into the decode tables at compile time; a
    duplicate encoding stops the build. Every encoding gives the same
    name/class/cycles/flags as before.
  - `PPCAnalyst`: block decoding, branch following, instruction reordering
    (cror/cmp/carry bubbling), register liveness and the per-op
    flag/format passes. `CodeOp`/`CodeBlock` contents are unchanged.
  - `PPCSymbolDB` (function detection, map file load/save in every format
    the old parser accepted), `SignatureDB` and its DSY, CSV and MEGA
    readers/writers (same checksums and matching rules).
  - `BreakPoints`/`Expression` (conditions still go through the expr
    library), `JitInterface` (core selection, block log dump byte for byte,
    icache invalidation, exception checks), `GDBStub`.
  - Deliberate differences (bug fixes, nothing a game sees):
    - `MMU`: when an old page-table entry's page address cannot be worked
      out, the fastmem mapping update now still adds the new entry (the old
      code skipped it, so stale mappings could stay); the page-table mirror
      is bounds-checked before it is read.
    - `PPCSymbolDB`: folding `(entry of ...)` in map files no longer uses
      an overlapping `strcpy` that could garble names or overflow; empty
      or wrapping notes no longer walk off the note map; `DeleteFunction`
      removes the symbol from the hash index too (left a dangling pointer).
    - `SignatureDB`: the checksum loop counts words, so a function ending
      at the top of the address space no longer loops forever; DSY loading
      stops at a truncated record (same resulting table).
    - `GDBStub`: `m` with a length too big for a reply only answers `E01`
      (it also overran a buffer and sent a second reply); `m`/`M` ranges
      that leave RAM answer `E00` (crashed); `G` reads registers from the
      payload (it started at the command letter, shifting every register);
      an interrupt byte no longer causes an extra empty `$#00` reply, and
      an oversized packet is no longer dispatched after its NAK; a dropped
      connection in the middle of a packet no longer spins forever; field
      parsing stops at the end of the packet; `z` for a watch that overlaps
      the range without starting at its address no longer loops forever;
      `Deinit` closes the sockets (they leaked); a local socket path that
      does not fit is refused instead of overflowing.
    - `PPCTables`: the compiled-instruction log lists ops in table order
      (cosmetic). `PowerPC.h`: the dead non-x64 `ps` placement and an
      unused member are gone (layout unchanged on x64).
  - Kept on purpose because other code or users see them: the CoreTiming
    event names (`GDBStubUpdate` etc.), JIT block log columns and the
    signature/map file text formats.
  - Checked: old and new code run side by side under ASan/UBSan on the
    same inputs, with identical results (old copies patched only for the
    fixes above):
    - Gekko bitfields, `PowerPCState` offsets (Linux and MSVC-style
      layout), ConditionRegister, PPCCache, BreakPoints/Expression.
    - MMU + PowerPC system test with random BATs, page tables, TLB use and
      exceptions (20 seeds).
    - PPCTables over every OPCD x XO x Rc word plus 2M random words.
    - PPCAnalyst on random code (13 seeds, 424 reorders seen).
    - PPCSymbolDB map load/save/detection (20 seeds), SignatureDB family
      (9 seeds), JitInterface against a logging fake JIT (5 seeds).
    - GDBStub: scripted debugger sessions over a real local socket (bad
      checksums, interrupts, oversized packets, every packet type, register
      and memory reads/writes, breakpoints and watches; 40 seeds), comparing
      the reply bytes and every side effect.
  - Compile check: the 15 W7 .cpp files and 80 files that include W7
    headers (JIT, interpreter, HW, HLE, boot, debugger, IOS) with g++ and
    with clang for x86_64-w64-windows-gnu, `-Wall` clean for the W7 files.
    The only failures are the same as on the base tree (missing SFML/lz4/
    glslang headers here, and the MinGW library's missing `std::expected`).
  - Not rewritten: nothing in W7. The CPU backends (Jit64, interpreter,
    cached interpreter) belong to later batches.

- W8 log: rewrote all 20 files of the PowerPC interpreter and the cached
  interpreter (Core/PowerPC/Interpreter and Core/PowerPC/CachedInterpreter).
  Every file starts with the original-code header and has no Dolphin
  copyright or SPDX line. File names and every public declaration other
  code uses are unchanged: the Interpreter class with all its static
  instruction handlers, `GetInterpreterOp`, `RunInterpreterOp`,
  `RunTable4/19/31/59/63`, `Helper_Carry`, ExceptionUtils'
  `GenerateAlignmentException`/`GenerateDSIException`/
  `GenerateProgramException`, and the CachedInterpreter, its block cache
  and its emitter (`Write`, `PoisonCallback`, `CachedInterpreterCodeBlock`).
  - Interpreter: FP helpers (`Interpreter_FPUtils.h`, now in namespace
    `InterpreterFP`) cover FPSCR bookkeeping, rounding to single (with the
    NI flush quirk), the 25-bit multiplier rounding, the fused multiply-add
    with the double-rounding correction, NaN propagation order,
    invalid-operation flags, and the lfs/stfs/psq bit conversions. Scalar
    and paired-single arithmetic share element-wise helpers. Integer ops
    compute carries from a 64-bit sum. Loads and stores go through small
    templates for update, indexed and byte-reversed forms. String and
    multiple-word transfers keep the old DSI behaviour and the aligned
    read-modify-write of the last word. The GQR scale tables are built at
    compile time. Decoding uses switch-based decode functions that are
    evaluated at compile time into the same flat tables. The fetch/execute
    loop, HLE hooking, the debug-mode run loop and timing are the same as
    before. The private helpers that used to be static members
    (Helper_UpdateCR0, Helper_FloatCompare*, Helper_StoreString, ...) are
    now file-local.
  - Cached interpreter: same record format (callback pointer plus operand
    struct), operand layouts, block sizes, free-range reuse, retry on a
    full cache, ISI on an untranslatable block start, FPU, breakpoint and
    idle checks, and disassembly text.
  - Deliberate differences (bug fixes):
    - fcmpu/fcmpo/ps_cmpu0/1/ps_cmpo0/1: FPSCR[FPCC] is now replaced by the
      comparison result. The old code applied FPCC_MASK (0xF << 12) to the
      5-bit FPRF field, which cleared nothing, so FPCC kept the OR of
      every earlier comparison. Jit64 and the hardware replace it.
    - addme/subfme with CA=1: XER[CA] is now the true carry out of
      rA + 0xFFFFFFFF + 1, which is always 1. The old code computed it as
      Helper_Carry(rA, CA - 1) and got 0. This now matches Jit64's ADC.
  - Checked:
    - Instruction harness: old (base) and new interpreter, each linked with
      the same base W7 files (PPCTables, ConditionRegister, FloatUtils, the
      FPU rounding code) and a stubbed system (generated memory with DSI
      faults, logged MMU, cache, timer, HLE and exception calls). Every
      handler runs on random state: special GPR values, special and random
      doubles (zeros, denormals, infinities, QNaN/SNaN with payloads),
      every rounding mode, NI on/off, random enable bits, FE0/FE1, PR, FP,
      HID2 PSE/LSQE, random GQR types and scales, and software and host
      flush-to-zero. Compared each time: all registers, FPSCR, CR, XER,
      MSR, SPRs, SRs, reservation, MXCSR control bits, branch-watch
      records, and the ordered log of memory and system calls.
      - Against the old code with only the two fixes above patched in:
        identical for 5.2M random instructions over all opcodes (-O2), 2M
        more with software flush-to-zero, about 24M per-handler cases
        (300k for each of the 83 FP, paired and FP/quantised load-store
        handlers), and 500k random plus 100k FP cases at -O0. The unpatched old
        code differs only in the compare and addme/subfme cases.
      - Decode tables: GetInterpreterOp gives the same handler for all
        65,536 (primary, extended) pairs.
      - Instruction streams: 72k random programs run through
        SingleStep/Run (HLE hooks, ISI/DSI, breakpoints, debug mode,
        varying slice lengths): identical.
    - Cached interpreter harness: old and new CachedInterpreter over the
      new interpreter, with the base JitCache and PPCAnalyst. Random
      programs with random memcheck, FP-exception, div-by-zero,
      profiling, branch following, debugging/breakpoint, stepping,
      no-block-cache and icbi invalidation settings. Compared: guest state
      and call log, plus every cached block's size and disassembly text
      and the free-range statistics. 6,000 programs (about 5.5 blocks
      each; 293 ran to the step limit) gave identical results. A mutant
      that never emits the memcheck exception check differs on 147 of 300
      programs, so the harness can see that check.
    - Syntax: every W8 .cpp (with -Wall -Wextra: no warnings from W8 code)
      and the files outside W8 that use W8 headers (System.cpp,
      PowerPC.cpp, JitInterface.cpp, Jit64/Jit.cpp, Jit64/Jit_Integer.cpp,
      JitCommon/ConstantPropagation.cpp), compiled with g++ (Linux) and
      clang `-target x86_64-w64-windows-gnu` and compared with the
      untouched tree: no new failures. System.cpp fails on the Windows
      target in both trees because the glslang headers are missing here.
    - tools/verbatim-audit.py (min run 8): the only runs left in W8 files
      are in headers: the handler declaration list, the class
      declarations and the operand structs. No function body is flagged.

- W11 log: rewrote 19 of the 21 W11 files, the DVD drive and memory card
  code (5389 lines now, from 6246): `DVD/DVDInterface`, `DVD/DVDMath`,
  `DVD/DVDThread`, `DVD/FileMonitor`, `GCMemcard/GCIFile`,
  `GCMemcard/GCMemcard`, `GCMemcard/GCMemcardBase.h`,
  `GCMemcard/GCMemcardDirectory`, `GCMemcard/GCMemcardRaw` and
  `GCMemcard/GCMemcardUtils`. Every rewritten file starts with the
  original-code header and has no Dolphin copyright or SPDX line. File names,
  public declarations, the DI register unions, the `ReadRequest` layout
  (stored raw in savestates), savestate `DoState` order and bytes, the
  CoreTiming event names and their registration order, the DVD command
  handling and error codes, the DVDMath numbers, the memory card on-disk
  formats and all translatable strings are unchanged.
  - `DVDMath`: seek and read timing from the disc geometry (inner/outer
    radius, track pitch, layer size) with one throughput-curve table for GC
    and Wii discs. The results are bit-identical to the old code.
  - `DVDInterface`: register MMIO, command decoding (reads, disc ID,
    inquiry, seek, request error, audio stream and status, stop motor,
    audio buffer config, the Wii-only commands), error codes, interrupt
    flags and masks, lid/cover handling, disc change and auto change, DTK
    audio streaming and the read-ahead timing model. The streaming and
    read-ahead state are grouped into two private structs, but are still
    saved field by field in the old order.
  - `DVDThread`: worker thread, read queue, result map, the timing of read
    completion events, and the disc-not-found and read-failure messages.
    `FileMonitor`: same FILEMON lines and log levels, same skipping of
    repeats.
  - `GCMemcard`: header, directory and BAT structures, checksums, serial
    numbers, format/create/open (with the same error bits for every kind of
    damage), import/export/delete, save data access, comments, banner and
    animated icon decoding, and the PSO and F-Zero GX re-signing.
    `GCMemcardUtils`: GCI/GCS/SAV reading and writing from one table of
    container layouts, identity checks and default names.
  - `GCMemcardRaw`: the raw image card and its flush thread.
    `GCMemcardDirectory`: the GCI folder card (choosing which saves to load,
    building the system area, block mapping for reads and writes, keeping
    the saves in step with directory writes, flushing to .gci files and
    renaming deleted saves to .deleted, migration from a raw card,
    NetPlay file lists). `GCIFile` and `GCMemcardBase.h`.
  - Deliberate differences (bug fixes, nothing a game sees):
    - `BlockAlloc::ClearBlocks` stops after BAT_SIZE steps, so a BAT with
      a cycle no longer hangs.
    - `GCMemcard::GetSaveData` refuses a block chain that leaves the card
      (the old code read past the data blocks).
    - `GCMemcard::ImportFile` pads a save whose block list is shorter than
      its entry's block count with erased blocks (the old code read past
      the vector, so the card got heap garbage). If the BAT runs out in the
      middle of an import it reports OUTOFBLOCKS with a "Fatal Error" panic
      instead of writing out of bounds.
    - `GCMemcard::PSO_MakeSaveGameValid` does nothing for a PSO save with
      fewer than 2 blocks (the old code wrote out of bounds).
    - Log and debug messages that users don't see are reworded or merged.
      `GCMemcardDirectory::Write` no longer logs every write whose length
      isn't 0x80. The compiled-out `_WRITE_MC_HEADER` debug code is gone.
  - Checked: old and new code side by side on the same inputs, using the
    untouched W10 files on both sides, with identical output streams (only
    hashes were kept):
    - DVDMath over grids of offsets/distances/speeds: 4,809,145 values
      bit-identical at -O0, -O2 and -O2 with FMA.
    - DVD: a fake system (CoreTiming queue, memory, MMIO, synthetic GC and
      Wii discs, file monitor) running random DI command sequences:
      registers, interrupts, scheduled event cycles, DMA bytes, DTK
      samples, panics/OSD messages and savestate bytes after each step, plus
      a savestate load round trip. 400 seeds, about 7M lines.
    - Memory card, about 1,180 seeds over five scenarios:
      - static and member Format.
      - Header/Directory/BAT operations and their checksums and error bits.
      - Create plus random import/remove/export/query sequences (including
        PSO and F-Zero saves, TITLEPRESENT, out of blocks/entries, banner,
        animation and comment decoding), Save and Open.
      - 12 kinds of damaged cards (bit flips in each area, bad sizes, free
        count, data in the unused area, dir/BAT inconsistency, size field,
        counters), opened and used.
      - GCI/GCS/SAV write/read round trips and damaged or random files,
        identity checks.
      - GCI folders with valid, duplicate, oversized, truncated and
        other-game saves, both CurrentGameOnly settings and MC_SYSTEM_AREA
        files, driven by game-like write sequences: GetFileNamesForGameID,
        card reads, ClearBlock, savestate bytes and load round trips, the
        output folder after each flush and at destruction, and migration.
      - Raw cards (new, existing and odd-sized files): read/write/clear
        with valid and invalid addresses, savestate bytes and the file
        written at shutdown.
      The harness keeps block lists matching their block counts, because
      the old code has undefined behaviour otherwise (see above).
  - Compile check: the 10 W11 .cpp files and the 20 non-Qt files that include
    W11 headers (boot, config, EXI, SI, Triforce, IOS DI/MIOS, Movie,
    NetPlay, System, HW), with g++ and with clang for x86_64-w64-windows-gnu.
    The rewritten files (headers included) are clean under clang with
    `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow`. Integer
    narrowing, sign changes and int/float conversions are written as
    explicit casts, keeping the same values (MSVC /W4 with warnings as
    errors). Other failures match
    the base tree exactly (missing SFML/glslang/UnixUtil headers here, and
    MinGW's missing `std::expected`). The DolphinQt users (GCMemcardManager,
    GCMemcardCreateNewDialog, GameCubePane, ...) can't be compiled here
    because there are no Qt headers. They only use declarations that are
    unchanged.
  - Verbatim audit (`verbatim-audit.py wii 8`): the runs left in W11 files
    are all public declarations in headers (struct/class layouts, enums,
    member lists). No run is left inside a function body.
  - Not rewritten: `DVD/AMMediaboard.cpp/.h` (Triforce media board, 2698
    lines). It emulates an undocumented, reverse-engineered protocol
    (network sockets, the media board's command set and memory map). The
    only description of its behaviour is the existing code, so it can't be
    rewritten clean-room, and it can't be tested here without Triforce
    software. It is unchanged and keeps its original header.

PixelU-Wii, vwii pass: verbatim-run clean-up of the rewritten files
=====================================================================

Scope: files whose header already read "Original code for RetroPixels (PixelU-Wii)" at 94a5c2f.
Tool: python3 tools/verbatim-audit.py wii 8 (runs of 8+ counted lines identical to Dolphin 6da5148).

Audit counts (runs of 8+ lines)
  before (94a5c2f): 244 total = 60 in .cpp + 184 in .h
  after:            155 total =  6 in .cpp + 149 in .h
  function bodies only (.cpp runs, plus header runs containing a statement such as a return,
  a branch or an assignment):
  before 91 (60 .cpp + 31 .h), after 10 (6 .cpp + 4 .h)

What was reworked (behaviour unchanged, checked by old-vs-new harnesses)
  - .cpp files: GekkoLexer (punctuator and type-name tables, digit-form table),
    PowerPC.cpp (DoState field helper with the same Do order, SPR reset table, Init/Reset),
    JitInterface, DirectIOFile, FileBlob/CompressedBlob/WIABlob output opening, PPCAnalyst
    exception-vector table, SignatureDB/MEGASignatureDB, VolumeGC/Wii/WAD getters, MMU (DSI alert
    helper, explicit instantiations), Blob type-name table, HttpRequest, FatFsUtil, DiscIO Enums
    region helpers, Timer, StringUtil ValueToString, Watches, VolumeVerifier, IOFile,
    TraversalClient, DiscUtils partition names and JoinPath, SymbolDB, VolumeFileBlobReader,
    DiscExtractor (H3 export), RiivolutionPatcher, FileSystemGCWii, WiiSaveBanner, Network.
  - Header inline code: Matrix.h vector operators, PowerPC.h PairedSingle, x64Emitter.h ABI call
    helpers, ChunkFile.h (PointerWrap arrays and pointers), BitField.h formatters,
    AssemblerShared.h, DirectIOFile.h, Contains.h, FileSearch.h, MathUtil.h Rectangle,
    StringUtil.h, CodeBlock.h, Profiler.h, MMU.h host access wrappers, Gekko.h FPSCR operators,
    BreakPoints.h, Config.h, Layer.h, IniFile.h.
  - Order-sensitive code (DoState field order, CoreTiming and config registration, side effects)
    was kept in its original order.

What was left, and why
  - Data tables (6 .cpp runs): the DiscIO/Enums.cpp publisher table (3 runs),
    the AssemblerTables.cpp SPR name table, and the FloatUtils.cpp frsqrte/fres expected-value
    tables. These are facts about the hardware or data formats, not code.
  - Interface defaults (4 header runs): the Volume.h default virtual bodies (return nullopt,
    INVALID_TICKET, 0, false and so on) and SymbolDB.h's default GetSymbolFromAddr and
    AddFunction. They are part of the public interface.
  - Interface declarations (the remaining 145 header runs): public class declarations,
    enums and struct layouts. This covers the Gekko.h register bitfields, PowerPCState and
    PowerPC.h layout and macros, the Network.h packet structs (which have static_asserts), the
    x64Emitter declarations, the FileUtil.h and Log.h enums, CommonPaths macros, Volume*,
    Blob and VolumeVerifier class declarations, Config enums, MMU and BreakPoints declarations,
    and the include/forward-declaration blocks. These were kept as the task allows.

Checks
  - Old-vs-new harnesses (stdout or hash compared) for each logic change, including: the lexer,
    PowerPC save/load/reset, Blob names, Enums, Timer, IOFile, partition names and JoinPath
    (740k inputs), Matrix, x64Emitter (1.49 MB of emitted code), ChunkFile, BitField and
    Contains. All were identical.
  - Exported symbols compared (symcmp) for the MMU explicit instantiations: identical.
  - Syntax checks of the changed .cpp files and their dependents, base vs new, on Linux (g++)
    and Windows (clang -target x86_64-w64-windows-gnu), with
    -Wall -Wextra -Wconversion -Wsign-compare. That was 101 translation units and 202 jobs.
    Pass/fail matched base in every one, and no warning appeared in a changed file that base
    did not already have. The files that fail on both sides fail for reasons outside this work:
    Qt and imgui headers are missing, windows.h is missing on Linux, and the MinGW library has
    no std::expected (MultithreadedCompressor.h, FileSystem.h, CheatSearch.h). The Linux build
    covers those files.

- W10 log: rewrote all 36 W10 files (CoreTiming and the core hardware
  in `Core/HW`) as original code. They now have about 8500 lines (from
  9898). Each starts with the RetroPixels original-code line and has no
  Dolphin copyright or SPDX header. The files are CoreTiming,
  AddressSpace, AudioInterface, CPU, DSP, GPFifo, HW, MMIO (with
  MMIOHandlers.h), Memmap, MemoryInterface, ProcessorInterface, Sram,
  StreamADPCM, SystemTimers, VideoInterface, WII_IPC and WiiSave (with
  WiiSaveStructs.h).
  - Kept unchanged: file names, and every public declaration that other
    files use (names, signatures, namespaces, enum values, constants,
    packed structs). Everything that affects emulation also stays the
    same:
    - CoreTiming: the event API, every event name ("_lost_event" and the
      device events), the `Globals` layout the JITs read, the slice and
      downcount arithmetic, the event heap order (the same std::ranges
      heap operations), throttling and the savestate layout;
    - MMIO: the handler factories, the visitor interface Jit64 uses to
      inline constant and direct reads, the handler tables and
      `UniqueID` indexing;
    - the MemoryManager layout, the shared memory segment and fastmem
      arena layout, DBAT and page-table mappings (small and large host
      pages), and the Memory savestate;
    - every register address, bit layout, handler kind (constant,
      direct, complex) and direct pointer, so JIT fast paths are the
      same;
    - SystemTimers, AI, DSP, VI, PI, MI and IPC timing to the cycle,
      including the integer arithmetic and promotions (the 32-bit
      decrementer product, the field position modulo done in u64, the
      31-bit ARAM DMA count);
    - the DSP ARAM DMA paths, including the configuration-4 mirror and
      the high speed port;
    - the CPU thread state machine (State values, stepping, breaks,
      PauseAndLock, jobs, GDB stub stepping);
    - the HW init, shutdown and savestate order and marker names;
    - the SRAM layout, its default contents (including the
      "DOLPHINSLOTA"/"DOLPHINSLOTB" flash IDs), the card flash ID
      derivation and the checksums;
    - the streamed ADPCM decoder and its savestate;
    - the data.bin format (header, Bk header, file records, MD5 blanker,
      SD IV, NG ID 0x0403AC68, end magic 0x2f536969), the NAND save walk
      and the FS call order, Copy/Import/Export/ExportAll and every log
      string;
    - every savestate is byte-identical.
  - Kept quirks: AddressSpace's default 16/32/64-bit writers still write
    the low part first, and the backward search still stops at a lower
    bound of length-1 and returns nothing from offset 0. Split 32-bit
    MMIO writes still write the high half first. The DSP ARAM DMA still wraps
    its count at 31 bits. SystemTimers still multiplies the decrementer
    value by TIMER_RATIO in 32 bits. Exported data.bin file records are
    still written with an all-zero IV, which is also the IV their
    contents are encrypted with.
  - Deliberate differences (genuine bugs, harmless fixes, cleanups):
    - The backward ARAM search in AddressSpace skips start positions
      whose match would run past the end of ARAM. It used to read past
      the end of the ARAM buffer.
    - Split 32-bit MMIO reads read the high half first, then the low
      half, in a fixed order. The order used to depend on the compiler's
      evaluation order. No handler in the tree has side effects that
      depend on it.
    - Memmap keys merged DBAT runs by their mapped start. The old code
      keyed them by run start, which would lose a run spanning two
      regions, but BAT_PHYSICAL_BIT always splits such runs, so nothing
      observable changes.
    - Removed public types that nothing else used: the VI register
      unions, WII_IPC's `CtrlRegister` and the
      `MMIO_PUBLIC_SPECIALIZATIONS` macros. MMIO declares and defines its
      explicit instantiations itself.
    - Empty MMIO slots behave as Invalid without allocating a handling
      method each.
  - Checked: a harness linked the old and the new code (each with its own
    CoreTiming and MMIO) against mocks of the rest of the emulator and
    compared every trace line, event schedule, interrupt and savestate
    byte. No Nintendo keys, BIOS, firmware or games were used; WiiSave
    ran on an in-memory NAND with fake crypto. Outputs are identical for:
    - CoreTiming (203 seeds), MMIO (50 seeds with the JIT visitor),
      AddressSpace (30), PI (20), MI (20), SystemTimers (30), AI (30),
      DSP (30), VI (30), IPC (30), GPFifo (30) and Memmap (50 seeds:
      BATs, page mappings, accesses, savestates, allocation failures);
    - CPU (340 scripted single-thread runs covering stepping, breaks,
      PauseAndLock, jobs, GDB stepping and power down);
    - HW order and savestate bytes (GC and Wii, with and without SRAM
      override), Sram (30 seeds, 2000 cards each), StreamADPCM (30 seeds,
      20000 blocks each) and WiiSave (360 seeds: NAND to data.bin and
      back, corrupted files, FS failures, Import/Export/ExportAll).
    - Compile check: every W10 .cpp builds with g++ (Linux) and clang
      `-target x86_64-w64-windows-gnu`, apart from WiiSave.cpp on the
      Windows target, which fails in the untouched tree too (this
      machine's mingw headers lack std::expected). Under clang and g++
      `-Wall -Wextra -Wconversion -Wsign-compare` the W10 files give no
      warnings. The 161 files that include W10 headers build with the
      same pass/fail status as the untouched tree (Linux: 131 ok, 30
      fail; Windows: 109 ok, 52 fail). The failures come from headers
      this machine lacks (Qt, SFML, libusb, std::expected in mingw and
      similar).
    - Verbatim audit (`tools/verbatim-audit.py wii 8`): no function body
      in W10 .cpp files shares 8 or more lines with the old code. The
      remaining runs are register and enum tables, struct layouts and
      class declarations in the headers, and one register address table
      in DSP.cpp.
- W13 log: rewrote 46 of the 68 W13 files: serial interface, controller
  devices, pad/keyboard/GBA input, the high speed port, the Game Boy
  Player, the mGBA core wrapper and the Triforce serial base class (5487
  lines now, from 6336). The batch is 10990 lines, half of it original now.
  Rewritten:
  - `SI/SI`, `SI_Device`, `SI_DeviceNull`, `SI_DeviceGCController`,
    `SI_DeviceGCSteeringWheel`, `SI_DeviceDanceMat`, `SI_DeviceGCAdapter`,
    `SI_DeviceKeyboard`, `SI_DeviceGBA`, `SI_DeviceGBAEmu`.
  - `GCPad`, `GCPadEmu`, `GCKeyboard`, `GCKeyboardEmu`, `GBAPad`,
    `GBAPadEmu`.
  - `HSP/HSP`, `HSP_Device`, `HSP_DeviceNull`, `HSP_DeviceARAMExpansion`,
    `HSP_DeviceGBPlayer`, `GBACore`, `Triforce/SerialDevice`.

  Every rewritten file starts with the original-code header and has no
  Dolphin copyright or SPDX line. These are unchanged:
  - File names, public declarations and enum values.
  - SI register behaviour (SICOMCSR, SISR, SIPOLL, the port words, the
    128-byte buffer), the poll and transfer timing, interrupts, and the
    CoreTiming event names and their registration order.
  - Every device's command responses: status/origin/recalibrate/direct
    polls, all analog modes, rumble and the steering-wheel force command,
    the X+Y+Start and B+X+Start combos, the bongo masking, the dance mat
    panel bits, the keyboard scancodes, counter and checksum, the GBA
    link protocol and transfer times, and the GBA emulator sync.
  - Netplay and movie hooks.
  - Savestate `DoState` order and bytes, including the HSP device-type
    switch, the "Have mGBA" marker and the mGBA core state blob.
  - Input config names, control groups and controls (order, names, UI
    names, settings) and all translatable strings.
  - The Game Boy Player register map: test inversion, control power edges,
    the IRQ read/acknowledge format, keypad bit swizzle, video and audio
    block layout, PWM conversion, the 4096 Hz update and the video pacing.
  - The mGBA wrapper: ROM/zip/archive opening, mGBA config values, BIOS and
    save loading, SIO joybus driver, AV stream, frame buffer placement for
    the GB Player, clock conversion with remainder, import/export, the
    ROM-mismatch check on state load, `GetRomInfo` and `GetSavePath`.
  - Deliberate differences (bug fixes, nothing a game sees):
    - `CSIDevice_GCController` CMD_DIRECT starts from zeroed hi/lo, so a
      pad that drops out between polls no longer sends uninitialised stack
      words.
    - `CSIDevice_GBA` zero-fills the reply bytes the peer did not send.
      The connection waiter sets its running flag before starting its
      thread (the old code raced on it), and drops an unused counter.
    - The Game Boy Player PWM ring wraps per byte. The old code wrapped
      once per sample and could write past the end of the ring if mGBA
      raised the PWM width mid-stream. When the old code stayed in bounds,
      positions and bytes are the same.
    - `GCPadEmu`, `GCKeyboardEmu` and `GBAPadEmu` keep only the Windows
      default key mappings. The Android, macOS and Linux branches are gone
      (Windows-only build, as in W1).
    - `GBACore.h` includes `<atomic>` itself instead of relying on
      `WorkQueueThread.h`.
    - Debug/log messages users don't see are reworded. Translator (i18n)
      comments are reworded; the strings are not.
  - Checked: old and new code side by side, with identical hashed output
    streams. Both sides use the untouched EXI and InputCommon files from
    c4ddb95. Both are built with `-ftrivial-auto-var-init=zero`, so the
    old uninitialised locals don't make the runs differ.
    - SI: a fake system (CoreTiming queue, PI, timers, pads, keyboard,
      Wii U adapter, movie, netplay, config, SFML sockets with a scripted
      GBA peer, a fake GBA core). Random u16/u32 MMIO sequences on every
      SI register and buffer word, random device choices per port, polls,
      transfers, device swaps with cooldown, reset button, rumble, adapter
      output, savestate saves and loads (with device swaps). The trace
      covers registers, interrupts, event cycles, response bytes and
      savestate bytes. Seeds 1-150, 100-699, 5000-5099, up to 10000
      steps; about 18M lines.
    - Input: GCPad/GCKeyboard/GBAPad against the real ControllerEmu,
      ControlReference and IniFile code. The old Emu files are compiled
      with their Windows branches selected. The trace covers group/control/
      setting structure, defaults written by SaveConfig, random INI
      settings and expressions loaded back, random input override
      functions (every group/control read and its order), GetInput,
      GetMicButton, rumble, GBA reset, group lookups and the
      Pad/Keyboard/GBA glue. 305 seeds, about 4M lines.
    - HSP: HSPManager with all three devices. Random block reads and
      writes (including addresses with bits above the register field),
      device swaps, Init/Shutdown, savestate loads across device types,
      the ARAM expansion with power-of-two and odd sizes, and the GB
      Player driven by a fake mGBA (scripted audio buffers, frame-end and
      rate callbacks, update events). 540 seeds, about 5M lines.
    - GBACore: a scripted, tracing mGBA, VFS and minizip (ROM found or not
      in archives/zips/plain files, failure at every step, GB and GBA
      cores, e-Reader flag), a synchronous stand-in for the work queue
      thread, and real files for state/save import/export. The trace
      covers every mGBA call and argument, joybus commands and replies,
      clock conversion, SIO driver callbacks, AV stream and host
      callbacks, mixer pushes, frame buffer contents and position, and
      savestate bytes and loads (including ROM mismatches). 300 seeds,
      5M lines. Changing the GB Player frame offset by one pixel made 3
      of 30 seeds differ, so the harness catches that kind of change.
    - Triforce `SerialDevice`: random rx/tx/pass-through/consume/state
      sequences on two devices. 300 seeds, 1.1M lines.
  - Compile check: the W13 .cpp files, plus the non-Qt files that include
    W13 headers (DSP, HW, System, MainSettings, NetPlayClient/Server,
    Core, ConfigManager, DolphinAnalytics, EXI_DeviceMic, UICommon,
    SystemTimers, ProcessorInterface, Movie, State, EXI). Built with clang
    on Linux and with `-target x86_64-w64-windows-gnu`. The rewritten
    files are clean under `-Wall -Wextra -Wconversion -Wsign-compare`.
    Narrowing and sign changes are written as explicit casts with the same
    values (for MSVC /W4 with warnings as errors). The only failures are
    in State, System and UICommon, and they match the base tree exactly
    (missing glslang and similar headers here). The DolphinQt users (GBA
    widget/host, TAS windows, mapping pages, NetPlay dialog) can't be
    compiled here because there are no Qt headers. They only use
    declarations that are unchanged.
  - Verbatim audit (`verbatim-audit.py wii 8`): the runs left in W13 files
    are all public declarations in headers (class declarations, enums,
    constant lists, member function lists). No run is left inside a .cpp
    function body.
  - Not rewritten (unchanged, original headers kept, as AMMediaboard in
    W11):
    - `Triforce/DeckReader`, `FZeroAX`, `ICCardReader`, `IOPorts`,
      `JVSIO`, `MarioKartGP`, `Touchscreen`.
    - `MagCard/C1231BR`, `C1231LR`, `MagneticCardReader`.
    - `SI/SI_DeviceAMBaseboard`.

    These emulate undocumented, reverse-engineered Triforce hardware: the
    baseboard's SI command set, the card reader and deck protocols, the
    per-board JVS identities and quirks, and the cabinet I/O maps. The
    existing code is the only description of how they behave, and they
    can't be tested here without Triforce software. That is 5503 lines.
- W14 log: rewrote all 56 W14 files (the Wii Remote: `Core/HW/Wiimote`,
  `WiimoteCommon`, `WiimoteEmu` with its extensions, and `WiimoteReal`
  with its I/O backends) as original code. They now have 11244 lines
  (from 12108). Each starts with the RetroPixels original-code line and
  has no Dolphin copyright or SPDX header. The files are Wiimote,
  WiimoteCommon (DataReport, WiimoteConstants.h, WiimoteHid.h,
  WiimoteReport.h), WiimoteEmu (Camera, DesiredWiimoteState, Dynamics,
  EmuSubroutines, Encryption, ExtensionPort, I2CBus, MotionPlus,
  Speaker, WiimoteEmu), the extensions (Classic, DesiredExtensionState.h,
  DrawsomeTablet, Drums, Extension, Guitar, Nunchuk, Shinkansen,
  TaTaCon, Turntable, UDrawTablet) and WiimoteReal (WiimoteReal,
  IOWin, IOhidapi, IODummy.h, IOAndroid.h, IOLinux.h).
  - Kept unchanged: file names, and every public declaration that other
    files use (names, signatures, namespaces, enum values, constants,
    packed report structs and their static_asserts, the calibration and
    data-format structs that ControllerInterface's WiimoteController
    reads). Everything that affects emulation also stays the same:
    - every HID report byte: report IDs, data-report layouts and their
      core/accel/IR/extension offsets, status, ack and read-reply
      contents, error codes, and the order and timing in which replies
      are queued (reads are answered in 16-byte chunks, one per update);
    - the I2C bus (slave order, first answer wins), the extension port,
      the extension encryption (key from 0x40, the known-answer tables
      and the fallback boxes with their 0x17/0xE0 XOR), the IR camera
      (basic, extended and full modes, sensor-bar geometry, FOV and
      clipping) and the speaker's Yamaha/Nintendo ADPCM decoder, volume
      and sample output;
    - MotionPlus: activation and deactivation timing, the passthrough
      modes (nunchuk and classic bit moves), the challenge/response
      registers, calibration data and the gyro encoding;
    - the input mapping and dynamics math (swing, shake, tilt, point,
      IMU cursor, gravity and accel encoding), kept bit-identical: the
      same operations in the same order, so float results match;
    - every extension's controls, settings, config names and their
      translatable strings, default calibration, report encoding and
      savestate;
    - DesiredWiimoteState serialization (the netplay/movie format) and
      the input-override hooks the TAS window uses;
    - the savestate layout and order of the remote, its extensions,
      MotionPlus, camera, speaker and dynamics;
    - WiimoteReal: report rewriting (SET_REPORT header, LEDs never all
      off, speaker data turned into rumble when the speaker cannot play,
      repeated rumble dropped), send times (emulated time on the CPU
      thread, now elsewhere; Prepare's 200 ms status request), the
      200 Hz report repeat, the balance-board probe, the idle write
      every second, connect retry, the scanner and pool threads, slot
      assignment and the config keys;
    - IOWin: the radio and device scans, pairing (3 rounds, inquiry
      length 3), authentication, the 0xa1 read header and the cached
      balance-board flag; IOhidapi: the same report framing and return
      codes.
  - Kept quirks: the balance-board probe still switches on a stale
    buffer after a failed read (with a real device a failed read ends
    the connection anyway). TaTaCon and Shinkansen still clear their
    registers in Reset.
  - Deliberate differences (genuine bugs, harmless fixes, cleanups):
    - An unknown data-report mode uses the core-only layout. The old
      code dereferenced a null manipulator.
    - Guitar: a slider position past the last table entry gives the last
      code. The old code read past the table.
    - The SerializedWiimoteState buffer and the balance-board read buffer
      are zero-initialised. Only their used bytes are ever read or sent.
    - IOWin's ReadDeviceIds closes the device handle it opens (the old
      code leaked it). The 4995 warning pragmas are only used under MSVC.
    - IOhidapi copes with a null manufacturer string (the old code
      converted a null pointer).
    - IOAndroid.h and IOLinux.h are aliases of the dummy scanner. Nothing
      in the build uses them.
    - Removed an unused counter in Wiimote.cpp and a dead branch in the
      pointing code. The internal encryption helper API is new; nothing
      outside W14 uses it.
    - Some log and panic messages are worded differently. None of them
      are translatable strings.
  - Checked: three harnesses linked the old and the new code against the
    same untouched code outside W14 (InputCommon ControllerEmu and
    expressions, Common config and math), with a fake steady clock and
    mocks that record every outside call. They compared a hash of every
    output report byte, I2C memory and register read, speaker sample,
    callback, savestate byte and recorded call. No Nintendo keys, BIOS,
    firmware or games were used. Outputs are identical for:
    - the emulated remote (3500 seeds, 3000 to 30000 steps each): random
      output reports (register reads and writes on every slave, modes,
      speaker data, garbage), each extension attached and switched,
      MotionPlus passthrough set up the way games do it, encryption with
      matching and non-matching keys, random mapping expressions and
      inputs, savestate save/load/compare, config toggles, link/unlink
      and reset;
    - Wiimote.cpp (5000 seeds): source changes, savestate load after a
      source change, initialize/shutdown, movie hook, group accessors;
    - WiimoteReal (5200 seeds, up to 2000 steps): output rewriting, the
      write queue with send times, reads, Update and the report repeat,
      buttons, link/unlink, Prepare, the balance-board probe and the
      device-name and ID helpers.
    - Mutation checks: single changes (speaker step table, fallback XOR,
      one answer byte, a MotionPlus bit move, the swing/tilt savestate
      order, a dynamics threshold, camera geometry, a float
      reassociation, uDraw bounds, a serializer bit, three Wiimote.cpp
      conditions, five WiimoteReal rules) each changed the output of at
      least 16 of the 40-60 seeds tried. One change that was not caught
      was a single byte of a known-answer encryption table, which random
      keys almost never reach.
    - Not covered by a harness, checked by review and compile only: the
      WiimoteReal threads (writer, reader, scanner and pool), IOWin and
      IOhidapi, which need real Bluetooth devices.
    - Compile check: the 25 W14 .cpp files build with clang on Linux and
      with `-target x86_64-w64-windows-gnu` (IOWin with a shim for
      Bluetooth declarations this machine's mingw headers lack), and the
      18 non-Qt files outside W14 that include W14 headers build as
      before. Under `-Wall -Wextra -Wconversion -Wsign-compare` the W14
      files give no warnings beyond the untouched tree's.
    - Verbatim audit (`tools/verbatim-audit.py wii 8`): no function body
      in W14 .cpp files shares 8 or more lines with the old code. The
      remaining runs are report structs, enums and class declarations in
      the headers.
- W9 log: rewrote all 46 W9 files (JitCommon, Jit64Common, Jit64 and
  its RegCache) as original code. They now have 14792 lines (from
  15169). Each starts with the RetroPixels original-code line and has no
  Dolphin copyright or SPDX header. The files are:
  - JitCommon: JitBase, JitCache, JitAsmCommon, DivUtils and
    ConstantPropagation;
  - Jit64Common: BlockCache, ConstantPool, EmuCodeBlock, FarCodeCache,
    Jit64AsmCommon, Jit64Constants, Jit64PowerPCState, TrampolineCache
    and TrampolineInfo;
  - Jit64: Jit, JitAsm, Jit64_Tables and the Branch, FloatingPoint,
    Integer, LoadStore, LoadStoreFloating, LoadStorePaired, Paired and
    SystemRegisters instruction files;
  - RegCache: CachedReg, RCMode, JitRegCache, GPRRegCache and
    FPURegCache.
  - Kept unchanged: file names, and every public declaration that other
    files use (class and member names, signatures, the JitBase and
    JitInterface contract, JitState/JitOptions, the JitBlock and
    JitBlockData layout the debugger views read, SafeLoadStoreFlags
    values, the TrampolineInfo layout, the code-size constants, the
    register-cache handle API with RCOpArg, RCX64Reg, forks and
    transactions). The x86 the JIT emits is also the same for every
    input:
    - the same register allocation (allocation orders, eviction score
      and tie-breaking), and the same order of Use/Bind/Scratch/Realize
      calls in every instruction, so the same host registers are chosen;
    - the same constant-pool contents and order (MConst is called at the
      same points);
    - the same sequences for every instruction, every exit and the
      dispatcher, including fastmem backpatching and trampolines, the
      memcheck paths, idle skipping and branch following, block linking,
      the BLR optimization and stack guard, cycle counts and downcount,
      FPSCR/MXCSR handling, accurate NaNs, FPRF, the fmadds
      error-free-transformation paths and the CPU feature paths (no
      BMI1/BMI2, AVX, SSE4.1, FMA, LZCNT and MOVBE paths);
    - the JIT_SETTINGS table, the profiling and branch-watch hooks and
      every debug/JIT-off option;
    - DivUtils gives the same magic constants for every divisor, checked
      over all 2^32 values.
  - Kept quirks: reserved GQR types (1-3) still assemble to UD2.
  - Deliberate differences (bug fixes, nothing a correct program sees
    except the first):
    - ConstantPropagation: divwuo with both operands known at compile
      time and a quotient of 2^31 or more set XER[OV]. It now sets OV
      only for a zero divisor, as the interpreter and the JIT's own
      non-constant path do.
    - JitCache walks address ranges with 64-bit arithmetic, so a range
      that ends at the top of the address space no longer wraps.
    - Jit64 no longer inserts a nullptr key into its exception-handler
      map.
    - EmuCodeBlock's 8/16-bit MMIO constant reads sign-extend with casts
      instead of signed shifts. The value is the same.
    - fmadd family: when ps1 of frC is needed after the software-FMA
      call, the register that holds it is now saved. The old code saved
      the wrong register (RCX) instead.
    - dcbx and the float compares read the next instruction only after
      checking that it can be merged; they used to read js.op[1] first.
    - subfx with a known operand computes the LEA displacement as an
      unsigned negation, so a value of 0x80000000 is no longer signed
      overflow. The emitted code is the same.
  - Fixed for MSVC /W4: the W9 files have no implicit narrowing, no ~ on
    a narrower type that is then widened, no unary minus on unsigned
    values, no unused locals and no signed/unsigned compares. They build
    with no warnings under clang and g++ `-Wall -Wextra -Wconversion
    -Wsign-compare` (and clang -Wshadow).
  - Checked: a harness linked the old JIT and the new one (each with its
    own JIT files) against the same interpreter, MMU, memory and MMIO,
    with mocks of the rest of the emulator. It ran random guest programs
    (integer, load/store with self-modifying code, FP, paired singles,
    system registers, branches and all of them mixed) and compared, per
    case, hashes of all guest state (GPRs, FPRs, CR fields, XER, FPSCR,
    MSR, every SPR, segment registers, reservation, pending exceptions,
    downcount, the global timer, the gather pipe and all of guest
    memory), of the scheduled events, idle skips and breakpoint hits,
    and of the block cache (every block's addresses, size, feature
    flags, exits and link state, profiling run counts) and the
    branch-watch records.
    No Nintendo keys, BIOS, firmware or games were used. Results:
    - 60 configurations: the default; MMU on and off; fastmem off;
      memchecks; no fastmem arena; real mode; the data cache; the low
      dcbz hack; page-table fastmem off; the large entry-points map off;
      FPRF, accurate NaNs, accurate fmadds, FMA on and off, FP and
      divide-by-zero exceptions; CPU feature masks from none up to all
      (with BMI1, BMI2, AVX, SSE4.1, FMA, LZCNT and MOVBE each turned
      off); branch following; branch watch; debugging and profiling;
      no linking; no block cache; external and FIFO interrupts;
      interpreter-safe programs; each of the 13 JIT-off options; very
      long and very short programs.
    - Every configuration for 2 seeds: 60 programs in each of the
      integer, FP, paired, system-register, branch and mixed families
      (43200 cases), plus 30 load/store programs with self-modifying
      code (3600 cases, capped at 20000 cycles). Earlier runs during the
      rewrite added several thousand more. No case differs.
    - DivUtils: all 2^32 divisors match. ConstantPropagation: 50 million
      random cases match, apart from the divwuo fix above.
    - Windows ABI: compiled only. This machine has no wine, so code
      emitted for the Windows ABI was not run.
    - Compile check: every W9 .cpp builds with g++ and clang (Linux) and
      clang `-target x86_64-w64-windows-gnu`. The files that include W9
      headers (Memmap, CachedInterpreter, CachedInterpreterBlockCache,
      JitInterface and PPCAnalyst) build against the new headers. The
      DolphinQt JIT widgets need Qt, which this machine lacks; a stub
      that uses every JitBlock field and JitInterface call they use
      builds.
    - Verbatim audit (`tools/verbatim-audit.py wii 8`): no function body
      in W9 .cpp files shares 8 or more lines with the old code. The
      remaining runs are all in headers: class declarations and member
      lists that other code depends on (JitRegCache.h, JitBase.h, Jit.h,
      JitAsmCommon.h, Jit64AsmCommon.h, FPURegCache.h, GPRRegCache.h,
      EmuCodeBlock.h, JitCache.h, BlockCache.h, ConstantPool.h and
      CachedReg.h).
- W12 log: rewrote 29 of the 35 W12 files, the EXI bus and its devices
  (6494 lines now, from 6978): `EXI`, `EXI_Channel`, `EXI_Device`,
  `EXI_DeviceDummy`, `EXI_DeviceIPL`, `EXI_DeviceMemoryCard`,
  `EXI_DeviceAD16`, `EXI_DeviceGecko`, `EXI_DeviceMic`,
  `EXI_DeviceEthernet`, `BBA/BuiltIn`, `BBA/IPC`, `BBA/TAPServerBBA`,
  `BBA/TAPServerConnection`, `BBA/TAP_Win32.cpp`, `BBA/XLINK_KAI_BBA` and
  `Modem/TAPServerModem`. Every rewritten file starts with the original-code
  header and has no Dolphin copyright or SPDX line. File names, public
  declarations, the EXI register layout (channels 0x14 apart: status, DMA
  address, DMA length, control, immediate data), register and interrupt
  behaviour, transfer timing, the CoreTiming event names and their
  registration order, savestate `DoState` order and bytes, the IPL font and
  SRAM handling and checksums, the RTC, the BBA/tapserver/XLink packet
  formats and all translatable strings are unchanged.
  - `EXI`, `EXI_Channel`, `EXI_Device`, `EXI_DeviceDummy`: the expansion
    interface manager (device creation per slot, hot-swap with the
    delay between removal and insertion, update and interrupt events, memory card slot
    changes), the channel registers (chip select, EXT/TC/EXI interrupt bits
    and masks, immediate and DMA transfers, the readwrite path) and the
    device base (byte-wise immediate and DMA defaults).
  - `EXI_DeviceIPL`: the IPL ROM (the user's own dump if present, otherwise
    the font files from Sys/GC), descrambling, SRAM and RTC reads and writes, UART
    text output, the ROM/SRAM/RTC address decoding and the savestate.
  - `EXI_DeviceMemoryCard`: the card command set (ID, status, read array,
    page program, sector and chip erase, interrupt enable, wake up),
    transfer timing and completion events, raw image and GCI folder cards,
    the slot's savestate and folder-card setup.
  - `EXI_DeviceAD16`, `EXI_DeviceGecko`, `EXI_DeviceMic`: the AD16 debug
    register; the USB Gecko (one shared TCP listener on 55020 and up, a pump
    thread per adapter, the LED, identify, send/receive and FIFO-status
    commands); the microphone (ID, status, buffer commands, cubeb capture
    into a ring, sample-rate and period handling, button state, interrupts).
  - `EXI_DeviceEthernet`: the MX98728 controller (register and memory
    access, the page ring with BP/RWP/RRP/RHBP, receive and transmit
    descriptors, interrupts, MAC handling), plus the network backends:
    `BuiltIn` (HLE stack: ARP, DHCP, DNS, UDP, TCP with retransmit and
    window handling, IGMP echo, UPnP/SSDP, 10 connection slots),
    `TAPServerConnection`/`TAPServerBBA`/`TAPServerModem` (2-byte
    little-endian length then the frame), `XLINK_KAI_BBA` (control
    messages, chat OSD, "e;e;" frames), `IPC` and `TAP_Win32.cpp`.
  - Deliberate differences (bug fixes, nothing a game sees):
    - Mic: the ring buffer is 256 bytes, so ring-size field 3 (256 bytes),
      which overflowed the old 128-byte buffer, works. ID reads past 5 bytes
      and status reads past 2 bytes return 0 (the old code read out of
      bounds); status writes past 2 bytes are ignored (the old code wrote
      out of bounds).
    - Gecko: the listener is reference counted and stops when the last
      adapter goes. The old code counted clients instead, so it could stop
      the listener while another adapter still existed, or keep it running
      with none. The old race where a new PC connection was handed over
      before the previous pump thread was stopped is gone. As before, a
      replacement connection starts with empty FIFOs and bytes queued before
      the first connection go to it.
    - Ethernet: controller memory accesses wrap at 0x1000, DMA reads past
      the end are clamped, zero-length frames are dropped and out-of-range
      page pointers no longer overflow the receive buffer.
    - TAPServerConnection: zero-length frames are dropped (the old reader
      hung). IPC: oversized frames are dropped.
    - TAP_Win32: an adapter registry subkey that can't be opened is skipped
      instead of ending the scan.
    - XLink: short "message"/"chat"/"directmessage" commands no longer
      throw, the command buffer is clamped and zero-length "e;e;" frames
      are dropped.
    - BuiltIn: no null-slot crash when all 10 connection slots are in use; a
      DHCP request whose first option is 2 bytes is handled; oversized
      frames are dropped instead of calling std::exit; the stop flag is
      cleared on Activate, so a reactivated adapter reads again;
      Deactivate joins the reader before closing sockets; the reader sleeps
      when the console can't take a frame instead of spinning; queue access
      in RecvStart/RecvStop is locked.
    - Log and debug messages that users don't see are reworded.
  - Checked: old and new code side by side against a mocked system (CoreTiming
    queue, PI interrupts, 32 MB RAM, MMIO, fake sockets, fake cubeb), using
    the untouched W10 files from the base on both sides, comparing streamed
    hashes of registers, interrupts, scheduled events, DMA bytes, files
    written, packets and savestate bytes:
    - EXI bus, IPL and memory cards: random register/DMA/immediate traffic
      on all channels, hot-swaps, SRAM/RTC/UART/ROM access with made-up IPL
      data, raw and GCI folder cards (read-only commands on folder cards),
      savestates; 60 seeds x 400 steps (300 x 700 earlier).
    - Ethernet controller with the tapserver backend: 100 seeds x 400 steps.
    - BuiltIn HLE stack with a scripted fake network and clock (single-pass
      reader in harness-only copies of both versions): 60 seeds x 2500
      steps, plus a new-only stress run filling all 10 slots.
    - XLink Kai: 150 seeds x 800 steps. Tapserver modem: 12 seeds x 30
      steps over real local sockets.
    - Microphone: 80 seeds x 600 steps with scripted capture callbacks.
    - USB Gecko: one or two adapters, scripted PC connections, data both
      ways and closes, listen failures; the harness waits for the helper
      threads before each traced step. 400 seeds x 400 steps. 6 seeds
      differed once while the machine was heavily loaded; each matched on
      three reruns, so these were harness timing.
    Not run: IPC (no cpp-ipc library here), TAP_Win32 (Windows only) and
    the invalid-MAC regeneration path (needs the config layer).
  - Compile check: the rewritten .cpp files and their dependents, with clang
    `-Wall -Wextra -Wconversion -Wsign-compare` on Linux and for
    x86_64-w64-windows-gnu. The rewritten EXI files are clean; narrowing,
    sign changes and complement/negation of unsigned values are explicit
    casts (MSVC /W4 with warnings as errors). The modem warnings are in the
    unchanged Dolphin files. Other failures match the base tree exactly
    (missing enet and glslang headers here).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run is left in any W12
    .cpp file. The runs left in W12 headers are includes, forward
    declarations, enums and class declarations other files depend on.
  - Not rewritten:
    - `EXI_DeviceModem.cpp/.h`: the GameCube modem adapter. Its behaviour
      comes from reverse engineering of PSO and the existing code is the
      only description, so it can't be rewritten clean-room.
    - `EXI_DeviceBaseboard.cpp/.h`: the Triforce baseboard, undocumented and
      tied to the unrewritten `AMMediaboard`.
    - `EXI_DeviceAGP.cpp/.h`: the Advance Game Port (GBA cartridge reader),
      whose AE command protocol is undocumented.
    These keep their original headers. `BBA/TAP_Win32.h` is third-party
    (OpenVPN) and is unchanged.
- W15 log: rewrote all 43 W15 files as original code (8445 lines now, from
  10311): the DSP LLE core in `Core/DSP` (`DSPCore`, `DSPCommon.h`,
  `DSPTables`, `DSPHWInterface`, `DSPMemoryMap`, `DSPStacks`,
  `DSPAccelerator`, `DSPAnalyzer`, `DSPBreakpoints.h`, `DSPHost.h`), the
  interpreter (`Interpreter/*`), the assembler and disassembler
  (`DSPAssembler`, `DSPDisassembler`, `LabelMap`, `DSPCodeUtil`),
  `DSPCaptureLogger`, `DSPEmulator` and the LLE glue in `HW/DSPLLE`
  (`DSPLLE`, `DSPHost.cpp`, `DSPSymbols`). Every file starts with the
  original-code header and has no Dolphin copyright or SPDX line. File
  names, public declarations, `SDSP`/`DSP_Regs` layout (the JIT bakes in
  its offsets), register numbers, opcode table contents, savestate
  `DoState` order (including the leading `is_hle` flag), config keys and
  every translatable and assembler message string are unchanged. The DSP
  JIT (W16) is untouched and still builds and runs against the new
  headers; `DSPCore::CheckExceptions` stays out of line because generated
  code calls it.
  - Core: `SDSP` (memories, mailboxes, IFX registers, DMA, exceptions and
    their priority, external interrupt, reset/init with the same ROM
    checks), the four hardware stacks, the memory map, `DSPCore` (state,
    stepping, the interpreter/JIT cycle loop with the same left-over-cycle
    results). Register read/write goes through one slot lookup.
  - Interpreter: every instruction and extended op, the condition-code
    helpers, control register, the cycle loops (normal, debug, threaded)
    and idle-skip handling, with the same flags, saturation and
    write-back order.
  - Assembler/disassembler: the same text and bytes. The assembler keeps
    the two-pass design, strtok-style tokenizing, the `INCLUDE`, `INCDIR`,
    `ORG`, `SEGMENT` (`CODE`/`DATA`, with the same segment PCs), `EQU`
    and `WARNPC` directives, expression syntax and its error codes
    and messages. `LabelMap` keeps label lookup order and the
    duplicate/predefined rules. `DSPCodeUtil` keeps dump file formats and
    names, `Compare` output and the CRC.
  - `DSPCaptureLogger`: the PCAP output is byte for byte the same (packet
    types, the packed 6- and 11-byte headers).
  - `DSPLLE`: the threaded mode (same start/stop rules, the Zelda
    external-interrupt switch back to unthreaded, determinism check),
    mailbox and control-register access, `DSP_UpdateRate` 12600, cycles/6,
    the incompatible-state message and its 3000 ms. `DSPSymbols` keeps the
    listing text and the address/line lookups.
  - Deliberate differences (no game sees these; they remove crashes and
    undefined behaviour):
    - Assembler: division by zero in an expression now yields 0 (it raised
      SIGFPE, and was reachable from text such as `still */ NOP`); an
      operand index of 8 or more no longer reads past the operand array;
      an unknown extended op with extended operands no longer
      dereferences a null template (reachable by reassembling a
      disassembly line whose mnemonic is longer than 12 characters); an
      empty string operand `""` gives an empty string instead of a null
      pointer; an `ORG` that lowers the program size no longer makes pass
      2 write out of bounds (out-of-bounds writes are skipped); operands
      are evaluated explicitly left then right; lines and names are
      `std::string`s instead of fixed buffers.
    - `get_mask_shifted_down(0)` returns 0 (it looped forever).
    - Some debug/log message texts changed (not translatable, not seen by
      users unless logging is on).
    - `DSPLLE`: the default capture logger is deleted when the PCAP logger
      replaces it (it leaked); the thread is only joined when joinable (a
      second stop joined twice).
    - Private parts of the headers were redesigned (simple `DSPCore`
      forwarders and accelerator setters are now inline, the PCAP logger
      constructors are explicit, `DSPBreakpoints::IsAddressBreakPoint` is
      const, `DSPSymbols.h` no longer includes `SymbolDB.h`, `Compare`
      loops over `size_t`). Public names and layouts are unchanged.
  - Found and not fixed (W16, the JIT): with `STATIC_REG_ACCS` the JIT
    keeps `$ac0`/`$ac1` in r8/r9 across the call to `CheckExceptions`, but
    after the call `LoadRegs(false)` does not reload them, so the result
    depends on whether the compiled `CheckExceptions` happens to clobber
    r8/r9. Proven by bisection: the old code with an asm clobber of r8 or
    r9 diverges. The comparison harness wraps `CheckExceptions` (linker
    `--wrap`) to preserve r8/r9 on both sides; the real fix belongs in the
    JIT.
  - Checked: old and new code side by side in a harness (same random
    inputs, one hash per case streamed through `cmp`), all identical:
    - Opcode tables; every opcode word with 24 random states each;
      2000 random programs; 800 system runs (mailboxes, DMA, IFX,
      accelerator, interrupts, register access) on the interpreter and
      300 on the JIT; debugger stepping and breakpoints (400); accelerator
      (20000); analyzer (200); savestates (500).
    - Disassembler over all 65536 opcode words for all 256 option
      combinations plus random programs; assembler (10000 random sources,
      4000 more with every directive and op forced); disassemble-and-
      reassemble round trip (1000); labels (2000); expressions (40000);
      code utilities (10000, plus 3000 with fixed dump paths including
      alerts); symbols listing (200); capture logger output byte for byte.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wimplicit-fallthrough -Wunused -Wdeprecated -Wextra-semi`, Linux
      and `x86_64-w64-windows-gnu`: no warnings in W15 files, nor from W15
      headers in the JIT and the other dependents (`AX`, `AXWii`, `AESnd`,
      the HLE ucodes and ROM, `DSPHLE`, `HW/DSP.cpp`, `SystemTimers`,
      `MIOS`, `Movie`). The JIT's own warnings and `Core.cpp`'s missing
      SFML header are the same on the base tree.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W15
      .cpp file. Remaining runs are in headers only (public declarations,
      enum values, register layout and the interpreter's op list).
- W17 log: rewrote all 26 W17 files (DSP HLE: `Core/HW/DSPHLE` and
  every ucode under `UCodes`) as original code. They now have 6575 lines
  (from 7731). Each starts with the RetroPixels original-code line and
  has no Dolphin copyright or SPDX header. The files are DSPHLE,
  MailHandler and the ucodes AX, AXWii, AXStructs.h, AXVoice.h, Zelda,
  GBA, CARD, INIT, ROM, ASnd, AESnd and UCodes (the shared base,
  ucode selection and the IRAM/DRAM dumps).
  - Kept unchanged: file names, and every public declaration that other
    files use (names, signatures, namespaces, enum values, constants,
    the AX and AX Wii parameter-block structs with their field order and
    sizes). Everything that affects emulation also stays the same:
    - ucode selection: every CRC and the ucode it picks (AX, AX Wii,
      Zelda with its per-version flags, GBA, CARD, INIT, ROM, ASnd,
      AESnd), the ASnd/AESnd hash fallbacks and the unknown-ucode path;
    - every mail, in the same order and at the same point: the ROM and
      INIT boot handshakes, task yield/resume/halt, the AX and AX Wii
      command-list protocol, Zelda's standard and light protocols
      (command queue, sync mails, frame-end mails), GBA, CARD, ASnd and
      AESnd; the mail queue and its interrupt timing, and the DSP control
      register and halt-bit handling;
    - every RAM and ARAM write: AX and AX Wii voice rendering (PB
      layouts per build including the System Menu 1.0 / Elebits hole
      before the filters, PB updates, the accelerator and its loop and
      end handling, ADPCM, the polyphase and linear resamplers, low-pass,
      biquad and high-pass filters, volume envelopes, initial time delay,
      dpop, aux buses and the Wii Remote buses), the mix and output
      commands with their volume ramps and clamping, the compressor, the
      coefficient file lookup (`dsp_coef.bin` in the GC user folder,
      then in Sys/GC); the Zelda mixer (AFC and PCM decoding,
      resampling, the reverb FIR and ring buffers, sine and pattern
      tables, ARAM and MRAM fetches, the per-version flags); ASnd and
      AESnd sample formats, pitch and looping; GBA's key computation;
    - integer results bit for bit: overflowing sums wrap the same way
      (done through unsigned arithmetic instead of signed overflow) and
      float volume ramps use the same operations in the same order;
    - the savestate layout and order of every ucode and of DSPHLE
      (including the pending boot request), the ucode-dump config key
      and the two translatable unknown-ucode panic strings.
  - Kept quirks: AX Wii's aux-mix upload leaves the last downloaded
    buffer in aux L; Zelda's MRAM fetch still fills from the start of
    its buffer, and its command queue still spins on a word without the
    top bit when the queue is empty, as on hardware.
  - Deliberate differences (genuine bugs, harmless fixes, cleanups):
    - Buffers the old code left uninitialised are zeroed: the AX PB
      update list, the parameter blocks, the volume-ramp words, the
      coefficient buffer, the PB of an unknown AX Wii build, and Zelda's
      raw and input sample buffers and a voice block read from an invalid
      address. The boot request's padding is an explicit reserved field
      (the same bytes in the savestate, now always zero).
    - A command list read past its 512 words stops processing instead of
      reading past the buffer.
    - An unknown Zelda CRC still raises the same panic and then runs
      with no version flags (the old code read the flags through the
      table's end iterator).
    - A null ARAM pointer reads as zeros; a Zelda reverb ring with no
      slots stays on slot 0 (the old code dereferenced null and divided
      by zero).
    - Zelda's raw sample buffer has room for AFC's whole-block decode.
      Once cached samples have been taken, that decode can write up to 15
      samples past the size the old code gave the buffer, which
      overflowed its stack (only samples within the needed count are
      used, so the output is unchanged).
    - Some log messages are worded differently. Panic messages and
      translatable strings are unchanged.
  - Checked: a harness linked the old and the new code against the same
    untouched code outside W17 (the DSP accelerator, PointerWrap, the
    mail and interrupt plumbing) with fake memory, ARAM, timing and
    config, and a zeroing allocator so that old padding is
    deterministic. It booted each ucode through the ROM and INIT mails,
    then sent structured random mails and command lists, switched tasks,
    and saved and reloaded state (in place and into a fresh DSPHLE). It
    compared a hash of every mail, interrupt, panic, RAM and ARAM byte
    and savestate byte. No Nintendo keys, BIOS, firmware, DSP ROMs or
    games were used. Outputs are identical for 1000 seeds each of AX
    (every GC build), AX Wii (every Wii build), ASnd, AESnd, GBA, CARD
    and unknown ucodes, and for 691 of 700 seeds of Zelda (every
    version, both protocols). Of the other 9 Zelda seeds, 8 make old and
    new spin the same way in the empty-queue quirk above. In the last
    one the old code overflows its stack in the AFC decode and crashes
    or not depending on stack layout; the new code is deterministic
    there. A run of the new code under AddressSanitizer and UBSan (60
    seeds per family) found nothing else, apart from unaligned reads
    from odd guest addresses, which the real Memmap copy makes the same
    way.
    - Mutation checks: single changes to the new code each changed the
      output of some of the 40 seeds tried for their family: the AX
      compressor gain (16) and aux download mix (24), the low-pass (7)
      and biquad (8) filters, the AX Wii PB hole (10), Zelda's AFC
      decoder (30) and reverb ring step (24), the ASnd resampling
      counter (1), the AESnd right volume (7), the GBA key (24) and the
      mail handler's savestate order (40). One change, the biquad's
      rounding constant, was found to be equivalent: the sum is always
      a multiple of 4, so the old 0x7FFF can never differ from 0x7FFE.
    - Compile check: the 12 W17 .cpp files and DSPEmulator.cpp (the only
      other file that includes W17 headers) build with clang on Linux and
      with `-target x86_64-w64-windows-gnu`. Under `-Wall -Wextra
      -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32` the
      W17 files give no warnings (no fixes were needed outside W17). A
      scan found no bool mixed with an integer under `|` or `&` (MSVC
      C4805).
    - Verbatim audit (`tools/verbatim-audit.py wii 8`): no function body
      in W17 .cpp files shares 8 or more lines with the old code. The
      remaining runs are the PB structs in AXStructs.h and class
      declarations in DSPHLE.h and AESnd.h.
- W16 log: rewrote all 16 W16 files, the x86-64 DSP JIT in `Core/DSP/Jit`
  (3625 lines now, from 7632): `DSPEmitterBase`, `x64/DSPEmitter`,
  `x64/DSPJitRegCache`, `x64/DSPJitTables`, `x64/DSPJitUtil`,
  `x64/DSPJitCCUtil` and the instruction files `x64/DSPJitArithmetic`,
  `Branch`, `ExtOps`, `LoadStore`, `Misc` and `Multiplier`. Every file starts
  with the original-code header and has no Dolphin copyright or SPDX line.
  File names, the `DSP::JIT::DSPEmitter` interface (`RunCycles`, `ClearIRAM`,
  `DoState`, `CreateDSPEmitter`, `DSPEmitterNull`) and the savestate bytes
  (`m_cycles_left`) are unchanged. The W16 files have no config keys or
  translatable strings; a few log texts (logging only) changed.
  - Design: a new structure, not a port. Guest registers stay in `SDSP`
    memory, so there is no register cache. Generated code keeps only fixed
    host registers (state pointer, block entry table, cycle counter, and two
    callee-saved registers for pending extension values). The value of
    every instruction is computed in scratch registers and stored back
    before the next instruction. Nothing caller-saved survives a call into
    C++. `DSPJitRegCache` keeps its name but now holds the register layout
    (offsets from the live `SDSP`) and the normalizing loads and stores. The
    handler tables are first-match pattern lists. They classify every
    opcode word (main and extended part) the same way as the old tables.
  - Kept as before: every instruction's result (registers, flags,
    saturation, sign extension, loop and call stacks, memory and IFX
    accesses, cycle counts), block building and linking, idle skip and the
    dispatcher. Details:
    - Blocks end at 250 instructions, at a branch, or before an idle-skip
      address. A block costs its instruction count in cycles, or 0x1000 at
      an idle skip off the DSP thread. A taken branch exit costs the count
      before the branch; the exception-check exit costs the count before
      the instruction. A zero-length block counts as 1.
    - The dispatcher subtracts in 16 bits and loops while cycles are left.
      It checks HALT, and the external-interrupt flag when the block was
      built on the DSP thread.
    - Only unconditional jumps and calls link. Targets inside the block
      being built don't link. A linked jump is taken only when the cycles
      left exceed both block lengths. An unbuilt target puts the block on
      the target's waiting list, and it is rebuilt when the target is
      built. `ClearIRAM` resets blocks 0..0xfff.
  - Quirks kept on purpose (the old JIT did these, so games and states see
    the same):
    - `lsr` immediate counts of 40 or more wrap (the shift count is taken
      mod 64).
    - The logical variable-shift ops set flags from the zero-extended
      value.
    - `mulmvz` sets flags from the sign of the new product.
    - `addpaxz` sets no carry or overflow.
    - Many ops set flags from the raw unwrapped value.
    - Every `$sr` write clears bit 8.
    - Data writes above 0x0fff all go to `WriteIFX`; IMEM reads above 0x0fff
      come from IROM.
    - HALT pops the call stack into `pc`; the interpreter steps `pc` back
      instead.
    - No stack-overflow exception.
    - A jump to itself at a block start costs 0 cycles and can spin.
  - Deliberate differences (no game sees these; they remove crashes,
    hangs and undefined behaviour):
    - The r8/r9 bug found in W15 is gone by design. The old JIT kept
      `$ac0`/`$ac1` in r8/r9 across the call to `CheckExceptions` and didn't
      reload them, so results depended on the compiler. Now no guest value
      lives in a caller-saved register across any call.
    - `$acX.h` is stored canonically: the 32-bit `h` field is sign-extended
      from bit 7. The old JIT could leave stale upper bits; the 40-bit value
      is the same.
    - `lr` from an unmapped data bank returns 0. The old JIT returned
      whatever was left in EAX.
    - `clrp` always clears the product. The old JIT wrote it straight to
      memory past its register cache, so a product cached in a host register
      in the same block overwrote it, or was lost.
    - Endless rebuilding stops. When blocks wait for each other in a cycle,
      the old `CompileCurrent` retry loop never finished. Case 2163 below
      hangs the old code. The new code notices the repeated waiting state,
      logs a warning and stops retrying; the blocks still run.
    - Code space: the old JIT had 2 MB with no overflow check and ran off
      the end. Final-mode case 435 below crashes the old code. The new JIT
      has 16 MB with a 1 MB reserve. When the reserve is reached, the
      compile stub leaves the dispatcher and the buffer is cleared after
      `RunCycles`. The new JIT emits about 0.69 times the old code size
      (over 979 cases: median 0.71, maximum 1.19).
  - Found and not fixed (W15, `SDSP::RunDMA`): a DRAM DMA whose length is
    accepted but runs past the 8 KB DRAM (lengths seen: 9624 and 14554
    bytes at DSP address 0) copies past the end of `dram`. The harness
    segfaults there on old and new alike (the "crash" cases below, reached
    through the JIT's data-write-to-IFX quirk). The bounds check belongs in
    W15's DMA code.
  - Checked:
    - Harness: old and new JIT side by side, with the W15 DSP core and
      interpreter on both sides. Each case is a random IRAM/IROM program
      (idle-loop patterns, loops, calls, branches back and out) with random
      registers, stacks, DRAM, mailboxes, interrupts, exceptions, code
      reloads and run budgets from 0 to 3000 cycles. Each case is hashed
      over every register, stack, IFX register, mailbox, accelerator state,
      DRAM and IRAM word, host memory and ARAM byte, interrupt count and
      leftover cycle count.
    - Harness adjustments for the old side:
      - `CheckExceptions` is wrapped to keep r8/r9.
      - `clrp` is patched to write through its register cache.
      - `lr` addresses in unmapped banks are kept out of the inputs.
      - A bad DMA length (which calls `exit(0)` in `SDSP`) and hangs are
        reported per case.
    - Results:
      - Opcode tables identical.
      - Old vs new, 12000 random cases: all identical, including the same
        hang and exit cases on both sides. The one exception is case 2163,
        where the old code hangs in `CompileCurrent` (above).
      - Final mode: up to 400000 cycles until HALT, hashing without the
        step counter. Old vs new over 5000 cases: identical; the only
        exception is case 435, where the old JIT overflows its code space.
      - New JIT with every caller-saved register clobbered after each call
        into C++ (`CheckExceptions`, `ReadIFX`, `WriteIFX`,
        `ApplyWriteBackLog`), vs the unclobbered new JIT, 5000 cases:
        identical.
    - New JIT vs the W15 interpreter (final mode, 1000 cases): they differ
      only by the old JIT's own behaviour. The new JIT gives the old JIT's
      hashes on all of these cases. Known causes:
      - HALT pops the call stack instead of stepping `pc` back.
      - `$sr` bit 8 is cleared.
      - Data writes to 0x1000..0xefff reach the IFX registers; the
        interpreter drops them.
      With the first two masked, 638 of the 792 cases where both halt
      match. The other 154 differ, and in 54 more only one side halts,
      hangs or exits. Only a few were traced; the old JIT gives the same
      results on all of them, so they are old JIT behaviour, not new.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32 -Wimplicit-fallthrough -Wunused -Wextra-semi`,
      Linux and `x86_64-w64-windows-gnu`: no warnings in W16 files, nor in
      `DSPCore.cpp`. The remaining warnings come from shared headers
      (`MsgHandler.h`, `x64Emitter.h`) and show the same on the base tree.
      The code avoids the MSVC /W4 patterns C4319, C4805, C4458 and C4267.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W16 file.
  - Follow-up (merge): `SDSP::RunDMA` now cuts a DMA that would run past
    the end of the 8 KB IRAM or DRAM at the end, and drops one that starts
    past it, with an error log. W16's harness found DRAM lengths of 9624 and
    14554 bytes overrunning `dram` (old and new alike); no real ucode does it.
- W21 log: rewrote all 69 W21 files, boot, configuration, debugger, FIFO
  player and HLE (13197 lines now, from 13282): `Core/Boot`, `Core/Config`,
  `Core/ConfigLoaders`, `Core/Debugger`, `Core/FifoPlayer` and `Core/HLE`.
  Every rewritten file starts with the original-code header and has no
  Dolphin copyright or SPDX line. File names, public declarations and
  layouts, config keys (section, system, name, default and type of every
  setting), boot memory contents, register/SPR/MSR values, boot failure
  messages, the DFF file format, debugger and FIFO results, the HLE hook
  table (all 23 entries, same order, names, types and flags) and hook
  addresses, savestate/NetPlay/movie hooks and all translatable strings are
  unchanged.
  - `Config`: Main, Graphics, Netplay, SYSCONF, FreeLook, UI, Session,
    Achievement and Wiimote settings, and DefaultLocale (system language,
    country and region guesses from the host locale).
  - `ConfigLoaders`: base INI/SYSCONF layer, game INI layers (game ID,
    revision and region fallbacks, controller profiles), NetPlay and movie
    layers, and IsSettingSaveable.
  - `Debugger`: BranchWatch, CodeTrace, Debugger_SymbolMap, Dump, OSThread,
    PPCDebugInterface and RSO.
  - `FifoPlayer`: FifoDataFile (DFF load/save, same alerts), FifoPlayer
    (frame and object ranges, register and texture memory loading, BAT
    setup, frame partitioning and timing) and FifoRecorder.
  - `HLE`: hook table, patching and unpatching, the OSReport/printf-style
    hooks with the same quirks for odd conversions (`*` widths and
    precisions, `%.0s`, `%n`), and the SVR4 va_list walker.
  - `Boot`: file type detection and M3U handling, BS2/IPL loading with the
    same known-dump CRC checks, the emulated BS2 for GC and Wii (BAT setup,
    apploader, low-memory globals, SYSCONF/setting.txt, IOS boot), WAD and
    NAND title boot, state flags, Riivolution patches, and the DOL (with
    Wii U ancast images) and ELF readers.
  - Deliberate differences (bug fixes, nothing a game sees):
    - `Config::GetInfoForWiimoteSource`: an index outside 0-4 returns the
      balance board entry instead of reading out of range.
    - DefaultLocale: an unreachable fallback branch is gone.
    - Movie config loader: the video backend name is read with strnlen,
      so a full (unterminated) 16-byte field no longer reads past the
      header.
    - `RSOView::LoadInternals`/`LoadExternals` fill the internals and
      externals views (the old code appended both tables to the imports
      view).
    - `CDump::Read32` returns UINT32_MAX for a word past the end of a
      truncated record instead of reading out of bounds.
    - FifoDataFile writes the reserved/padding bytes of the header, frame
      and memory-update entries as zero (they were uninitialised), and
      zero-initialises a memory-update entry before reading it, so a failed
      read no longer leaves indeterminate fields.
    - DolReader initialises `m_is_valid` and `m_is_wii` to false
      (`m_is_wii` was uninitialised for files shorter than 0x100 bytes).
    - Assert expression texts differ.
  - Checked: old and new code side by side on synthetic inputs only (no
    game data), using the base versions of all non-W21 files on both
    sides, comparing hashes of the output streams:
    - Config: all 450 declared settings dumped under 38 locale
      configurations, plus 200 random sequences over the path, region,
      ISO and USB helpers.
    - ConfigLoaders: 300 seeds (8 cases each) of random INI trees, SYSCONF
      stores, game IDs/revisions, controller profiles, NetPlay settings and
      DTM headers; layer contents, saved files, SYSCONF writes and DTM
      output.
    - Debugger: 1100 seeds of random BranchWatch, CodeTrace, OSThread,
      RSO, call stack, PPCDebugInterface and Dump sequences over fake RAM.
    - FifoPlayer: 400 seeds of synthetic DFF files (valid, truncated and
      damaged) and command streams through load, save, playback (with
      looping, frame/object ranges, early memory updates) and recording;
      RAM, texture memory, registers, BATs and alerts.
    - HLE: 500 seeds fuzzing format strings, va_list walks, hook-table
      operation sequences and every hook body against synthetic guest
      state.
    - Boot: 400 seeds of synthetic DOL (including ancast), ELF, M3U, IPL
      and WAD-style inputs through the readers, GenerateFromFile and
      BootUp for 7 boot kinds, with a fake apploader, file system, disc and
      IOS: guest memory, registers, SPRs, MSR, logs, alerts and NAND
      writes. The harness skips `IsWii`/entry point for DOLs shorter than
      0x100 bytes because the old value is uninitialised (see above).
  - Compile check: the W21 .cpp files are clean with clang `-Wall -Wextra
    -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32`, on Linux
    and for x86_64-w64-windows-gnu. The 44 other files that include W21
    headers (BootManager, ConfigManager, Core, Movie, NetPlay client and
    server, IOS, WiiRoot, PatchEngine, ActionReplay, HW, the PowerPC
    interpreters, JITs, breakpoints and symbol DB, Riivolution, VideoCommon
    BP/opcode/texture cache, three DolphinQt files) compile or fail file
    for file exactly as on the base tree, on both targets (15 and 17
    failures, from headers that are missing here, such as Qt and some
    third-party libraries).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run of 8 or more lines
    is left in any W21 .cpp file. The runs left in W21 headers are public
    declarations (settings lists, struct layouts, enums, member lists).
  - Not rewritten: nothing. The ancast AES keys and IV in DolReader.cpp were
    already in the base tree and are unchanged; no keys, firmware or game
    data were added.
- W25 log: rewrote all 22 W25 files in `VideoCommon`: the vertex loaders
  (`VertexLoader`, `VertexLoaderBase`, `VertexLoaderX64`, `VertexLoaderUtils`,
  `VertexLoader_Position`, `_Normal`, `_Color`, `_TextCoord`), the loader
  manager (`VertexLoaderManager`), `NativeVertexFormat.h`, the
  `IndexGenerator` and `VertexManagerBase`. Every file starts with the
  original-code header and has no Dolphin copyright or SPDX line. The public
  names, signatures and layouts used outside W25 are kept: `VB_HAS_*`,
  `AttributeFormat` / `PortableVertexDeclaration` (memcmp-compared, memset
  constructor), the `VertexLoaderManager` globals and functions,
  `VertexLoaderBase` members, every `VertexManagerBase` virtual, protected
  member and constant the backends use. Members no backend touches are now
  private. Config keys, log, panic and analytics texts are unchanged (assert
  condition texts differ).
  - Design:
    - C++ loader: a list of steps (`VertexLoaderStep`: function, scale,
      array, slot) run over a `VertexLoaderCursor` (src, dst, vertices
      left, drop flag, texture matrix indices). Readers come from
      `GetReader` templates per format, element count and source (inline,
      Index8, Index16; a missing position reads nothing). Sizes are
      computed instead of looked up. Colour conversion widens channels by
      bit replication into packed RGBA.
    - JIT: fixed registers, all caller-saved in both ABIs (src R10, dst
      R11, count R9, anchor RAX, index RCX, array RDX, temp R8, XMM0/1), an
      8-byte frame for count and dropped vertices, 4 KB code space. Globals
      (array bases, strides, caches, colour tables, shuffle and scale
      constants) are reached as displacements from one anchor pointer
      passed in. Shuffle masks and 2^-frac vectors are constexpr tables;
      the 565/4444/6666 colours use byte lookup tables ORed together.
      Dropped vertices branch to an out-of-line stub. The loop starts on a
      16-byte boundary.
    - Loader manager: `RefreshLoader` (dirty check inline), `LoadDraw` as
      the main-thread half of `RunVertices`, `CheckCPConfiguration` split
      into small helpers, ubershader formats built from a zeroed
      declaration with `CopyOrDummy`.
    - Index generator: one switch over the primitives with restart as a
      template parameter; the writer is passed by value so its pointer
      stays in a register (the first draft kept it in memory and was up to
      4x slower on lines; fixed before the checks below).
    - Vertex manager: the index budget per primitive is a small table
      (`BudgetFor`), flush statistics, the draw (`DrawBatch`) and the host
      primitive choice are helpers; calls into the backend, caches, shader
      managers and graphics mods are in the old order with the old
      arguments.
  - Kept as before (outputs, quirks):
    - Every output vertex byte, the native declaration, vertex counts,
      zfreeze caches (last three positions and matrix indices, normal,
      tangent, binormal), statistics and the loader/format caches.
    - C++ loader: texture coordinates after the last used one are
      not declared, but the slot that stops the run still gets offset and
      Float type; colours are always declared as 4 x UByte, offset and
      enable only when present; matrix indices masked to 0x3f; normal caches
      written for the last vertex even when it is dropped; an invalid colour
      format (6/7) panics, writes nothing and still reserves 4 bytes;
      texture coordinate formats 5-7 assert.
    - JIT: positions and texture coordinates scaled only with ByteDequant;
      normals by 2^-7/-6/-15/-14; matrix index bytes not masked; the
      binormal cache stored for every vertex; a dropped vertex keeps its
      matrix bytes and cache writes; 0xFF/0xFFFF indices drop; an indexed
      invalid colour still eats its index; the SSE2 fallback lane contents
      (u8/s8 read 4 bytes, shorts give [X,0,0,0]/[X,Y,0,0]/[X,Y,Z,X],
      floats gathered through a GPR, the stale XMM0 lanes); the vertex size
      assert.
    - Compare mode builds the JIT loader first, then the C++ one, with the
      same mismatch messages.
    - Index generator: with primitive restart a strip is copied as is and
      then a restart index, even with 0-2 vertices; three vertices left in
      a quad draw make a triangle; the QUADS_2 warning; the
      `GetRemainingIndices` panic text and its live config read.
    - Loader manager: a split draw steps the source by 16380 vertices every
      run, including the last; `Clear()` leaves the current format pointer.
  - Deliberate differences:
    - A C++ loader with no position in the VCD reads nothing; the old one
      called a null function pointer (crash).
    - The JIT returns 0 for a count of 0 or less; the old loop ran ~2^32
      times.
    - `VertexLoaderUID` and `PortableVertexDeclaration` hash differently
      (bucket placement only).
    - The debug-only PRIM_LOG dump and an unreachable trailing ERROR_LOG
      are gone; assert condition texts differ.
    - `NativeVertexFormat`'s constructor is `explicit`.
  - Found and not fixed (old and new alike): with an invalid colour format
    the declared stride is 4 bytes larger than what the C++ loader writes,
    and a dropped vertex steps back a whole stride, so the next vertex starts
    before the buffer; at the start of a batch this writes before the vertex
    buffer (both harnesses corrupted the heap there with the C++ loader, on
    old and new alike, until given slack or valid colours). Fixing it would change the output bytes
    of those draws, so it is left as it was.
  - Checked:
    - Loader harness, old vs new with the untouched non-W25 files on both
      sides: random VCD/VAT (junk bits, sparse, valid-only and invalid
      formats, ByteDequant), array strides and bases, 1-600 vertices in
      1-3 batches, 0xFF-biased indices, fixed XMM0/XMM1 on entry. Hashes
      the declaration, sizes, every output byte, loaded counts, all caches,
      the loaded-vertex counter and the alert/log texts (assert condition
      text cut). The output buffer has 8 KB of slack in front, hashed too,
      for the stepping-back case below. 100000 cases each for C++, JIT, JIT without BMI, without
      SSSE3, without SSSE3/SSE4.1 (pure SSE2), without MOVBE, Compare mode
      (valid colours only: invalid ones overflow Compare's scratch buffers
      on both sides) and the size helpers: all identical. Every value of
      every direct colour format (all 2^16, all 2^24, a 2^24 sample of the
      32-bit formats) through C++ and JIT: identical old vs new, and C++ =
      JIT.
    - Index generator harness: random primitives, counts (0-6000), external
      indices, restart / VS expansion settings and config changes after
      Init, overflow panic: 100000 cases identical.
    - Vertex manager harness: VertexLoaderManager + VertexManagerBase with
      recording fakes for the gfx backend, texture cache, framebuffer
      manager, shader cache, graphics mods (skip, custom shaders, uniform
      blocks), custom shader cache, bounding box, perf query, CPU cull,
      shader managers and uids, render states, memory, timers, analytics and
      video events. Random draws (both threads, splits past 16380), flushes,
      config changes, CP/XF/BP state, EFB access and copy scheduling, frame
      end/present events, DoState round trips, utility uploads, texel
      buffers and format lookups; every fake call is logged with its
      arguments, plus committed vertex/index bytes. 100000 sequential cases
      identical.
    - Performance (clang -O2, best of several alternating runs; the machine
      was shared, run-to-run noise about +-10%): JIT loaders the same
      speed (identical instruction sequences in the loops, generated code
      within 1.2 KB of 4 KB); C++ loader 15-30% faster; index generator the
      same in total (0.93-1.03 over 64 primitive/size/restart rows).
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`: no warnings
      in W25 files (VertexManagerBase on the Windows target needs the
      glslang headers on the path, as on the base tree). The 74 files that
      include a W25 header compile as on the base tree on both targets; the
      ones that fail, fail the same way on the base tree for missing
      third-party headers (Qt, SFML, tinygltf, D3D/WinRT, VMA). With a VMA
      header added, the Vulkan backend files compile, except VKPipeline.cpp
      (old Vulkan headers, same on base). The code avoids the MSVC /W4
      patterns C4319, C4805, C4458 and C4267.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W25 .cpp.
      The headers keep some 8-12 line runs of declarations (the
      `VertexManagerBase` interface and member layout).
- W18 log: rewrote all 46 W18 files (the IOS kernel and its core
  devices under `Core/IOS`) as original code. They now have 14614 lines
  (from 15395). Each starts with the RetroPixels original-code line and
  has no Dolphin copyright or SPDX header. The files are IOS, IOSC,
  MIOS, Device, DeviceStub, DolphinDevice, Uids.h, VersionInfo, the AES
  and SHA devices, /dev/di, the SD slot, STM, every ES file (ES,
  Formats, Identity, NandUtils, TitleContents, TitleInformation,
  TitleManagement, Views), the FS layer (FileSystem.h,
  FileSystemCommon, FileSystemProxy and the host backend FS and File)
  and the WFS server and installer.
  - Kept unchanged: file names, and every public declaration that other
    files use (names, signatures, namespaces, enum values, constants,
    the TMD/ticket/certificate and ISFS request layouts, the FS
    Metadata and handle structs). The IOSC key and certificate tables
    are byte for byte the same; no key, certificate or firmware was
    added. Everything that affects emulation also stays the same:
    - every ioctl and ioctlv result, reply buffer and reply length of
      ES, FS, AES, SHA, STM, /dev/dolphin, the stub devices, DI, SD and
      WFS, with the same CoreTiming ticks for each reply;
    - IOS boot and reload (version table, memory setup, RAM override,
      device lists per version, the PPC uid/gid, the title context
      switch), the IPC event queue and its ordering;
    - ES and IOSC results: title import and export, ticket and TMD
      checks, content decryption, signatures, the console identity and
      the device certificate; every NAND file written, with the same
      names, contents, owners and modes;
    - FS metadata and error codes, the FST and its host file, the
      handle table, and the SD card (CID/CSD values, SDHC detection,
      block reads and writes);
    - the savestate layout and order of the kernel and every device,
      every config key, the netplay and movie NAND hooks, and every
      translatable string.
  - Kept quirks: WFSSRV GET_ATTRIBUTES still clears its reply with the
    old length; ARCUnpacker::Extract still reads past a malformed
    archive; ExportTitleInit and ADDTMD with no active title still read
    an empty TMD; the FS handle padding is still saved raw; and after a
    host FS state load the FST children change order, so a save, load,
    save is not byte identical (as before). In the host FS, deleting
    "//a" still skips the directory-in-use check, a rename onto an open
    file still goes ahead, and a rename that grows an FST child list can
    still leave a stale parent pointer (the FS harness avoids these last
    two cases). Three guest-reachable null or out-of-bounds accesses are
    also unchanged: ImportContentEnd hashes the TMD's content size even
    when less data was imported, GetSharedContentsCount uses the
    /shared1 listing even when it cannot be read, and any read, write,
    seek or file ioctl on a file opened with no access mode uses its
    missing host file.
  - Deliberate differences (genuine bugs, harmless fixes, cleanups):
    - Device::Unsupported names an unknown command "an unknown command"
      in its log (the old code's map lookup threw for one).
    - LoadNandFile checks the file status before using it (the old code
      dereferenced a failed GetFileStatus for "/dev/fs").
    - The SHA context and the FST Metadata padding are zeroed (the same
      savestate bytes, now always zero instead of indeterminate).
    - FileHandle's move assignment releases the old handle correctly.
    - SetUpStreamKey's handle and WFSI GET_TMD's size start at 0
      instead of uninitialised.
    - LaunchPPCTitle closes its ScopedFd earlier and BootstrapPPC clears
      the boot path before the call; neither changes any result.
    - OpenContent looks up the content before the title ID (same
      results), and DeleteDirectoriesIfEmpty stops on a path with no
      slash instead of looping.
    - The SD CID/CSD words are written in a different order (same final
      bytes).
    - Some DI, SD and WFS log messages are worded differently. Panic
      messages and translatable strings are unchanged.
  - Checked: harnesses linked the old and the new code against the same
    untouched code outside W18 (Common, Config, PointerWrap, crypto)
    with fake memory, timing, IPC and system objects and a zeroing
    allocator. No Nintendo keys, BIOS, firmware, DSP ROMs or games were
    used: all titles, tickets, TMDs and certificate chains were
    synthetic, built and signed by the harness.
    - VersionInfo: every table and lookup identical.
    - FS host backend: random FS operations and state saves, 40 seeds x
      3000 steps, identical.
    - ES formats, ES core and IOSC: random synthetic titles, tickets,
      imports, signatures and key operations, 40 seeds x 500 steps,
      identical.
    - Devices (DI, SD, WFSSRV, WFSI): 40 seeds x 3000 steps, identical.
      A guard skips feeding malformed archives to WFSI, which reads out
      of bounds on both sides.
    - Kernel: the full IOS kernel with ES, FS, STM, AES, SHA,
      /dev/dolphin and the stubs, driving every ES ioctlv 0x00-0x46,
      composite imports and exports, FS and file ops, IPC events,
      memory setup, RAM override, NAND file loads, device updates, the
      title context, setting.txt and savestate save and reload. 40
      seeds x 3000 steps, KERNEL_RESULT. The harness keeps the three
      shared crashes listed under kept quirks out of the stream (both
      sides crashed at the same step before the guards).
    - Not covered by a harness: MIOS and the boot paths (BootIOS,
      BootstrapPPC, Launch) were checked by reading only; DI, SD and WFS
      were covered by the device harness rather than the kernel one.
    - Compile check: the W18 .cpp files and all 47 other .cpp files that
      include W18 headers build with clang on Linux and with `-target
      x86_64-w64-windows-gnu` (apart from Socket.cpp and UICommon, which
      fail the same way on the base commit with mingw). Under `-Wall
      -Wextra -Wconversion -Wsign-compare` the W18 files give no new
      warnings in any dependent (the base code gave some, for example
      9 in ES.cpp). A scan found no bool mixed with an integer under `|`
      or `&` (MSVC C4805).
    - Verbatim audit (`tools/verbatim-audit.py wii 8`): no function body
      in W18 .cpp files shares 8 or more lines with the old code. The
      remaining runs are the BootMiiKeyDump struct and the key tables
      in IOSC.cpp (kept byte for byte), the ISFS request structs in
      FileSystemProxy.cpp, and declarations in headers.
- W24 log: rewrote all 37 W24 files as original code (11051 lines now, from
  12077): the VideoCommon command core in `VideoCommon` (`AsyncRequests`,
  `BPFunctions`, `BPMemory`, `BPStructs`, `BoundingBox`, `CPMemory`,
  `CPUCull` and `CPUCullImpl.h`, `CommandProcessor`, `Fifo`,
  `OpcodeDecoding`, `PixelEngine`, `Statistics`, `VideoState`, `XFMemory`,
  `XFStateManager`, `XFStructs`, plus `Constants.h`, `DataReader.h`,
  `LookUpTables.h` and `VideoEvents.h`). Every file starts with the
  original-code header and has no Dolphin copyright or SPDX line. File
  names, public declarations, register unions and their layouts
  (`BPMemory`, `CPState`/VAT, `XFMemory`, `SCPFifoStruct`), enum values,
  savestate `DoState` order, config keys and every translatable and
  debugger string (including the FIFO analyzer's register descriptions
  and the "exponenential-sequared" typo) are unchanged.
  - Command stream: `OpcodeDecoding` (the run-command template with
    primitives first, then a switch; the same cycle costs and the same
    display-list preprocess/execute rules), `Fifo` (single- and dual-core
    run loops, the same sync points, wake-ups, idle skipping and
    `SyncGPU` timing), `CommandProcessor` (MMIO registers, FIFO pointers,
    watermark and breakpoint interrupts with the same timing, the
    dual-core read/write distance handling), `PixelEngine` (registers,
    token and finish interrupts with the same delivery events).
  - Register files: `BPStructs` (dispatch split into command registers
    that are never deduplicated, passive registers, fixed registers and
    register blocks; EFB copies, TLUT loads and TMEM preloads with the
    same renderer and texture-cache calls in the same order),
    `BPFunctions` (scissor rectangles with the same candidates, sort and
    best choice; blend, depth, generation mode, EFB clears and format
    reinterpretation), `BPMemory` (register layout and formatters with
    byte-identical output; the TEV equation views are built by one
    template for color and alpha), `CPMemory` (VAT accessors, the same
    register info text and the same checks on out-of-range writes),
    `XFStructs` and `XFStateManager` (XF writes, indexed loads and the
    same dirty ranges, including the position-matrix and post/light
    range quirks).
  - `CPUCull`, `BoundingBox`, `AsyncRequests`, `Statistics` (the same
    ImGui calls with the same arguments and float arithmetic, including
    the scissor overlay's layout quirks), `VideoState` (same savestate
    order).
  - Deliberate differences (no game sees these):
    - `XFStructs`: `LoadIndexedXF` returns early when the source pointer
      is null (the old code dereferenced it). `address` is marked
      `[[maybe_unused]]` in `LoadIndexedXF` and `PreprocessIndexedXF`.
    - `CPUCull`: the culling maths is plain scalar code; the decisions are
      the same, but NaN payloads in transformed positions may differ. The
      inline-asm barrier was dropped.
    - `CommandProcessor`: a condition that was always true is simplified;
      the dual-core distance check reads the distance once into a local
      instead of twice.
    - `BPMemory.h`: the `ScissorPos` and `ScissorOffset` formatters are
      `const`; private parts were redesigned (the per-unit debugger views
      in `AllTexUnits`, a private helper in `RAS1_IREF`). Public names and
      layouts are unchanged.
    - `Statistics.h`: the macros for builds without statistics expand to
      `((void)0)`.
    - Explicit casts replace implicit narrowing and int/float conversions
      (same values); see the warning fixes below.
  - Warning fixes (MSVC-relevant ones included):
    - `IND_MTX::GetScale` (int to u8) and `FogRangeKElement::GetValue`
      (u32 to float), the TEXSCALE and matrix formatters (int to float).
    - `OpcodeDecoding`: sign conversions on the `LoadBPReg` cycle count
      and the `RunVertices` result.
    - `XFStateManager`: `BitSet` index (int to size_t).
    - `Statistics`: int to float in `AddRectFilled` and the table helper;
      a `const auto*` on a `std::ranges::find` result became `const auto`
      (MSVC checked iterators).
    - `if constexpr` early returns restructured to avoid MSVC C4702.
  - Checked: old and new code side by side in a harness (same random
    inputs, one hash per case streamed through `cmp`), all identical:
    - Command decoding and display lists (4000), register info text for
      every BP/CP/XF register and value pattern (10000), BP/CP/XF
      formatters (10000), BP writes and BPFunctions (10000), XF writes and
      indexed loads (3000), CP/PE MMIO in single- and dual-core mode
      (3000), threaded FIFO runs with sync points (3000), bounding box
      (3000), CPU culling (3000; also built for SSE2-only and AVX2/FMA),
      statistics overlays (10000), async requests (3000), savestates
      (3000), determinism runs (3000).
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`: no warnings
      in W24 files, nor from W24 headers in the dependents checked
      (`PixelShaderGen`, `VertexLoaderManager`, `VertexShaderManager`,
      `PixelShaderManager`, `VideoBackendBase`, `TextureCacheBase`).
      The missing glslang and xxhash headers on the Windows target are the
      same on the base tree.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W24
      .cpp file. Remaining runs are in headers only (public declarations,
      enum values and names, register layouts).
- W30 log: rewrote all 44 W30 files and both CMake lists as original code
  (5400 lines now, from 7360): the software renderer in
  `VideoBackends/Software` and the null backend in `VideoBackends/Null`.
  Each file starts with the RetroPixels original-code line and has no
  Dolphin copyright or SPDX header.
  - Software: `Clipper`, `CopyRegion.h`, `EfbCopy`, `NativeVertexFormat.h`,
    `Rasterizer`, `SWBoundingBox`, `SWEfbInterface`, `SWGfx`, `SWOGLWindow`,
    `SWTexture`, `SWVertexLoader`, `SWmain.cpp`, `SetupUnit`, `Tev`,
    `TextureCache.h`, `TextureEncoder`, `TextureSampler`, `TransformUnit`,
    `VideoBackend.h` and `CMakeLists.txt`.
  - Null: `NullBackend.cpp`, `NullBoundingBox.h`, `NullGfx`, `NullTexture`,
    `NullVertexManager`, `PerfQuery.h`, `TextureCache.h`, `VideoBackend.h`
    and `CMakeLists.txt`.
  - Kept unchanged: file names, every declaration other files use (the
    `Clipper`, `Rasterizer`, `TransformUnit`, `EfbInterface`, `EfbCopy`,
    `TextureEncoder`, `TextureSampler` and `BBoxManager` functions, the
    `SetupUnit`, `SWVertexLoader`, `SWGfx`, `SWTexture`,
    `SWStagingTexture`, `SWFramebuffer`, `SWEFBInterface`,
    `SWBoundingBox` and Null classes, `InputVertexData` and
    `OutputVertexData`), the CMake targets, sources and links. Everything
    that affects emulation also stays the same:
    - every EFB colour and depth value for every pixel format, blend,
      logic op, dither, alpha/Z test and update mask, the EFB clear, the
      pixel engine peeks and pokes, the bounding box and the perf-query
      counters (including the every-third-call quad counting and its
      phase surviving ResetPerfQuery);
    - every EFB copy format from the RGBA6, RGB8, RGB565 and Z24 buffers,
      full and half scale (including the swapped R/B of half-scale Z24
      copies to RGBA8, RG8 and GB8, and the RGB8 A8 copy writing 0xff),
      and the XFB bytes (copy filter, gamma, YUV conversion, vertical
      scaling, the persistent encode buffer and its border column);
    - texture sampling for every texture and TLUT format, wrap mode,
      LOD, mip level and filter, from RAM and from preloaded TMEM;
    - the transform unit (matrices, lighting, attenuation, every texgen
      type, post-transform and normalisation), primitive assembly for
      every primitive type, clipping, culling, the rasterizer's fill
      convention, 2x2 blocks, LOD and perspective correction, z-freeze,
      and the TEV (indirect texturing, swap tables, konstants, both
      combiners, z textures, fog including range fog) with the same
      float expression trees, so results are bit-identical;
    - the config name "Software Renderer", the translatable display name
      and warning text, every backend-info flag, the presentation shaders
      and GL calls and the GL error texts; for Null, "Null" with its
      i18n note, every backend-info flag and `Adapters.clear()`;
    - every log, panic and assert condition.
  - Kept quirks: TEV state carries over between pixels as before (last
    texture colour, raw texture colour, bump alpha, indirect texels, the
    coordinate left by an invalid indirect matrix, and block UVs/LODs of
    unused texgens). Texture coordinates and indirect offsets wrap at 24
    bits. The alpha combiner still rounds a subtraction as `-x >> 8` and
    the colour combiner as `-(x >> 8)`. Strips alternate winding, fans
    use (v0, vn-1, vn) and quads split into (0,1,2) and (0,2,3). Lines
    clip the near plane at (0,0,-1,1), triangles at (0,0,0,1). Range fog
    still scales k by 4.
  - Deliberate differences (genuine bugs, harmless fixes, cleanups):
    - EFB pixels are read and written as 3 bytes. The old code used a
      4-byte read-modify-write, which for the last depth pixel read and
      wrote one byte past the EFB array.
    - EFB copies read whole blocks; blocks reaching past the end of the
      EFB array (depth copies at the bottom edge) now read zero. They
      used to read past the array.
    - The rasterizer's edge functions are evaluated in wrapping 32-bit
      arithmetic. The old code relied on signed overflow for vertices far
      off screen (with clipping disabled), which is undefined; with clang
      -O2 the old binary rasterized differently from the same code built
      with -fwrapv in 2 of 1500 random scenes. The new code matches the
      old code built with -fwrapv (and MSVC's wrapping behaviour).
    - Range fog's upper k value at index 10 (which always has weight 0)
      is taken from index 9 instead of reading past `FogRangeParams::K`.
      The result is the same.
    - The clipper's vertex buffers are larger and their indices are
      guarded.
    - The XFB row buffer starts zeroed (it was uninitialised stack), and
      the copy-filter setting is read once per XFB copy.
    - `EfbInterface::BlendTev` no longer writes into the caller's colour
      array; no caller read it afterwards.
    - `SWVertexLoader` looks up the vertex declaration once per batch.
    - Texture copies with an empty or negative rectangle copy nothing.
    - Removed declarations nothing else used: the rasterizer's
      `RasterBlock` types (now private), `OutputVertexData::Lerp` and its
      anonymous enum, Tev's internal reference and LUT types, and
      `SW::PerfQuery` (now an unnamed class in SWmain.cpp).
    - Assert message texts differ (same conditions).
  - Checked: a harness built the old (untouched) and new Software
    sources into two programs against the same untouched VideoCommon,
    Common and Config objects, with stubs for Core::System, RAM, logging
    and panics (messages hashed, assert texts reduced to their firing).
    Each case randomises BP, XF, TEV registers and konstants, texture
    units, RAM and TMEM, then prints one hash of the whole EFB (colour
    and depth), bounding box, perf counters, statistics and messages;
    old and new streams went through cmp. No Nintendo keys, BIOS,
    firmware or games were used. Outputs are identical for:
    - draw (random vertices through transform, setup, clip, raster and
      TEV, 1-4 batches per scene, state changes between batches) and
      clip (random clip-space vertices): 20000 scenes each against the
      old code built with -fwrapv; against the plain old build, clip is
      identical for 8000 scenes and draw for 1498 of 1500 fresh scenes,
      the other 2 being the signed-overflow cases above;
    - EFB copies (20000: every copy format, colour and depth, full and
      half scale, intensity, XFB with gamma, y scale and both
      copy-filter settings, plus EFB clears);
    - texture sampling (20000 x 400 samples over all formats, TLUTs,
      wraps, LODs and mips), peeks/pokes/BlendTev/ZCompare/bounding box
      (20000 x 300 operations) and vertex parsing (20000 x 8 vertices
      over all component formats).
    - The harness found one difference, fixed before the final runs: the
      range-fog factor has to be rounded to float before scaling ze.
      Changing one rounding in the rasterizer or the TEV alpha rounding
      in a copy of the new code made the harness report mismatches.
    - Speed: 200 TEV-heavy scenes of full-screen quads took 7.6-7.8 s
      with the old code and 7.0-7.1 s with the new code (about 9% faster).
    - The device layer (SWGfx, SWOGLWindow, SWmain, SWTexture's texture
      copies) and the Null backend need a GL context or the full
      VideoCommon, so they were checked by review and compile only.
    - Compile check: every W30 .cpp and VideoCommon/VideoBackendBase.cpp
      (the only file outside W30 that includes a W30 header) build with
      clang on Linux and with `-target x86_64-w64-windows-gnu`, apart
      from VideoBackendBase.cpp on the Windows target, which fails in the
      untouched tree too (this machine lacks the glslang headers). Under
      `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32` the W30 files give no warnings (the old files
      gave 362). The warning fixes are explicit, value-preserving casts.
    - Verbatim audit (`tools/verbatim-audit.py wii 8`): no function body
      in W30 .cpp files shares 8 or more lines with the old code. The
      remaining runs are class declarations in the headers (SWGfx.h,
      NullGfx.h, NullTexture.h, SWTexture.h, SWEfbInterface.h and the
      VideoBackend.h files).

- Wii keys log: removed every Nintendo key from PixelU-Wii. The engine now
  reads the owner's own keys file, and RetroPixels Native asks for it
  before a Wii game.
  - Removed (now loaded from the owner's files):
    - `IOSC.cpp`:
      - retail and development common keys, Korean common key, SD key;
      - the default console private key, its signature, key ID and device
        ID;
      - the root public keys (retail and development).
    - `DolReader.cpp`: the Espresso ancast keys (retail and development)
      and their IV.
    - `WiiSave.cpp`: `SD_INITIAL_IV`.
    - `NetKDRequest.h`: the WC24 mail-check HMAC key.
    - `MotionPlus.cpp`: the secret square root used to answer the
      MotionPlus challenge (`CHALLENGE_SQRT_V`). The public modulus `n` is
      kept.
  - Kept (not Nintendo secrets):
    - `WiiSave.cpp` `MD5_BLANKER`: the fixed filler that replaces the MD5
      while a save header is hashed. It is a format constant, not a key.
    - `WiiSave.cpp` `NG_ID`: a placeholder console ID written into
      exported backups.
    - Wiimote extension encryption tables: the algorithm's tables; the key
      comes from the game.
    - Not Nintendo's:
      - ARDecrypt tables (Datel's);
      - Infinity and Skylanders constants (the toy makers');
      - the updater's ed25519 public key (Dolphin's);
      - the sect233r1 curve parameters (public);
      - SSL.cpp's SHA-256 hashes of the user's own certificate dumps.
    - `Data/Sys/Wii/shared2/wc24/nwc24msg.cfg`: Dolphin's default WC24
      account values (password, mlchkid). They are service credentials,
      not keys, so they are left as they were; flagged for review.
  - Owner's files, in the Wii NAND root (`User/Wii`, or Dolphin.ini
    `[General] NANDRootPath`):
    - `keys.bin` (BootMii, 1 KiB) is read first, then `otp.bin`:
      - a Wii's 128-byte OTP: retail MS/CA IDs, no NG signature, no Korean
        key;
      - a Wii U's 1 KiB OTP: bank 0, plus bank 6 at 0x300 for the MS/CA
        IDs, NG key ID and signature, and the Korean key at 0x348
        (WiiUBrew "Hardware/eFuse").
    - `wii_keys.txt` (optional, `name = hex` lines) holds the keys a
      console dump doesn't have:
      - `sd_key`, `sd_iv`;
      - `root_key`, `root_key_dev`;
      - `espresso_ancast_key`, `espresso_ancast_key_dev`,
        `espresso_ancast_iv`;
      - `wc24_mail_check_key`;
      - `motionplus_sqrt_v`.
    - The Espresso ancast key is not in the Wii U's Latte OTP (it is in the
      Espresso's own fuses), so vWii ancast images only load when it is
      given in `wii_keys.txt`. Otherwise there is a clear message.
    - Common keys load only when the dump's console type (CA ID 2 means
      development) matches the IOSC console type.
  - Without a keys file:
    - GameCube is unchanged. Homebrew DOL/ELF still boots.
    - Wii discs, WADs and NAND titles stop before boot with one message
      ("RetroPixels needs a keys file from your own Wii...").
    - Title keys can't be decrypted:
      - VolumeWii reads fail;
      - WAD content checks fail;
      - WIA/RVZ conversion fails;
      - DirectoryBlob won't encrypt;
      - WFSI returns ENOTICKET.
    - The game list still shows Wii discs: ID, maker, revision, country and
      name fall back to the disc's own header. There is no banner.
    - The verifier adds a "results may not be real" problem.
    - SD save import/export says the SD keys are missing. It never opens
      (or truncates) data.bin.
  - Without the extra keys, even with a keys file:
    - Root-to-CA certificate signatures are not checked (the rest of the
      chain is).
    - WC24 mail HMACs are not checked.
    - MotionPlus "y1" challenge answers are wrong; games retry. This is a
      possible gameplay regression for MotionPlus titles and is not
      confirmed on hardware.
  - RetroPixels Native: before a Wii/WiiWare game, if the Wii root has no
    usable keys, a "Wii keys file" window offers:
    - Find keys on this PC: a background search with a Stop button.
      - It looks in Downloads, Desktop and Documents, then fixed drives and
        memory cards (C: to Z:).
      - It skips system, program, cache and VCS folders and symlinks; the
        limits are 12 folders deep and 90 s.
      - Names searched: `*keys*.bin`, `*otp*.bin`, `*nand*.bin`, by exact
        size.
      - Matches are checked by layout, copies are listed once, and the
        list shows each console's ID.
    - Choose file...
    - How to get yours: opens wiibrew.org/wiki/BootMii, plus two lines
      saying the file comes from the owner's own Wii.
    - Cancel.
    - The chosen file is converted to keys.bin (an old one is kept as
      keys.bin.old) and the game starts. Nothing is downloaded.
  - Checked:
    - IOSC harness: an old build with the tables replaced by synthetic keys
      vs the new build with the same synthetic keys.bin/otp.bin/
      wii_keys.txt, with hash streams compared. Retail, development, Wii
      otp.bin and Wii U otp.bin are identical. The no-keys, mismatch and
      parser checks pass. A mutated sd_key gives DIFF (negative control).
    - Go tests (synthetic dumps only) for:
      - root lookup with NANDRootPath;
      - HaveWiiKeys, matching the engine's checks;
      - layout checks and conversions;
      - nand.bin tail;
      - install;
      - scan (skips, depth, dedupe, cancel, timeout).
    - `go vet`, `go test`, the `GOOS=windows` vet and the Windows build.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`: diagnostics
      are identical to the base tree for every changed file and every
      includer of `IOSC.h` and `ES/Formats.h`. MSVC was not run.

- W27 log: rewrote all 29 W27 files in `VideoCommon` as original code (8650
  lines now, from 9950): the shader generators (`ShaderGenCommon`,
  `PixelShaderGen`, `VertexShaderGen`, `GeometryShaderGen`,
  `LightingShaderGen`, `FramebufferShaderGen`), the ubershaders
  (`UberShaderCommon`, `UberShaderPixel`, `UberShaderVertex`), the constant
  managers (`PixelShaderManager`, `VertexShaderManager`,
  `GeometryShaderManager`, `ConstantManager.h`) and the glslang glue
  (`Spirv`, `ShaderCompileUtils`). Every file starts with the original-code
  header and has no Dolphin copyright or SPDX line. The public names,
  signatures and layouts used outside W27 are kept: every UID struct
  (`pixel_shader_uid_data`, `vertex_shader_uid_data`,
  `geometry_shader_uid_data`, `LightingUidData`, the ubershader UIDs) bit
  for bit, `ShaderUid`, `ShaderCode`, `ShaderHostConfig`, `WriteSwitch`, the
  `I_*` uniform names, `ShaderAttrib`, the three constant structs in
  `ConstantManager.h` (declarations unchanged, comments new), every manager
  method and public member, `SPIRV::Compile*` and `ShaderIncluder`.
  Removed from the headers, with no user outside W27: the
  `s_shader_uniforms`, `s_geometry_shader_uniforms` and `s_lighting_struct`
  strings (now `WriteVertexUniformMembers`, `WriteGeometryUniformMembers` and
  a writer in `LightingShaderGen.cpp`), the `LIGHT_*` format macros, and the
  constant-usage interface (`ShaderGeneratorInterface`,
  `ShaderConstantProfile`, `SetConstantsUsed`, `ConstantIsUsed`), which had
  been a no-op.
  - Design:
    - Generators: each shader is written by a small writer class
      (`PixelShaderWriter`, `VertexShaderWriter`, `GeometryShaderWriter`,
      `UberPixelWriter`, `UberVertexWriter`) holding the UID, API and host
      config, with one method per section (interface, uniforms, texgens,
      TEV stages, alpha test, fog, logic op, blending, depth, output).
      Repeated text comes from tables (TEV inputs, konst selections, logic
      ops, blend factors, light fields, primitive layouts, the uniform
      block members) and loops instead of literal runs. Text without format
      fields goes through a `Put` helper so braces stay single. The UID
      builders read the registers in helpers per area
      (`FillTexGenKey`, `SetTevindref`/`GetTevindref` as a switch, the
      lighting key per channel).
    - `UberShaderPixel`: switches built at run time from string tables
      (`WriteCases`), the index-select trees (`WriteIndexSelect`, a D3D
      switch or nested pairs), the combiner fields as one template for
      colour and alpha.
    - `FramebufferShaderGen`: shared helpers for samplers, varyings (as a
      block or loose), vertex and pixel `main` openings and the fetch
      preamble; texture reinterpretation is a decode step into `raw_value`
      and an encode step, each a switch over the formats.
    - Managers: register reads grouped per constant, a `Store` helper that
      reports a change, light conversion, colour unpacking and matrix row
      copies as helpers in `VertexShaderManager.cpp`; the stereo, VS-expand
      and viewport parts of `GeometryShaderManager::SetConstants` reduced to
      single assignments.
    - `Spirv`: a `SpirvCompile` object per compile (parse, link, convert,
      log, failure dump); the resource limits are built by named field
      assignment.
  - Kept as before (outputs, quirks):
    - Every generated shader text for every UID, API (D3D, OpenGL, Vulkan,
      Metal, Nothing) and host-config bit, byte for byte, including the
      odd spacing and the lines with trailing spaces; the disk shader cache
      file names.
    - Every UID byte and how it is built: pixel UIDs (including
      `ClearUnusedPixelShaderUidBits`), vertex UIDs, geometry and lighting
      UIDs, ubershader UIDs and their enumeration order.
    - `VertexShaderGen`: in the expanded line/point path the cached
      "binormal" is tied to `VB_HAS_TANGENT` and "tangent" to
      `VB_HAS_BINORMAL`, as before.
    - `PixelShaderGen`: frag_input texture coordinates past 8 are still
      copied, with the zero fill only for numtexgens..8; channel 1 is lit in
      the fragment body only with two colour channels.
    - `FramebufferShaderGen`: loose colour varyings are placed after the
      *input* texcoord count; `GS_OUTPUT` has no newline after its brace; the
      per-sample (SSAA) format conversion line has no newline; RGB565
      reinterpretation output reads the value as 5:5:5 (`>> 10`); an
      unsupported reinterpret format logs and returns the text "{}\n".
    - Managers: every uploaded value and dirty flag. The destination-alpha
      flag is never cleared once set (the value is recomputed on every
      `SetConstants`); `SetBoundingBoxActive` compares against the effective
      state but stores the requested one; the line/point texture offsets
      {0, 16, 8, 4, 2, 1, 1, 1}; light directions normalised in double and
      NaN/inf sanitised; the viewport height scaled with the horizontal EFB
      scale; the fog range centre bias 342 and the four-times K factors;
      the indirect reference bit 16; fog colour untouched with fog disabled;
      the fixed konst rows; the savestate order of every manager.
    - `Spirv`: the glslang messages (SPIR-V rules, Vulkan rules for Vulkan
      and Metal), version 450 core, the resource limits (compared field by
      field), the debug-info/validate and optimise/strip option sets, the
      info-log thresholds (108 and 25), the warning, panic and bad-shader
      dump texts and the dump counter.
  - Deliberate differences:
    - `GetVertexShaderUid` stops the texgen loop at 8; with more texgens
      set the old loop wrote past `texGenInfo[8]` (undefined behaviour).
    - `GetPixelShaderUid` stops the texture-matrix projection loop at 8
      (the old one read past the array; the result is the same).
    - `ShaderIncluder` stores the file text in its map entry before
      pointing the glslang result at it. The old code took the pointer
      first and then moved the string twice, so for a short file (inline
      string storage) the pointer dangled.
    - `ShaderIncluder::releaseInclude` ignores a null result. glslang
      releases the null result of a failed local lookup before trying the
      system paths, which crashed the old code.
    - A dead branch in the pixel shader blend writer and the no-op
      constant-usage calls are gone (no text change).
    - `api_type` is `[[maybe_unused]]` where it is unused; bitfield and
      index conversions are explicit casts; `bool | bool` and `u32 &= bool`
      are written as plain conditions.
  - Checked:
    - Generator harness, old vs new with the untouched non-W27 files on
      both sides, one hash per case streamed through `cmp`: random pixel,
      vertex, geometry, uber pixel, uber vertex and framebuffer shaders,
      the common pieces (headers, VS output members and assignment, line
      and point expansion, interpolation qualifiers, lighting) and UIDs
      built from random register states, for all five API types, random
      host-config bits, driver-bug masks and custom shader contents.
      Hashes the shader text, UID bytes and log/alert events. 12000 to
      40000 cases per kind over several seeds, plus 40000 mixed: all
      identical.
    - Manager harness: random sequences (1-200 steps) of register writes,
      XF invalidations, config changes, every manager setter,
      `SetConstants`, `Dirty`, `Init`, savestate loads from random data,
      clip-space transforms and vertex formats, with free-look and
      graphics-mod projection actions stubbed the same on both sides.
      Hashes all three constant blocks, the dirty flags, the savestate
      images (so the private flags too), the projection statistics and
      every side effect. About 90000 sequences over 12 seeds: identical.
      The only difference seen: when several NaN inputs meet in
      `TransformToClipSpace`, the NaN payload of the result can differ (the
      compiler orders the operands), so the harness compares NaN-ness
      there.
    - SPIR-V harness (system glslang 15.1): generated shaders and small
      hand-written ones for every stage, all API types, three SPIR-V
      versions, the validation option, broken sources (truncated or with
      junk) and #include through the includer (nested local includes and
      system includes), plus direct includer sequences with random depths,
      lookups and releases. Hashes the SPIR-V words, the includes held,
      the logs, alerts and dump file names. About 11000 cases: identical.
      The resource limit tables compared field by field: identical.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`, for the W27
      .cpp files and every .cpp that includes a W27 header directly or
      through other headers (77 files on Linux, 85 on Windows): no warnings
      in W27 files or from W27 headers (the old files gave 139), no new
      errors. Missing third-party headers were fetched into scratch
      (xxHash, VulkanMemoryAllocator, Vulkan-Headers, SPIRV-Cross, imgui,
      implot, a stand-in `winrt/base.h`). Still failing on both trees:
      `Boot_BS2Emu.cpp` (`std::expected` in `FileSystem.h`) and, on the
      Windows target, two D3D12 files (`ID3D12Debug` missing from MinGW).
      No `bool` mixed with an integer under `|` or `&` (MSVC C4805), no
      `~` with widening, no unary minus on unsigned values.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W27
      .cpp file. Remaining runs are in headers only (UID and constant
      struct layouts, `ShaderHostConfig`, manager and generator
      declarations).

- W26 log: rewrote all 20 W26 files in `VideoCommon` as original code (9127
  lines before, 7527 now): the texture cache (`TextureCacheBase`, with
  EFB/XFB copies to texture and RAM), `TMEM`, `TextureInfo`,
  `TextureConfig`, `TextureUtils`, the texture decoders
  (`TextureDecoder.h`, `TextureDecoder_Util.h`, `TextureDecoder_Common`,
  `TextureDecoder_x64`), the conversion shader generators
  (`TextureConversionShader`, `TextureConverterShaderGen`) and
  `HiresTextures`. Every file starts with the original-code header and has
  no Dolphin copyright or SPDX line. Public names, signatures, enum values
  and layouts used outside W26 are kept, including every
  `TextureCacheBase` virtual, data member (name, order, type) and the
  `TCacheEntry` layout. Config keys, savestate layout, log/panic texts and
  dump file names are unchanged.
  - Design:
    - Decoders: block shapes come from one `ShapeOf` table (texture and
      EFB copy formats, including the copy base format); a tile walker
      drives the SSE2/SSSE3 per-format block decoders instead of the old
      hand-unrolled loops; single-texel decoding goes through shared
      locate/palette/CMPR/YUV helpers.
    - TMEM: each unit has two regions and a three-state residency
      (invalid / valid / cached) recomputed from region overlap at bind.
    - TextureInfo/TextureConfig/TextureUtils/HiresTextures: rewritten around
      helpers for level walking, name building, pack discovery and id /
      wildcard matching.
    - Shader generators: new emitter code; the fixed GLSL bodies of the
      compute decoders are kept as data (they are the output).
    - Texture cache: file-local helpers for the shared parts (single-level
      render-target configs, the palette/reinterpret conversion pass, copy
      naming and dumping, host copy rectangles and clamp rows, save-state
      level extents, the full-hash test, scaled coordinates); the mip-chain
      check is a small class; uniform blocks are declared as arrays with
      `static_assert`ed sizes; `TCacheEntry::DoState` is one fold over the
      fields; `OnConfigChanged` compares `std::tie` tuples;
      `EFBCopyParams::operator<` orders a `std::tie` key.
  - Kept as before (outputs, quirks):
    - Every decoded texel for every format, palette format and size; TMEM
      residency answers and savestate bytes; texture names (including the
      C14X2 palette-range calculation) and the pool hash.
    - Emitted shader text byte for byte.
    - Texture cache: every backend call in order with its arguments, cache
      hits/misses/invalidations, pool reuse, savestate bytes. Quirks kept:
      a deferred XFB copy's flush stores the overlapping copy's hash on
      itself; the arbitrary-mipmap check steps rows by `row_length` bytes
      (not texels) and reads scratch it may not have written; an XFB copy
      with only EFB dumping on is dumped (and named) as an EFB copy; the
      map slot of an EFB copy is replaced by its reinterpreted/palette
      version during partial updates; `CopyEFB` keeps its unused stride
      parameter and the "Skipping EFB copy to VRAM" text on the RAM path.
  - Deliberate differences:
    - `CreatePaletteEntryWithOffset` returns offset 0 when the texel upload
      fails (was uninitialised).
    - Uniform padding is zeroed: the `CopyEFB` and `CopyEFBToCacheEntry`
      pad words and the 12-byte pads in the material built-ins (were
      uninitialised).
    - Loading a state whose entry has no texture data builds an empty entry
      (the old code dereferenced an empty optional).
    - `UninitializeXFBMemory` writes the {1,254} pattern with memcpy instead
      of SSE stores (same bytes).
    - Assert source lines differ.
  - Found and not fixed (old and new alike): a failed host texture
    allocation in texture creation, EFB/XFB copies, XFB fetches or
    material passes leads to a null dereference; a failed texel upload
    during GPU decoding of one mip level makes the CPU fallback write
    through a null buffer; the RGBA8 odd-bank TMEM read can run past TMEM
    for large bank offsets; a loaded state's bound-texture unit index is
    not range-checked. The harness keeps clear of these.
  - Checked:
    - Decoder harness: random formats, palette formats, sizes and data,
      SSE2 and SSSE3 paths, old vs new: identical; mutation checks
      detected.
    - TMEM, TextureInfo/TextureConfig, HiresTextures (synthetic packs on
      disk, 800 seeds, every file, asset, resource and OSD call logged) and
      shader generator (exhaustive over uids, parameters, APIs and backend
      flags) harnesses: identical, all mutations detected.
    - Texture cache harness: the real `TextureCacheBase` against recording
      fakes for the gfx backend, textures, framebuffers, staging textures,
      the vertex manager (uniforms hashed with pads masked, texel buffers),
      shader cache, framebuffer manager, presenter, graphics mods
      (materials, skips, names), hires packs, memory, FIFO recorder,
      statistics, config and alerts. Random walks of guest RAM writes and
      clones, TMEM writes, texture unit setup, loads and binds, EFB/XFB
      copies (partial-update copies at texture pitch), XFB fetches, frame
      ends, config changes, invalidations, save/measure/load round trips,
      EFB scale and backend flag changes, injected allocation, staging,
      pipeline and texel-upload failures. Every fake call and the full cache
      state after each step are logged; assert messages have their source
      line masked. 300 seeds x 1000 steps and 120 seeds x 1500 steps: all
      identical except three seeds whose save blobs differ only in the
      order of a linked pair's ids (the pair order follows heap addresses;
      two builds of the old code differ on the same seeds the same way).
      40 mutations of the new cache code: 32 detected. Of the 8 missed,
      four cannot change behaviour here (the XFB fill loop bound, the
      `m_temp` size estimate while the 16 MiB start size is never
      exceeded, a reset of the pending palette copy that is always
      overwritten, the overlap window for textures under 2 MiB); four
      touch paths the walk rarely reaches and were checked by reading:
      the mip-reduction rounding (moves the mean difference only
      slightly), the reinterpreted entry's frame count, the
      same-texels-other-palette rule in recycling and the stitch order
      of mutually overlapping XFB pieces.
      Valgrind on the old code: clean apart from the arbitrary-mipmap
      scratch reads above. Coverage (gcov, 300 seeds x 1500 steps): all
      but 61 old lines run; the rest are allocation-failure paths, measure
      overflow, locked entries, the reinterpret-plus-palette combination
      (unreachable), plus two reachable but unhit lines (a partial update
      from a copy onto a copy, and the scaled XFB stitch).
    - Performance: decoders the same or faster for most formats (IA4 about
      4x, RGB5A3 about 2x, C4 about 3x); RGBA8 at the smallest sizes is
      about 1.1x slower per call (tens of ns), equal from 256x256 up. The
      cache does the same work in the same order; the cache harness runs
      in the same time old and new (3.1-3.4 s per 30 seeds, noise).
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`: no warnings
      in W26 files. The code avoids the MSVC /W4 patterns C4319, C4805,
      C4458, C4267, C4100 and signed/unsigned compares.
      The files that include the changed headers compile as on the base
      tree; the ones that fail, fail the same way on base for missing
      third-party headers (xxhash, implot, Qt, D3D, VMA).
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W26
      .cpp except `TextureConversionShader.cpp`, whose runs are the GLSL
      text of the compute decoders (shader output, kept as data). The
      headers keep 8-17 line runs of declarations and layout
      (`TextureInfo` accessors, `TextureDecoder.h` prototypes,
      `TextureConfig`, the `TCacheEntry` members and accessors and the
      `BackupConfig` fields).

- W19 log: rewrote all 58 W19 files as original code (11616 lines now, from
  12334): the IOS USB host layer and network devices in `Core/IOS/USB`
  (`Common`, `Host`, `LibusbDevice`, `USBScanner`, `USBV0`, `USBV4`,
  `USBV5`, `OH0/OH0`, `OH0/OH0Device`, `USB_HID/HIDv4`, `USB_HID/HIDv5`,
  `USB_KBD`, `USB_VEN/VEN`) and `Core/IOS/Network` (`Socket`, `SSL`,
  `IP/Top`, `ICMP.h`, `ICMPLin`, `MACUtils`, `NCD/Manage`,
  `NCD/WiiNetConfig`, `WD/Command`, and the WiiConnect24 code in `KD`:
  `NetKDRequest`, `NetKDTime`, `NWC24Config`, `NWC24DL`, `VFF/VFFUtil`,
  `WC24File.h`, `Mail/MailCommon.h`, `Mail/WC24Send`,
  `Mail/WC24FriendList`). Every file starts with the original-code header
  and has no Dolphin copyright or SPDX line. File names, public
  declarations, on-disc and guest structure layouts (`DLList`,
  `ConfigData`, `SendList`, `FriendList`, `WC24File`, `WC24PubkMod`, the
  USB descriptor and V0/V4/V5 request layouts, `BSSInfo`, `WD::Info`), enum
  values, savestate `DoState` order, config keys and every translatable
  string are unchanged.
  - USB: the host/scanner pair (hotplug, the first-scan wait, device
    add/remove notifications in the same order), the libusb backend
    (claiming and detaching interfaces, control/bulk/interrupt/isochronous
    submission and the same reply values and lengths), OH0 and OH0Device
    (path parsing, the same vector counts per transfer type and the
    `EACCES` on IOS versions with the new USB stack), USBV0/V4/V5 (device
    change buffers, `GETDEVPARAMS` layouts, interface and alt-setting
    handling), HIDv4/HIDv5, VEN, and the keyboard (`USB_KBD`, the same
    key tables and modifier handling).
  - Network: the socket manager (blocking and non-blocking calls,
    `poll`/`select` results, the same host error codes and their Wii
    mappings), the SSL device (the same mbedtls calls, results and reply
    buffers), IP top (all ioctl/ioctlv commands, `getaddrinfo`,
    interface and route lookups), ICMP, NCD, WD and MAC address handling.
  - WiiConnect24: the KD request device (mail check, send and download,
    the scheduler thread and its spans, async replies), the KD time
    device, and the config, download list, send list, friend list and VFF
    code; the same files are read and written with the same contents.
  - Deliberate differences (no game sees these):
    - `USBScanner`: the device list is cleared while holding its lock.
    - `LibusbDevice`: the transfer-type name used in a log line falls back
      to "Unknown" for a type outside the table (the old code threw
      `std::out_of_range`).
    - `USB_KBD`: the host key tables are defined on every platform; they
      are only used on Windows, as before.
    - `WD/Command`: the scan reply is written with `CopyToEmu` (same
      bytes); an invalid guest address is now reported by the memory
      manager instead of crashing through an unchecked pointer.
    - `WC24FriendList`: the in-memory list is value-initialised, so a list
      that cannot be read is written back as zeros instead of leftover
      memory.
    - `Socket`, `IP/Top`: the `sockaddr` and option buffers handed to the
      host are zero-initialised; the old code could copy uninitialised
      stack bytes to the guest (for example the `getsockopt` value of a
      short option).
    - `IP/Top`: the global `format_as(addrinfo)` was replaced by a local
      formatting helper (same text).
    - Some log message texts changed (log lines only, not translatable).
  - Old behaviour kept on purpose:
    - `IP/Top`: the route lookup still leaks its netlink socket when
      `setsockopt(SO_RCVTIMEO)` fails.
    - The padding byte at the end of the config and endpoint descriptors
      copied to the guest is still whatever the host structure held.
    - `GetConfigurations()[0]` is still read without checking for an empty
      list; VEN still reads the first input vector without checking that
      there is one; NCD still uses `io_vectors.at()` (throws when a vector
      is missing).
    - KD: the download task search still leaves a subtask ID set by an
      entry that was not picked in the end; the config, download list and
      send list are still written back as they are when they could not be
      read; `ASSERT(!IsDisabled())` fires in the same places.
  - Fixes after the rewrite:
    - `LibusbDevice`: the `#ifndef __APPLE__` guard around
      `libusb_detach_kernel_driver` in `ClaimAllInterfaces` had been lost
      in the rewrite and was restored (macOS must not detach kernel
      drivers).
    - `SSL`: each certificate digest carries a comment naming its file;
      the digest bytes are unchanged.
  - Warning fixes: the rewritten files have none of the old code's sign,
    narrowing and unused-parameter warnings (`USBV5`, `LibusbDevice`,
    `HIDv5`, `OH0`, `Host`, `USB_KBD`, `Socket`, `WD/Command`,
    `NetKDRequest`); conversions are explicit casts with the same values,
    and unused parameters are unnamed or `[[maybe_unused]]`.
  - Checked: old and new code side by side in harnesses built from the W19
    files and the untouched base for everything else, with fake host
    sockets, DNS, mbedtls entry points, libusb, HTTP, crypto and clock that
    record every call. Each case is one hash of reply values and delays,
    guest memory, host calls, async replies, alerts, savestate bytes and
    NAND file contents, streamed through `cmp`; all identical, and each
    old build run twice to confirm it is deterministic:
    - Sockets, SSL and IP top: 1000 cases of 2000 steps and 300 of 5000
      (a mutated `EALREADY`/`EINPROGRESS` mapping was caught in 22 of 300).
    - USB host, scanner, libusb, OH0, V0/V4/V5, HID, VEN and keyboard with
      hotplug and transfer completion: 300 cases of 3000 steps.
    - WiiConnect24 over synthetic config, download list, friend list, send
      list, VFF and key-module files (random bytes, no real keys) with a
      fake HTTP server and the scheduler thread stepped by the clock: 300
      cases of 2000 steps.
    - NCD, WD and MAC address handling with varied MAC settings and config
      files: 500 cases of 3000 steps.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`: no warnings
      in W19 files, nor from W19 headers in the dependents checked
      (`IOS`, `MainSettings`, `NetKDRequest`, `Socket`). `AMMediaboard`,
      `BBA/BuiltIn` and `NetworkWidget` fail on missing headers
      (`UnixUtil.h`, SFML, Qt) the same way on the base tree; on the
      Windows target `IP/Top` stops at `NET_IFINDEX_UNSPECIFIED`, which
      the MinGW headers lack, also on the base tree (MSVC's have it).
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W19 .cpp
      file. Remaining runs are in headers only (public declarations,
      structure layouts, enum values and names).
  - Key material: none added. The files keep the three SHA-256 digests of
    the user's SSL files (`SSL.cpp`), the mail-check HMAC key
    (`MAIL_CHECK_KEY` in `NetKDRequest.h`) and the `WC24PubkMod` layout
    (no key bytes), unchanged.

Merge note: W19 was based on ec993f7, before the keys step removed
MAIL_CHECK_KEY. On merge, KDCheckMail in the rewritten NetKDRequest.cpp takes
the key from the owner's wii_keys.txt (IOSC::LoadOwnerKey("wc24_mail_check_key",
20)) and skips the HMAC check with a warning when it is absent, as the keys
step did for the old file. No key bytes are in the tree.
