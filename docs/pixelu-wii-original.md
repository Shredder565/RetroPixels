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
| Original (RetroPixels) | 396667 | 98.6% |
| Dolphin, untouched | 0 | 0.0% |
| Dolphin, with PixelU changes | 0 | 0.0% |
| PixelU-added files (no original mark) | 0 | 0.0% |
| Third-party | 5763 | 1.4% |
| **All** | **402430** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| Core/AudioCommon | 2803 | 2803 | original |
| Core/Common (top level) | 25299 | 25299 | original |
| Core/Common/Assembler | 3757 | 3757 | original |
| Core/Common/Config | 915 | 915 | original |
| Core/Common/Crypto | 1202 | 1202 | original |
| Core/Common/Debug | 436 | 436 | original |
| Core/Common/GL | 10527 | 4855 | mixed (46% original), 5672 lines third-party |
| Core/Common/Logging | 651 | 651 | original |
| Core/Core (top level) | 23204 | 23204 | original |
| Core/Core/Boot | 2838 | 2838 | original |
| Core/Core/Config | 2398 | 2398 | original |
| Core/Core/ConfigLoaders | 1245 | 1245 | original |
| Core/Core/Debugger | 3384 | 3384 | original |
| Core/Core/DSP (top level) | 4938 | 4938 | original |
| Core/Core/DSP/Interpreter | 2883 | 2883 | original |
| Core/Core/DSP/Jit | 3625 | 3625 | original |
| Core/Core/FifoPlayer | 1943 | 1943 | original |
| Core/Core/HLE | 1429 | 1429 | original |
| Core/Core/HW (top level) | 9970 | 9970 | original |
| Core/Core/HW/DSPHLE | 6575 | 6575 | original |
| Core/Core/HW/DSPLLE | 571 | 571 | original |
| Core/Core/HW/DVD | 4591 | 4591 | original |
| Core/Core/HW/EXI | 8065 | 7974 | mixed (99% original), 91 lines third-party |
| Core/Core/HW/GCMemcard | 3145 | 3145 | original |
| Core/Core/HW/HSP | 964 | 964 | original |
| Core/Core/HW/MagCard | 996 | 996 | original |
| Core/Core/HW/SI | 3023 | 3023 | original |
| Core/Core/HW/Triforce | 2796 | 2796 | original |
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
| Core/Core/IOS/USB/Bluetooth | 4670 | 4670 | original |
| Core/Core/IOS/USB/Emulated | 4434 | 4434 | original |
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
| Core/DolphinNoGUI | 808 | 808 | original |
| Core/DolphinQt (top level) | 14786 | 14786 | original |
| Core/DolphinQt/Achievements | 1364 | 1364 | original |
| Core/DolphinQt/Config | 18036 | 18036 | original |
| Core/DolphinQt/Debugger | 13379 | 13379 | original |
| Core/DolphinQt/EmulatedUSB | 386 | 386 | original |
| Core/DolphinQt/FIFO | 1321 | 1321 | original |
| Core/DolphinQt/GameList | 2599 | 2599 | original |
| Core/DolphinQt/InfinityBase | 446 | 446 | original |
| Core/DolphinQt/LinuxCheck | 488 | 488 | original |
| Core/DolphinQt/NetPlay | 3464 | 3464 | original |
| Core/DolphinQt/QtUtils | 1703 | 1703 | original |
| Core/DolphinQt/Settings | 4485 | 4485 | original |
| Core/DolphinQt/SkylanderPortal | 1392 | 1392 | original |
| Core/DolphinQt/TAS | 2003 | 2003 | original |
| Core/DolphinTool | 1245 | 1245 | original |
| Core/InputCommon (top level) | 1981 | 1981 | original |
| Core/InputCommon/ControllerEmu | 3621 | 3621 | original |
| Core/InputCommon/ControllerInterface (top level) | 1787 | 1787 | original |
| Core/InputCommon/ControllerInterface/DInput | 1080 | 1080 | original |
| Core/InputCommon/ControllerInterface/DualShockUDPClient | 948 | 948 | original |
| Core/InputCommon/ControllerInterface/ForceFeedback | 288 | 288 | original |
| Core/InputCommon/ControllerInterface/SDL | 1229 | 1229 | original |
| Core/InputCommon/ControllerInterface/SteamDeck | 352 | 352 | original |
| Core/InputCommon/ControllerInterface/WGInput | 688 | 688 | original |
| Core/InputCommon/ControllerInterface/Wiimote | 1700 | 1700 | original |
| Core/InputCommon/ControllerInterface/Win32 | 139 | 139 | original |
| Core/InputCommon/ControllerInterface/XInput | 336 | 336 | original |
| Core/InputCommon/ControllerInterface/Xlib | 930 | 930 | original |
| Core/InputCommon/ControlReference | 2015 | 2015 | original |
| Core/InputCommon/DynamicInputTextures | 502 | 502 | original |
| Core/InputCommon/LinuxCheck | 458 | 458 | original |
| Core/UICommon | 3872 | 3872 | original |
| Core/UpdaterCommon | 817 | 817 | original |
| Core/VideoBackends/D3D | 3741 | 3741 | original |
| Core/VideoBackends/D3D12 | 5306 | 5306 | original |
| Core/VideoBackends/D3DCommon | 1139 | 1139 | original |
| Core/VideoBackends/Null | 552 | 552 | original |
| Core/VideoBackends/OGL | 5554 | 5554 | original |
| Core/VideoBackends/Software | 4785 | 4785 | original |
| Core/VideoBackends/Vulkan | 9050 | 9050 | original |
| Core/VideoCommon (top level) | 45558 | 45558 | original |
| Core/VideoCommon/Assets | 4000 | 4000 | original |
| Core/VideoCommon/GraphicsModSystem | 2446 | 2446 | original |
| Core/VideoCommon/Resources | 1870 | 1870 | original |
| PCH | 114 | 114 | original |
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
| W1 | Prune: delete code no Windows x64 build compiles (ARM64, Metal, macOS, Android, Linux/BSD) | 2293 | 2293 | original |
| W2 | Common: types, bits, containers, strings, files, threads, config, logging | 13468 | 13468 | original |
| W3 | Common: platform, memory, network, crypto, file formats | 9032 | 9032 | original |
| W4 | Common: x64 emitter, disassemblers, PowerPC assembler | 9417 | 9417 | original |
| W5 | DiscIO: blob formats and compression | 7637 | 7637 | original |
| W6 | DiscIO: volumes, filesystems, verifier, Riivolution, NAND import | 8329 | 8329 | original |
| W7 | PowerPC: CPU state, MMU, caches, analyst, tables, symbols, breakpoints, GDB stub | 10769 | 10769 | original |
| W8 | PowerPC: interpreter and cached interpreter | 5474 | 5474 | original |
| W9 | PowerPC: JIT common and Jit64 | 14792 | 14792 | original |
| W10 | HW: memory map, MMIO, CPU/PI/MI/VI/AI/DSP interfaces, timers, IPC, CoreTiming | 8543 | 8543 | original |
| W11 | HW: DVD and GameCube memory cards | 7736 | 7736 | original |
| W12 | HW: EXI devices (memory card slots, BBA, mic, modem, IPL) | 7819 | 7819 | original |
| W13 | HW: SI, pads, keyboard, GBA link, HSP, Triforce, MagCard | 9683 | 9683 | original |
| W14 | HW: Wii Remote (common, emulated, real) | 11267 | 11267 | original |
| W15 | DSP: core, interpreter, LLE glue | 8461 | 8461 | original |
| W16 | DSP: x64 JIT | 3625 | 3625 | original |
| W17 | DSP: HLE (AX, Zelda, GBA, CARD, ROM ucodes) | 6575 | 6575 | original |
| W18 | IOS: kernel, ES, FS, DI, SDIO, STM, WFS, crypto | 14838 | 14838 | original |
| W19 | IOS: network (SO, KD/WC24, NCD, WD, SSL) and USB host devices | 11630 | 11630 | original |
| W20 | IOS: Bluetooth and emulated USB devices (Skylanders, Infinity, keyboard, mic) | 9104 | 9104 | original |
| W21 | Core: boot, config, config loaders, HLE, debugger, FIFO player | 13237 | 13237 | original |
| W22 | Core: Core.cpp, System, state saves, movies, Wii root, title DB, hotkeys | 9462 | 9462 | original |
| W23 | Core: cheats, patches, NetPlay, achievements, analytics, USB utils | 12850 | 12850 | original |
| W24 | VideoCommon: GX state and command processing (BP/CP/XF, FIFO, PE, CPU cull) | 11051 | 11051 | original |
| W25 | VideoCommon: vertex loaders and vertex manager | 4192 | 4192 | original |
| W26 | VideoCommon: texture cache and decoders | 7527 | 7527 | original |
| W27 | VideoCommon: shader generators and shader managers | 8649 | 8649 | original |
| W28 | VideoCommon: renderer abstraction, framebuffers, shader cache, present, post-processing | 10177 | 10177 | original |
| W29 | VideoCommon: OSD, frame dumping, performance, assets, graphics mods, resources | 12278 | 12278 | original |
| W30 | Video backends: Software and Null | 5337 | 5337 | original |
| W31 | Video backends: OpenGL (with Common/GL) | 9637 | 9637 | original |
| W32 | Video backends: Vulkan | 9050 | 9050 | original |
| W33 | Video backends: D3D11, D3D12, D3DCommon | 10186 | 10186 | original |
| W34 | InputCommon: controller emulation, control references, dynamic input textures | 8119 | 8119 | original |
| W35 | InputCommon: ControllerInterface (DInput, XInput, WGInput, SDL, DSU, Wii Remote, Steam Deck) | 8547 | 8547 | original |
| W36 | AudioCommon, UICommon, UpdaterCommon, DolphinTool, PCH | 8851 | 8851 | original |
| W37 | DolphinQt: main window, menus, host, settings, render widget, hotkeys, Qt utils | 10924 | 10924 | original |
| W38 | DolphinQt: game list and the other top-level dialogs (cheats, memcards, GBA, convert, NAND) | 8071 | 8071 | original |
| W39 | DolphinQt: config dialogs (general, graphics, controls, tooltips, SDL hints) | 11734 | 11734 | original |
| W40 | DolphinQt: controller mapping and settings panes | 10787 | 10787 | original |
| W41 | DolphinQt: debugger | 13379 | 13379 | original |
| W42 | DolphinQt: NetPlay, TAS, FIFO, achievements, portals, emulated USB | 10864 | 10864 | original |
| - | not in any batch | 1266 | 1266 | original |
<!-- batches:end -->

After the last batch, Dolphin's files outside `Source/` are what is left:
the top-level `CMakeLists.txt`, `CMake/*.cmake`, `CMakePresets.json`,
`patches/0002`, `Data/Sys` (shaders, game INIs, themes, resources, title
databases, the Gecko code handler), `Languages/` (Dolphin's translations)
and the CMake glue in `Externals/`. Those need their own steps (an original
top-level build in `pixelu/wii`, original or dropped data files, fetching
the third-party Externals directly) before the submodule can go.

Decided 2026-10-21 (project owner, decision card): keep Dolphin's per-game
settings (Sys/GameSettings, ApprovedInis.json) and its translations
(Languages) as clearly labelled Dolphin community data, shipped unchanged and
credited in the package notice. The rewrite covers code; this data stays.

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

- W28 log: rewrote all 40 W28 files in `VideoCommon` as original code (10177
  lines now, from 11210): the renderer abstraction (`AbstractGfx`,
  `AbstractTexture`, `AbstractStagingTexture`, `AbstractFramebuffer`,
  `AbstractPipeline.h`, `AbstractShader.h`), `AsyncShaderCompiler`,
  `DriverDetails`, `EFBInterface`, `FramebufferManager`, `FreeLookCamera`,
  `GXPipelineTypes.h`, `PerfQueryBase`, `PipelineUtils`, `PostProcessing`,
  `Present`, `RenderState`, `ShaderCache`, `VideoBackendBase`,
  `VideoCommon.h`, `VideoConfig` and `Widescreen`. Every file starts with the
  original-code header and has no Dolphin copyright or SPDX line. The public
  names, signatures, enums and layouts are kept: every pipeline UID and
  render-state union bit for bit, `AbstractPipelineConfig`, `TextureConfig`
  users, the `VideoConfig` and `BackendInfo` members in their order, the
  `CONFIG_CHANGE_BIT_*` values, the `DriverDetails` enums, the
  `EFBReinterpretType` formatter names, and every public method of the
  presenter, shader cache, framebuffer manager, post-processor and
  free-look controllers. Changed in the headers, with no user outside W28:
  the protected internals of `FramebufferManager` (`EFBCacheData`,
  `EFBPokeVertex`, `PopulateEFBCache`, `IsUsingTiledEFBCache` and the other
  helpers) are replaced by private ones (`PeekCache`, `PeekTile`,
  `PokeVertex`, `FillTile`, `TileRect`, ...); the private parts of
  `ShaderCache`, `Present`, `AsyncShaderCompiler`, `AbstractGfx`,
  `Widescreen` and `FreeLookCamera` have new names (declaration order kept
  where it decides destruction order).
  - Design:
    - `ShaderCache`: compile jobs are two private templates, `StageJob<Uid>`
      and `PipelineJob<Uid>` (a pipeline job queues a fresh copy of itself
      while its stages are still compiling); disk loading goes through
      `ShaderBlobLoader` and `PipelineBlobLoader`; serialised UIDs are built
      by `ToDisk`/`FromDisk`; the stage lookups share `ReadyOrBuild` and
      `StageBuiltElseQueue`; the utility shaders and pipelines are built by
      `BuildUtilityShaders` with a `UtilityPipeline` helper; the uber driver
      bug workaround is `AdjustUberUid`.
    - `Present`: the aspect and size maths is split into `WidenSourceAspect`,
      `EffectiveAspectMode`, `FindClosestIntegerResolution`,
      `TryToSnapToXFBSize` and `PadToMultiple`; `UpdateDrawRectangle` uses
      one fit factor; the UI and post-processing calls go through small
      forwarders.
    - `PostProcessing`: option parsing and the ini round trip are file-local
      helpers (`OptionTypeFromSection`, `TargetsForKey`, `ParseComponents`,
      `OverrideComponents`, `JoinIntegers`, `JoinFloats`); the built-in
      uniform block is a struct with a table of its GLSL members, and the
      GLSL getters are generated from a table; `BlitFromTexture` has one
      upload-and-draw step used by both passes.
    - `FramebufferManager`: one `PeekCache` per colour and depth (texture,
      framebuffer, staging, copy pipeline, tiles, stale and refill flags);
      clear pipelines in an array indexed by the three enables; `Initialize`
      runs a table of build steps with the same panic texts.
    - `VideoConfig`: a `Load(out, info)` template; the watched settings are
      captured in a `WatchedSettings` object and compared by `ChangedBits`.
    - `DriverDetails`: the bug table grouped by bug, with API masks; the
      analytics names in a table.
    - `RenderState`/`PipelineUtils`: blend-factor and logic-op tables and
      helpers; `ApplyDriverBugs` split into four steps applied in the old
      order.
    - `AsyncShaderCompiler`, `FreeLookCamera`, `Widescreen`, `EFBInterface`
      and the `Abstract*` classes: new structure and names around the same
      steps (a shared Euler-angle base for the free-look cameras, a
      `Detection` state for the widescreen heuristic, a red/blue swap helper
      for EFB values, a row-copy helper for staging textures).
  - Kept as before (outputs, quirks):
    - Every backend call, in order and with the same arguments, for
      presenting, post-processing, EFB peeks, pokes, clears, resolves,
      reinterprets and savestates, shader and pipeline compiles and the
      utility pipelines.
    - `Present`: target rectangle, integer scaling, crop and aspect results
      with the same float operations (rounding and ceil/floor order); the
      suggested window size; XFB skipping and the duplicate-XFB rules; the
      present and frame-end events and their fields; the frame dump hook
      arguments; the savestate layout.
    - `ShaderCache`: UID serialisation (`FromDisk` runs before the stale
      check), the UID list file header (magic 0x44495550 and version), the
      disk cache file names and contents, a stale pipeline cache being
      deleted and reloaded with the same loader, the compile priorities and
      queue order, texture decoding shaders generated for OpenGL whatever the
      API, the order `ClearCaches` drops things in.
    - `PostProcessing`: the configuration parser (sections, CRLF handling,
      four-component limit, the bool parse for every value key), the ini
      section names and value formatting (C locale floats), the shader lists,
      the GLSL header, uniform block and vertex shader text, the uniform
      bytes, the intermediary-buffer rules, the panic texts.
    - `FramebufferManager`: `ClearEFB` clamps both axes to the framebuffer
      width; `DestroyEFB` keeps the colour resolve target; the colour resolve
      path calls `FinishedRendering` twice; the peek cache refresh goes tile
      by tile, colour before depth; peek values and caching (tile size,
      deferred invalidation, async refill); the savestate layout.
    - `VideoConfig`: every config key and its member, the side-effect order
      of `CheckForConfigChanges`, the OSD texts, the thread-count rules.
    - `AsyncShaderCompiler`: a worker waits on its condition variable before
      looking at the queue, without a predicate. A wake-up sent while no
      worker is waiting is lost, so with workers a `WaitForAsyncCompiler`
      straight after a resize can wait forever. The old code does the same
      (seen in both builds in the threaded harness runs) and the brief keeps
      the async behaviour, so this is unchanged.
    - `FreeLookCamera`: the savestate order and the load message; the orbital
      camera moves by `distance - amount`.
  - Deliberate differences:
    - `PostProcessing`: the built-in uniform block is value-initialised, so
      `stereo_views` (which nothing sets) is uploaded as zeros instead of
      indeterminate stack bytes.
    - `PostProcessing`: copying an option's values into the uniform buffer
      stops at four components. With more than four values from the ini the
      old copy overflowed its four-element union (undefined behaviour); the
      ASSERT is kept.
    - `VideoBackendBase`: the `__APPLE__` branches (the Metal backend, whose
      sources W1 removed, and Vulkan ahead of OpenGL) are gone, as with the
      other Apple paths in this tree. The Windows order is unchanged: D3D11,
      D3D12, OpenGL, Vulkan, Software, Null.
    - Conversions are explicit casts (`FrameCount`, the frame-dump frame
      number, the UI sizes, staging strides, BBox indices); unused
      parameters of empty default virtuals are unnamed.
  - Checked:
    - Old vs new, each built from its own W28 files with the untouched
      non-W28 base files on both sides, against a recording fake backend
      (every call, object id, texture generation and readback contents
      hashed; staging readbacks are a function of the source texture's
      contents). One hash per round streamed through `cmp`; verbose logs
      only for locating differences:
      - `VideoCommon.h`: `Z24ToZ16ToZ24` and `CompressZ16` over all 2^24
        values and all 8 formats, the RGBA6 and RGB565 round trips over all
        2^32 values. `DriverDetails`: every API, vendor, driver and family
        over 30 driver versions, every bug. `RenderState`: 200000 random BP
        states. `PipelineUtils`: 300000 random UIDs with random backend
        flags and driver bugs. Identical.
      - Presenter: 4 rounds of 20000 random actions (back-buffer sizes and
        surface info, window size, fit and crop helpers, VI and immediate
        swaps with random XFB rectangles, config changes, presents with and
        without info, savestate round trips with corrupted data, key and
        mouse forwarding, surface changes, failing post-processor and UI
        initialisation) under random aspect, crop, stereo, scale,
        frame-dump and XFB settings. Identical (375806 logged events).
      - Post-processing: 6 rounds of 6000 actions over random shader files
        (random configuration blocks with CRLF and junk lines) in the user
        and system folders, random ini options, load/save/set option, the
        shader lists, initialise/recompile, blits to random framebuffer
        formats, sizes and layers under random colour correction,
        resampling, stereo and API settings, the generated header and
        uniform text, the uploaded uniforms and the saved ini. Identical
        once uniform bytes 48-63 (`stereo_views`, see above) are masked.
      - Shader cache: 8 rounds of 3000 actions over 160 GX and 160 uber
        UIDs built from random BP/XF state: lookups (sync and async), uber
        lookups, EFB copy to VRAM/RAM, palette, reinterpret and decoding
        shaders, reloads with host-config changes, shader binaries and
        pipeline cache data on and off, failing compiles, the caches kept
        between rounds and truncated or corrupted before round 6. Hashes
        every call and every cache file. Identical with no worker threads
        (the deterministic order); with 1-3 workers, 5 runs per build of 4
        rounds: both finish cleanly (completion order not compared).
        Pipeline maps order UIDs by their bytes, pointers included, so the
        runs disable ASLR and put the fake vertex formats at a fixed
        address.
      - Framebuffer manager with `EFBInterface`: 10 rounds of 4000 actions
        (direct and interface peeks and pokes, flushes, cache invalidation,
        refresh and tile sizes, clears, resolves, reinterprets, savestates
        with and without EFB contents and failing deserialisation,
        recreation at random scales, MSAA, SSAA, stereo and HDR, shader
        recompiles, draws into the EFB) under random backend capabilities.
        Identical.
      - `VideoConfig` (with the real `FreeLookCamera::RefreshConfig`):
        60000 steps of random values (including junk) for all 128 GFX
        settings and the emulation speed, random adapters, AA modes, core
        counts and driver bugs, `Refresh`, `VerifyValidity`,
        `UpdateActiveConfig`, the thread counts, `CheckForConfigChanges`
        and the after-frame hook. Dumps all 109 members after each step
        and logs every side effect. Identical. Graphics mods stay off (they
        need the mod loader).
      - Free-look camera: 200000 actions (control-type switches, movement,
        rotation, FOV, speed, savestates). Widescreen manager: 8 rounds of
        100000 actions (GameCube and Wii, overrides, random projection
        statistics, config events, savestates). The `Abstract*` classes:
        format helpers for 32 formats, then 100000 actions (texture saves
        through a hashing `SavePNG`, staging reads and writes, framebuffer
        validation, and `AbstractGfx`'s own method bodies). All identical.
      - Not run in a harness: `VideoBackendBase` (its shared init and
        shutdown need every subsystem; compared line by line with the old
        file: the same creation, `Initialize` and destruction order),
        `PerfQueryBase` (one function) and the declaration-only headers.
    - Speed, interleaved runs on the same machine: `UpdateDrawRectangle`
      68.9 ns old, 68.8 ns new; a GX plus uber pipeline lookup 98-123 ns
      old (mean 111) and 103-120 ns new (mean 113) over 5 runs each,
      within the run-to-run noise; a cached EFB peek 7.5 ns old, 7.6 ns
      new. No measurable change.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`: the 18 W28
      .cpp files are clean. The 135 files outside W28 that include a W28
      header (Core, DolphinQt, UICommon, VideoBackends, VideoCommon), same
      flags: no warning from a W28 header on either target; 92 compile
      clean on Linux and 91 on Windows; the other 43/44 stop on headers
      this machine does not have (Qt, the D3D/DXGI SDK, `vk_mem_alloc.h`,
      implot, SFML, `winerror.h`), and fail the same way with the base
      tree. With the base tree, 121 of the 135 fail on each target, the
      extra ones on warnings from the old W28 headers.
      No `bool` mixed with an integer under `|` or `&`, no unary minus on
      unsigned values. MSVC was not run.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W28 .cpp
      file. Remaining runs are in headers only (declarations, the
      `VideoConfig` and `BackendInfo` members, the render-state unions and
      UID layouts).

- W20 log: rewrote all 32 W20 files (Wii Bluetooth and the emulated USB
  devices under `Core/IOS/USB`) as original code. They now have 9104
  lines (from 13973). Each starts with the RetroPixels original-code line
  and has no Dolphin copyright or SPDX header. The files are:
  - Bluetooth: BTBase, BTStub, BTEmu, BTReal, LibUSBBluetoothAdapter,
    RealtekFirmwareLoader, WiimoteDevice, WiimoteHIDAttr, hci.h and
    l2cap.h.
  - Emulated: Skylanders/Skylander, SkylanderFigure and SkylanderCrypto,
    Infinity, Microphone, WiiSpeak and LogitechMic.
  - Kept unchanged: file names, and every public declaration that other
    files use (names, signatures, namespaces, enum values, the public
    structs and the Skylander/Infinity figure APIs the Qt windows use).
    No key, certificate or firmware was added, and none was found in
    these files. The checksum and crypto format constants are byte for
    byte the same:
    - Skylanders: CRC16 (poly 0x1021, init 0xFFFF), CRC48 (poly
      0x42f0e1eba9ea3693, init 2*2*3*1103*12868356821), the sector 0
      key A expression (73*2017*560381651, left as it was), the
      HASH_CONST Activision string (0x35 bytes, kept verbatim) and the
      MIFARE access bytes (0F 0F 0F 69 for sector 0, 7F 0F 08 69 for the
      others).
    - Infinity: SHA1_CONSTANT (kept verbatim), CRC32 0xEDB88320 with
      init 0, the scramble mask 0x8E55AA1B3999E8AA, the JSF seed
      0xF1EA5EED and 23 warm-up rounds, the 0x80 response bytes, the
      trailer bytes 17 87 8E / 77 87 88, tag bytes 89 44 00 C2, date
      0D 08 12 and block bytes 01 D1 1F.
    Everything that affects emulation also stays the same:
    - every ioctl and ioctlv result, reply byte and reply delay of the
      emulated and passthrough Bluetooth devices, the HCI event order,
      and the libusb calls of passthrough (transfers, control requests,
      Realtek firmware upload, link keys);
    - the emulated device descriptors and every control, interrupt and
      isochronous reply of the Skylander portal, the Infinity base, the
      Wii Speak and the Logitech microphone, with the same timings
      (Wii Speak isochronous 2000 us, Logitech 1000 us), the 121-byte
      Logitech configuration descriptor and the Wii Speak registers;
    - the figure files read and written (Skylander and Infinity), the
      figure tables (517 Skylanders, first entry wins on duplicates;
      104 Infinity toys), the microphone capture path (gain
      10^(dB/20) in float, saturation, byte swap for the Wii Speak,
      the loudness level formula and its 128-sample window);
    - the savestate layout and order, every config key and every
      translatable string.
  - Kept quirks: the Logitech sampling rate is set little-endian but
    read back big-endian; GET_CUR volume wraps to 0 at volume 255; the
    Logitech isochronous transfer still copies its buffer onto itself;
    a window whose peak is -32768 still reports loudness 0 (the peak is
    kept as a 16-bit sample); reopening the capture stream at a new rate
    keeps the read position and buffered count; the Infinity base still
    takes no lock in HasFigureBeenAddedRemoved, PopAddedRemovedResponse
    and GetPresentFigures; the Infinity figure block index still wraps
    as a u8.
  - Deliberate differences (genuine bugs, harmless fixes, cleanups):
    - Bluetooth: queued event padding is zeroed; debug name lookups no
      longer read out of bounds; oversized ACL packets are dropped; names
      are bounded to 247 bytes; a null guard on outgoing ACL; size guards
      on passthrough event parsing and a payload clamp; the Realtek
      loader checks bounds in 64 bits and rejects patch_length < 4
      (the old code crashed); the 0xff vendor event is written with one
      copy; the sync state is read once; the unused HCICommandPayload
      template is gone; the QoS service constants are now
      QOS_BEST_EFFORT/QOS_GUARANTEED (SERVICE_GUARANTEED is a Windows
      SDK macro).
    - Skylanders: a figure file's buffer is zeroed before loading;
      RemoveSkylander ignores a slot with no figure; checksums are
      stored explicitly little-endian (same bytes on every supported
      host); ScheduleTransferCompletion gets an explicit u32 cast.
    - Infinity: the generator state starts at zero instead of
      uninitialised.
    - Microphone: a read that would run past the end of the ring buffer
      (sample rates that are not a multiple of 250 Hz, such as 44100,
      or a rate change that shrinks the ring) wraps to its start; the
      old code read outside the buffer. A zero-sized or missing ring
      returns no data instead of dividing by zero. The loudness level
      of a silent window is defined as 0 (the old code converted -inf
      or NaN to u16, which gave 0 on x86). The debug-only
      WII_SPEAK_LOG_STATS statistics are gone.
    - Wii Speak: reading the loudness register before the device is
      opened reports 0 (the old code dereferenced a null microphone).
    - Some debug log messages are worded differently. Panic messages
      and translatable strings are unchanged.
  - Checked: harnesses linked the old and the new code against the same
    untouched code outside W20 (IOS Device, USB Common, IOFile, crypto,
    mbedtls, StringUtil) with fake memory, timing, IPC, libusb, cubeb,
    config and system objects. Only synthetic data was used: no
    Nintendo keys, BIOS, firmware, DSP ROMs or games.
    - Emulated Bluetooth: 1000 cases, 10.8M events, identical
      (HCI commands, L2CAP/SDP/HID traffic, connections, savestates).
    - Bluetooth passthrough: 3000 cases, 2.45M events, identical (fake
      libusb with a deterministic transfer pool; Realtek upload, link
      keys, QoS, sync, downloads, failed submits and savestates).
    - Skylander portal: 2000 cases, 1.1M events, identical. Mutations
      were caught (counter +2 in 35/100, slot retention 37/100, CRC init
      100/100).
    - Infinity base: 2000 cases, 1.28M events, identical. Mutations were
      caught (rotate 94/100, CRC poly 100/100, pad mapping 83/100, order
      100/100).
    - Microphones (Wii Speak and Logitech): 3000 cases, 4.08M events,
      identical: control requests, register reads and writes, Audio
      Class requests, isochronous reads, synthetic capture buffers fed
      through the cubeb callback, config changes, core state changes,
      cubeb and worker failures. Mutations were caught (loudness range
      57/200, -32768 peak 35/200, gain 117/200, byte swap 56/200,
      volume offset 71/200, volume clamp 24/200, descriptor byte
      91/200, Wii Speak ready flag 98/200). The harness keeps Logitech
      rate changes to multiples of 250 Hz that keep the read position
      inside the ring, and keeps Wii Speak register reads after the
      device is opened, so the two deliberate differences above are
      not hit.
    - Compile check: every W20 .cpp file and the other .cpp files that
      include W20 headers (IOS, USBScanner, Wiimote, Movie, WiiUtils,
      BaseConfigLoader, System) build with clang on Linux and with
      `-target x86_64-w64-windows-gnu` (WiiUtils and System fail the
      same way on the base commit with mingw: pugixml and glslang
      headers are missing). Under `-Wall -Wextra -Wconversion
      -Wsign-compare -Wshadow-all -Wshorten-64-to-32` the W20 files give
      no warnings. A test file that includes every W20 header after
      windows.h, winsock2.h, qos.h and winsvc.h builds. The DolphinQt
      users (Skylander and Infinity windows, MainWindow, MenuBar,
      WiimoteControllersWidget) can't be built here because there are no
      Qt headers; they only use declarations that are unchanged.
    - Verbatim audit (`tools/verbatim-audit.py wii 8`): no function body
      in W20 .cpp files shares 8 or more lines with the old code. The
      remaining runs are declarations in headers (class and struct
      layouts, enums and member lists).
  - Not rewritten: nothing. Every W20 file is original code.

- W23 log: rewrote all 34 W23 files in `Core/Core`: cheats, patches, cheat
  search, NetPlay, achievements, analytics, USB utilities and network
  capture (12850 lines now, from 14044). Every rewritten file starts with
  the original-code header and has no Dolphin copyright or SPDX line. File
  names, public declarations and layouts, config keys, ini/file formats
  (AR/Gecko/patch ini sections, Gecko codelist and codehandler layout, the
  pcap and SSL dump files), the savestate layout, every memory write a cheat
  or patch makes and when it makes it, cheat search results, NetPlay packet
  bytes and their order, sync and desync decisions, every rcheevos call and
  its order, hardcore and approved-code decisions, analytics report
  contents, Windows behaviour and all translatable strings are unchanged.
  - Cheats and patches: ARDecrypt (AR alphabet, DES and verification line),
    ActionReplay (the AR simulator: every opcode, conditional and
    fill-and-slide path), GeckoCode (codehandler install, codelist layout,
    hook patching), GeckoCodeConfig (ini load/save, the online code list
    download format), PatchEngine (ini patches, speed hacks, the patch and
    on-frame application order), WC24PatchEngine, CheatSearch,
    CheatGeneration and CheatCodes.h.
  - NetPlay: NetPlayProto.h (message ids, enums and constants), NetPlayCommon
    (compressed buffers, files and folders, the folder packet format, the
    external IP lookup), NetPlayClient (the message table, pad/Wii Remote
    buffers, golf mode, chunked data, save/code/GBA sync, desync checks,
    MD5 digests) and NetPlayServer (connection and kick handling, pad and
    Wii Remote mapping, the start sequence, save/code/SD/GBA sync, chunked
    data thread, traversal, UPnP and the session index).
  - AchievementManager: init/login/logout/shutdown, game load and disc
    changes through the custom hash reader, per-frame and idle work,
    hardcore mode, approved-code filtering, badges and their cache,
    leaderboards, client events, HTTP requests, memory reads, DoState and
    the RAIntegration (Windows) block.
  - DolphinAnalytics (the same report keys, values and order; quirk names
    now come from a switch over GameQuirk), USBUtils (the known-device table
    is listed per vendor; same names), LibusbUtils and NetworkCaptureLogger.
  - Deliberate differences (malformed input, undefined behaviour or
    internal only; nothing a game or a well-formed peer sees):
    - ARDecrypt: a character outside the AR alphabet decodes as 0 (the old
      code did pointer arithmetic on strchr's nullptr).
    - CheatSearch: an invalid CompareType fails the search with a
      DEBUG_ASSERT (the old code threw std::bad_function_call).
    - GeckoCodeConfig: no longer special-cases being handed the same ini
      object as both the global and the local ini (no caller does).
    - PatchEngine: LoadPatches no longer loads the merged game ini that it
      never used (it only cost file reads).
    - NetPlay packet reads into locals start zeroed, so a truncated packet
      reads 0/false instead of indeterminate values (client: save counts,
      Mii and "exists" flags, has-redirect, raw/GCI card slot, size and
      count, GBA slot, SerializedWiimoteState; server: the TimeBase frame
      and digest progress). The Wii save Header and BkHeader buffers are
      value-initialised.
    - NetPlayCommon: a folder level whose entry count is missing is a
      failed decompression (the old code used the count uninitialised).
    - NetPlayClient: m_wait_on_input and m_wait_on_input_received start
      false (they were uninitialised until the first game start or golf
      message, so an early GetNetPads could return false at random).
    - NetPlayClient: a reassembled chunked message is erased by id after it
      has been handled (the old code erased an iterator that a nested
      ChunkedDataEnd/Abort for the same id inside the reassembled data had
      already invalidated, and crashed).
    - NetPlayServer: SyncCodes asks for the game's ini file names once
      instead of twice (a pure function).
    - Common::Flag members became std::atomic<bool> (same semantics).
  - Checked: old and new code side by side on synthetic inputs only (no
    games, servers or accounts), using the base versions of all non-W23
    files on both sides, comparing hashes of the output streams:
    - Cheats: 2000 seeds of AR codes (encrypted and plain), Gecko codes,
      patches, WC24 patches and cheat searches over fake guest memory:
      every memory write in order, search results, ini output and alerts.
    - USB, capture, analytics: 300 seeds of fake libusb/udev device lists,
      VID:PID parsing, every product id up to 0x3600 for each known vendor,
      pcap/SSL capture output and analytics reports.
    - NetPlay, over a fake in-process ENet with per-thread send logs:
      400 seeds of buffer/file/folder round trips and damaged folder
      packets; 300 client seeds of random server messages (with the
      GUI/CPU-thread calls) and 200 server seeds of random peers and client
      messages: every packet's bytes, channel and peer, UI callbacks,
      config and file writes. Cases whose outputs differed were rerun one
      at a time and then matched, except where noted: client case 127 (the
      old code crashes on a nested chunk end, see above) and server case
      135 (a chunk-thread versus server-thread interleaving that also
      varies between runs of the old code; same packets in both). The
      client harness never removes the local player (that leaves
      m_local_player dangling in both versions), the server harness only
      starts a game once the host's own game is selected (both versions
      dereference a missing game), and the old side was compiled with
      m_wait_on_input initialised (see above).
    - AchievementManager, against a recording fake rcheevos (every rc_*
      call and argument, stored callbacks answered by the driver): 600
      seeds with a valid approved-code list, 200 with a list whose hash
      does not match and 200 without the list, of random login/load/disc/
      frame/idle/pause/DoState/code-filter/event/HTTP/memory-read
      sequences: identical rc call logs, OSD messages, update events,
      config writes, badge cache files and savestate bytes. One difference
      found this way was fixed (DoState now asks the PointerWrap mode at
      each step, as before, so a short state buffer still stops the load).
  - Compile check: the W23 .cpp files are clean with clang `-Wall -Wextra
    -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32`, on Linux
    and for x86_64-w64-windows-gnu (AchievementManager with
    USE_RETRO_ACHIEVEMENTS, and with RC_CLIENT_SUPPORTS_RAINTEGRATION on
    Windows, against stand-in rcheevos headers since rcheevos is not in the
    tree here). NetworkCaptureLogger now converts sockets to SOCKET for the
    Windows socket calls (the base tree had -Wsign-conversion there).
    Lines over 100 columns were wrapped. Element and value types match in
    every find/count/contains (MSVC C4389). The other non-Qt files that
    include AchievementManager.h compile as on the base tree, with and
    without USE_RETRO_ACHIEVEMENTS (OnScreenUI and VerifyCommand fail on
    both trees for missing implot/OptionParser headers; DolphinQt was not
    built).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run of 8 or more lines
    is left in any W23 .cpp file. The runs left in W23 headers are public
    declarations (NetPlay message ids and enums, class member lists, the
    AchievementManager interface).
  - Key material: the GameCube Action Replay DES key in ARDecrypt.cpp
    (`gensubtable`: 34 1C 84 9E FD A4 B6 7B) was already in the base tree
    and is kept byte for byte. `ACHIEVEMENT_APPROVED_LIST_HASH` is the SHA-1
    of Data/Sys/ApprovedInis.json, generated by CMake into
    AchievementApprovedHash.h; it is a file checksum, not a key. No keys,
    certificates, firmware or game data were added.
  - Not rewritten: nothing.

- W31 log: rewrote all 33 W31 files as original code: the OpenGL backend
  (`VideoBackends/OGL`: `OGLBoundingBox`, `OGLConfig`, `OGLGfx`, `OGLMain.cpp`,
  `OGLNativeVertexFormat.cpp`, `OGLPerfQuery`, `OGLPipeline`, `OGLShader`,
  `OGLStreamBuffer`, `OGLTexture`, `OGLVertexManager`, `ProgramShaderCache`,
  `SamplerCache`, `VideoBackend.h`) and `Common/GL` (`GLContext`, `GLUtil`,
  `GLExtensions`, `GLInterface/WGL`). They have 8686 lines now (from 9166).
  Every file starts with the original-code header and has no Dolphin
  copyright or SPDX line. The OGL `CMakeLists.txt` is not a W31 file and is
  unchanged. The public names, signatures, enums and layouts are kept:
  `OGL::VideoConfig` and its enums in their order, the `GLContext` interface,
  the `GLExtensions` entry points and every `dol*` pointer, the
  `StreamBuffer`, `PerfQuery`, `SamplerCache` and `ProgramShaderCache` public
  methods, the `GLVertexFormat`, `OGLTexture`, `OGLFramebuffer`,
  `OGLStagingTexture`, `OGLShader` and `OGLPipeline` interfaces. Changed in
  the headers, with no user outside W31: private members and helpers have
  new names (`OGLGfx::m_window_framebuffer`, the `SamplerCache` maps, the
  `ProgramShaderCache` statics, the `GLContext` surface fields), and the
  `PerfQuery` base now holds the shared query ring (see below).
  - Design:
    - `GLExtensions`: the entry-point table is one `ENTRY_POINTS` array built
      with `RP_GL`/`RP_GL_AS` and named requirement constants; the version
      feature lists are per-version tables; `Supports` is a plain lookup.
    - `OGLConfig`: driver identification is a `DriverId` filled by one
      reader per vendor (`ReadQualcommVersion`, `ReadMaliVersion`,
      `ReadIntelDetails`, `ReadNvidiaVersion`, `ReadImgTecVersion`);
      `PopulateConfig` runs `CheckDesktopRequirements`,
      `ReadExtensionFeatures`, `ApplyESProfile`/`ApplyDesktopProfile`,
      `DetectAAModes` (one `QuerySampleCounts` for colour and depth),
      `DetectSubgroupSupport` and `ReportMissingFeatures` in the old order.
    - `OGLStreamBuffer`: one class per technique (`OrphaningStream`,
      `FencedMappingStream`, `PersistentStream`, `PinnedStream`,
      `SubDataStream`, `RespecifyStream`) over shared fence helpers
      (`FenceChunks`, `WaitChunks`, `WaitForSpace`); `Create` picks them with
      the same driver and bug rules in the same order.
    - `OGLGfx`, `OGLPipeline`, `OGLShader`, `OGLTexture`: GL enum mappings
      are tables (primitives, shader stages, blend factors, logic ops,
      compare functions, texture formats); state application is split into
      `SetRasterization`/`SetDepth`/`SetBlending` with the same caching.
    - `ProgramShaderCache`: the GLSL header is built from a line table and an
      alias table; program lookup, linking and binary loading are
      `FindAndReference`, `LinkFromSource` and `LoadFromBinary`; the worker
      context copies the main context state through `MirrorMainContextState`.
    - `OGLPerfQuery`: the ring of pending queries lives in the base class,
      with four hooks implemented by the GL and NV variants.
    - `WGL`, `GLContext`, `GLUtil`, `SamplerCache`, `OGLBoundingBox`,
      `OGLMain`: new structure around the same steps (pixel format,
      context-attribute and pbuffer helpers; one compile helper for both
      shader stages; byte offset and size helpers for the SSBO).
  - Kept as before (outputs, quirks):
    - Every GL and WGL call, in order and with its arguments, and every
      entry-point lookup (the same 939 table rows, names, suffixes and
      requirement strings, in order).
    - The GLSL header and shader texts, bounding-box and perf-query results
      (the u64 product divided by the EFB area; the GL variant truncates to
      u32 and divides by the MSAA sample count above 1), EFB peeks and
      pokes, the stream-buffer technique per driver and bug, the context
      creation choices, backend info, config keys, driver-bug workarounds
      and the translatable and log texts.
    - Quirks: the static PFD stereo flag; the `BufferSubData` stream size;
      the depth AA-mode query using the colour format;
      `ARB_get_texture_sub_image` looked up without the `GL_` prefix; LIMA
      reported as ARM; staging textures unmapped through the pack target in
      the destructor; `CopyToTexture` ignoring `dst_level`; the link-failure
      dump writing vertex, geometry, pixel; "Dolphin Version: " in the
      bad-shader dumps; info logs keeping their reported length; the 18
      robust `glGetn*` queries left out of the entry-point table.
  - Deliberate differences:
    - Values the old code left uninitialised are zero: `InitVersion` when the
      integer queries fail, the Intel market name, the IMGTEC branch and
      change numbers when the version string does not fully match, the
      `GL_SAMPLES` query, the pbuffer pixel format, the WGL client rectangle
      (on failure the old size is kept), the shader compile and link status
      (`GL_FALSE` before the query).
    - Null extension strings are skipped; `isspace` gets an unsigned char.
    - `GLExtensions::Supports` no longer inserts unknown names into the map.
    - The stream-buffer fence wait is clamped to the last chunk, and the
      sample-count reserve is guarded against a negative count.
    - The non-Windows context branches and the dlsym fallback are gone
      (Windows-only tree).
    - `SamplerCache`: the invalid wrap value 3 maps to clamp-to-edge (the
      old code read past a three-element array).
    - `PerfQuery`: the base class is abstract; the old base delegated to a
      member that was never set.
    - `ProgramShaderCache::WorkerThreadInitMainThread` sets its out
      parameter to null on failure (the caller already starts from null).
    - The texture format name lookup is a table search; it runs when a
      texture is created, not per draw.
    - Conversions are explicit casts (stream-buffer chunk indices, the
      back-buffer size, vertex strides).
  - Checked:
    - Old vs new, each built from its own W31 files with the untouched
      non-W31 base files on both sides (VideoCommon renderer abstraction,
      vertex manager base, framebuffer manager, presenter, driver details,
      shader compiler, graphics settings), against a recording fake GL: a
      table of 901 entry points that logs every call with its arguments and
      hashes of the data it reads (buffer contents, uploads, indices and
      vertices of draws, shader sources), returns scripted values (object
      names, statuses, info logs, program binaries, fences, query results,
      readbacks as a function of the source) and keeps buffer, VAO, texture
      buffer and SSBO state. Each case draws a random scenario: 17 vendor
      and driver flavours, GL or GLES, random versions and shading-language
      versions, a random subset of 144 extensions, random limits and sample
      counts, random video settings. It then runs `GLExtensions::Init`
      (with all 171 `Supports` answers and the loaded-pointer map logged),
      `FillBackendInfo` (backend info, `OGL::VideoConfig` and video config
      dumped member by member, the driver-bug table before and after the
      `OGLGfx` constructor), the backend start-up and 80 random actions of
      40 kinds (textures, uploads, copies, resolves, staging reads and
      writes, framebuffers, scissor and viewport, textures and samplers,
      compute images, shaders through the real shader includer, vertex
      formats, pipelines with cache blobs, GX batches through the vertex
      manager with uniform uploads, utility draws, texel buffers, region
      clears, surface changes and present, flush and stereo buffer
      selection, compute dispatch, perf queries, bounding box, stream
      buffers of every technique), then teardown. One hash per case,
      streamed through `diff`; verbose logs only for locating differences.
      3000 cases: identical, with returned values and GL call logs equal.
      26 cases crash at the same call on both builds: GLES 3.1 without
      `GL_OES_texture_storage_multisample_2d_array` sends a multisampled 2D
      array texture to the desktop-only `glTexImage3DMultisample` (25), and a
      desktop 3.0 context with `ARB_texture_buffer_object` binds a texel
      buffer through the 3.1 `glTexBuffer` (1). Both are base behaviour and
      are unchanged.
    - The harness found and fixed two differences in the rewrite before the
      final runs: the 18 `glGetn*` rows commented out in the base table were
      active, and two unsupported-texture-type checks raised `ASSERT_MSG`
      with new texts instead of `ASSERT(false)`.
    - Not compared: assertion locations and quoted expressions (they name
      the source file and line); two `ASSERT_MSG` and two log calls are now
      one parameterised call each with the same output text (seen equal in
      the harness). `WGL.cpp` and `GLContext::Create` need a real Windows
      context and were not driven; they were reviewed against the old files
      for the same calls, attributes and fallback order, and compile clean
      for the Windows target. `MirrorMainContextState` and the
      worker-thread compile path were not driven (no shared contexts in the
      fake).
    - Speed, interleaved runs on the same machine with the fake GL in a
      no-logging mode: 16 cases, each 1000 rounds of up to 8 pipelines with a
      GX batch, uniform upload, pipeline switch, 8 samplers and textures,
      scissor, viewport and an indexed draw. Minimum of 5 runs per case:
      1287 ms old, 1276 ms new in total; per-case results differ in both
      directions within the run-to-run noise. No measurable change in draw
      submission or state changes.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`, against the
      untouched tree: the 17 W31 .cpp files and their outside users
      (`VideoBackendBase.cpp`, `SWOGLWindow.cpp`, `SWGfx.cpp`) show no
      warning from W31 code. Remaining messages are outside W31 and the same
      on the base tree: `MsgHandler.h` (unused `format`),
      `WorkQueueThread.h` (shadow), `FrameDumpFFMpeg.h` (unused parameters),
      the missing `glslang/Public/ShaderLang.h` for `OGLShader.cpp` and
      `VideoBackendBase.cpp` on Windows, and `windows.h` for `WGL.cpp` on
      Linux. No `bool` mixed with an integer under `|` or `&`, no unary minus
      on unsigned values, no `~` widened, no signed/unsigned compares.
      Fixes made for these checks (casts only): stream-buffer fence index
      and chunk shift, `SetBackbuffer` arguments, the vertex stride; a local
      named `near` renamed (a Windows macro); an initializer list of GL enums
      made a `std::array<GLenum, 3>`. MSVC was not run.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W31 .cpp
      file. Remaining runs are in headers only (declarations: `OGLGfx.h`,
      `OGLConfig.h`, `OGLVertexManager.h`, `OGLTexture.h`,
      `ProgramShaderCache.h`, `WGL.h`, `SamplerCache.h`).

- W22 log: rewrote all 34 W22 files in `Core/Core`, the core session and its
  helpers (9463 lines now, from 10095): BootManager, CPUThreadConfigCallback,
  CommonTitles.h, ConfigManager, Core, FreeLookManager, Host.h,
  HotkeyManager, MachineContext.h, MemTools, Movie, State, SyncIdentifier.h,
  SysConf, System, TimePlayed, TitleDatabase, WiiRoot and WiiUtils. Every
  rewritten file starts with the original-code header and has no Dolphin
  copyright or SPDX line. File names, public declarations and layouts,
  config keys (section, system, name and default), paths, the SYSCONF, DTM,
  savestate and WAD/NAND bytes, the order of every callback and host call
  in Core start, stop, pause and frame stepping, and all translatable
  strings are unchanged. The window title stays
  "PixelU Wii (Dolphin {GetScmDescStr()}) | settings".
  - `Core`/`System`: session start and stop sequencing (EmuThread and
    CPU/GPU thread start-up and teardown order, dual and single core, FIFO
    player, GDB stub, fastmem/EMM, SD card sync), pause, frame step, host
    jobs, CPU-thread guards and the title string; System keeps its parts in
    the same construction and destruction order.
  - `BootManager`/`ConfigManager`: BootCore (config layers, NetPlay and
    movie settings, Wii root, SYSCONF hand-over) and RestoreConfig;
    running game metadata, title hooks, boot-parameter metadata, language
    and region rules, GameTDB region codes and the game INI merges.
  - `Movie`: DTM header and input layout, recording, playback, the input
    and Wii Remote readers, desync checks and their messages, and the
    savestate hooks.
  - `State`: savestate file layout (header, LZ4/LZO blocks, version
    strings), the save worker, the undo slots and the last-saved list.
  - `SysConf`: SYSCONF parsing and writing, entry types and defaults.
  - `WiiRoot`/`WiiUtils`: temporary and NetPlay Wii roots (setup,
    save seeding and redirects, teardown and copy-back), WAD install and
    uninstall, the online and disc system updaters (same SOAP request bytes),
    NAND check and repair, and the backup TMD lookup. Both still use the
    owner's keys file exactly as before.
  - `HotkeyManager`/`FreeLookManager`: hotkey groups and default mappings,
    pressed/held tracking, the config sections, and the free look camera
    controls and background input handling.
  - `MemTools`/`MachineContext`: the SIGSEGV handler (with its own signal
    stack, falling back to the previous handler) and the Windows vectored
    exception handler.
  - `TimePlayed`, `TitleDatabase`, `CPUThreadConfigCallback`,
    `CommonTitles.h`, `Host.h`, `SyncIdentifier.h`.
  - Deliberate differences (no change for a game, a DTM or a save):
    - MachineContext and MemTools drop the macOS (Mach exception port),
      BSD and ARM branches, and FreeLookManager drops the Apple/Android
      branches; PixelU-Wii builds only for x86-64 Linux and Windows.
    - Core: the GPU thread owns the start-up promise it fulfils.
    - SConfig's title ID, revision and memory-size fields start at 0
      (they were uninitialised until the first metadata reset).
    - State: an LZO block whose output would overrun the buffer is
      rejected; `LoadLastSaved` refuses an index equal to the list size
      (the old code read past the end), and keeps the old index rule
      otherwise. The 16-bit LZO paths are gone.
    - WiiUtils: the SOAP request is built differently (same bytes);
      `GetDeviceRegion` returns "EUR" for a System Menu region with no
      code, where the old map lookup threw.
    - Movie: each DSP ROM is checked for in the user directory and opened
      before the next one (the old code checked both first); same hashes.
    - Assert expression texts differ.
  - Bugs found by the old-vs-new harnesses and fixed before the end:
    Movie showed the controller hint in the opposite case, WiiRoot dropped
    the temporary SYSCONF before its saves were seeded, and the NAND check
    skipped the ticket lookup for disc titles.
  - Checked: old and new code side by side on synthetic inputs only (no
    game data), using the base versions of all non-W22 files on both
    sides, comparing per-case hashes of the traced output:
    - SysConf: 2000 seeds of random, truncated and damaged SYSCONF images
      through load, edits and save.
    - TitleDatabase and TimePlayed: 300 seeds of name files and INIs.
    - HotkeyManager and FreeLookManager: 500 seeds of mappings, input
      sequences and config round trips.
    - Movie: 1000 seeds of DTM recording, playback, header and input
      checks.
    - State: 1000 seeds of saves, loads, undo slots and damaged files.
    - WiiRoot: 1000 seeds of root setup and teardown on fake NANDs.
    - WiiUtils: 1000 seeds of WAD installs, updates (fake HTTP and disc),
      NAND checks and repairs.
    - Core: 1000 seeds of session start/stop/pause, frame step and host
      jobs with about 120 fakes.
    - BootManager: 1000 seeds of BootCore and RestoreConfig.
    - ConfigManager: 1000 seeds of metadata, languages, region codes,
      game INIs and boot metadata.
    - MemTools: 300 forked cases of claimed and unclaimed faults, with and
      without an earlier handler, and install/uninstall cycles.
    - The harnesses skip inputs that are undefined behaviour in the old
      and new code alike: a DTM shorter than its 256-byte header, corrupt
      LZO data, an LZO block bigger than its buffer, a huge uncompressed
      state size, and the undo buffer after a failed capture.
  - Compile check: the W22 .cpp files are clean with clang `-Wall -Wextra
    -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32`, on Linux
    and for x86_64-w64-windows-gnu. The 223 other non-Qt files that include
    W22 headers (Core, IOS, HW, PowerPC, NetPlay, VideoCommon, the video
    backends, AudioCommon, InputCommon, UICommon, DiscIO and DolphinTool)
    compile or fail file for file exactly as on the base tree, on both
    targets (51 failures over 27 files, from headers that are missing here,
    such as DirectX, FFmpeg, mGBA and some Windows networking headers), with
    no warning located in a W22 file. DolphinQt was not built (no Qt
    headers here).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run of 8 or more lines
    is left in any W22 .cpp file. The runs left in W22 headers are public
    declarations (the hotkey enum, System and Movie member lists, Host and
    Core function lists, SysConf and WiiUtils enums).
  - Not rewritten: nothing. No key, IV, token or credential bytes were
    found in the W22 files; no keys, certificates, firmware or game data
    were added.

- W29 log: rewrote all 95 W29 files in `VideoCommon` as original code (12278
  lines now, from 13335): the on-screen display and ImGui UI
  (`OnScreenDisplay`, `OnScreenUI`, `NetPlayChatUI`, `NetPlayGolfUI`, the
  overlay font `sfont.inc`), `PerformanceMetrics` and `PerformanceTracker`,
  `FrameDumper` and `FrameDumpFFMpeg`, the custom asset layer (`Assets/`:
  asset cache, loader, `CustomTextureData` with DDS and PNG loading,
  texture, shader, material and mesh assets, `TextureSamplerValue`,
  `DirectFilesystemAssetLibrary`), the graphics mod system
  (`GraphicsModSystem/`: the mod, group, target, feature and asset config
  parsers, `GraphicsModManager`, the action factory and actions, `FBInfo`,
  `CustomPipeline`, `CustomShaderCache`) and the custom resources
  (`Resources/`: `Resource`, `TexturePool`, `InvalidTextures`, the texture,
  shader and material resources, `CustomResourceManager`). Every file starts
  with the original-code header and has no Dolphin copyright or SPDX line.
  The public names, signatures, enums and layouts are kept: the OSD message
  types, colours, durations and the `OSD::Message` API, the
  `PerformanceMetrics` and `FrameDumper` public methods, every asset and
  material data struct (`MaterialData`, `RasterSurfaceShaderData`,
  `TextureAndSamplerData`, `MeshData`, `CustomTextureData`), the shader
  property and sampler variants, the `GraphicsTargetConfig` variant and
  every target struct, the `GraphicsModActionData` payloads, `FBInfo`, the
  resource `TaskComplete`/`State` enums and the resource manager's Get*
  methods. Changed in the headers, with no user outside W29: private
  members of `FrameDumper` (the worker and capture helpers are `WorkerLoop`,
  `BeginDump`, `WriteFrame`, `SubmitFrame`, `EnsureScaleTarget`,
  `EnsureReadback`; the unused screenshot-completed event is gone),
  `GraphicsModManager` (the decorated action is a file-local
  `ModGatedAction`), `GraphicsModGroupConfig`, `CustomAssetCache`,
  `CustomResourceManager` and the resources (declaration order kept where it
  decides destruction order).
  - Design:
    - `OnScreenDisplay`: messages live on one `Board` (per-type queues and
      the obscured margins); each message is drawn by `EmitNotice`, its
      expiry decided by `IsFinished`, icons uploaded by `UploadIcon`.
    - `OnScreenUI`: setup split into `CreateUiContexts`, `FindOsdFont`,
      `UiTextureConfig` and `LoadRgba`; the movie window and debug text are
      built by small writers (`WriteMovieStatus`, `DrawWhen`); the texture
      updates (atlas and dynamic textures) go through
      `UpdateImguiTexture`/`CreateAtlasTexture`.
    - `PerformanceTracker`/`PerformanceMetrics`: the sample window is kept
      by `AddNewest`/`DropOldest`/`TakeInterval`; the overlay layout is one
      `OverlayLayout` struct; the graph grid rates are a table.
    - `FrameDumpFFMpeg`: one `FrameDumpContext`; codec and pixel-format
      choice, segment naming and libav log forwarding are file-local helpers;
      a segment is opened by `OpenSegment`/`CreateSegmentFile` and closed by
      `CloseSegment`, packets drained by `DrainEncoder`.
    - `CustomTextureData`: DDS header parsing returns a `DdsLayout`; level
      reads, the four-byte fixups (`FixFourByteTexels`) and the BGR widening
      (`ExpandBGRTexels`) are separate steps; PNG and DDS share one level
      builder.
    - `MeshAsset`: glTF reading is split into node, primitive, index and
      attribute readers; the dolmesh format is read through a bounds-checked
      `ByteReader`.
    - `GraphicsModManager`: two passes (collect targets per group and
      register assets with `ResolveAssetFiles`; then one action per feature
      attached to its group's targets); the lookups share `ActionsFor`.
    - `CustomShaderCache`: the four caches are one `Cache` template with
      pending/ready holders; the pipeline and pixel shader jobs are small
      work-item classes (a pipeline job queues a fresh copy of itself while
      its pixel shader is still compiling).
    - Resources: one `Resource` state machine drives every resource
      (primary data, dependencies, processing); the material resource builds
      its texture references and shader key in separate steps.
    - `sfont.inc`: a newly drawn 9x10 font with the same character map,
      glyph indices and per-glyph widths.
  - Kept as before (outputs, quirks):
    - Every backend call, in order and with the same arguments, for the UI
      (textures, pipelines, draws, scissors, uniform uploads), the OSD
      icons (released newest first on `ClearMessages`), frame dumping
      (staging readbacks, scale targets), the resources and the custom
      shader cache.
    - Every FFmpeg call in order with the same arguments (codec and format
      selection, options, `avcodec_*`/`avformat_*`/`sws_*` calls, packet
      timing, segment restarts on resolution change), the dump and
      screenshot file names, the savestate layout, the OSD and log texts.
    - OSD text, colours, timing, fade and layout; the performance numbers
      (FPS, VPS, speed, frame times, the same sample windows and float
      operations) and the overlay layout.
    - Asset load and reload behaviour: the cache's priority queues, memory
      accounting and unload order, the loader's two threads and its
      scheduling, last-loaded times, dirty marks, load sizes and every log
      message. Graphics mod parse results: the same JSON accepted and
      rejected with the same messages, profile round trips, weight sort
      (not stable), the directory scan stopping at the first mod the
      profile lists, `GetXFBActions` answering from the EFB table.
    - Resource manager: one cache entry per asset id whatever the asset
      type, material chains followed without a cycle check (a cycle recurses
      without end in both versions), the generated GLSL text.
  - Deliberate differences:
    - `CustomTextureData` (DDS): the four-byte fixups stop at the end of the
      level's data, and the BGR widening reads missing source bytes as zero.
      The old code read and wrote past the buffer for files whose pitch flag
      or row length promises more data than the file holds (valgrind errors
      on 37 of 1500 generated DDS files; new: none). Six of those files
      decode differently (the old output was undefined) and four crashed
      the old code.
    - `PurgeInvalidMipsFromTextureData` skips a slice with no levels (the
      old code read `m_levels[0]` of an empty slice).
    - `MeshData::FromDolphinMesh` checks every read against the input size
      and fails on truncated data (the old code read past the end).
    - `DirectFilesystemAssetLibrary::LoadMesh` reads the file into a sized
      buffer. The old code reserved capacity, read into it and parsed the
      still-empty vector, so every dolmesh load read past the vector (a
      garbage chunk count, usually ending in a `length_error` abort).
    - `FrameDumper`: the screenshot-completed event, which was set but never
      waited on, is gone.
    - `sfont.inc`: redrawn; every glyph now has all ten rows.
  - Checked:
    - Old vs new, each built from its own W29 files with the untouched
      non-W29 base files on both sides, against a recording fake backend
      (every call and object id), a recording fake libav and a fake clock;
      random sequences with fixed seeds and synthetic files. One hash per
      step streamed through `cmp`; verbose logs only for locating
      differences:
      - Assets: 3000 mutated JSON documents through every parser and the
        mod config parsers, 600 PNG and 1500 DDS files, 400 glTF files,
        dolmesh round trips, `PurgeInvalidMips`/validation, and 800
        generated asset libraries loaded through
        `DirectFilesystemAssetLibrary`. Identical, except the DDS and mesh
        cases above: DDS 1490 identical, 6 different and 4 old-only
        crashes (all on the over-read files); libraries 650 identical, 150
        abort only in the old code (all in the old `LoadMesh`).
      - OSD with the NetPlay chat and golf UIs and the performance
        overlay: 300 seeds x 600 frames of random message and timing
        sequences (each frame's ImGui draw data, backend calls and log
        hashed). Identical, including the render-time log files.
      - `OnScreenUI` with `PerformanceMetrics`/`PerformanceTracker`: 300
        seeds x 200 frames (initialisation with injected backend failures,
        input, config changes, the movie, achievement and NetPlay overlays,
        scale and pipeline changes). Identical.
      - Frame dumping: 400 seeds x 400 steps of capture, flush, screenshot
        and config sequences with libav failures and delays injected (4616
        output files hashed); without FFmpeg, 200 x 300. Identical.
      - Custom resources with the real asset cache, loader and filesystem
        library: 300 generated asset worlds x 200 steps (texture, shader
        and material requests by kind, file edits, dirty marks, host
        config changes, frame ends, resets, reinitialisation), workers run
        under a deterministic pthread-level scheduler. Identical.
      - Graphics mods: 300 generated mod trees (user and system folders,
        game-id folders and marker files, profiles) x 150 steps
        (`Initialize`, group `Load`/`Save`/copy/`GetMod`, the manager's
        `Load`, every `Get*Actions` query with each action called on live
        and null payloads, frame ends, mod and profile edits). Identical.
      - `CustomShaderCache`: 300 seeds x 150 steps with GX and uber UIDs
        built from random BP/XF state (specialised and uber requests,
        retrieval, frame ends, host config changes, reloads, rebuilds,
        failing shaders and pipelines), compilers inline; 150 x 150 with
        worker threads under the scheduler (no reloads there: `Reload`
        spins until the workers drain, which the scheduler does not allow).
        Identical.
      - Cases left out of the generated inputs because both versions fail
        on them: a staging-texture failure while a frame capture is pending
        (both crash), a UI texture failure after initialisation and
        destroying a UI whose `Initialize` failed (both assert), one asset
        id requested as two asset types (both confuse the types), cyclic
        material chains (both recurse without end), truncated framebuffer
        target names (both throw from the target parser; the assets harness
        covers it), and generator asserts from random BP state (shared
        code; answered "continue").
    - Speed: callgrind instruction counts over the harness runs, old vs new:
      `OnScreenUI::Finalize`, `DrawImGui`, `DrawDebugText`,
      `BeginImGuiFrame`, `OSD::DrawMessages` and
      `PerformanceMetrics::DrawImGuiStats` within 0.1% (new slightly lower);
      `FrameDumper::DumpCurrentFrame` +0.03%, the dump thread -0.05%,
      `FFMpegFrameDump::AddFrame` -0.1%; `CustomShaderCache::GetPipelineAsync`
      -0.02%. The seven `GraphicsModManager` lookups per draw: 195-216
      cycles old, 160-187 new over 3 interleaved runs. No slowdown.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`: the 43 W29
      .cpp files are clean. The 32 files outside W29 that include a W29
      header, same flags: no warning from a W29 header on either target; 20
      compile clean on Linux and 19 on Windows, the rest stop on headers
      this machine does not have (Qt, SFML, lz4, wil, the achievement hash
      header) or on unrelated errors, exactly as with the base tree.
      Element and value types match in every find/contains/== written
      (MSVC C4389). MSVC was not run.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W29 .cpp
      file. Remaining runs are in W29 headers only (declarations, data
      struct layouts, the target variant and action payloads).

- W34 log: rewrote all 65 W34 files in `Core/InputCommon` (outside
  ControllerInterface): control references and the expression parser,
  ControllerEmu and its control groups and settings, input configs and
  profiles, dynamic input textures, image operations, the GameCube adapter
  driver and the pad/keyboard status headers (8109 lines now, from 8661).
  Every rewritten file starts with the original-code header and has no
  Dolphin copyright or SPDX line. File names, public declarations and
  layouts, every control group's state (deadzones, gates, calibration,
  smoothing, the IMU and cursor maths, bit for bit), expression parse
  results, parse errors and evaluated values, every config key, default
  mapping and UI name, the controller and profile ini bytes, the dynamic
  input texture JSON format and the images it produces, adapter packets and
  rumble writes, enum values, the savestate layout and all translatable
  strings are unchanged.
  - Control references: ControlReference (input/output references, the
    input gate, the hotkey suppression bookkeeping), ExpressionParser (the
    lexer, every token and operator, precedence, variables, comments,
    barewords, the device-qualified control syntax and every error) and
    FunctionExpression (all functions, their argument checks and the
    timer/toggle/smooth/hold/tap/relative state, with the same clock reads).
  - ControllerEmu: EmulatedController (default device, ini load/save,
    mapping expressions, the state mutex), ControlGroup and Control/Input/
    Output, AnalogStick, Attachments, Buttons, Cursor, Force, IMUAccelerometer,
    IMUCursor, IMUGyroscope (calibration and its timing), IRPassthrough,
    MixedTriggers, ModifySettingsButton, Slider, Tilt, Triggers,
    NumericSetting (the getters now live in the header) and StickGate (all
    gate shapes, calibration, notch snapping and the deadzone maths).
  - InputConfig and InputProfile (the profile cycling and the per-game
    profile paths), DynamicInputTextureManager, DITConfiguration,
    DITSpecification (split into helpers per JSON section; the same error
    strings, in the same order, and textures read before an error stay in
    the list), DITData.h and ImageOperations (nearest/bilinear resize,
    aspect-preserving padding, region copy, PNG load/write).
  - GCAdapter: the scanning, read and write threads, hotplug where libusb
    has it, the macOS no-detach rule and FreeBSD's always-on hotplug, the
    poll-rate measurement, rumble, the SI device-change callbacks and the
    config callbacks. GCPadStatus.h and KeyboardStatus.h keep all 170
    enum values (checked by script).
  - Deliberate differences (malformed input, undefined behaviour or
    unbuilt platforms; nothing a game or a well-formed mapping sees):
    - ImageOperations: CopyImageRegion skips pixels whose index falls
      outside the source or destination image. The old code wrote out of
      bounds for a region whose top is below its bottom (seen with
      fractional region values in a DIT JSON file) and crashed with
      "free(): invalid pointer"; images are byte-identical otherwise.
    - GCAdapter: the endpoint scan only runs when libusb returns a config
      descriptor (the old code dereferenced a null descriptor when
      libusb_get_config_descriptor failed).
    - GCAdapter: IsDetected reads the adapter status once per call (the old
      code read the atomic twice, so the result and the message could come
      from different states).
    - GCAdapter and IMUCursor: the Android (JNI) branches are not kept;
      there is no Android build in this tree. The IMU cursor defaults to
      enabled, as it did on every non-Android build.
  - Checked: old and new code side by side on synthetic inputs only (no
    games, controllers or user files), using the base versions of all
    non-W34 files on both sides, with a fake ControllerInterface (devices,
    inputs and outputs driven by a seeded generator) and an interposed
    steady clock that advances on every read, so the number and order of
    clock reads is part of each case's hash. Outputs were streamed through
    cmp or reduced to one hash per case:
    - Expressions: 300000 lexer cases, 100000 + 50000 random expressions
      (parse status, error, the printed tree, evaluation over random input
      sequences with the clock running), 100000 function cases and 300000
      small hand-shaped cases (operators, precedence, comments, quoting,
      device qualifiers, malformed input).
    - Groups: 20000 stick-gate cases (every gate shape, calibration, notch
      snapping) and 8000 controller cases building every group type with
      random settings, mappings and inputs: group states (deadzone,
      calibration, smoothing, IMU and cursor maths) as raw bits, ini output
      and reload, default mappings, setting expressions and output values.
    - Configs and textures (with a stand-in PNG codec): 7000 cases of
      random controller configs, profile files, profile cycling and DIT
      JSON packs (valid and damaged): ini bytes, OSD messages, logs, error
      strings and every generated image's bytes; 50000 image-operation
      cases (resize modes, padding, region copies). The old side was run
      with an assertion-checked standard library, which is how the
      CopyImageRegion overflow above was found; the generator then left
      out such regions, and the new code was run with them for 3000 more
      cases under the same checks.
    - GCAdapter, against a lockstep fake libusb (device lists, open and
      claim results, kernel driver states, interrupt transfers, failures)
      and fake SI/Core/config: 500 cases of random reports, rumble, device
      type resets, rescans and config changes: pad states, detection
      status and messages, callbacks, every outgoing packet and the logs.
      The harness's thread hand-offs are timing-dependent: the old code
      itself gives different hashes between runs for a few cases (114,
      212, 213, 246, 313 seen over six runs of the old code). Every case
      where old and new differed in a long run (136, 213, 246, 318, 433)
      matched when rerun from a few cases earlier, or gave the same pair
      of alternative hashes in both versions; all other cases matched in
      every run.
    - Performance: the expression and group cases run 3.2% and 2.8% fewer
      instructions in the new code (callgrind, 2000 expression cases and
      200 group cases). Wall-clock timings of the parse-and-evaluate and
      group benchmarks were within run-to-run noise (about ±8% on this
      shared machine).
  - Compile check: the W34 .cpp files are clean with clang `-Wall -Wextra
    -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32`, on Linux
    and for x86_64-w64-windows-gnu (GCAdapter also with __APPLE__ and
    __FreeBSD__ defined). Fixes made for it: InputConfig's profile slot loop
    uses std::size_t, Cursor's SetRelativeInput parameter no longer shadows
    the ControlGroup member (also MSVC C4458), and ImageOperations computes
    pixel indices in u32. The 167 other non-Qt files that include W34
    headers compile exactly as on the base tree: the same files pass (96 on
    Linux, 94 for Windows) and the ones that fail on both trees (missing
    third-party headers here) give the same errors. With extra warnings,
    the only W34 reports are the deprecated implicit copy of
    DITConfiguration (already in the base tree) and IMUGyroscope's
    `period == 0` (-Wfloat-equal; the base tree's `!period` gives
    -Wfloat-conversion instead, which is in the required set). DolphinQt
    was not built.
  - Verbatim audit (`verbatim-audit.py wii 8`): no run of 8 or more lines
    is left in any W34 .cpp file. The runs left in W34 headers are public
    declarations (the KeyboardStatus and GCPadStatus enums, class member
    lists, the expression parser's token and parse-result types).
  - Not tested: the hotplug success path (the fake libusb has no hotplug
    events) and the timing of "reads failing" detection on a real adapter.
  - No keys, BIOS, firmware, DSP ROMs or game data were added.
  - Not rewritten: nothing.

- W32 log: rewrote all 38 W32 files as original code: the Vulkan backend
  (`VideoBackends/Vulkan`: `CommandBufferManager`, `Constants.h`,
  `ObjectCache`, `ShaderCompiler`, `StagingBuffer`, `StateTracker`,
  `VKBoundingBox`, `VKGfx`, `VKMain.cpp`, `VKPerfQuery`, `VKPipeline`,
  `VKShader`, `VKStreamBuffer`, `VKSwapChain`, `VKTexture`, `VKVertexFormat`,
  `VKVertexManager`, `VideoBackend.h`, `VulkanContext`,
  `VulkanEntryPoints.inl`, `VulkanLoader`). They have 8872 lines now (from
  10213). Every file starts with the original-code header and has no Dolphin
  copyright or SPDX line. The Vulkan `CMakeLists.txt` is not a W32 file and
  is unchanged. `VulkanEntryPoints.inl` stays a list of the public API names
  (same names, same order, same module/instance/device and required
  split; the row macros have new names, used only by `VulkanLoader`).
  The public names, signatures, enums and layouts are kept: `Constants.h` (descriptor
  set layouts, pipeline layouts, binding numbers, buffer sizes),
  `VulkanContext` and its `PhysicalDeviceInfo`, `CommandBufferManager`,
  `ObjectCache`, `StateTracker`, `StagingBuffer`, `StreamBuffer`,
  `SwapChain`, `VKTexture`, `VKFramebuffer`, `VKStagingTexture`, `VKShader`,
  `VKPipeline`, `VertexFormat`, `VertexManager`, `VKBoundingBox`,
  `PerfQuery`, `VKGfx` and `VideoBackend`. Changed in the headers, with no
  user outside W32: private members and helpers have new names (the
  `StateTracker` dirty bits and cached sets, the command-buffer slot ring,
  the swap-chain image helpers), unused forward declarations are gone, and
  `StagingBuffer` is `final` with a non-virtual destructor (nothing derives
  from it).
  - Design:
    - `VulkanLoader`: one `EntryPoint` table built from the `.inl` list with
      a scope (module, instance, device) and a required flag;
      `ResolveScope` fills and checks one scope; `VkResultToString` is a
      table lookup.
    - `VulkanContext`: `PhysicalDeviceInfo` is filled by one constructor
      from the properties and features chains (`ChainBehind`); the Mesa
      driver ids and the broken-feature driver bugs are tables
      (`MESA_IDS`, `BROKEN_FEATURES` of {bug, `BackendInfo` member}) applied
      in the old order; device creation is split into
      `SelectDeviceExtensions`, `FindQueueFamilies`, `CreateDevice`,
      `CreateAllocator` and `EnableDebugUtils`.
    - `CommandBufferManager`: a ring of slots with `CreateSlots`,
      `DestroySlots`, `WaitForSlot`, `RecycleFramePools`, `QueueSlot` and
      `BeginNextSlot`; deferred destruction is one `DeferredObject` variant
      list with one `DestroyDeferred`.
    - `ObjectCache`: set and pipeline layouts come from binding tables; the
      pipeline cache file goes through `BlobReader`/`DiscardReader` and a
      `PipelineCacheHeader` check; samplers and render passes are map
      lookups with the same keys.
    - `ShaderCompiler`: the GLSL preludes are line tables with an HLSL alias
      table, built once (`GraphicsPrelude`, `ComputePrelude`,
      `SubgroupPrelude`) and joined with `WithPrelude`.
    - `VKStreamBuffer`: `CreateBacking`, `ReserveMemory`, `CommitMemory`,
      `RecordWritePosition`, `RetireCompletedMarks` and `WaitForClearSpace`
      around the same fence-tracked marks.
    - `VKTexture`: layout transitions use `LeavingScope`/`EnteringScope`
      (stage and access per layout); view types, linear formats
      (`SRGB_TO_UNORM` pairs) and depth formats are tables.
    - `VKPipeline`: state translation is one helper per block (`CullFlags`,
      `Topology`, `AllowsRestart`, `DepthCompare`, `BlendFactor`,
      `RasterState`, `SampleState`, `DepthTest`, `AttachmentBlend`) with
      shared viewport and dynamic states.
    - `StateTracker`: one dirty mask with named bits; `StartPass` begins all
      render passes; set updates are split into GX, utility and compute
      writers.
    - `VKVertexManager`: the three stream rings are created from a table of
      {member, usage, size}; texel buffer views through `WholeBufferView`.
    - `VKVertexFormat`: attribute formats come from a
      `[type][normalized][components]` table.
    - `VKGfx`: clear values and clear attachments come from small helpers
      (`ColorClearValue`, `DepthClearValue`, `ClearOf`); factories and
      present handling are regrouped.
    - `VKMain`: `PickAdapter`, `WantDebugUtils` and `AbandonInstance`
      helpers; the backend objects are built as typed locals in the old
      order and handed over.
    - `VKSwapChain`, `VKBoundingBox`, `VKPerfQuery`, `StagingBuffer`,
      `VKShader`: new structure around the same steps.
  - Kept as before (outputs, quirks):
    - Every Vulkan call, in order and with its arguments, on the main thread
      and on the submission thread: instance and device creation, the
      extension and layer lists, queue family and memory type choices,
      barriers, descriptor writes, render passes, pipelines, draws, copies,
      readbacks, presents, fences and semaphores.
    - The SPIR-V input (the same GLSL preludes and sources passed to the
      compiler), bounding-box and perf-query results, EFB peeks and pokes,
      swap-chain choices (format, present mode, image count, extent,
      transform, recreation), threaded submission, backend info, config keys,
      driver-bug workarounds, translatable and log texts.
    - Quirks: the uploads-not-coherent warning is checked before the
      `vmaCreateBuffer` result; clear render passes pass a null clear-value
      pointer when there are no attachments; the Vulkan 1.0 instance on
      drivers without `vkEnumerateInstanceVersion`; the depth clear value
      only sets the depth and stencil members of the union.
  - Deliberate differences:
    - The non-Windows branches are gone (Xlib, Android, Metal layer setup,
      adrenotools driver loading); `VideoBackend::PrepareWindow` is empty
      (it only did Metal work). Windows-only tree.
    - The Intel vendor match always reports `DRIVER_INTEL` (the Mesa branch
      for Intel only applied off Windows).
    - `StagingBuffer::AllocateBuffer` zero-initialises its
      `VmaAllocationInfo`. When `vmaCreateBuffer` failed the old code read the
      memory type of an uninitialised struct (an out-of-range index; the
      VMA debug assert fires); the new code reads type 0 and may log the
      non-coherent warning before the failure message.
    - Log, panic and assert texts are built through shared helpers with the
      same output; assertion expressions and locations differ, and the
      function names quoted in `LOG_VULKAN_ERROR` prefixes are the new
      ones.
    - `StagingBuffer` is `final` and non-virtual; unused forward
      declarations removed from the headers.
    - Conversions are explicit casts (vertex attribute indices, the
      back-buffer size, sampler LOD values, the pipeline sample mask).
  - Checked:
    - Old vs new, each built from its own W32 files with the untouched
      non-W32 base files on both sides (renderer abstraction, vertex manager
      base, index generator, driver details, perf-query and bounding-box
      bases, render state, texture config, VMA), compiled as the Windows
      build (`WIN32`, `VK_USE_PLATFORM_WIN32_KHR`) against a recording fake
      Vulkan generated from `vk.xml`: all 170 entry points of the list, 641
      structures serialised member by member with their `pNext` chains,
      one FNV hash per thread (main and submission thread), hashes of the
      data the backend hands over (uniform, vertex, index and upload
      contents, flushed ranges, SPIR-V sources), returned values (handles,
      results, counts, properties, readbacks as a function of the source),
      and scripted failures. Each case draws a random world: 0 to 3
      physical devices of random vendors and driver ids, API versions,
      random instance and device extension sets, features, limits, queue
      families, memory types and heaps, surface capabilities, formats and
      present modes, random video settings (adapter, validation layer,
      vsync, MSAA, SSAA, HDR, threaded submission, shader cache, bounding
      box, stereo, EFB scale). It runs
      `InitBackendInfo`, `Initialize` (backend info dumped member by
      member), 80 random actions (textures, uploads, copies, resolves,
      staging reads and writes, framebuffers, shaders, vertex formats,
      pipelines, scissor, viewport, textures and samplers, compute images,
      GX batches through the vertex manager with uniform uploads, utility
      draws, texel buffers, region clears, present, resize and surface
      changes, perf queries, bounding box), then teardown and `Shutdown`.
      Half of the cases use threaded submission; each thread has its own
      hash, so the result does not depend on the interleaving (present
      failures are only injected without the thread). One hash per case,
      verbose logs streamed only to locate differences.
      3000 cases: 2994 identical. The other 6 are the
      `VmaAllocationInfo` difference above: the old build stops at the VMA
      assert, the new build continues; the logs are equal up to that point.
      659 cases stop at the same call with the same hash on both builds,
      all base behaviour: 227 VMA asserts for devices without
      `vkGetPhysicalDeviceMemoryProperties2KHR`; 269 a surface reporting an
      undefined current extent during `Initialize`, before the presenter
      exists (Win32 drivers report the window size); 150 failed
      initialisation paths destroying stream buffers or swap-chain
      framebuffers after the command buffer manager is gone; 9 destroying
      swap-chain textures before the state tracker exists; 4 other
      (identical on both builds, not investigated further).
    - The harness found one difference in the rewrite before the final
      runs: `VKFramebuffer::SetAndClear` reserved its clear-value vector, so
      a framebuffer without attachments passed a non-null pointer; fixed.
    - Not compared: assertion texts and locations; the `LOG_VULKAN_ERROR`
      function-name prefix. `VulkanLoader` library loading uses a fake
      library in the harness; the Windows exclusive-fullscreen query runs
      against a stubbed monitor.
    - Speed, interleaved runs on the same (shared, busy) machine with the
      fake in a no-logging mode: 16 cases, each 40000 rounds of a pipeline
      switch, 8 textures and samplers, scissor, viewport, a 64-vertex GX
      batch with vertex and pixel uniform uploads and an indexed draw.
      Minimum of 5 runs per case: 2304.5 ms old, 2265.5 ms new in total;
      per-case results differ in both directions within the run-to-run
      noise. No measurable change in draw submission or state changes.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, Linux and `x86_64-w64-windows-gnu`, against the
      untouched tree: the 18 W32 .cpp files and their outside user
      (`VideoBackendBase.cpp`) show no warning from W32 code. No `bool`
      mixed with an integer under `|` or `&`, no unary minus on unsigned
      values, no `~` widened, no signed/unsigned compares or mixed-type
      finds. Fixes made for these checks (casts and names only):
      `static_cast<void>` for parameters only used off Windows in
      `VulkanContext`; a missing `ShaderGenCommon.h` include; float casts in
      `ObjectCache::GetSampler`; a `u8` cast in `VKPipeline`; an unnamed
      unused parameter in `VKTexture::Load`; vertex attribute index casts in
      `VKVertexFormat`; int casts for `SetBackbuffer` in `VKGfx`. MSVC was
      not run.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W32 .cpp
      file. Remaining runs are in headers only (declarations: `VKGfx.h`,
      `VulkanContext.h`, `StateTracker.h`, `VKTexture.h`,
      `VKVertexManager.h`, `VKPerfQuery.h`, `VKSwapChain.h`, `Constants.h`).

- W36 log: rewrote all 58 W36 files in `Core/AudioCommon`,
  `Core/UICommon` (with ResourcePack), `Core/UpdaterCommon`,
  `Core/DolphinTool` and `PCH` (8837 lines now, from 9693), and deleted
  the three Linux/Android backend stand-in headers W1 left in
  `Core/AudioCommon` (`AlsaSoundStream.h`, `PulseAudioStream.h`,
  `OpenSLESStream.h`): the new `AudioCommon.cpp` no longer includes them
  and nothing else does. Every rewritten file starts with the original-code
  header and has no Dolphin copyright or SPDX line. File names, public
  declarations and layouts, the mixer output (bit for bit), the WAV bytes,
  the savestate layout, the GameFileCache bytes (header, revision 27,
  entry order), the resource pack and Packs.ini formats, the updater
  manifest format and actions, command-line parse results, NetPlay index
  requests and parsed replies, Discord presence fields, dolphin-tool output
  text and exit codes, every config key, flag and translatable string, and
  the current auto-update behaviour (Windows-only updater, same URLs,
  changelog HTML and updater flags) are unchanged. CMake lists, `.rc`,
  `.manifest` and `.filters` files were not touched.
  - AudioCommon: the mixer (granule resampler, per-source volumes, the
    Wii Remote speaker routing, the DTK/DSP dumps, savestate), the
    surround decoder wrapper, the WAV writer (header, silence skipping, the
    file split on a rate change), backend selection and volume control,
    the Cubeb stream and utilities (context, device lists and lookup, the
    log forwarding, the Wii Remote speaker streams), the OpenAL stream
    (the same entry points, resolved from openal32.dll in a table), the
    WASAPI stream and the null stream.
  - UICommon: GameFile (the private helpers renamed; the two-disc prefix
    table is a free function), GameFileCache, CommandLineParse (options
    from a table), AutoUpdate, DiscordPresence, NetPlayIndex, the resource
    pack Manifest, ResourcePack and Manager (Manifest and ResourcePack
    getters now live in the headers), and UICommon itself (start-up and
    shutdown order, controller start/stop from a table, user folder
    selection including the Windows registry and portable rules, XDG
    folders on Linux, locale, FormatSize).
  - UpdaterCommon: manifest download, gzip, signature check, planning,
    content-store fetch, file replacement with .bak copies and the mode
    carry-over, deletion, the argument table, Platform.h and UI.h.
  - DolphinTool: convert, extract (options from a table), header, verify,
    the command table with `wmain` on Windows, the headless host hooks and
    `resource.h` (`IDI_ICON1` only; the APSTUDIO block is gone).
  - PCH: `pch.h` (the same headers and compiler checks), `pch.cpp` and
    `nopch/pch.h`.
  - Keys and IDs left byte for byte as they were: the Discord application
    ID `455712169795780630` (`DiscordPresence.h`) and the updater's two
    ed25519 public keys, `UPDATE_PUB_KEY` and `UPDATE_PUB_KEY_TEST`
    (`UpdaterCommon.cpp`).
  - Deliberate differences (malformed input, undefined behaviour or unbuilt
    platforms; nothing a game, a well-formed file or a server reply sees):
    - Mixer: MixSurround no longer writes past the caller's buffer when
      the decoder is asked for more frames than its block allows.
    - WaveFile: the "file too large" check is written so it cannot
      overflow.
    - CubebUtils: device lookups skip devices without an ID string (the
      old code built a string_view from a null pointer and crashed).
    - CubebStream: the main and speaker streams share one state callback.
    - OpenALStream: the destructor returns early when OpenAL was never
      loaded.
    - WASAPIStream: the HRESULT of IMMDeviceCollection::Item is checked.
    - GameFileCache: the header's padding word is written as zero.
    - NetPlayIndex: the destructor also joins a keep-alive thread that has
      already ended after a server error.
    - ResourcePack: GetLowerPriorityPacks returns nothing for a pack that is
      not in the list (the old code started at end() + 1); zip entries with
      no info are reported instead of dereferenced.
    - UICommon: registry strings get an extra zeroed terminator; the
      Apple, Android and D-Bus branches (AutoUpdate, UICommon) are not
      kept, as no such build exists in this tree.
    - dolphin-tool header/verify read all values before printing (same
      output).
  - Checked: old and new code side by side on synthetic data only (no
    games, keys, servers or accounts), using the base versions of all
    non-W36 files on both sides, each side built with its own W36 headers.
    Outputs were compared in memory (od) or as one hash per case:
    - Mixer, surround and WAV (-O2, -ffp-contract=off, no FMA): 25 cases of
      seeded random pushes on every source with random rates, divisors,
      volumes and read sizes, over output rates 0/32k/48k/96k, gap
      filling, pitch keeping, emulation speeds (including 0), buffer sizes,
      Wii Remote routing, a paused core and clipping input; MixSurround at
      every DPL2 quality; DTK/DSP dumps including the rate-change splits
      and silence skipping (every WAV file's bytes); savestate measure,
      write and read back into a fresh mixer that keeps mixing. All equal.
    - GameFile and GameFileCache, with a spec-driven fake volume (GC and
      Wii discs, WADs, NKit/Datel flags, banners, names per language),
      DOL/ELF files, homebrew XML and stand-in PNG banners, mod
      descriptors, covers (fake HTTP) and NAND banners: 8 cases (languages,
      covers on/off) x 6 phases: path search, a fresh cache and its bytes,
      reload and update after files change, halted updates, six damaged
      cache files and Clear, and the staging calls on a lone GameFile;
      every getter, sync identifier comparison and the cache bytes.
    - Command lines: 92 cases (GUI and non-GUI parsers, argv and vector
      forms, every option, -C overrides good and bad, choices, --help,
      --version, unknown options): help text, values, leftovers and the
      CommandLine config layer.
    - Updater, with fake HTTP, crypto (a stand-in signature tied to the
      first key byte; the full key hash is logged) and UI: 26 cases (a full
      update with renames, .bak copies, deletions, duplicates and the mode
      carry-over, missing flags, bad or extra signatures, corrupt gzip,
      malformed manifests, hash mismatches, missing content, symlinks,
      partial updates, test mode, a failing platform check, no temp
      folder): UI calls, the log file, the files left and their modes.
    - NetPlay index, with a fake server: 13 cases (list replies valid,
      partly malformed, missing, bad JSON, error status; add failures;
      keep-alive with the real 5 s interval, its no-reply and error paths)
      plus the server ID scrambling over 16 id/password pairs and raw
      decodes: every request URL and header and every parsed field.
    - Resource packs, with a fake minizip: 15 manifest texts and a full
      run (Init with a stale Packs.ini, installs with overlapping textures,
      uninstall with hand-over to lower packs, Remove, Add at an offset,
      SetInstalled, re-Init): pack fields, priorities, every file written
      and Packs.ini.
    - dolphin-tool, over spec-driven fake images, converters and verifier:
      762 command lines across all four commands and nine inputs (every
      format, block size, compression and level, scrubbing, JSON output,
      listing to stdout or a file, partitions, single files): output text
      and exit codes.
    - Cubeb, with a fake cubeb that logs every call and pulls data: 21
      cases (DPL2, Wii speaker routing, init/latency/stream/start/stop/log/
      enumerate failures, device counts, log manager off): call logs with
      parameters, log lines and output buffer hashes.
    - Discord, with a fake discord-rpc: 12 cases (presence on/off, join
      and join-request callbacks, RetroAchievements rich presence, empty
      titles): every presence field, handler calls and the NetPlay config
      written by a join.
    - OpenAL and WASAPI cannot run here (no Windows runtime). The OpenAL
      code makes the same 35 calls to the same 24 entry points; the WASAPI
      call counts differ only where a helper is now called from two places
      (the friendly-name read, the exclusive-mode Initialize).
    - The same aborts happen in both versions, so they are kept: a resource
      pack manifest that is a JSON array, a NetPlay list whose "sessions"
      is not an array, a Discord join secret that is not host:port, and
      optparse errors in dolphin-tool (it throws an int).
  - Compile check: the W36 .cpp files are clean with clang `-Wall -Wextra
    -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32`, on Linux
    and for x86_64-w64-windows-gnu (Windows side with the PCH and
    `_MSC_EXTENSIONS`); the old tree gives the same results except that
    Linux `WASAPIStream.cpp` fails on a header in both. Fixes made: the
    NetPlay JSON parse uses a `const char*` cursor (picojson's overload
    needs a mutable iterator), and AutoUpdate zero-initialises
    STARTUPINFO and assigns its fields (no missing-initialiser warning).
    For the MSVC warnings that broke CI before: C4267, C4389 and C4458
    are what -Wshorten-64-to-32, -Wsign-compare and -Wshadow-all report on
    the Windows target (none); the new code has no bitwise `~` (C4319) and
    no bool compared or combined with an integer or a bool literal
    (C4805), checked by search. The 20 non-Qt files
    outside W36 that include W36 headers compile exactly as on the base
    tree (15 pass on Linux, 13 for Windows, the rest fail identically on
    missing third-party headers). WinUpdater's sources compile against the
    new UpdaterCommon headers (its `Main.cpp` fails on a pre-existing
    `ShlObj.h` case issue). DolphinQt was not built.
  - Verbatim audit (`verbatim-audit.py wii 8`): no run of 8 or more lines
    is left in any W36 .cpp file. The runs left in W36 headers are public
    declarations (GameFile's and GameFileCache's member lists, the
    Mixer, WaveFile, CubebStream, NetPlayIndex, Manifest, Manager,
    DiscordPresence and CommandLineParse declarations, UpdaterCommon's
    UI.h).
  - Not tested: AudioCommon's backend selection and volume glue,
    UICommon's Init/Shutdown, folder and registry code (FormatSize computes
    the same expression), the Windows auto-update launch, and the OpenAL
    and WASAPI streams at run time.
  - No keys, BIOS, firmware, DSP ROMs or game data were added.
  - Not rewritten: nothing.

- W33 log: rewrote all 58 W33 files as original code: the D3D11 backend
  (`VideoBackends/D3D`: `D3DBase`, `D3DBoundingBox`, `D3DGfx`, `D3DMain.cpp`,
  `D3DNativeVertexFormat.cpp`, `D3DPerfQuery`, `D3DState`, `D3DSwapChain`,
  `D3DVertexManager`, `DXPipeline`, `DXShader`, `DXTexture`,
  `VideoBackend.h`), the D3D12 backend (`VideoBackends/D3D12`: `Common.h`,
  `D3D12BoundingBox`, `D3D12Gfx`, `D3D12PerfQuery`, `D3D12StreamBuffer`,
  `D3D12SwapChain`, `D3D12VertexManager`, `DX12Context`, `DX12Pipeline`,
  `DX12Shader`, `DX12Texture`, `DX12VertexFormat`, `DescriptorAllocator`,
  `DescriptorHeapManager`, `VideoBackend`) and `VideoBackends/D3DCommon`
  (`D3DCommon`, `Shader`, `SwapChain`). They have 10186 lines now (from
  10047). Every file starts with the original-code header and has no Dolphin
  copyright or SPDX line. The backend `CMakeLists.txt` files are not W33
  files and are unchanged. The public names, signatures, enums and layouts
  are kept: `D3DCommon` (`LoadLibraries`, the factory and format helpers,
  `SetDebugObjectName`), `D3DCommon::Shader` and `SwapChain`, `DX11::D3D`
  globals and functions, `StateManager`/`StateCache` public methods,
  `DX12::DXContext` and its enums and root-parameter indices, the descriptor
  heap and allocator interfaces, `StreamBuffer`, the texture, framebuffer,
  staging, shader, pipeline and vertex-format classes, both `Gfx` classes,
  vertex managers, perf queries and bounding boxes, and both
  `VideoBackend` classes. Changed in the headers, with no user outside W33:
  private members and helpers have new names (the D3D11 `StateManager`
  dirty mask and bindings, the D3D12 `Gfx` pending bits and binding state,
  the vertex managers' buffer members), and the D3D11 framebuffer keeps one
  extra member for its integer render-target array (see below).
  - Design:
    - `D3DCommon`: the loaded runtime libraries and entry points are one
      `RuntimeLibraries` struct; the format mapping is one row table indexed
      by `AbstractTextureFormat` (storage, typeless, SRV, RTV, integer RTV,
      DSV) plus a reverse table; debug names go through one `TryNameChild`
      template, first as a D3D11 child, then as a D3D12 child.
    - `Shader`: the HLSL preamble is written by a small `PreambleWriter`
      from an alias table (byte-identical text); `ProfileFor` picks the
      profile by shader-model generation; SPIR-V to HLSL, compile and the
      failure report are separate helpers.
    - `SwapChain`: creation is `CreateFlipModel` (flip-discard, then
      flip-sequential) or `CreateBitBltModel`, with `TryEnableHDR`,
      `LeaveFullscreen` and `RecreateWith` around the same DXGI calls.
    - `D3DBase`: device creation is `MakeDevice` and `TryCreateDebugDevice`
      with `ConfigureInfoQueue`; the capability probes (`SupportsLogicOp`
      and the others) share a `ProbeDevice` that owns its library, factory,
      adapter and device; teardown is `ReleaseRuntime`.
    - `D3DState`: `StateManager` tracks wanted and bound state in two
      `Bindings` with one `u64` dirty mask (textures, samplers, then one bit
      per group); `Apply` runs `ApplyOutputMerger`, `ApplyConstants`,
      `ApplyInputAssembler`, `ApplyShaders` and `ApplyTextures` in the old
      order; compute bindings use one `SyncRuns` template. `StateCache`
      builds descriptors from filter, address-mode and blend-factor tables.
    - `DXTexture`, `DXShader`, `DXPipeline`, `D3DNativeVertexFormat`,
      `D3DVertexManager`, `D3DPerfQuery`, `D3DBoundingBox`, `D3DGfx`: view
      and desc builders as helpers (`MakeTextureDesc`, `MakeRtvDesc`,
      `MakeDsvDesc`, `ResolveViewFormat`), one `MakeStage` template for the
      shader stages, a vertex-format table, `DynamicBufferDesc` /
      `CreateConstantBuffer` / `WriteConstants` / `CreateTexelView` in the
      vertex manager, a `RingEntry` ring with `CollectReady` /
      `CollectOldest` / `ToNativeResolution` in the perf query.
    - D3D12 `DX12Context`: the loaded runtime is a `D3D12Runtime`; root
      signatures are built by a `RootSignatureBuilder` (`AddCBV`,
      `AddConstants`, `AddTable`, `Build`).
    - D3D12 `StreamBuffer`, `DescriptorHeapManager`, `DescriptorAllocator`:
      space search is `SpaceIfGPUAt` returning an optional range; free
      blocks are bitsets filled with `assign` and searched lowest-first;
      sampler descriptors come from the same filter and address tables as
      D3D11.
    - D3D12 `DX12Texture`, `DX12Pipeline`, `DX12VertexFormat`: file-local
      helpers (`HeapOfType`, `LinearBufferDesc`, `BoxFor`, `FootprintAt`,
      `MakeUploadBuffer`, `ArrayRtvDesc`, `ArrayDsvDesc`; `TopologyFor`,
      `FillRasterizer`, `FillDepth`, `FillBlend` with factor and logic-op
      tables; packed-format tables for the vertex attributes).
    - D3D12 `Gfx`: a `PendingBit` enum (same bit values) and one `Bindings`
      struct; descriptor staging is `StageDescriptor`. The vertex manager
      names its constant-buffer slots and offsets; `CreateBufferView` and
      `StageBlock` replace the inline sequences. `PerfQuery` uses
      `IsCountedGroup` and `ScaleToNative` around the same ring.
  - Kept as before (outputs, quirks):
    - Every D3D11, D3D12, DXGI and d3dcompiler call, in order and with its
      arguments, including the descriptors' field values, debug object
      names, the private-data GUIDs, feature checks and their order, and
      the info-queue filters.
    - The HLSL preamble and all shader texts, the bytecode cache blobs (the
      D3D12 `GetCacheData` still calls `GetBufferSize` twice), the
      bounding-box and perf-query results (the D3D11 ready-store without and
      the spin-wait with the MSAA division; D3D12 scaling), EFB peeks and
      pokes, the swap-chain choices (flip-discard, flip-sequential, bit-blt;
      tearing, HDR colour space, buffer count and format), command-list
      submission and fence waits, descriptor sizes and counts (65536
      descriptors per D3D12 list, the 1024 reset threshold), stream-buffer
      sizes and wrap rules, backend info, config keys, driver-bug
      workarounds, savestate layout (none in W33) and the translatable,
      panic and log texts.
    - Quirks: the D3D11 `SetPipeline` current-pipeline handling; the
      D3D11 bounding-box staging buffer with stride 4; the global input
      layout lock; constant buffers created vertex, geometry, pixel; the
      `>= BUFFER_SIZE` wrap comparison; `SetPixelConstants` passing the
      vertex buffer only with per-pixel lighting; the D3D12 SRV descriptor
      writing `Texture2DArray`/`Texture2DMSArray` fields through the union
      for every dimension; the D3D12 DSV allocation panic reading "Failed to
      allocate RTV descriptor"; extra D3D12 attachments using the colour
      format; staging textures destroyed without an Unmap; the D3D12 root
      signature build reporting success when `CreateRootSignature` fails;
      the pixel and custom constant reserve before the dirty check, no
      retry in `UploadUtilityUniforms`; `SetName` on a failed shader; the
      `"Direct3D 12"` name untranslated; "Dolphin Version: " in the
      bad-shader reports.
  - Deliberate differences:
    - The invalid wrap value 3 maps to clamp in both sampler builders (D3D11
      `StateCache`, D3D12 `SamplerHeapManager`); the old code read past a
      three-element array.
    - D3D11 framebuffers with additional colour attachments return
      [integer RTV, additional RTVs...] as their integer render-target
      array; the old code read past `m_integer_rtv` into `m_dsv` (undefined
      behaviour). New private member `m_integer_targets_raw`.
    - D3D12 swap chain: `DestroySwapChainBuffers` destroys its textures
      normally (the texture clears its descriptor handles in
      `DestroyResource`); the old code leaked them through `release()`.
      `CreateSwapChainBuffers` returns false when `GetBuffer` fails; the old
      code went on with a null resource.
    - The D3D12 `RootSignatureBuilder` zero-initialises its parameter and
      range arrays; only unused union bytes differ.
    - The D3D11 perf-query read index is `u32`; the dead null check after a
      dereference in D3D12 `SetTexture` is gone (same behaviour).
    - `DXShader` raises one parameterised ASSERT ("Failed to create {}
      shader: {}") with the same output text as the per-stage ones.
    - `D3DBase` capability probe: the factory and adapter are kept until the
      D3D11.1 query is done (the old order released them first; same calls
      otherwise).
    - Conversions are explicit casts (adapter index, back-buffer size,
      copy rectangles, vertex-format loops, stream sizes);
      `ReportLiveDeviceObjects` takes a cast flag combination (no
      `operator|` for `D3D11_RLDO_FLAGS` in every SDK); the D3D12
      `ResourceBarrier` helper is `inline` instead of `static`; the format
      support mask is `const` instead of `constexpr` (flag operators are
      not constexpr in every SDK). Unused parameters are unnamed.
  - Checked:
    - Old vs new, each built from its own W33 files with the untouched
      non-W33 base files on both sides (VideoCommon renderer abstraction,
      vertex manager base, framebuffer manager, presenter, shader
      generators, SPIR-V compiler interface, config), as a Windows x64
      program (mingw g++, DirectX-Headers, SPIRV-Cross at the pinned commit)
      run under Wine, against recording fake COM objects. A generator reads
      the preprocessed SDK headers and writes one logging stub for every
      method of 37 interfaces and their parent chains (D3D11 device 5,
      context 4, resources, views, shaders, states, queries, debug and info
      queue; D3D12 device, queue, allocator, list, fence, descriptor and
      query heaps, resource, root signature, PSO, debug and info queue;
      DXGI factory 7, adapter 4, output 6, swap chain 4; `ID3D10Blob`), with
      semantic dumps of every descriptor struct (unions keyed on their
      discriminator, pointed-to arrays, byte hashes of initial data, shader
      bytecode and root signatures). The fakes keep resource contents, map,
      copy, update, resolve and readback data, fence values that complete late,
      query results that are not ready at first, and return scripted values:
      random adapter counts, feature levels, D3D12 availability, debug layer,
      tearing, HDR colour space, `IDXGISwapChain4`, logic-op support, MSAA
      counts, format support, and injected `E_OUTOFMEMORY` failures on creation
      and texture maps; `D3DCompile` returns bytecode as a function of the
      source, or fails. SPIR-V is canned per source hash and goes through the
      real SPIRV-Cross. Each case picks D3D11 or D3D12 and runs library loading,
      backend info (dumped field by field), the backend start-up, a warm-up
      (EFB-like framebuffer, shaders, vertex formats, pipelines) and random
      actions of 22 kinds (textures, uploads, staging copies and maps,
      framebuffers, shaders with compile failures and bad-shader reports, vertex
      formats, pipelines with cache data, bindings, GX draws through the vertex
      manager with uniform uploads, utility draws, copies and resolves, compute
      dispatch, present and resize, fullscreen switching, flush, perf-query
      bursts, bounding box, destruction), then teardown. One hash per case,
      streamed; verbose logs only to locate differences.
      - 3000 cases of 80 actions: 2993 identical. 7 cases crash on both
        builds with the same calls up to the crash: an injected failure of
        a buffer the D3D11 code checks with an ASSERT and then uses (the
        stream buffer, a constant buffer, or a depth-stencil state), then
        `Map` on the null buffer. Base behaviour, unchanged.
      - 300 cases of 400 actions: all identical, no crash.
      - Coverage over 100 cases: 397 GX draws, 223 distinct logged calls
        and actions; perf-query begin, end, get-data and
        `ResolveQueryData`; bounding-box reads and writes; staging
        readbacks; swap-chain resizes; failure paths.
    - The harness found one difference in the rewrite before the final
      runs: the capability probe released the factory and adapter before
      the D3D11.1 query (fixed, so the calls are the same as before).
    - Not driven, compared line by line against the old files: the
      `GetBuffer` failure path (see above), `Map` failures on the stream
      buffers, real `D3DCompile` output and GPU semantics, fullscreen on a
      real output (the fake keeps the state; `SetFullscreenState`,
      `GetFullscreenState` and the request logic are the same calls in the
      same order). Assertion locations and quoted expressions are not
      compared.
    - Speed, interleaved runs on the same machine with the fakes and
      recorder in a no-logging build at -O2: 20 cases (D3D11 and D3D12),
      each 20000 actions of a draw-heavy mix (55% GX draws through the
      vertex manager, 25% texture, sampler, buffer and constant bindings,
      10% pipeline switches, 5% utility draws, 5% presents). Six runs per
      side: median 10392 ms old, 9946 ms new; minimum 9076 ms old, 9414 ms
      new. The differences go both ways within the run-to-run noise (about
      10%); no measurable change in draw submission or state changes. Most
      of the time is in the fakes, so this bounds the backend cost rather
      than isolating it.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, `x86_64-w64-windows-gnu` with the DirectX-Headers,
      against the untouched tree: the 29 W33 .cpp files and
      `VideoBackendBase.cpp` show no warning from W33 code (151 in the old
      files). The remaining message is outside W33 and the same on the
      base tree (`MsgHandler.h`, unused `format`). On Linux every W33 file
      stops at the missing D3D headers, on both trees, with no W33 warning
      before that. No `bool` mixed with an integer under `|` or `&`, no unary
      minus on unsigned values, no `~` widened, no signed/unsigned compares.
      Fixes made for these checks: casts (copy rectangles, `SetBackbuffer`,
      adapter index, stream sizes, vertex-format loops, the perf-query
      index), `? TRUE : FALSE` for `BlendEnable`, the `D3D11_RLDO_FLAGS`
      cast, `inline` for `ResourceBarrier`, `const` for the format-support
      mask, unused parameters unnamed, and a lambda capturing `[this]`.
      MSVC was not run.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W33 .cpp
      file. Remaining runs are in headers only (declarations: `D3DGfx.h`,
      `D3D12Gfx.h`, `DX12Context.h`, `SwapChain.h`, `DXTexture.h`,
      `DX12Texture.h`, `D3D12VertexManager.h`, `D3D12SwapChain.h`,
      `D3DVertexManager.h`, `D3D12PerfQuery.h`, `DXPipeline.h`,
      `DescriptorHeapManager.h`, `DX12Shader.h`, `DXShader.h`,
      `D3D12StreamBuffer.h`, `VideoBackend.h`, `DescriptorAllocator.h`,
      `D3D12BoundingBox.h`).

- W37 log: rewrote all 68 W37 files, the DolphinQt main window, menu bar,
  toolbar, host, settings object, render widget, hotkey scheduler, main.cpp,
  resources/theme handling, translation loader and the Qt utility helpers
  (10528 lines now, from 10960): `AboutDialog`, `GameCount`, `Host`,
  `HotkeyScheduler`, `Main.cpp`, `MainWindow`, `MenuBar`, `RenderWidget`,
  `Resources`, `SearchBar`, `Settings`, `ToolBar`, `Translation`,
  `resource.h` and every file under `QtUtils/`. Every rewritten file starts
  with the original-code header and has no Dolphin copyright or SPDX line.
  File names, public declarations, signals and slots, object names, every
  menu and toolbar action (text, order, shortcut, enabled/checked rules),
  command-line flags, window titles, QSettings and Config keys, the order and
  arguments of Core/UICommon calls, and all tr() strings (byte for byte,
  with their disambiguation) are unchanged.
  - PixelU changes kept exactly: `RenderWidget::IsEmbedMode()` and
    `ShowForEmbedding(geometry)` for RetroPixels' embedded launch, no saving
    of the render geometry on stop in embed mode, the embedded show path
    in MainWindow, and the idle title
    "PixelU Wii (based on Dolphin %1)" with the SCM description.
  - `MainWindow`: construction runs the same Connect* steps in the same
    order from one table; boot, stop (with the same confirm/hardcore/cursor
    rules), disc change and second-disc scan, state load/save and slots,
    NetPlay host/join/quit, FIFO, recording and read-only mode, drag and
    drop, the debugger widgets, the Wii remote/GBA windows and the
    platform window-system description. The confirm-to-stop dialog,
    resource pack check and settings-pane routing are now private helpers.
  - `MenuBar`: the menus are built from small row tables and helper
    lambdas, in the old order. `Settings`: QSettings and Config access,
    the game-list/theme/debug signals. `HotkeyScheduler`: polling thread,
    frame advance state machine, throttle, speed limit, stereo, EFB scale,
    aspect ratio, profiles, GBA, state slots. `Translation`: gettext .mo
    loading with context lookup, language choice.
  - Preserved quirks (callers or users may rely on them): HK_SET_PC emits
    Skip, "Copy XFB:" keeps its text, the inverted fog OSD message,
    `OnStartRecording` emitting ReadOnlyModeChanged(true) after
    SetReadOnly(false), MAIN_CURRENT_STATE_PATH being set even when the
    dialog is cancelled, and the u32 wrap in DecrementSelectedStateSlot.
  - Deliberate differences (nothing a user sees in normal use):
    - `Translation`: the .mo reader checks the file size and the table and
      string bounds and logs "too small" for a short file. Only malformed
      files are affected; the old code read out of bounds on them.
    - Qt connections that had no context object now use `this` (or the
      event filter) as context, so they are dropped when it is destroyed.
    - The throttle hotkey is read with one Held() call per frame (repeated
      calls returned the same value).
    - `Settings`: the volume is cast to u16 explicitly; a member replaces a
      function-static. `MenuBar`: a member replaces the `m_quiet_block_dump`
      static.
    - Main.cpp defines `wWinMain` directly instead of through an
      `app_main` macro. The NAND import progress callback takes ints.
    - `MainWindow`: explicit casts for State::Load/Save slots, Wii remote
      indexes and `sa_flags`. `m_stack` is created before the render
      widget (no visible effect). NetPlayHost no longer keeps the server
      pointer alive across NetPlayQuit.
    - The tr("Check for updates") call for an unused placeholder action
      is gone; the action never showed it.
  - Checked: old and new code side by side, built with Qt 6.4 offscreen
    against fakes that record every call, using the untouched non-W37
    files on both sides (only hashes were kept):
    - QtUtils: the widgets, event filters, validators, file dialogs, modal
      message boxes, image conversion, aspect-ratio widget, parallel
      progress dialog and queued-call helpers driven through a scripted
      session. Transcripts identical.
    - Translation and AboutDialog: 107 cases (About with and without a
      pending update, 21 language settings including damaged, empty,
      unsorted and other-revision catalogues, times 5 system languages).
      All identical.
    - Compared line by line instead, because they need the full Core
      (emulator state, boot, NetPlay, debugger, video backend):
      `MainWindow`, `MenuBar`, `ToolBar`, `Settings`, `Host`,
      `HotkeyScheduler`, `RenderWidget`, Main.cpp, `Resources`,
      `resource.h`. Every menu/toolbar action, tr() string, shortcut,
      connection and config key was checked off against the old file;
      a tr() string scan finds every old string in the new code.
  - Compile check: the W37 .cpp files and the 199 DolphinQt sources that
    include W37 headers, old against new, with clang on Linux and for
    x86_64-w64-windows-gnu, `-Wall -Wextra -Wconversion -Wsign-compare
    -Wshadow-all -Wshorten-64-to-32`. The rewritten files are clean except
    for two trigraph warnings from the "*.s??" filter string, which the old
    file has too. The other files have the same diagnostics as before
    (missing `QtUtils/SignalDaemon.h` and `DATA_DIR` on Linux,
    `DWM_WINDOW_CORNER_PREFERENCE` in the MinGW headers). Integer
    narrowing and sign changes are written as explicit casts (MSVC /W4
    with warnings as errors).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run is left in a W37
    .cpp file. The runs left are public declarations in the headers
    (signals, slots, member lists, enums).
  - No keys, IVs, tokens or credentials are in these files.

- W35 log: rewrote all 37 W35 files, `InputCommon/ControllerInterface`
  (8498 lines now, from 9252): `ControllerInterface`, `CoreDevice`,
  `InputBackend`, `MappingCommon`, `DInput/DInput`, `DInput/DInput8.h`,
  `DInput/DInputJoystick`, `DInput/DInputKeyboardMouse`, `DInput/NamedKeys.h`,
  `DInput/XInputFilter`, `ForceFeedback/ForceFeedbackDevice`, `XInput/XInput`,
  `WGInput/WGInput`, `Win32/Win32`, `SDL/SDL`, `SDL/SDLGamepad`,
  `DualShockUDPClient/DualShockUDPClient`, `DualShockUDPClient/DualShockUDPProto.h`,
  `SteamDeck/SteamDeck` and `Wiimote/WiimoteController`. Every rewritten file
  starts with the original-code header and has no Dolphin copyright or SPDX
  line. File names, public declarations, the DSU wire structs, enum values,
  device names, source names, ids and their order, input and output names,
  hotplug handling, host API calls and their order, config keys, defaults
  and translatable strings are unchanged. No file outside ControllerInterface
  was touched.
  - Core: the device container and registry (add/remove/refresh, ids per
    source and name, default device, the devices-changed callbacks and
    their platform-thread handling), `Device` (input/output lookup, the
    combined "|"-style inputs, full-analog surface), `InputDetector`,
    `RelativeInputState`, `MappingCommon` (expression building from
    detections) and `InputBackend`.
  - DSU (cemuhook) client: server list config, the per-server socket and
    request timing, version/port list/pad data requests, CRC checks, the
    slot → device mapping, the motion and touch inputs and their scaling,
    and the battery input. The protocol header keeps its packed layouts.
  - Steam Deck: the hidapi device scan and hotplug, the input report
    decoding (buttons, sticks, pads, triggers, IMU) and the periodic
    IMU enable request.
  - SDL: the SDL3 backend thread, gamepad/joystick hotplug with the same
    device order, `SDLGamepad` inputs, motion sensors, rumble, trigger
    rumble, LED and haptic effects.
  - Bluetooth Wii Remote: discovery hand-off, the connection set-up task
    chain (status, extensions, M+ activation and passthrough, IR camera
    set-up and sensitivity), report handling, calibration, extension
    inputs, IR, the battery and rumble timing.
  - Windows: Win32 (window and hotplug notification thread), DirectInput
    (enumeration, joystick axes/buttons/hats, keyboard and mouse with
    cursor and relative axes, the XInput filter), force feedback
    (effect creation, the worker that sends effect updates), XInput and
    Windows.Gaming.Input (gamepads, raw controllers, switches, haptics,
    battery, vibration and trigger rumble).
  - Deliberate differences (robustness only, nothing a configuration or game
    sees):
    - Macros for backends that no longer exist in this tree are dropped.
    - DSU: a PortInfo reply with pad_id 4 or more is ignored (the old code
      indexed past the slot array). `Decode` writes into the destination
      only when the packet is valid (same result as before).
    - Steam Deck: the report read buffer is a member cleared once, so a
      short read leaves no undefined bytes.
    - SDL: event ids start at 0, dead code is gone, and the haptic effect
      union is cleared in full before use.
    - Wii Remote: reports under 2 bytes are rejected, short reports are
      zero-filled without reading or writing out of bounds, the M+
      passthrough copy is capped at 6 bytes, and the rumble report's
      padding is zero.
    - DInput: a joystick ignores byte offsets outside its state. The
      keyboard/mouse device reads the render window once per update (atomic)
      and value-initialises its temporary mouse state.
    - WGInput: an empty button vector passes `data()` rather than the address
      of element 0.
    - `RelativeInputState` arrays are zero-initialised.
  - Checked: old and new code built side by side with the untouched non-W35
    base files on both sides, against recording fakes for every host API,
    driven by random devices, capabilities, input streams and hotplug.
    Device lists, every input value (bit for bit), output calls and the host
    call logs are compared by a hash per case (no full outputs kept). All
    runs are identical:
    - Core (fake backends, containers, expressions, detector, relative
      inputs): 300×200 seed 1, 2000×400 seed 7, 1000×600 seed 8 (cases ×
      steps).
    - DSU with a fake socket layer and random server packets (good, bad CRC,
      wrong magic/version/type/size): 1500×1000 seed 9, 1000×1000 seed 41.
    - Steam Deck with fake hidapi: 5000×800 seed 3, 1000×600 seed 4.
    - SDL with a fake SDL3: 1500×400 seed 2.
    - Bluetooth Wii Remote with random report streams: 1000×3000 seed 21,
      1000×1500 seed 31.
    - Win32 + DInput + XInputFilter + force feedback + XInput, built for
      x86_64-w64-windows-gnu and run under wine with fake DirectInput COM
      objects, XInput, user32, SetupDi/CM notification and tick count, the
      FF worker kept in lockstep: 100×150 seed 1, 1500×300 seed 2, 1000×400
      seed 7.
    - WGInput, against stand-in WinRT projection headers (the real ones are
      missing here), run under wine with a fake object model whose calls can
      throw: 200×150 seed 1, 1500×300 seed 3; also deterministic across
      repeated runs.
    Not tested: a mouse reporting more than 3 axes (both old and new read
    past the state, unchanged).
  - Speed (callgrind instructions inside UpdateInput, GetState and
    ControlReference::State, -O2, old → new): core 10,705,657 →
    10,501,184 (−1.9%); Steam Deck 15,990,283 → 15,997,069 (+0.04%, about
    one instruction per update, equal at -O1); SDL 272,645,257 →
    272,670,200 (+0.009%); DSU 26,687,563 → 26,713,947 (+0.10%, clang
    does not inline `Decode`); Wii Remote 51,085,926 → 51,177,580
    (+0.18%, all in the one-off connection set-up chain; report handling
    is 82k lower and GetState is equal). The Windows backends can't be
    timed under wine; they were compared line by line for per-update work
    and do no more of it.
  - Compile check: all 17 W35 .cpp files for x86_64-w64-windows-gnu and
    the 9 portable ones on Linux with clang `-Wall -Wextra -Wconversion
    -Wsign-compare -Wshadow-all -Wshorten-64-to-32`: no warnings in W35
    files (the remaining ones are in Common/MsgHandler.h,
    WorkQueueThread.h, MathUtil.h and ControllerEmu.h, as in the base).
    Narrowing and sign changes are explicit casts (MSVC /W4 /WX). The 10
    non-Qt dependents (Core, HotkeyManager, SI, BTEmu, WiimoteReal,
    InputConfig, InputProfile, ControllerEmu, UICommon, Present) on both
    targets and the 12 DolphinQt dependents (Linux, system Qt 6 headers)
    give exactly the base tree's diagnostics.
  - Verbatim audit (`verbatim-audit.py wii 8`): the runs left in W35 files
    are public declarations in headers (the DSU packed structs and enums,
    `CoreDevice.h` class declarations). No run is left inside a function
    body.
  - Not rewritten: nothing; every W35 file is original code.

- W41 log: rewrote all 42 W41 files, the DolphinQt debugger (13379 lines
  now, from 14683): `AssembleInstructionDialog`, `AssemblerWidget`,
  `AssemblyEditor`, `BranchWatchDialog`, `BranchWatchTableModel`,
  `BreakpointDialog`, `BreakpointWidget`, `CodeViewWidget`, `CodeWidget`,
  `EditSymbolDialog`, `GekkoSyntaxHighlight`, `JITWidget`,
  `JitBlockTableModel`, `MemoryViewWidget`, `MemoryWidget`, `NetworkWidget`,
  `PatchInstructionDialog`, `RegisterColumn`, `RegisterWidget`,
  `ThreadWidget` and `WatchWidget`. Every rewritten file starts with the
  original-code header and has no Dolphin copyright or SPDX line. File
  names, public declarations, signals and slots, enum values, object names,
  widgets and their layout order, labels, tooltips, enabled/visible rules,
  context menus, model columns, roles and sort/filter results, QSettings and
  Config keys (and when they are read or written), the order and arguments
  of Core/UICommon calls, and all tr() strings (byte for byte, in the same
  context) are unchanged. No file carries a PixelU change.
  - `CodeViewWidget`: rows are filled by one helper; the branch arrows are
    a private struct drawn by a private delegate, and the lane assignment,
    follow-PC and scroll logic are separate helpers. `CodeWidget`: the code
    pane and the side panes (callstack, function calls/callers, symbols,
    notes) are built by helpers; step over/out and the return check keep
    the old rules.
  - `MemoryViewWidget`: a private table class does the drawing; value text
    to bytes, typed writes, the cell menu, label edits, breakpoint marks and
    the scroll bar are helpers. `MemoryWidget`: the side bar is built from
    one helper per group box in the old order; the input and display type
    lists are tables with QT_TRANSLATE_NOOP("MemoryWidget", ...).
  - `BranchWatchDialog` and `BranchWatchTableModel`: the proxy filter keeps
    the branch kinds in a bitset and the address bounds in an array; the
    model's marking, symbol rebuild and per-role text/colour/font/alignment
    are helpers. `JITWidget` and `JitBlockTableModel`, `BreakpointWidget`,
    `WatchWidget`, `RegisterWidget`, `ThreadWidget`, `NetworkWidget` and
    the dialogs are built by small helpers in the old order.
  - Preserved quirks (callers or users may rely on them): the swapped
    highlight spans in GekkoSyntaxHighlight, the never-connected
    RequestTableUpdate signal, the Windows socket numbers in NetworkWidget,
    setChecked before setCheckable on actions, the code view context menu
    using the PC's instruction for the load/store check, a memory
    breakpoint at address 0 resetting the row flag, MemoryViewWidget
    reusing the value prototype's alignment, the unsorted data list in
    MemoryWidget, the HexString input quirk, the hex prefix being added
    while its checkbox is disabled, the unblocked setCurrentText in the
    search, and WriteValue/WriteFile calling Update while holding the
    CPU guard.
  - Deliberate differences (nothing a user sees in normal use):
    - Qt connections that had no context object now use `this`, so they
      are dropped when the widget is destroyed.
    - `BranchWatchDialog`: hiding the dialog now undoes exactly the
      connections that showing it made; the old code disconnected other
      slots, so the connections leaked and stayed live while it was hidden.
    - `BranchWatchTableModel`: the font role returns an adjusted copy
      instead of changing the stored font.
    - `BreakpointWidget` no longer leaks a menu; `CodeWidget`,
      `CodeViewWidget` and `MemoryWidget` create list items only for rows
      that pass the filter, so filtered-out items are no longer leaked.
    - `MemoryViewWidget`: Copy Value looks the cell up again by row and
      column instead of keeping a pointer to it.
    - `CodeViewWidget`: the arrow-lane assert uses `<` (the old `<=` let
      an out-of-range lane through).
    - Bounds fixes: the clamped error span in the assembler, the assembler
      loop bound, the JIT feature text lookup; `PatchInstructionDialog`
      zero-pads a short block; `AssemblyEditor` initialises its flag;
      `RegisterColumn` calls the getter once. The JIT and branch watch
      proxy models have no debug crash override.
  - Checked: old and new code side by side, with the untouched non-W41
    files on both sides (only hashes were kept):
    - `MemoryViewWidget::ConvertTextToBytes`: a harness drives all 15 types
      over 41 inputs (615 cases). Hashes identical.
    - Compared line by line instead, because they need the full Core
      (CPU thread, PPC state, symbol DB, JIT, memory, sockets): every other
      file. Each widget, menu, tooltip, connection and config key was
      checked off against the old file; a tr() string scan finds every old
      string in the new code with the same context.
  - Compile check: the W41 .cpp files and MainWindow.cpp (the only other
    source that includes W41 headers), old against new, with clang on Linux
    and for x86_64-w64-windows-gnu, `-Wall -Wextra -Wconversion
    -Wsign-compare -Wshadow-all -Wshorten-64-to-32`. No warning in a W41
    file; the remaining diagnostics come from shared headers and are the
    same as before (MainWindow.cpp: `std::expected` and the missing
    `qpa/qplatformnativeinterface.h` on Linux, the Win32 display constants
    in the MinGW headers). Integer narrowing and sign changes are written
    as explicit casts, and no local or parameter hides a Qt data member
    (`data`, `d_ptr`, `staticMetaObject`).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run is left in a W41
    .cpp file. The runs left are public declarations in the headers
    (signals, slots, member lists, enums).
  - No keys, IVs, tokens or credentials are in these files.

- W38 log: rewrote all 42 W38 files as original code: the game list
  (`GameList/GameList`, `GameListModel`, `GameTracker`, `GridProxyModel`,
  `ListProxyModel`), the cheats manager and cheat search tabs
  (`CheatsManager`, `CheatSearchWidget`, `CheatSearchFactoryWidget`),
  `ConvertDialog`, `DiscordHandler`, `DiscordJoinRequestDialog`, the GBA
  windows (`GBAWidget`, `GBAHost`), the memory card dialogs
  (`GCMemcardManager`, `GCMemcardCreateNewDialog`), `NANDRepairDialog`,
  `NKitWarningDialog`, `ResourcePackManager`, `RiivolutionBootWidget`,
  `Updater` and `WiiUpdate`. They have 8071 lines now (from 8126). Every file
  starts with the original-code header and has no Dolphin copyright or SPDX
  line. None of them carries a PixelU change (the DolphinQt PixelU change is
  in W37 files). The public names, signatures, signals, slots, enums
  (`GameListModel::Column`, the model roles) and the
  `GuiRiivolutionPatchIndex` metatype are kept; private members and helpers
  have new names, with no user outside W38.
  - Design:
    - `GameList`: the table and grid are built by `BuildTable`, `BuildGrid`
      and `BuildPlaceholder` around one `SetUpProxy`; column names, settings
      keys and resize modes come from `ColumnIndex`/`ColumnSetting`/
      `ColumnResizeMode` lookups; the context menu is `ShowGameMenu` with
      `AddMultiSelectionEntries`, `AddSingleGameEntries` and `AddTagEntries`;
      result boxes go through `ShowResultBox`; `FitColumnsToWidth`,
      `ApplyColumnVisibility`, `InvalidateProxies` and `ShowFittingPage`
      replace the inline sequences; the column-visibility labels are built
      once by a static lambda.
    - `GameListModel`: `TitleData`, `TimePlayedData`, `BannerPixmap`,
      `PadNumbersForSorting`, `PlatformFilter`/`RegionFilter`; header texts
      are a `QT_TR_NOOP` table translated with `tr(text)` (same context and
      source text); tag storage is `SetTagsOf` and `StoreTagNames`.
    - `GameTracker`: one `Post` for the queued commands and `RunTask`
      dispatching to `ReadCache`, `FirstScan`, `AddFolder`, `DropFolder`,
      `RescanFolder`, `RescanFile`; folder scans are `ScanFolder` with
      `AnyCaseExtension` and `GameFileNameFilters`; `Watch`/`Unwatch`,
      `GamesGoneFrom`, `LoadGame`.
    - `GridProxyModel`/`ListProxyModel`: the tile is drawn in one `data`
      branch with named cover and banner sizes; filtering and sorting are the
      same comparisons.
    - `CheatsManager`: `BuildTabs` with an `add_permanent_tab` lambda,
      `Connect`, `FollowGame`, `RefreshAllSearches`, `AddSearchTab`,
      `CloseTab`. `CheatSearchWidget`: `BuildLayout`, `ConnectSignals`,
      `DescribeSession`, `RunScan`/`ReportScanCounts`, `RefreshRows`,
      `ShowCurrentValue(s)`, `RebuildTable` with an `add_cell` lambda,
      `ValueSize` as an if-chain. `CheatSearchFactoryWidget`:
      `BuildAddressSpaceGroup`, `BuildDataTypeGroup`,
      `UpdateCustomRangeControls`, `CreateSession`.
    - `ConvertDialog`: `FillBlockSizes`/`AddBlockSize`,
      `FillCompressionMethods`, `OnFormatChanged`, `OnCompressionChanged`,
      `ConfirmSettings`, `Convert` with `OutputPathNextTo` and `FileOf`.
    - `GCMemcardManager`: `BuildWidgets`, `ConnectWidgets`, `SetUpSaveTable`,
      `SetUpSaveColumns`, `FillTable`, `SelectSlot`, `OpenConfiguredCards`,
      `BrowseForCard`, `ImportInto` shared by import and copy, `BannerOf`/
      `IconOf`, `FormatName` and `GetErrorMessageForErrorCode` as if-chains.
    - `GBAWidget`: `SwapRom`, `InsertCard`, `WithCore`, `PickFile`,
      `NetPlayPadOf`, `SettingKey`, `ApplyVolume`, `ScreenRect`,
      `RestoreSettings`/`StoreSettings`; the volume step is a named constant.
      `GBAHost` queues through one `QueueOnObject` per callback.
    - `DiscordHandler`: `ShowNewJoinRequest`, `CloseExpiredRequests`,
      `PollLoop`. `DiscordJoinRequestDialog`: `FetchAvatar`, `BuildLayout`,
      `Answer`.
    - `NANDRepairDialog` (`WrappedLabel`, `DescribeTitles`),
      `NKitWarningDialog`, `ResourcePackManager` (`BuildLayout`, `WireUp`,
      `FillTable`, `UpdateButtons`, `SelectedPack`, `PackIndexOfRow`,
      `ToggleInstalled`, install/uninstall/delete/move helpers, `Rescan`,
      `ShowPackFolder`, `FollowLink`), `RiivolutionBootWidget` (a `PatchFile`
      struct, `AddXMLsFromUserFolder`, `AskForXMLs`, `AddPatchFile`,
      `GuessSDRoot`, `ReadSavedChoices`/`WriteSavedChoices`, `ExportPreset`,
      `Start`), `Updater` (`AskUser`, `CloseParent`), `WiiUpdate` (a
      file-local `UpdateProgressDialog` whose `reject` does nothing,
      `RunWithProgress`, `ShowResult`, `ReportFailure`/`ReportSuccess`).
  - Kept as before (outputs, quirks):
    - Every widget, label, tooltip, objectName, layout order, enabled and
      visible rule, context menu entry, shortcut and dialog flow; the model
      columns, roles, sort and filter rules; Config and QSettings keys and
      when they are read and written; Core, UICommon, DiscIO, IOS and
      Discord calls in order and with their arguments; the Windows-only
      paths (desktop shortcuts, `QApplication::alert`); every tr() text byte
      for byte in its old context (`QObject` for the `WiiUpdate` and memory
      card format names, `QDialog` for `ResourcePackManager`, which has no
      `Q_OBJECT`). Long literals were rewrapped as adjacent strings with the
      same bytes.
    - Quirks: `ResourcePackManager` still calls `setLayout` twice and keeps
      the old up/down row handling; `GCMemcardCreateNewDialog`, the NKit
      dialog's OK enabling and the Riivolution choice files are unchanged.
  - Deliberate differences:
    - `GameTracker` members are destroyed in a different order (no
      observable effect: the worker is stopped first in both).
    - `OpenGCSaveFolder` iterates the GCI folder with `std::error_code`
      (no exception on an unreadable folder).
    - `GCMemcardManager::AnimateIcons` returns early on an empty animation
      schedule (the old code indexed it).
    - Pure queries are called a different number of times (Config reads in
      the grid proxy, `CanControlCore`, the Wii-disc check); same results.
    - `NANDRepairDialog` destroys its `TitleDatabase` when the title texts
      are built instead of at the end of the constructor.
    - `WiiUpdate` result boxes translate the text before the title (same
      strings).
    - `FormatName` asserts `format == Format::GCI` before the GCI fallback.
    - Redundant repeated `setEnabled` calls in `ConvertDialog` are gone
      (same final state).
    - Conversions are explicit (`size_t` and float casts in `GBAWidget`,
      `u8` file indices in `GCMemcardManager`, `u64` sizes in
      `ConvertDialog`, `uint` logo size in `ResourcePackManager`); a lambda
      capture and parameters were renamed so nothing shadows a member. No
      local or parameter is named after an inherited Qt member (`data`,
      `d_ptr`, `staticMetaObject`).
  - Checked:
    - Old vs new harnesses, Qt 6.4 offscreen, each built from its own W38
      files against recording fakes (Config, Settings on a temporary
      QSettings file, FileUtil, ModalMessageBox, DolphinFileDialog,
      HttpRequest, Discord, Core state, the update, NAND and card
      back-ends) that log every call; dumps of the widget tree (class,
      objectName, text, tooltip, enabled, visible, checked), tr() calls with
      their context, config and QSettings writes, opened URLs and model
      contents after seeded random interaction; one hash per case, streamed.
      - `NKitWarningDialog` (40 seeds, toggles, OK/Cancel/Esc, the skip
        setting): identical.
      - `NANDRepairDialog`: identical except the `TitleDatabase` destructor
        position in the log (above).
      - `ResourcePackManager` (synthetic packs, install, uninstall, move,
        delete, links): identical.
      - `GCMemcardCreateNewDialog` (60 seeds, sizes, encodings, file dialog
        answered or cancelled, failing creation): identical.
      - `WiiUpdate` (online and disc updates, every result, progress and
        cancel): identical except the tr order in the result boxes (above).
      - `Updater` (update or not, both restart modes, the environment
        override): identical.
      - `GameListModel` with `ListProxyModel` and `GridProxyModel`
        (synthetic game files of every platform and region, tags, search,
        scale, every column and role, sort and filter): identical, with
        Config read counting off (counts differ by design, above).
      - `RiivolutionBootWidget` (user folder XMLs, file dialog picks, saved
        choices, start and preset export): identical once Qt's own
        file-dialog tr() noise is filtered; the harness found that the
        start folder was computed after the file dialog call, fixed.
      - `CheatSearchFactoryWidget` (address spaces, data types, custom
        ranges): identical.
      - `DiscordJoinRequestDialog` (with and without avatar, download ok,
        failing or garbage; each answer, Esc, close): identical.
    - Not driven, compared line by line against the old files:
      `GameList` (view, context menus, column menu, tags, shortcuts),
      `GameTracker`, `CheatsManager`, `CheatSearchWidget`, `ConvertDialog`,
      `GCMemcardManager`, `GBAWidget`, `GBAHost` and `DiscordHandler`. Their
      Core, DiscIO, mGBA and Discord calls, menu entries, tr() texts and
      settings keys were matched one by one.
    - clang `-Wall -Wextra -Wconversion -Wsign-compare -Wshadow-all
      -Wshorten-64-to-32`, on Linux and `-target x86_64-w64-windows-gnu`,
      against the untouched tree (mGBA 0.10 headers, a minimal `wil` stub):
      the 21 W38 .cpp files show no warning from W38 code on either target
      (the old files had conversion and sign warnings), and the files
      including W38 headers (Host, HotkeyScheduler, Main, MainWindow, MenuBar, GameCubePane, the NetPlay
      dialogs) show the same totals on both trees. The remaining errors are
      outside W38 and the same on the base tree (`ntohs` in `NetPlayDialog`
      on Linux; `DEVMODE`/`CDS_*`/`ES_CONTINUOUS` in `MainWindow` and
      `SendMessageW` in `NetPlayDialog` with mingw). MSVC was not run.
    - `verbatim-audit.py wii 8`: no run of 8 or more lines in any W38 .cpp
      file (longest left: 7). Remaining runs are in headers only
      (declarations: `GameList.h`, `GameListModel.h`, `GBAWidget.h`,
      `GCMemcardManager.h`, `DiscordHandler.h`, `CheatsManager.h`,
      `CheatSearchWidget.h`, `RiivolutionBootWidget.h`, `GameTracker.h`).
    - No key, IV, token or credential bytes in W38 files.

- B1 log (branding, art, licensing, version): PixelU-Wii's icon, pictures,
  dark style, licence notice and version are its own now; none of them is
  taken from Dolphin any more. Done from HEAD f2330ef on
  `claude/wiiu-emulator-urpcen`; only the files below were touched.
  - Art (`pixelu/wii/art/make_art.py`, new): draws everything from shapes
    with Pillow, reads no image. `--check` redraws in memory and compares
    with the tree (pixels for PNG/ICO, bytes for SVG).
    - Program icon `Source/Core/DolphinQt/PixelU-Wii.ico` (new): a pixel-art
      disc with a pink spark on a stepped navy tile; frames 16, 20, 24, 32,
      40, 48, 64, 96, 128, 256 (16-24 drawn on a 16 grid, the rest on a 32
      grid), PNG-compressed entries, 18 KB (Dolphin.ico was 230 KB).
    - `pixelu/wii/Data/Sys/Resources/` (new, 69 PNG): every picture of
      Dolphin's Sys/Resources with the same name and pixel size:
      `dolphin_logo` (the app icon art at 200/400; name kept because
      `Resources.cpp` and the Discord asset key use it), 14 flags at
      1x/2x/4x, the five platform icons (a plain violet cube for GameCube,
      an arcade cabinet for Triforce, a motion remote for Wii, a channel
      tile for WAD, a page for files; no Nintendo logos), `nobanner`, the
      four achievement placeholders and the three file-system icons.
      `OSD_Font.ttf` and its copyright file are Bitstream's and still come
      from Dolphin's Sys.
    - Dark style (`Source/Core/DolphinQt/Styles/Dark/`): `dark.qss`,
      `dark.qrc` and the 25 SVGs rewritten. Same file names and resource
      prefix (`:/dolphin_dark_win`, which `Settings.cpp` loads); own palette
      (window #1b1e24, accent #3fa9f5), rounded check boxes and buttons,
      accent bar on the selected tab. Still no `QToolTip` rule, so
      `Settings.cpp` keeps adding its own; `QGroupBox` is styled, as before.
  - Resource scripts and manifests: `DolphinQt.rc` and `DolphinTool.rc`
    have one icon each (`IDI_ICON1`, id 101 from the existing
    `resource.h`) pointing at `PixelU-Wii.ico` (the tool uses
    `../DolphinQt/PixelU-Wii.ico`). The unused second icon named "dolphin"
    is gone: no code loads it. `DolphinQt.manifest` keeps Windows 10+ and
    `longPathAware`; `DolphinTool.exe.manifest` keeps common controls 6 and
    Windows 10+ with identity `RetroPixels.PixelUWii.Tool`.
  - Version: `Source/Core/Common/PixelUWiiVersion.cmake` (new) replaces
    Dolphin's `ScmRevGen.cmake`/`dolphin_scmrev` for `common`. At configure
    it finds RetroPixels' `main.go` by walking up from the build tree and
    reads `appVersion` (2.21.0); cache `PIXELU_WII_VERSION` and
    `PIXELU_WII_ROOT` override. The target `pixelu_wii_version` runs the
    same file with `-P` on every build and writes `Common/scmrev.h` from the
    new `scmrev.h.in` (copy_if_different, so nothing recompiles when
    nothing changed):
    - `SCM_DESC_STR` = 2.21.0, `SCM_REV_STR` = RetroPixels' commit hash
      (the version when git is missing), `SCM_BRANCH_STR` = RetroPixels'
      branch (`GITHUB_REF_NAME` when detached, else "unknown"),
      `SCM_COMMITS_AHEAD_MASTER` 0, distributor "RetroPixels", update track
      empty.
    - New `SCM_COMPAT_DESC_STR` = "2609a-pixelu" and
      `Common::GetCompatDescStr()`: the HTTP user agent ("Dolphin/2609a-
      pixelu", as before) and the RetroAchievements integration DLL keep
      the string they had, because those servers compare client versions
      with Dolphin release numbers (2.21.0 would read as an ancient
      Dolphin). This is a compatibility constant, not read from Dolphin.
    - `GetScmRevStr()` (shown by `--version`, savestate "made by", shader
      dumps) says "PixelU-Wii 2.21.0" instead of "Dolphin 2609a-pixelu".
      `GetEmulatorName()` is still "Dolphin" (netplay handshake, user
      agent).
    - Titles: `Core.cpp` "PixelU Wii 2.21.0 | ..." (was "PixelU Wii
      (Dolphin 2609a-pixelu) | ..."), `MainWindow.cpp` idle title "PixelU
      Wii 2.21.0", `AboutDialog.cpp` "Version 2.21.0 (based on Dolphin)".
    - Netplay peers, the lobby browser filter and `/dev/dolphin` now see
      2.21.0; peers must run the same PixelU-Wii build, as before.
    - Other templates rewritten with the same keys and placeholders:
      `build_info.txt.in` (unused while the updater is off),
      `AchievementApprovedHash.h.in` (still filled by Dolphin's
      `GenerateAchievementHash.cmake`, B3), `target_qt.conf.in` (the same
      23 keys), `qtpaths.bat.in`, `qt.conf.win` (a `;` comment line added;
      Qt reads the same `Paths/Plugins`).
  - Licensing: `pixelu/wii/COPYING` rewritten: RetroPixels' original code
    and art (files with the original-code line, make_art.py output), with
    no licence chosen and none granted by the notice; the executable as a
    whole is distributed under GPL-2.0-or-later while Dolphin code and GPL
    components are in it; what is still Dolphin's code; "Dolphin community
    data (GPL-2.0-or-later), shipped unchanged": `Sys\GameSettings\`,
    `Sys\ApprovedInis.json`, `Languages\` (owner decision 2026-10-21); the
    rest of Dolphin's Sys and GameTDB's title lists; a table of the
    third-party components with their licences. `pixelu/wii/LICENSES`
    gains the SPDX texts (from spdx/license-list-data) of LGPL-2.1-or-later,
    LGPL-3.0-only, GPL-3.0-only, Apache-2.0, Zlib, ISC, BSD-2-Clause,
    BSD-1-Clause, MPL-2.0, curl, bzip2-1.0.6, 0BSD and Bitstream-Vera; the
    four existing texts are unchanged.
  - Workflow (`.github/workflows/pixelu-wii.yml`):
    - "Use PixelU's Source" also copies `pixelu/wii/Data/Sys` over the
      submodule's `Data/Sys` and replaces its `COPYING` and `LICENSES`, so
      the build folder's links match the package.
    - "Apply PixelU patches" no longer commits and tags the submodule (only
      needed for "2609a-pixelu"); `VERSION.txt` uses the submodule's HEAD
      and names the PixelU-Wii version from `main.go`.
    - "Package" copies Sys from the submodule and then `pixelu/wii/Data/Sys`
      over it, `COPYING`, `license.txt` and `Licenses` from `pixelu/wii`,
      and each Externals project's own licence files (LICENSE*, COPYING*,
      NOTICE*, *copyright*, up to two folders deep; not gtest, MoltenVK,
      libadrenotools, gettext) to `Licenses\Externals\<project>\`.
    - "List package contents" fails unless `COPYING`,
      `Licenses/LGPL-3.0-only.txt`, `Sys/Resources/dolphin_logo.png` and
      `nobanner.png` are byte-identical to `pixelu/wii` and
      `Sys/GameSettings` is there. The smoke test warns when `--version`
      does not name PixelU-Wii.
    - Unchanged: `PixelU-Wii.exe`, the `PixelU/wii` layout, the DLLs,
      `qt.conf`, `QtPlugins`, `Languages`, `portable.txt`.
  - Checked:
    - `make_art.py --check`: 95 files, all match. The 69 Resources have
      the same names and pixel sizes as Dolphin's and none is
      byte-identical to one of Dolphin's.
    - `llvm-rc` compiles both .rc files (the tool's path uses `/`, which
      rc.exe accepts too); `xmllint` accepts both manifests; Pillow reads
      the 10 ICO frames back.
    - Qt 6.4 offscreen (libqt6svg6 installed for the check): a gallery of
      every styled widget (menus, check boxes and radio buttons in all
      states, combo, spin box, buttons, tool button menu, progress, slider,
      tab scroll arrows, sortable table with header and corner, checkable
      list) built against the old and the new `dark.qrc`: all pictures
      load, the roles match. QSettings reads `qt.conf.win`
      (`Paths/Plugins=./QtPlugins`) and the 23 keys of `target_qt.conf.in`.
    - Version: a stub tree (top level with the `Source/Core` include path,
      a `Common` dir with the real `Version.cpp` and the new cmake file)
      configured with Ninja: from a scratch tree with a test `main.go`
      (9.8.7, no git) and with `PIXELU_WII_ROOT` = the worktree (2.21.0,
      real hash and branch); every `Common::Get*` value printed as above;
      a second build only reruns the header target.
    - Shared lines with Dolphin's old files (whitespace-normalised): COPYING
      none; manifests and conf templates only the schema lines (keys,
      GUIDs, element names); `dark.qss` 12 lines in a row, the check box
      state-to-picture rules (Qt selector syntax plus the kept file names).
    - No key, firmware, BIOS, DSP ROM or game added.
  - Left as Dolphin's, and why:
    - `Sys/Themes` (the five Clean icon themes) and the rest of Sys apart
      from Resources: outside B1 (Sys data decision pending, B2).
    - `Languages` and `Externals/gettext`: kept by the owner's decision.
    - `DolphinTool/DolphinTool.filters` (an unused Visual Studio filters
      file) and the `.natvis` files: not in B1's file list.
    - `CMake/GenerateAchievementHash.cmake`, `DolphinSymlinkOrCopy.cmake`
      and the top-level `dolphin_scmrev` target (still defined, never
      built): Dolphin's build system, B3.
    - Qt and FFmpeg binaries from `dolphin-emu/ext-win-qt` /
      `ext-win-ffmpeg`: B4.
    - The About box text ("lightly modified build of Dolphin", links to
      Dolphin's COPYING and forums) and `bundle-test.yml`'s Dolphin 2609a
      line are wording outside B1.
    - `docs/pixelu-wii-original.md` still says `pixelu/wii/COPYING` is
      Dolphin's; this log is the update.
  - CI must confirm: rc.exe takes the PNG-framed ICO and the exe shows the
    new icon; the VS generator builds `pixelu_wii_version` before `common`
    and `GITHUB_REF_NAME` reaches it; `--version` prints "PixelU-Wii
    2.21.0"; the PowerShell overlay and licence-copy loops (not run here:
    no PowerShell in this sandbox) and the new package checks pass; the
    dark style looks right on real Windows dark mode.

- W40 log: rewrote all 84 W40 files, the DolphinQt controller mapping
  window and its pages plus the settings panes (10787 lines now, from
  11333): every file under `Config/Mapping/` (`MappingWindow`,
  `MappingWidget`, `MappingButton`, `MappingNumeric`, `MappingCommon`,
  `MappingIndicator`, `IOWindow`, `GCPadWiiUConfigDialog` and the
  GameCube, GBA, keyboard, microphone, Wii Remote, extension, motion,
  hotkey and free-look pages) and every file under `Settings/`
  (`AdvancedPane`, `AudioPane`, `BroadbandAdapterSettingsDialog`,
  `GameCubePane`, `GeneralPane`, `InterfacePane`, `OnScreenDisplayPane`,
  `PathPane`, `TriforcePane`, `USBDevicePicker`, `WiiPane`). Every
  rewritten file starts with the original-code header and has no Dolphin
  copyright or SPDX line. File names, public declarations, signals and
  slots, enum values, object names, widget order and layout, labels,
  tooltips, enabled/visible rules, context menus, every config key a
  control reads or writes (and when), the order and arguments of
  Core/UICommon/InputCommon calls, and all tr() strings (byte for byte, in
  the same translation context) are unchanged.
  - Translation contexts kept exactly: the classes without Q_OBJECT
    (`MappingDouble`/`MappingBool`, the mapping processor,
    `CalibrationWidget`, `IPRedirectionsDialog`, `OnScreenDisplayPane`, the
    IOWindow delegate and line edit) still have none, and the explicit
    `QWidget::tr("%1%")` and `QObject::tr("<System Language>")` stay.
  - Indicators paint with the same arithmetic and expression grouping, so
    the pixels are identical; `MixedTriggers` still reads threshold and
    dead zone before the state.
  - Preserved quirks: the IOWindow highlighter's
    `start >= block_start + block.length()` test, the VBI checkbox being
    set without a signal blocker, skipped IP redirection rows still saving
    empty entries, `m_lock_cursor` being reparented before the layout is
    set, and the repeated `AutoUpdateChecker::SystemSupportsAutoUpdates()`
    calls in GeneralPane (build, read, tooltips, wiring, write).
  - Deliberate differences (nothing a user sees in normal use):
    - Qt connections that had no context object now use `this`, so they
      are dropped when the widget is destroyed.
    - `MappingWindow`: the "Successfully deleted" message box that was
      built and never shown is gone, and the "Extension Motion Input" /
      "Extension Motion Simulation" tab names are only looked up for Wii
      Remote windows (same texts).
    - `USBDevicePicker`: after a refresh the selection is restored by
      comparing device info under a signal blocker, instead of through an
      item pointer that `clear()` had already freed; the unused
      `DEVICE_REFRESH_INTERVAL_MS` constant and the `WiiPane.h` include are
      gone.
    - Redundant no-op calls are dropped: a second `setLayout` on the mouse
      group box (InterfacePane), repeated `setPen` calls before a pen was
      used (MappingIndicator), and `ReshapableInputIndicator::Update` calls
      that did nothing.
    - `GameCubePane` translates each device name once instead of once per
      slot (same strings).
    - Float/bool tests in MappingIndicator are written as explicit
      comparisons, and integer-to-float conversions as explicit casts.
  - Checked: old and new code side by side, built with Qt 6.4 offscreen
    against fakes that record every call, using the untouched non-W40
    files on both sides (real Common::Config, ConfigControls, ToolTip
    controls, InputCommon ControllerEmu/ControlReference/ControllerInterface,
    InputConfig and the real GC pad, keyboard, GBA pad, hotkey, free-look
    and emulated Wii Remote controllers; only hashes were kept):
    - Settings panes: every pane, the broadband adapter dialog in all five
      modes and the USB device picker, driven with seeded random clicks,
      combo/slider/spin/text/table/list edits, emulation state changes,
      NetPlay toggles and pane rebuilds, with every dialog they open
      answered and poked (10 seeds of 150 steps per pane, dialog and
      picker; the BBA dialog with 10 opens per seed; 5 seeds of 120 steps
      without the optional defines; all identical). Compared: widget tree after
      every step, config changes as they happen, fake calls in order, and
      the tr() lookups. Also built without Cubeb, Discord, analytics and
      mGBA.
    - Mapping: every MappingWindow type and port on a virtual clock (Qt
      timers, input detection and indicator animation only advance when
      the driver moves time), with fake input devices, random input
      changes, device hotplug, left/middle/right clicks on mapping
      buttons, the IOWindow and the profile menu, and a hash of every
      indicator's pixels after each step; plus the Wii U adapter dialog
      (all 11 window types for seeds 1-2 at 40 steps and seeds 3-6 at 60
      steps, the Wii U dialog for 6 seeds; all identical apart from the
      dropped "Successfully deleted" box).
    - Compared line by line instead, because the harness builds for Linux
      with Qt 6.4: the `_WIN32` branches (WASAPI device in AudioPane, tap
      server texts in the BBA dialog, the light/dark styles and the cursor
      lock in InterfacePane) and the Qt 6.5/6.7 branches (RTC time zone in
      AdvancedPane, `checkStateChanged` in MappingNumeric and
      GameCubePane). A tr() literal scan over all `#ifdef` branches finds
      every old string in the new code; the only strings gone are the two
      of the unshown "Successfully deleted" box.
  - Compile check: the W40 .cpp files and the DolphinQt sources that
    include W40 headers (MainWindow, Settings window, GameCube and Wii
    Remote controller widgets, free-look widget, NetPlay dialog, GBA
    widget), old against new, with clang on Linux and for
    x86_64-w64-windows-gnu, `-Wall -Wextra -Wconversion -Wsign-compare
    -Wshadow-all -Wshorten-64-to-32`, with and without HAVE_CUBEB,
    HAS_LIBMGBA, USE_DISCORD_PRESENCE, USE_ANALYTICS and HAVE_CPPIPC. The
    rewritten files are clean except for the existing sign-conversion
    warning in `ConfigChoice.h:78`, which the old build has too; the
    dependents have the same diagnostics as before. No local or parameter
    is named after an inherited Qt member (`data`, `d_ptr`,
    `staticMetaObject`), which MSVC reports as C4458.
  - Verbatim audit (`verbatim-audit.py wii 8`): no run is left in a W40
    .cpp file. The runs left are public declarations in the headers.
  - No keys, IVs, tokens or credentials are in these files.

- W39 log: rewrote all 108 W39 files in `Core/DolphinQt/Config` (50 in
  the folder itself, ConfigControls 17, ControllerInterface 10, Graphics
  14, SDLHints 2, ToolTipControls 15; 11734 lines now, from 12387). Every
  rewritten file starts with the original-code header and has no Dolphin
  copyright or SPDX line. File names, class names, public declarations,
  signals and slots, objectNames, widgets and their layout order, labels,
  tooltips, menus and actions, enabled/visible rules, every config key and
  when it is read or written, the Core/UICommon calls (order and
  arguments) and every tr() text (byte for byte) are unchanged, as is the
  Windows behaviour. No files outside W39 were touched; CMake lists and
  project files were not touched.
  - Config: the properties dialog and its pages (Info, Filesystem,
    Verify, Game Config with its INI editor and highlighter, Graphics
    Mods, Patches, AR and Gecko codes with the code editor, new-patch
    dialog and the cheat/hardcore/graphics-mod warnings), the settings
    window, the controllers pane (GameCube ports, Wii Remotes with
    Bluetooth passthrough and real Wii Remotes, common controllers),
    the free-look window, the controller interface window, the log
    widgets, the DSU (DualShock UDP) settings and the mapping-related
    helpers.
  - ConfigControls / ToolTipControls: the config-bound bool, choice,
    radio, integer, slider, float slider and combo controls (same
    layer-aware reads and writes, same bold-on-override rule) and the
    tooltip wrappers with the balloon tip.
  - Graphics: the graphics pane (General, Enhancements, Hacks, Advanced),
    the color correction and post-processing windows.
  - SDLHints: the SDL hints window.
  - Keys and credentials: none found in W39; nothing was added.
  - Deliberate differences (crashes, undefined behaviour or unbuilt
    platforms; nothing a user sees in a working run):
    - ToolTipWidget::timerEvent no longer dereferences an empty optional.
    - BalloonTip ends its painters before setMask.
    - PostProcessingConfigWindow: ownership of its configuration is
      decided at construction; an option whose parent is unknown becomes
      top-level (the old code dereferenced null); grandchild options
      without widgets are null-checked; connections use the dialog as
      context.
    - HacksWidget connections have `this` as context.
    - ConfigSliderU32 drops a redundant setValue; ConfigInteger uses
      setRange.
    - CheatCodeEditor copies the creator capture before reassigning the
      name (the old code read a dangling sub_match).
    - LogWidget's Clear takes the queue mutex.
    - VerifyWidget reads GetBytesProcessed once per loop iteration.
    - The u16 casts in GraphicsModListWidget and WiimoteControllersWidget
      are explicit; SettingsWindow uses static_cast<int> instead of
      std::to_underlying.
    - The DSU dialog sets each field's text before its validator.
    - PropertiesDialog creates its pages in separate statements.
    - The unused m_*_groups arrays were dropped.
    - The Apple-only branches are not kept (no such build in this tree).
  - Checked: old and new code side by side, offscreen against Qt 6.4,
    with the base versions of every non-W39 file on both sides (pristine
    Common/Config) and logging fakes for Core, UICommon, DiscIO-free
    services, VideoBackend, input and Bluetooth. Each run pokes visible,
    enabled widgets in a seeded random order (clicks, toggles, combo and
    slider changes, typed text, context menus, table edits, modal dialogs
    answered both ways) mixed with outside events (emulation state,
    config changes, running game), and dumps the widget tree (texts,
    tooltips, enabled/visible, checked, values, objectNames), every tr()
    lookup, every config write and every fake call after each step.
    Old and new outputs were compared in memory; all equal:
    - ctl (config and tooltip controls, balloon tip): 30 seeds x 100 steps.
    - dsu (DSU settings, widget, dialog, validator; SDL hints): 40 x 80.
    - cht (AR, Gecko, patches, code editor, new-patch dialog, warnings):
      40 x 100.
    - pad (GameCube ports, common controllers, free look, graphics-mod and
      hardcore warnings, controller interface window): 40 x 80.
    - wii (Wii Remotes widget with passthrough, scanning thread): 40 x 60.
    - gfx (graphics pane, four tabs, color correction and post-processing
      windows): 30 x 60.
    - gcw (game config widget with its graphics tabs and INI editor):
      30 x 50.
    - gml (graphics mods list): 40 x 80.
    - gcf (game config editor and highlighter, log widget and log config):
      30 x 60.
    The harness found and this batch fixed two order differences: the
    cheat warning's tr()/GetCheatsEnabled order and the Graphics tooltip
    tr() order (AdvancedWidget backend name, HacksWidget help text).
  - Compile check: the W39 .cpp files are clean with clang `-Wall -Wextra
    -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32`, on Linux
    and for x86_64-w64-windows-gnu (with UNICODE); C4267, C4389 and C4458
    map to the clang flags (none); no bitwise `~` on narrow types (C4319)
    and no bool mixed with integers (C4805), checked by search; no locals
    named after inherited Qt members. Non-W39 files including W39 headers
    compile as on the base tree. MSVC itself was not run.
  - Verbatim audit (`verbatim-audit.py wii 8`): no run of 8 or more lines
    is left in any W39 .cpp file. The runs left in W39 headers are class
    declarations and member lists (BalloonTip, EnhancementsWidget,
    ConfigChoice, ConfigSlider, ConfigInteger, GraphicsModListWidget,
    ColorCorrectionConfigWindow, PostProcessingConfigWindow,
    PropertiesDialog, SettingsWindow, GeckoCodeWidget, ARCodeWidget,
    PatchesWidget, DualShockUDPSettings, FilesystemWidget, CheatCodeEditor,
    GraphicsPane, GeneralWidget).
  - Not driven, compared line by line only: InfoWidget, FilesystemWidget
    and VerifyWidget (they need a full DiscIO volume and file system),
    PropertiesDialog, SettingsWindow and ControllersPane (containers of
    the driven pages), the _WIN32-only paths (Wii Remote pair/forget via
    RunWithSpinner, borderless fullscreen) and the removed Apple branches.

- B3 log (build system and Externals glue): PixelU-Wii now builds from
  `pixelu/wii` with its own CMake. No Dolphin-written build file is used;
  only third-party sources, the prebuilt Qt and FFmpeg, the Sys data and the
  translations still come from the Dolphin 2609a submodule. Work started
  from HEAD b94f740 (B1 merged) on `claude/wiiu-emulator-urpcen`. Only the
  files below were touched.
  - Top level (new): `pixelu/wii/CMakeLists.txt`, `CMakePresets.json`,
    `.gitignore` (`/build/`).
    - Windows x64 with MSVC only: configure stops for any other system or
      CPU, for MSVC older than 19.32 and for toolsets older than v143.
    - Third-party code is read from the cache path `PIXELU_WII_THIRD_PARTY`
      (default `dolphin/Externals`). Sys data and `Languages/po` are read
      from `PIXELU_WII_DOLPHIN_DATA` (default `dolphin`). Configure checks
      that both paths exist.
    - Options: the Windows ones Dolphin had (Qt, CLI tool, Vulkan, cubeb,
      SDL, LLVM, LTO, frame dumps, UPnP, Discord, mGBA, RetroAchievements,
      RAIntegration, FASTLOG). `ENABLE_AUTOUPDATE` and `ENABLE_ANALYTICS`
      default OFF. This is what `patches/0002` did, so `pixelu/wii/patches/`
      is deleted.
    - MSVC settings are the same as before:
      - compiler options: C++23, /EHsc, /GR- /W4 /WX, the same /wd and /w1
        list, /utf-8, /external, /experimental:deterministic,
        /Gy /Oi /GS, the /Zc set, /volatile:iso and /fp:precise;
      - linker options: /NXCOMPAT /DEBUG /OPT:REF /OPT:ICF;
      - optimised builds use "/O2 /DNDEBUG /Z7" (`CMake/PixelUFlagsOverride.cmake`);
      - _ARCH_64/_M_X86_64, the analytics/autoupdate/FASTLOG defines and
        HAVE_FFMPEG;
      - the program goes to `Binaries`, PDBs to `pdb`, and `dolphin-emu`
        is the VS startup project.
    - The third-party libraries are added in Dolphin's order, each followed
      by the same directory-wide define or include (HAS_VULKAN, HAS_OPENGL,
      HAVE_CUBEB, __LIBUSB__, HAVE_SDL3, USE_UPNP, HAVE_CPPIPC, the ed25519,
      discord-rpc, wil, OpenAL and picojson include folders). Their binary
      folders are in the same places (`build/Externals/<name>/...`).
    - Version: `Source/Core` is added after `${PROJECT_BINARY_DIR}/Source/Core`
      goes on the include path. B1's `PixelUWiiVersion.cmake` (included by
      Common) builds `pixelu_wii_version` before `common`. There is no
      `dolphin_scmrev` target any more.
    - The preset `windows-x64-release` is the only one, and it is the one CI
      uses: "Visual Studio 18 2026", x64, toolset v145, Release, binary
      folder `build/windows-x64-release`, with the toolchain file below.
  - CMake modules (new, `pixelu/wii/CMake/`):
    - `PixelUHelpers.cmake`:
      - `pixelu_add_sources` ("IN <folder>" source groups);
      - `pixelu_library`;
      - `pixelu_use_pch`;
      - `pixelu_drop_option` (replaces remove_cxx_flag_from_target);
      - `pixelu_third_party_target` (drops the project's -W and /W options
        and adds /W0; replaces dolphin_disable_warnings);
      - `pixelu_after_build_link`.
    - `PixelUWindowsToolchain.cmake`: system Windows, x86_64, find-root
      modes. It sets `QT_HOST_PATH` to the bundled Qt (ARM64 when the host
      is ARM64).
    - `FindFFmpeg.cmake`: own finder with the same results (FFmpeg::<lib>
      targets with their links to each other, FFmpeg_FOUND and the
      per-library variables). `FFMPEG_DIR` limits the search to that folder.
    - `PixelULinkOrCopy.cmake`: makes a symlink, or copies when it can't.
    - `PixelUAchievementHash.cmake`: puts the SHA-1 of the json into the
      header template.
  - Externals (new, `pixelu/wii/Externals/<name>/CMakeLists.txt`, 36 folders):
    - Through the library's own upstream CMake, with the same options
      Dolphin set: fmt, glslang, pugixml, enet, zstd, zlib-ng, minizip-ng
      (bzip2/lzma/zlib/zstd taken from here, no pkg-config), lz4, libspng,
      SDL, miniupnpc, curl (Schannel, HTTP only), mGBA (library only),
      cpp-ipc.
    - From source lists: bdisasm, cpp-optparse, imgui, implot,
      spirv_cross, tinygltf, xxhash, bzip2, liblzma, LZO, FreeSurround,
      ed25519, hidapi, SFML (system and network), rcheevos (no
      libretro/ROM/zip/encrypted hashing, RAIntegration bridge), and
      discord-rpc.
    - Windows-only builds, replacing what Dolphin had changed in the
      upstream CMake:
      - cubeb: WASAPI and WinMM backends with the bundled speex resampler,
        and the same generated `exports/cubeb_export.h` (byte-identical).
      - libusb: the Windows backends with libusb's msvc config;
        `LibUSB::LibUSB` is a global interface target.
      - mbedtls: mbedcrypto, mbedx509 and mbedtls as three static libraries
        linked to ws2_32, with `MbedTLS::` aliases.
    - Header-only or interface targets: expr, watcher, FatFs.
    - Own config headers:
      - `imgui/imgui_user_config.h`: IM_ASSERT goes to ASSERT, no demo
        windows. Its folder comes before Dolphin's in the include path.
      - `implot/implot_force_include.h`: `std::isnan` for implot, forced
        in with /FI.
      - `liblzma/config.h`: a config for MSVC x64, written fresh.
  - Source CMakeLists: all 19 that were Dolphin's are rewritten:
    - Source, PCH, Core, Common, Core/Core, DiscIO, InputCommon, UICommon,
      VideoCommon, AudioCommon, UpdaterCommon, DolphinTool, DolphinQt;
    - VideoBackends and its D3D, D3D12, D3DCommon, OGL and Vulkan.
    Null and Software were already original and are unchanged.
    - They have the same targets, sources, definitions and link libraries.
      The WIN32 and _M_X86_64 source blocks are merged into the main lists.
      Non-Windows branches are dropped (Apple, Android, OpenBSD, Haiku,
      Linux, libudev, X11, Iconv, the -W flag checks). The optional parts
      stay behind their options or targets.
    - PCH: an own version of the shared-PCH scheme. `build_pch` makes
      dolphin.pch with /Yc. `use_pch` gives /Yu /FI and
      `force_build_pch`. Debug information is embedded (/Z7) through the
      CMP0141 property.
    - Core: `AchievementApprovedHash.h` is made from the shipped
      `Sys/ApprovedInis.json`: PixelU-Wii's `Data/Sys` copy if there is
      one, else the submodule's.
    - DolphinQt:
      - Qt comes from the third-party folder; windeployqt runs through
        qtpaths.bat and target_qt.conf, with the same options.
      - Translations are built with msgfmt from the submodule's
        `Languages/po`.
      - /GR- is dropped and /GR added.
      - After the build, `Binaries/Sys` is assembled as a copy: the
        submodule's `Data/Sys` with `pixelu/wii/Data/Sys` laid over it.
      - `Binaries/COPYING`, `Binaries/Licenses` and `Binaries/qt.conf` link
        to pixelu/wii's own files.
      This replaces the workflow's overlay into the submodule.
  - Workflow (`.github/workflows/pixelu-wii.yml`):
    - Configures with `cmake --preset windows-x64-release` and builds with
      `--target dolphin-emu`, both in `pixelu/wii`. The binaries are in
      `pixelu/wii/build/windows-x64-release/Binaries`.
    - The steps that swapped Source/ in, overlaid Sys/COPYING/LICENSES onto
      the submodule and applied the patches are gone. The package copies
      `Sys` from the build.
    - The VERSION.txt line now says the build uses PixelU-Wii's own CMake
      and takes the third-party libraries, Sys and translations from
      Dolphin 2609a.
    - Unchanged: the submodule fetch, the artifact name
      `PixelU-Wii-windows-x64`, the package layout, `PixelU-Wii.exe`, the
      VC++ runtime copy, and all of B1's steps (COPYING, license.txt,
      Licenses, Licenses/Externals, the hash checks, the Sys/GameSettings
      check, the --version warning).
  - Checked (Linux, configure only): the old tree (B1's Source in Dolphin's
    build, patch 0002 applied) and the new tree were both configured with
    clang-cl as an MSVC-style compiler, Ninja, the same Externals and stub
    Qt/FFmpeg. CMake's file API and compile_commands.json were compared.
    - Same targets. Only two are gone: `dolphin_scmrev` (as asked) and
      mbedtls's helper target `lib`.
    - Every library and program has the same source list. cubeb also
      builds cubeb_wasapi.cpp and cubeb_winmm.c: upstream turns them on by
      looking for audioclient.h and mmsystem.h, which MSVC has and the test
      headers don't. avrt.lib and ksuser.lib come in through cubeb for the
      same reason.
    - Link lines: the same libraries, apart from the output paths of
      mbedtls/discord-rpc.
    - Compile commands of PixelU-Wii's own sources: identical, include
      order included, except for these differences:
      - DATA_DIR is now ".../share/pixelu-wii/";
      - `Externals/imgui` (the config header) is added;
      - -msse2 and -fdiagnostics-color are no longer added. Dolphin added
        each one only if the compiler accepted it: -msse2 only with
        clang-cl, since cl.exe rejects it (and SSE2 is the x64 baseline
        anyway), and -fdiagnostics-color only under Ninja.
    - Third-party differences change only warnings or are MSVC no-ops:
      - discord-rpc no longer gets /Wall and its /wd list, or its empty
        RAPIDJSON include;
      - cpp-optparse no longer gets -fexceptions (cl ignores it; /EHsc is
        on);
      - mbedtls no longer gets upstream's GCC/Clang -W set (with MSVC it
        gets /O2 /DNDEBUG as before);
      - liblzma no longer gets _LARGEFILE64_SOURCE.
    - liblzma, all 72 files preprocessed with the old and the new config.h:
      - 70 give identical output.
      - `tuklib_exit.c` and `tuklib_mbstr_width.c` differ only because
        Dolphin's config.h, a Linux one, defines _GNU_SOURCE,
        HAVE_MBRTOWC and HAVE_WCWIDTH. Under MSVC those make no
        difference, and liblzma calls neither file.
    - Verbatim check against every Dolphin build file (top level, CMake/,
      the Externals wrappers and config headers, the old Source lists): no
      comment and no multi-line block is shared. The lines that match are
      single statements that any build of these libraries must contain:
      upstream option names (`set(SDL_TEST_LIBRARY OFF)`), alias names
      (`add_library(zstd::zstd ALIAS libzstd_static)`), target link
      lines, find_package calls, and #define lines of the liblzma config
      that name xz's own features.
  - CI must confirm (none of this can be run off Windows):
    - MSVC v145 compile and link with the VS 2026 generator, including the
      shared PCH;
    - that upstream cubeb's WASAPI and WinMM detection matched what is now
      always on;
    - windeployqt through qtpaths.bat;
    - the msgfmt translations;
    - that the Sys copy, COPYING and Licenses in Binaries end up in the
      package with B1's hashes.
    LLVM is not expected on the runner, so HAVE_LLVM stays off as before.
  - Still from Dolphin's submodule, and why:
    - Prebuilt binaries, kept as they are per the brief: Qt 6.8.3
      (`Externals/Qt`, from dolphin-emu/ext-win-qt), FFmpeg
      (`Externals/FFmpeg-bin`, static .lib files from
      dolphin-emu/ext-win-ffmpeg) and gettext's `msgfmt.exe` (a build tool,
      not shipped).
    - Third-party sources that Dolphin vendors with its own changes, used
      unchanged as third-party code: Bochs_disasm (with its config.h),
      FatFs (with ffconf.h), FreeSurround, SFML, discord-rpc, ed25519, LZO,
      xxhash, bzip2, liblzma and the others in Externals/.
    - `Data/Sys` (game INIs, shaders, fonts, ApprovedInis.json; B1's own
      pictures are laid over it) and `Languages/po`. These are data, not
      build files, and stay until their own batch.
    - `.github/workflows/bundle-test.yml` still says PixelU-Wii is Dolphin
      "with the changes in pixelu/wii/patches". That file is not part of
      this batch, so the line is left for whoever owns it.

- W42 log: rewrote all 62 W42 files, the DolphinQt NetPlay, TAS, FIFO,
  achievements, portal and emulated-USB windows (10376 lines now, from
  10093): `Achievements/*` (`AchievementBox`, `AchievementHeaderWidget`,
  `AchievementLeaderboardWidget`, `AchievementProgressWidget`,
  `AchievementSettingsWidget`, `AchievementsWindow`), `LogitechMicWindow`,
  `WiiSpeakWindow`, `FIFOAnalyzer`, `FIFOPlayerWindow`,
  `InfinityBaseWindow`, `ChunkedProgressDialog`, `ClickBlurLabel`,
  `GameDigestDialog`, `GameListDialog`, `NetPlayBrowser`, `NetPlayDialog`,
  `NetPlaySetupDialog`, `PadMappingDialog`, `SkylanderModifyDialog`,
  `SkylanderPortalWindow` and `TAS/*` (the GC, GBA and Wii input windows,
  `TASInputWindow`, the stick and IR widgets, and the check box, slider,
  spin box and control state). Every rewritten file starts with the
  original-code header and has no Dolphin copyright or SPDX line. File
  names, public declarations, signals and slots, enum values, object names,
  widgets and their layout order, labels, tooltips, shortcuts,
  enabled/visible rules, context menus, model columns, roles and
  sort/filter results, QSettings and Config keys (and when they are read or
  written), the order and arguments of Core/UICommon calls, dialog flow and
  all tr() strings (byte for byte, in the same context) are unchanged. No
  file carries a PixelU change.
  - TAS: `TASInputWindow` polls through private helpers per kind (button,
    ranged, scaled); the input overrider keys its table by control. The GC,
    GBA and Wii windows are built from button tables, and the Wii window
    builds its IR, motion, classic trigger and button boxes in helpers in
    the old order. `TASControlState` keeps a generation/value snapshot.
  - `LogitechMicWindow` and `WiiSpeakWindow` build each group box in a
    helper. `InfinityBaseWindow` adds one row per position from a helper;
    the create-figure dialog has a private create handler.
  - `SkylanderPortalWindow`: the slot column, finder column, collection
    folder, suggested file name and collection scan are helpers; the
    filters and the game/element/type labels are tables (with the i18n
    comments). `SkylanderModifyDialog` checks its fields from a table of
    QT_TR_NOOP messages.
  - `FIFOPlayerWindow`: the range group, layout, saved options, signals and
    descriptions are helpers, and the emulation-state lambda is a member.
    `FIFOAnalyzer`: object spans and bytes are looked up by helpers, the
    command summary and explanation are separate callbacks, and the search
    scans by index.
  - Achievements: the window builds its tabs from a table and hides the
    game tabs in one helper; the settings tab saves from a table of RA_*
    settings; the header, leaderboard and progress widgets use small
    helpers for badges, the grid and detaching entries.
  - NetPlay: `NetPlayDialog` builds each menu (data, network, checksum,
    other) in a helper, with shared toggle and exclusive-group helpers, and
    the player table, host code and game status text are helpers.
    `NetPlaySetupDialog` builds the connect and host tabs separately and
    checks host requirements from a QT_TR_NOOP table. `NetPlayBrowser`
    keeps its QSettings keys as named constants and builds the filter box
    in a helper. The other dialogs use small helpers in the old order.
  - Preserved quirks (callers or users may rely on them): the Skylander
    `arg(u16, u16)` id text, which treats the second number as a field
    width; the modify dialog zeroing toy_code on save; `PadMappingDialog`
    using the stored player id directly as the combo index; the stale
    layout rows an achievements tab can leave behind on a refresh (seen as
    a crash in the harness on both sides);
    the NetPlay browser connecting only the all/private radios and the two
    check boxes; the achievements window asking IsGameLoaded before
    HasAPIToken; and the Wii TAS window fetching the remote before choosing
    overrides. Chained `.arg()` calls were kept wherever user text could
    contain '%'.
  - Deliberate differences (nothing a user sees in normal use):
    - Qt connections that had no context object now use `this` (the TAS
      shortcut, the create-figure dialog, the NetPlay setup dialog), so
      they are dropped when the widget is destroyed.
    - `NetPlayDialog`: the traversal port is read byte by byte instead of
      with `ntohs`, and the private `SendMessage` is now `SendChatLine`
      (`SendMessage` is a Win32 macro). Together these fix the base tree's
      compile errors on the Windows target.
    - Dead private code dropped: Skylander GetGameID/GetElementID/GetTypeID,
      AchievementSettingsWidget::OnControllerInterfaceConfigure (with its
      include), FIFOAnalyzer's INCLUDE_HEX_IN_PRIMITIVES block, and a no-op
      setDirection call in the Skylander window.
    - The FIFO search result is copied instead of held by reference;
      `GameListDialog` reads the row count once; `SkylanderModifyDialog`
      initialises every field of the figure it saves. Integer narrowing
      and sign changes are explicit casts.
  - Checked: old and new code side by side with Qt 6.4 offscreen harnesses,
    built against fakes for Core, UICommon, Settings, the file dialogs and
    the emulated devices, with the untouched non-W42 files on both sides.
    Each run dumps the widget tree (class, object name, text, tooltip,
    state, layout positions, focus chain), model contents, tr() strings,
    Config and QSettings writes and call logs after random interaction
    sequences. Only hashes were kept.
    - TAS (6 seeds x 400 steps): same tree, calls and tr() set; only the
      tr() call count differs, where one string is now looked up once.
    - EmulatedUSB (5 seeds, plus 3 with HAVE_CUBEB): identical.
    - InfinityBase (5 seeds), Skylanders (6 seeds, covering modify, load
      and create): same tree, calls and tr() set.
    - FIFO (8 seeds), achievements (6 seeds): identical.
    - NetPlay (all 8 dialogs; 14 seeds, 5 with Discord presence, 3 without
      mGBA, 3 with UPnP): identical. After the final audit edits, 8 seeds
      give the same tree, calls and tr() set.
    - Also compared line by line against the old files for the paths the
      fakes do not reach. A tr() string scan finds every old string in the
      new code with the same context.
  - Compile check: the W42 .cpp files and MainWindow.cpp (the only other
    source that includes W42 headers), old against new, with clang on Linux
    and for x86_64-w64-windows-gnu, `-Wall -Wextra -Wconversion
    -Wsign-compare -Wshadow-all -Wshorten-64-to-32`, with HAVE_CUBEB, with
    and without USE_DISCORD_PRESENCE, and with USE_UPNP for the setup
    dialog. No warning in a W42 file. MainWindow.cpp's diagnostics are the
    same before and after (they come from shared headers and the Win32
    display constants in the MinGW headers). No local or parameter hides a
    Qt data member (`data`, `d_ptr`).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run is left in a W42
    .cpp file. The runs left are public declarations in the headers
    (signals, slots, member lists, enums).
  - No keys, IVs, tokens or credentials are in these files.

- W43 log: rewrote all 30 W43 files (15 .cpp, 15 .h) from
  `docs/w43-spec.md`: the AM media board (`Core/HW/DVD/AMMediaboard`), the
  SI AM baseboard (`Core/HW/SI/SI_DeviceAMBaseboard`), the Triforce serial
  devices (`Core/HW/Triforce/`: JVSIO, IOPorts, ICCardReader, DeckReader,
  FZeroAX, MarioKartGP, Touchscreen), the magnetic card readers
  (`Core/HW/MagCard/`: MagneticCardReader, C1231BR, C1231LR) and the EXI
  baseboard, AGP and modem (`Core/HW/EXI/EXI_DeviceBaseboard`,
  `EXI_DeviceAGP`, `EXI_DeviceModem`). 7850 lines now, from 9581. Every
  rewritten file starts with the original-code header and has no Dolphin
  copyright or SPDX line. `Triforce/SerialDevice` and
  `EXI/Modem/TAPServerModem` are not W43 files and were not touched.
  - Clean room: phase B worked from the spec, the old public headers and
    the output of the old harness binary only. The old .cpp files, their
    git history and Dolphin's copies were not opened; the old files were
    replaced by writing over them without reading them. Where the spec
    left a log text or byte layout open, the old harness binary (built
    from a scratch worktree at fdccad9 and only run) was run with
    `--trace`, and its trace lines were used as the reference.
  - Accidental exposure: one search for `WSAPoll\|ioctlsocket` over
    `Core` and `Common`, run before the old `AMMediaboard.cpp` had been
    written over, printed three of its lines: a variadic `ioctlsocket`
    wrapper declaration, a `WSAPoll(...)` return with casts of the vector
    size and the timeout, and an `ioctlsocket(host_socket, FIONBIO, &val)`
    call. Nothing was taken from them; the new file wraps the same two
    Winsock calls in its own helpers (`SetBlocking`, `PollSockets`) with
    its own names and types. No further searches were run over the old
    files.
  - Public interfaces kept as the spec allows (sections 2.2, 3.1, 4.1, 5.1,
    6, 7.1, 8.1, 9.1, 10.1, 11.1, 12.1). Private members were renamed
    where the spec does not fix them. The modem header keeps
    `NetworkInterface`, `TAPServerNetworkInterface`, `SendComplete` and
    `AddToReceiveBuffer` (12.1). The fixed identifiers in section 15
    (serial numbers, version strings, region table, the All.Net reply, IC
    card serial prefixes, board names) are kept byte for byte.
  - Spec questions and how they were settled:
    - All.Net reply size: 2.11 and 15 say 252 bytes with the NUL, but the
      text given is 229 characters (230 with the NUL). The harness reads
      230 bytes (`Read overflow ... length=230`, and the bytes after the
      NUL are not written). The copy uses the size of the text with its
      NUL; a static_assert pins it at 230.
    - SI baseboard overflow names: the spec gives no name for the data-in
      overflow message of commands 0x12, 0x14, 0x21, 0x22, 0x23, 0x24 and
      0x60. The old trace prints `Unknown_22` and `Unknown_60`, so these
      use `Unknown_` plus two hex digits; commands outside the table use
      `Unknown_` plus decimal, as 3.3 says.
    - Modem reset descriptor: 12 and the harness use `0x00008000`; the old
      header's helper produced `0x80000000`. The spec value is used and
      the helper was dropped from the header.
    - Accept (2.9): the table row does not say whether the address check
      is nested in the addrlen check. The harness calls NetDIMMAccept with
      both pointers null when the addrlen offset is unusable (no "bad
      parmeters" error), so the address is only looked at when the length
      word is usable.
    - Recv and Send: "logs by result" does not say what a would-block
      result logs. The old trace has no line for a recv that fails with
      EAGAIN, so would-block results are not logged. Send follows recv;
      the harness never makes a send would-block, so that half is not
      checked.
    - Order of the "Bad GuestSocket" warning: Recv, Send and SetSockOpt
      look up the host socket before checking the buffer (the warning
      comes before the "Bad data offset" or "Bad optval" error); Connect
      looks it up after the destination and redirection logs. Both orders
      are as the old trace shows.
    - BindEphemeralPort binds with family AF_INET, whatever family the
      guest passed; the connect that follows uses the guest's family (old
      trace: `bind ... fam=2` then `connect ... fam=3`).
    - "Invalid firmware write" logs the offset into the firmware buffer
      (offset - 0x400000), as the old trace shows.
    - IC card SetBaudrate logs the whole payload, not only the 8 bytes the
      length check asks for.
    - AGP `AE03` 4-byte read: the spec does not apply the status-mask
      check there; it is not applied.
    - The setsockopt notice prints the socket error even when the call
      succeeded (a stale value), and the socket() notice prints the result
      as unsigned, as the old trace shows.
    - Log texts that the spec gives only as a format were filled in from
      the old trace: IC card byte lists joined with spaces and the full
      card path; magnetic card logs with the card name only; EXI baseboard
      ReadISR arguments; the AGP slot through the EXI slot formatter; modem
      errors with the descriptor; the media board SetupLink, SearchDevices
      and 0x608 field layouts.
    - 13 says the harness never drives the undefined paths. It does drive
      13.4 (SwitchInput for more than two players) in some seeds; see the
      harness results below.
  - Deliberate differences (undefined behaviour in the old code, spec 13;
    nothing a well-formed request sees):
    - AGP (13.1): an empty ROM reads as zero; EEPROM shifts past the
      64-bit word change no bit or read as 0; u64 EEPROM accesses use
      memcpy and are skipped when the EEPROM is shorter than 8 bytes; the
      `AE04` read past the EEPROM reads FF.
    - Modem (13.2): immediate reads and writes are clamped to 4 bytes; a
      null DMA pointer copies nothing but the transfer still counts.
    - Magnetic cards (13.3): WriteData stores the k-th listed track from
      offset 69 * k.
    - JVS (13.4): SwitchInput sends zero bytes for players 2 and up; the
      player assertion is kept, so the log still shows one ASSERT per
      missing player.
    - Deck reader (13.5): out-of-range JSON numbers are clamped, NaN as 0.
    - Media board (13.6): a game read with no DIMM disc reader returns 1;
      the EINTR retry is a local helper (no Common/UnixUtil.h); the
      ephemeral port range `first + [0, 999]` is computed in u16 with
      wrap-around instead of the old signed arithmetic. Select with no
      usable timeout still polls with -1.
    - EXI baseboard (13.7): kept as specified (srand(time) and one rand()
      for the fallback backup name).
  - Checked: `tools/w43-harness/` built unchanged against the new files.
    - Seed 1, 6 cases per kind (174 cases): equal to `expected-seed1.txt`
      line for line, 0 crashes.
    - Fresh seeds against the old files built in a scratch worktree at
      fdccad9: seed 7 x 25 (725 cases), seeds 2, 3, 4, 5 x 20 (580 each),
      seed 11 x 30 (870 cases). 4089 cases in all, 0 crashes. All
      behaviour and log hashes are equal except two cases, both 13.4:
      - seed 2 si-unknownid #3: SwitchInput for 48 players (the old code
        read past the switch array; one more output byte, probably an
        escaped 0xE0 or 0xD0 in the garbage it read);
      - seed 11 si-shortid #17: SwitchInput past player 2, where the old
        code copied analog input bytes and heap pointers into the reply.
        The old code is not repeatable here: two runs of the old binary
        gave different behaviour hashes for this case.
      Log hashes are equal in both cases.
  - Compile check: the 15 W43 .cpp files are clean with clang 18 `-Wall
    -Wextra -Wconversion -Wsign-compare -Wshadow-all -Wshorten-64-to-32`,
    on Linux and for x86_64-w64-windows-gnu (only third-party and non-W43
    headers warn). libstdc++ 13 with clang 18 hides std::expected, so
    `__cpp_concepts` was raised for the check (JVSIO.h used std::expected
    before W43 too). Fix made: recv/send lengths use the platform length
    type (int on Windows, size_t on POSIX). For the MSVC warnings: C4267,
    C4389 and C4458 are what -Wshorten-64-to-32, -Wsign-compare and
    -Wshadow-all report (none); the two `~` uses complement a value of the
    target's own width (no C4319); no bool is combined with an integer
    (C4805); no pointer to member is a template argument. Dependents
    compile with the same results as on the base tree: SerialDevice,
    TAPServerModem, DVDInterface, Boot, Boot_BS2Emu (clean), SI_Device and
    EXI_Device (both fail on the missing SFML header, on both trees), and
    DolphinQt TriforcePane, NetworkWidget and MappingWindow (syntax check
    against the system Qt 6 headers, clean on both trees).
  - Verbatim audit (`verbatim-audit.py wii 8`): no run of 8 or more lines
    is left in any W43 .cpp file. Runs fixed: the JVS, IC card and deck
    command enums are written in grouped form, the AGP savestate goes
    through one fold, and the single-word media board status replies come
    from a table. The runs left are in W43 headers and are public
    declarations the spec keeps (the media board enums and address list,
    the modem's network interface classes, IOPorts, MagneticCardReader,
    JVSIO, the EXI and SI baseboard class declarations).
  - Not tested: the Windows socket branch at run time, real tapserver
    traffic and threads, and the AGP/baseboard fallback paths the harness
    does not drive (13.7, 13.9).
  - No keys, BIOS, firmware or game data were added.
  - Not rewritten: nothing.

- B2 log: replaced Dolphin's Sys data with PixelU-Wii's own where the
  program can do without Dolphin's copy, and made the build ship only that
  plus a short third-party list. Sys in the package is now 2091 files: 187
  of PixelU-Wii's own (`pixelu/wii/Data/Sys`) and 1904 third-party
  (GameSettings is most of them). The exe name (PixelU-Wii.exe) and the
  PixelU\wii\ package layout are unchanged.
  - Clean room: Dolphin's Sys files and the code that reads them were
    opened only to learn what each file must contain (names, formats,
    sizes, which uniforms and options a shader gets, which icon names the
    program asks for). Every new file was written fresh or is made by a
    new generator; no body or comment was copied. No Nintendo keys, BIOS,
    firmware, DSP ROMs or games were added.
  - Themes (`Sys/Themes/PixelU`, 96 PNGs at 1x, 2x and 4x): one own icon
    theme of 32 icons, drawn by `pixelu/wii/art/make_art.py` with the
    other art. `DEFAULT_THEME_DIR` is now "PixelU" (`Common/CommonPaths.h`).
    `Resources::GetThemeIcon` falls back, icon by icon, to the bundled
    theme when the chosen theme folder does not exist or lacks the icon,
    so a config that still names "Clean" or any other theme shows the
    PixelU icons. The help text in the Interface pane says "If unsure,
    select PixelU."
  - Shaders (`Sys/Shaders`): own `default_pre_post_process.glsl` (the
    resampling modes 0 to 6 in linear light, clamped to the source
    rectangle; the NTSC-M and PAL gamut rows computed from their
    primaries; an own NTSC-J matrix from SMPTE-C primaries and the
    9300K+27MPCD white point (0.2838, 0.2984), with no chromatic
    adaptation; HDR paper white; sRGB or custom gamma encoding), the
    shaders asked for by name (`Anaglyph/dubois.glsl` with Dubois'
    published coefficients, cited; `Passive/horizontal.glsl`), and a few
    own extras: `Anaglyph/red_cyan`, `AutoHDR`, `grayscale`, `sepia`,
    `scanlines`, `sharpen`.
  - Profiles (`Sys/Profiles`): own `GCPad/SDL Gamepad.ini` (positional
    face buttons) and `Wiimote/Wii Remote with MotionPlus Pointing.ini`,
    using the input names PixelU-Wii's SDL and Wii Remote backends give.
  - WiiConnect24 NAND defaults (`Sys/Wii/shared2/wc24`), made by the new
    `pixelu/wii/Data/make_wc24.py`: `nwc24msg.cfg` and `.cbk` in the
    initial state with no Wii ID, no password and no mlchkid (the same
    state the program's own config reset writes), `nwc24dl.bin` with its
    120 empty entries, `nwc24fl.bin`, and the empty send and receive
    mailboxes (`mbox/*.ctl` with the free-entry chain, `mbox/*.mbx` with
    the VFF header and the start of a FAT12 table for send, FAT16 for
    receive). `misc.bin`, `nwc24fls.bin` and `mbox/Readme.txt` are no
    longer shipped; nothing reads them from Sys.
  - IPL fonts (`Sys/GC/font_western.bin`, `font_japanese.bin`), made by
    the new `pixelu/wii/Data/make_ipl_fonts.py` from Noto Sans Regular
    and Noto Sans JP Regular (SIL OFL 1.1; the script pins their SHA-256
    and needs the two .ttf paths). It writes the boot ROM font format
    (header, 2bpp 8x8 tiles, 21x21 cells of 24 pixels on 512x512 sheets)
    and compresses it with its own Yay0 encoder. The Japanese glyph order
    is worked out from the Shift JIS codecs. 7136 and 274412 bytes, under
    the size limits the loader checks; unpacked they are the sizes the
    loader expects. `Sys/GC/font-licenses.txt` names the fonts and carries
    the OFL text.
  - Sys assembly: the new `pixelu/wii/CMake/PixelUAssembleSys.cmake` runs
    after the build (from `Core/DolphinQt/CMakeLists.txt`) and makes
    Binaries/Sys from the third-party list it names (GameSettings,
    ApprovedInis.json, wiitdb-*.txt, triforcetdb-*.txt, the OSD font and
    its copyright file, codehandler.bin, GC/dsp_rom.bin and
    GC/dsp_coef.bin, all from Dolphin's Data/Sys) with
    `pixelu/wii/Data/Sys` over it, plus an empty Load/GraphicMods folder.
    Nothing else of Dolphin's Data/Sys is copied.
  - Package check: the new `pixelu/wii/Data/check_sys_package.py` checks a
    packaged Sys file by file. Every file must be PixelU-Wii's own (same
    bytes) or match the third-party list read from
    PixelUAssembleSys.cmake (same bytes as Dolphin's copy). Every own file,
    every file the program reads by name and GameSettings must be there.
    Every icon the Qt code asks for (`GetThemeIcon` calls and the
    `DEBUG_ICONS` lists) must be in the bundled theme, and no other theme
    may be shipped. The workflow's "List package contents" step runs it
    and fails on any problem; the old checks stay.
  - COPYING: section 1 lists the own Sys data; section 3 now names only
    what is not RetroPixels' own: GameSettings, ApprovedInis.json and
    Languages (Dolphin community data), Dolphin's free DSP ROMs, the
    GameTDB title lists (credited to GameTDB), the Gecko code handler and
    the OSD font.
  - codehandler.bin: `docs/codehandler.s` in Dolphin's tree is its source.
    It carries an MIT licence, "Copyright (c) 2010-2017 Nuke, brkirch,
    Y.S, Kenobi, gamemasterplc", and is based on Gecko OS's code handler.
    It is third-party and is kept and credited as such.
  - Checked:
    - `make_art.py --check` (191 files, 0 differ; the B1 outputs are
      byte for byte as before), `make_wc24.py --check` (8 files, 0
      differ), `make_ipl_fonts.py --check` with the pinned Noto fonts
      (both .bin files the same).
    - The assembly script run on its own with CMake 3.28 over a Dolphin
      2609a checkout: 2091 files. `check_sys_package.py` on that output:
      0 problems, exit 0. On Dolphin's raw Data/Sys: 1332 problems
      (other themes, shaders, GraphicMods, totaldb.dsy, WC24 files,
      fonts), exit 1.
    - Every shader compiled with glslangValidator through a scratch
      harness that puts the Vulkan prelude, the builtin uniforms and the
      option uniforms in front of it.
    - The fonts: every glyph in both tables was rendered and compared with
      the font's missing-glyph box. The two characters Noto lacks (U+203E
      overline, U+2252 approximately equal) are drawn by the script; the
      control-code cells are blank. The derived Shift JIS order matches
      the order the loader expects except for the deliberate choices
      below.
    - The blank WC24 templates (mailbox control and VFF files,
      nwc24fl.bin) come out byte for byte the same as Dolphin's: their
      bytes are fixed by the format. nwc24msg.cfg and nwc24dl.bin differ.
  - Deliberate differences:
    - The Japanese font has the yen sign at 0x5C, ヵ, ヶ and ヴ, the
      voiced katakana where Shift JIS puts them, and cp932's U+301F.
    - The WC24 config has no account (no ID, password or mlchkid).
    - The theme help text no longer names Clean; its translations in
      Languages are now out of date for that one string.
  - Lost by not shipping Dolphin's files:
    - `totaldb.dsy`: the debugger's "Generate symbols from signature DB"
      says the database is not found, and loading symbols for a game with
      no map only finds unnamed functions. A user's own .dsy still works.
    - `Load/GraphicMods`: the 58 stock per-game graphics mods (bloom,
      depth of field, HUD removal and the like) are gone. User mods in the
      User folder still work.
    - Dolphin's other 45 post-processing shaders and its other 4 Clean
      themes.
    - The shared WC24 account: WiiConnect24 mail with Dolphin's shared
      credentials no longer works out of the box; the console registers as
      a new, unregistered one.
  - Left as Dolphin's, and why:
    - GameSettings, ApprovedInis.json and Languages: kept by the owner's
      decision.
    - GC/dsp_rom.bin and GC/dsp_coef.bin (Dolphin's free replacement
      ROMs, not Nintendo's). An own IROM would have to reproduce the
      ucode boot protocol and the mixing and GBA helper routines at their
      fixed addresses. It can only be checked end to end with game ucode,
      which this work has none of and may not ship. In dsp_coef, three of
      the four tables are generated filters, but 23 entries are constants
      the GBA ucode needs, which come from Nintendo's own DROM. An own
      table would break GBA boot over the link cable under LLE, and
      keeping those values would mean shipping Nintendo data. AX HLE also
      reads dsp_coef.bin to resample, so any change alters every game's
      audio, which could not be checked by ear here.
    - Third-party, credited in COPYING: wiitdb-*.txt and
      triforcetdb-*.txt (GameTDB), codehandler.bin (Gecko, MIT),
      Resources/OSD_Font.ttf (Bitstream Vera Sans Mono).
  - Found, not fixed (W19 code): the VFF reader picks FAT12 or FAT16 from
    the volume size divided by the cluster size, so the 2 MB send mailbox,
    FAT12 on a real Wii, is read as FAT16.
  - Only CI can confirm:
    - The C++ changes (Resources.cpp, CommonPaths.h, InterfacePane.cpp)
      compile with MSVC.
    - The Windows build runs the post-build assembly script, and the
      package check passes in the workflow.
    - The shaders compile on the D3D11, D3D12, OpenGL and Vulkan backends
      (only Vulkan GLSL was checked here).
    - The theme looks right in the running app, in light and dark style.
    Not covered by CI either, and to be checked by hand: the fonts show
    in GameCube games and the boot menu without a ROM dump, and the WC24
    defaults work in games that use WiiConnect24 (for example Mario Kart
    Wii's channel).

- B4 log (third-party code from its own projects): the build no longer
  fetches anything from Dolphin's repository except the data PixelU-Wii
  keeps (Data/Sys/GameSettings, ApprovedInis.json and the other files in
  B2's third-party Sys list, Languages/po). Work started from HEAD 1148c9a
  on `claude/wiiu-emulator-urpcen` and was rebased onto 2b1f3d9 (B2
  merged), keeping B2's check_sys_package.py step and PixelUAssembleSys
  wiring. No C++ source, Sys data, icon or rc file was touched.
  - One fetch method for everything (new, original code):
    - `pixelu/wii/Externals/sources.json` lists 42 sources. A git source is
      pinned by its full 40-character commit, an archive by its SHA-256
      (with mirrors). Optional fields: `paths` (sparse checkout), `patches`,
      `group` (fetched only on request), `write` (small files to add).
    - `pixelu/wii/Externals/fetch.py` fetches them into
      `pixelu/wii/third_party` (gitignored). Git: init, fetch --depth 1 of
      the pinned commit (blobless and sparse when `paths` is set), checkout,
      HEAD checked against the pin, .git removed. Archives: downloaded,
      SHA-256 checked, extracted with path checks. Then the patches are
      applied (own diff applier, exact context, the file's line endings
      kept), and a `.pixelu-source` stamp is written. `--check` tells
      whether third_party matches the manifest; CMake runs it at configure
      and warns when something is missing or out of date. `--list`,
      `--only`, `--group`, `--jobs`, `--keep-git` are there for CI and
      local use.
    - third_party keeps the sub-layout of Dolphin's Externals
      (`<Name>/<checkout>`), so most of B3's glue paths are unchanged.
      `PIXELU_WII_THIRD_PARTY` now defaults to `pixelu/wii/third_party`.
  - Libraries at the same commit as Dolphin 2609a's submodules, unchanged:
    bzip2, cpp-ipc, cpp-optparse, cubeb, curl, enet, fmt, glslang, hidapi,
    imgui, implot, libspng, libusb, lz4, mGBA, miniupnpc, minizip-ng,
    pugixml, rcheevos, SDL, SFML, SPIRV-Cross, tinygltf, Vulkan-Headers,
    VulkanMemoryAllocator, watcher, WIL, xxHash, zlib-ng, zstd. Nested
    submodules (picojson's gtest, cubeb's sanitizers) are not needed and
    not fetched.
  - Libraries Dolphin had copied into its tree, now from their upstream at
    the version Dolphin's copy was taken from (each compared file by file
    with Dolphin's copy):
    - LZO 2.10 (oberhumer.com tarball, Ubuntu orig tarball as mirror,
      SHA-256 c0f89294...a072): miniLZO, identical. Dolphin had renamed
      the header path; `Externals/LZO/include/lzo/lzo1x.h` (own shim)
      includes minilzo.h.
    - ed25519 (orlp/ed25519@7fa6712e): identical.
    - discord-rpc (discord/discord-rpc@963aa9f3) and RapidJSON
      (Tencent/rapidjson@dfbe1db9, include only): identical.
    - OpenAL Soft 1.16.0 headers (kcat/openal-soft@b907c21f, include
      only): identical.
    - mbedTLS 2.28.0 (Mbed-TLS/mbedtls@8b3f26a5): identical apart from the
      CMake files, which B3's glue does not use.
    - picojson (kazuho/picojson@a944d59e): Dolphin changed two throws to
      abort; with exceptions off both end the program, so no patch.
    - XZ Utils 5.2.4 (tukaani-project/xz@b5be61cc, src/liblzma and
      src/common): identical apart from a windows.h include Dolphin put in
      tuklib_integer.h. `Externals/liblzma/config.h` now includes
      windows.h and intrin.h under MSVC instead. The glue leaves out the
      upstream files Dolphin had deleted (crc*_small.c, the BCJ filters).
    - Bochs disassembler (bochs-emu/Bochs@94864fb9, bochs/disasm only):
      identical apart from `$Id$` lines and one fix, re-applied as
      `patches/bochs-disasm-adc-imm32.patch` (ADC r/m64, imm32 is decoded
      with a sign-extended 32-bit immediate). `Externals/Bochs_disasm/
      include/config.h` (own) gives the types Bochs' configure would write;
      Dolphin's stdafx.cc is not built.
    - FatFs R0.14b (elm-chan.org ff14b.zip): two patches,
      `patches/fatfs-utf8-putc.patch` (ChaN's published fix to the UTF-8
      check in f_putc) and `patches/fatfs-ffconf.patch` (the four options
      the build needs: mkfs, long file names on the heap, UTF-8 names,
      relative paths). Dolphin had renamed TCHAR to FATFS_TCHAR inside
      ff.h; `Externals/FatFs/include/ff.h` (own wrapper) does the same from
      outside so it does not clash with Windows' TCHAR.
    - `patches/README.txt` explains each patch.
  - Kept as third-party copies in `pixelu/wii/Externals` (no upstream can
    give the code the build uses; each has a README.txt):
    - expr (`expr/include/expr.h`, MIT, LICENSE): the extended dialect the
      breakpoint conditions use was written inside the Dolphin project;
      upstream zserge/expr has none of it.
    - FreeSurround (`FreeSurround/include`, `source`, GPL-2.0-or-later):
      Kothe's decoder only shipped inside a foobar2000 plugin; the build
      uses the cut-down port made for Dolphin in 2017 (with a 2023
      delete[] fix). Copied unchanged.
  - Qt: the Qt Company's official 6.8.3 MSVC 2022 x64 build (qtbase and
    qtsvg), installed by aqtinstall 3.3.0 into
    `third_party/Qt/6.8.3/msvc2022_64` (`PIXELU_WII_QT_DIR`, also the
    default `QT_HOST_PATH`). windeployqt gets
    `--exclude-plugins qgif,qico,qjpeg,qtuiotouchplugin` so the deployed
    plugins are the four Dolphin's Qt had: qwindows, qmodernwindowsstyle,
    qsvg, qsvgicon.
  - FFmpeg: Dolphin's ext-win-ffmpeg was static .lib files (no DLLs) of
    FFmpeg N-106414-g28d011516b. They are now built here from FFmpeg's own
    repository at that commit (group "ffmpeg" in sources.json) by
    `Externals/FFmpeg/build-ffmpeg.sh`, with MSVC and the same configure
    choices (GPL v3, static, no programs, only the AVI muxer, FFV1/MPEG-4/
    Ut Video encoders and the file protocol; w32threads), into
    `third_party/FFmpeg/x64` (`PIXELU_WII_FFMPEG_DIR`). sources.json
    writes FFmpeg's VERSION file so the version string stays
    N-106414-g28d011516b. CI caches the result.
  - msgfmt: GNU gettext from MSYS2 (mingw-w64-x86_64-gettext-tools),
    passed as GETTEXT_MSGFMT_EXECUTABLE. The Externals/gettext fallback in
    DolphinQt's CMake is gone.
  - Dolphin's repository: CI makes a sparse, blobless, depth-1 checkout at
    the commit the pixelu/wii/dolphin submodule pins. The paths are read
    from the `from_third_party` list in PixelUAssembleSys.cmake, plus
    /Languages/po/. The step fails if a path brought nothing, or if Source,
    Externals, Data/Sys/Themes or Data/Sys/Shaders were checked out.
  - Package: same artifact names, layout and PixelU-Wii.exe. The licence
    files in Licenses/Externals/<project>/ now come from third_party (FFmpeg's
    from its install folder) and from Externals/expr and FreeSurround.
    COPYING says where each library comes from; FFmpeg is listed as
    GPL-3.0-or-later as configured.
  - Files: new `Externals/fetch.py`, `sources.json`, `patches/*`,
    `FFmpeg/build-ffmpeg.sh`, `FatFs/include/ff.h`, `LZO/include/lzo/
    lzo1x.h`, `Bochs_disasm/include/config.h`, `expr/*` and
    `FreeSurround/*` copies with README.txt, `Externals/.gitignore`.
    Changed: the glue of Bochs_disasm, FatFs, LZO, liblzma, ed25519,
    discord-rpc, expr, FreeSurround; `CMakeLists.txt`,
    `CMake/PixelUWindowsToolchain.cmake`, `Source/Core/DolphinQt/
    CMakeLists.txt` (build file only), `.gitignore`, `COPYING`, the
    workflow. Nothing removed.
  - Checked here (Linux):
    - All git sources fetched by fetch.py at their pins; each copied
      library compared with Dolphin's copy as above.
    - FatFs wrapper compiled with g++ -Werror, with a Windows-style
      TCHAR/_T defined before and after; miniLZO round trip through the
      shim; Bochs decodes `48 81 D0 imm32` as 7 bytes with the patch.
    - Windows configure (clang-cl, Ninja, the toolchain, stub Qt/FFmpeg)
      generates. Compared with the old tree's compile_commands, the only
      differences are the expected ones (stdafx.cc gone, FatFs define and
      include, Bochs config.h, LZO includes).
    - FFmpeg built on the host from the pinned commit with the script:
      all 124 installed headers are byte-identical to ext-win-ffmpeg's.
  - Only CI can confirm (Qt's server, elm-chan.org, MSYS2 and GitHub are
    blocked here):
    - FatFs ff14b.zip's SHA-256: sources.json has a placeholder of zeros;
      the first run fails and prints the real hash to put in.
    - FatFs zip layout (source/ at the top, no strip).
    - aqtinstall with `--archives qtbase qtsvg` gives windeployqt, qtpaths,
      Qt6Svg and the four plugins (the Check Qt step), and windeployqt
      accepts --exclude-plugins and deploys the same DLLs and plugins as
      before.
    - MSYS2 package names (make, diffutils, nasm, gettext-tools) and the
      msgfmt path.
    - FFmpeg's MSVC build under VS 2026 through ilammy/msvc-dev-cmd (cl and
      link ahead of MSYS2's link in PATH), and that it detects the same
      Windows parts as Dolphin's (bcrypt, d3d11va, dxva2); the link of
      PixelU-Wii.exe against it.
    - ff.h wrapper and liblzma's config.h with windows.h under MSVC and the
      PCH.
    - The sparse Dolphin checkout (patterns, sizes) and the licence loop
      over third_party.
  - Left from Dolphin: the kept data above (sparse checkout); the expr and
    FreeSurround copies (third-party code with Dolphin-project changes, no
    upstream); FFmpeg's configure choices mirror Dolphin's build so the
    libraries match.

- L5a log (PixelU-Wii Linux platform code, own): PixelU-Wii now configures,
  builds and links on Linux x86-64, and dolphin-emu-nogui boots a program
  with the Null video backend and the Null sound output. Work started from
  HEAD c5a0601 on `claude/wiiu-emulator-urpcen`. Every new file is written
  from scratch (POSIX and Linux man pages, the kernel's TUN/TAP
  documentation); Dolphin's deleted MemArenaUnix.cpp, ConsoleListenerNix.cpp,
  TAP_Unix.cpp and DolphinNoGUI were not read for their bodies, only the
  names other files call, which our own headers already had.
  - New files (all with the original-code header, so the tracker counts them
    as original):
    - `Common/MemArenaUnix.cpp`: the emulated memory is an anonymous memory
      file, `memfd_create` (`shm_open` + `shm_unlink` where the kernel or a
      sandbox refuses it), sized with `ftruncate`. Views are `MAP_SHARED`
      mappings of it, so mirrors see the same bytes. The fastmem region is
      one `PROT_NONE`, `MAP_NORESERVE` reservation; pieces are mapped into it
      with `MAP_FIXED`, and unmapping maps a fresh reservation over the piece,
      so the address range never goes back to the system while it is held.
      `ChangeMappingProtection` is `mprotect`. `LazyMemoryRegion` is a
      private anonymous mapping (the kernel already gives zero pages and only
      backs written ones), cleared with `MADV_DONTNEED`.
    - `Common/Logging/ConsoleListenerNix.cpp`: log lines to standard error,
      coloured by level on a terminal unless `NO_COLOR` is set or TERM is
      dumb.
    - `Core/HW/EXI/BBA/TAP_Unix.cpp`: the broadband adapter's TAP mode on
      Linux: `/dev/net/tun`, `TUNSETIFF` with `IFF_TAP | IFF_NO_PI` on the
      interface "Dolphin" (the name BBA guides tell Linux users to create
      with `ip tuntap add Dolphin mode tap user $USER`), a reader thread that
      polls the descriptor every 50 ms for its stop flag. Same interface as
      TAP_Win32 (Activate/Deactivate/SendFrame/RecvInit/RecvStart/RecvStop).
    - `DolphinNoGUI/` (`CMakeLists.txt`, `MainNoGUI.cpp`, `Platform.h`,
      `PlatformHeadless.cpp`): the `dolphin-emu-nogui` program. It takes
      CommandLineParse's shared options (no GUI ones) plus `-p/--platform`
      (only `headless` so far; L5d can add `x11`), defines every `Host_*`
      call, prints alerts to standard error answering "yes", boots with
      BootManager, runs the host-job loop until the core is Uninitialized,
      and stops cleanly on SIGINT or SIGTERM (a second signal exits at once).
      A bad command line exits with optparse's status 2 instead of aborting.
  - Changed:
    - `Common/MemArena.h`: the Unix members (descriptor, sizes, reservation)
      next to the Windows ones. `Common/Logging/ConsoleListener.h`: comment.
    - `Core/HW/EXI/EXI_DeviceEthernet.h`: the Linux members of
      `TAPNetworkInterface`.
    - Top-level `CMakeLists.txt`: Linux is accepted (Windows still requires
      MSVC); GCC 12+ (std::expected, std::byteswap; Contains.h
      already falls back for ranges::contains); C++23 without extensions,
      `-fno-strict-aliasing -fwrapv -fvisibility-inlines-hidden -msse2 -Wall`
      without -Werror, PIC, `RPATH=$ORIGIN/lib`, `--as-needed`,
      `-z noexecstack`. Clang needs 19+: Clang 18 cannot use libstdc++'s
      `<expected>` (checked: DiscIO fails with Clang 18); clang-cl (the MSVC
      front end) is not subject to that check. New `ENABLE_NOGUI` (default
      ON on Linux, OFF on
      Windows). On Linux `ENABLE_CUBEB` defaults OFF until L5c builds cubeb's
      Pulse/ALSA backends, and RAIntegration is forced OFF. The Qt default
      folder is `.../6.8.3/gcc_64` on Linux. The Bcrypt link of FFmpeg is
      Windows only.
    - `Source/CMakeLists.txt`: the Windows header definitions only on WIN32,
      the shared PCH only with MSVC. `Source/Core/CMakeLists.txt`:
      `DolphinNoGUI` when `ENABLE_NOGUI`. (Neither file is in a batch's list;
      L5a took them because nogui needs them.)
    - `Common/CMakeLists.txt` and `Core/CMakeLists.txt`: the Windows-only
      files (CompatPatches, HRWrap, LdrWatcher, MemArenaWin, WindowsDevice,
      WindowsRegistry, WGL, ConsoleListenerWin; TAP_Win32, IOWin) and
      libraries (kernel32, shlwapi, winmm, opengl32, setupapi, iphlpapi,
      videod3d, videod3d12, the PowerShell build_info.txt step and
      -INCLUDE:enableCompatPatches) moved into WIN32 blocks; the Linux files
      above otherwise, plus libdl/librt for common.
  - Needed from other batches' files (not committed here; in
    `docs/l5a-handoff.patch`, which applies with `git apply` on top of these
    commits and is what the local build below used):
    - `VideoBackends/CMakeLists.txt` (L5b): D3DCommon, D3D, D3D12 only on
      WIN32. With that, OGL, Null, Software and Vulkan all compile on Linux
      (Vulkan still makes only Win32 surfaces and OGL has only WGL: L5b).
    - `InputCommon/CMakeLists.txt` (L5c): DInput, ForceFeedback, WGInput,
      Win32 and XInput only on WIN32. SDL, DSU, Steam Deck and the Wii
      Remote backends build on Linux.
    - Externals glue (L5c's folder; none of it touches cubeb or SDL):
      - `liblzma/config.h`: a Linux block (POSIX threads with the monotonic
        clock, sched_getaffinity CPU count, sysconf RAM size); the Windows
        part is unchanged.
      - `libusb`: the usbfs backend with netlink hot-plug and a new
        `libusb/linux/config.h` (own, no libudev).
      - `hidapi`: the hidraw backend, linked to libudev (pkg-config).
      - `SFML`: Unix/SocketImpl.cpp off Windows.
      - `mbedtls`: ws2_32 only on Windows.
      - `curl`: on Linux Mbed TLS (the one built here) instead of Schannel,
        CA path /etc/ssl/certs, no bundle file, no pkg-config.
      - `discord-rpc`: connection_unix/discord_register_linux with
        DISCORD_LINUX.
      - `rcheevos`: RC_CLIENT_SUPPORTS_RAINTEGRATION only when the option is
        on.
  - Checked here (Ubuntu 24.04 container, no GPU, no USB):
    - Third-party sources fetched with `Externals/fetch.py` (all but FatFs:
      elm-chan.org is blocked; Dolphin's ff14b copy was put in its place for
      the local build only). Dolphin's Sys data from a sparse checkout.
    - `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=OFF` with
      gcc-12/g++-12 (the 22.04 compiler): configures; `ninja
      dolphin-emu-nogui` and `ninja dolphin-tool` build and link with the
      handoff patch applied. 20 warnings in PixelU-Wii's code, none in the
      new files (stringop-truncation on fixed-size name fields, an unused
      lock variable in WorkQueueThread.h, -Wattributes visibility notes, one
      parentheses note in LogitechMic.cpp that is the intended precedence).
    - Clang 19 (apt clang-19) build of dolphin-emu-nogui: builds, links and
      passes the same start-up check; no warnings in the new files.
    - `dolphin-emu-nogui --version` prints "PixelU-Wii 2.21.0"; `--help`
      lists the options; no game exits 1 with a message; a bad `-p` exits 2.
    - A 288-byte test DOL (one text section at 0x80003100 that counts in r3
      and stores it to 0x80001800, made by a throwaway Python script, no
      Nintendo code) boots with `-v Null`: "Emulated memory ready; MEM1 at
      0x7f..." (the memfd arena with fastmem views), "Booting from
      executable", the emulation keeps running, and SIGINT or SIGTERM end it
      with "Shutdown complete" and exit status 0 within a second. With the
      JIT (CPUCore=1) and the interpreter (CPUCore=0); the interpreter run
      uses about one host core, as a busy guest loop should.
    - `libusb_init` fails in the container (no /dev/bus/usb); PixelU-Wii
      logs it and carries on, as on a machine without USB access.
    - Binary: NEEDED libudev.so.1, libstdc++.so.6, libm, libgcc_s, libc;
      RUNPATH $ORIGIN/lib; GLIBCXX_3.4.30 at most. GLIBC_2.38 here only
      because the container's glibc is 2.38+ (C23 strtol aliases); built on
      ubuntu-22.04 the floor is 2.35.
    - Windows unchanged: the base tree (c5a0601) and this tree were both
      configured as Windows with clang-cl (MSVC-style, Ninja, mingw headers,
      ENABLE_QT=OFF). compile_commands.json is identical for every source
      (only build-folder paths differ), and all 59 libraries and programs
      have the same objects and link libraries.
  - CI must confirm (L5d's build-linux job, or a step added to it):
    - ubuntu-22.04 with `gcc-12 g++-12 ninja-build libudev-dev pkg-config`
      (and FatFs from elm-chan.org through fetch.py);
      `cmake -S pixelu/wii -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 -DENABLE_QT=OFF`
      then `ninja -C build dolphin-emu-nogui dolphin-tool`.
    - `objdump -T build/Binaries/dolphin-emu-nogui | grep -o 'GLIBC_[0-9.]*'
      | sort -V | tail -1` is at most GLIBC_2.35.
    - Start-up: a test DOL as above with `build/Binaries/dolphin-emu-nogui
      -u "$PWD/u" -v Null -e test.dol &`, alive after 8 s, `kill -INT`, exits
      0 within 10 s, the log (`-C Logger.Logs.BOOT=True`) has "Booting from
      executable" and "Shutdown complete".
  - Left for other batches: cubeb on Linux (L5c, ENABLE_CUBEB defaults OFF
    there until then); Vulkan Xlib/XCB/Wayland surfaces and GLX/EGL (L5b);
    an X11 platform for nogui and Qt on Linux (L5d; ENABLE_QT still defaults
    ON, so Linux configures need -DENABLE_QT=OFF until then); Sys next to
    dolphin-emu-nogui (DolphinQt's post-build step assembles Binaries/Sys;
    nogui alone falls back to DATA_DIR, which is enough for DOLs but not for
    games that need Sys files); the shared parser shows "[%choices]"
    literally for -a (UICommon, L5d). No BlueZ Wii Remote backend (hidapi
    finds remotes on Linux) and no MemoryWatcher (a Linux-only debugging
    feature Dolphin had).

- L5b log (PixelU-Wii video on Linux, own): the Vulkan backend makes Xlib, XCB and Wayland
  surfaces, and Common/GL has GLX and EGL contexts, so the OpenGL, Vulkan and software
  backends draw on Linux. Work started from HEAD 70b1a3e on `claude/wiiu-emulator-urpcen`.
  Every new file is written from scratch from the Khronos specifications (Vulkan WSI, GLX 1.4,
  GLX_ARB_create_context(_profile), GLX_EXT/MESA_swap_control, EGL 1.5, EGL_KHR_create_context,
  EGL_KHR_platform_x11/wayland, EGL_MESA_platform_surfaceless, EGL_KHR_surfaceless_context),
  the Xlib manual and wayland-egl's three-function API. Dolphin's EGL*/GLX11* files were not
  read; only the names our own headers already had (GLContext, WindowSystemInfo) are used.
  - New files (original-code header):
    - `Common/GL/GLInterface/X11Window.{h,cpp}`: X11 support shared by GLX and EGL. libX11.so.6
      is opened at run time. The context gets its own connection to the frontend's X server
      (XDisplayString of the frontend's Display*), so the frontend's display need not be
      thread-safe (no XInitThreads needed): every Xlib/GLX call on it is made under one mutex
      shared by the main context and its worker contexts. It draws into a child window of the
      frontend's render window with the visual the GL config wants (24-bit preferred, so a
      compositor does not blend with alpha), selects no input (events still reach the
      frontend's window) and follows the parent's size on Update(). An X error handler,
      chained to the previous one, logs and ignores errors on our connections only (a
      frontend that destroys its window first would otherwise end the program through Xlib's
      default handler); errors expected while probing context versions are logged at info.
    - `Common/GL/GLInterface/GLX.{h,cpp}`: desktop GL through GLX 1.3+ (libGL.so.1, or
      libGLX.so.0), no <GL/glx.h> (it would clash with GLExtensions' pointers; the tokens are
      declared with the registry's values). Core profiles 4.6 down to 3.2 newest first via
      glXCreateContextAttribsARB, legacy context as the fallback (as WGL does). Worker contexts
      on a 1x1 pbuffer (or no drawable for 3.0+ where no pbuffer config exists), the same
      profile and version as the main one. Swap interval through GLX_EXT_swap_control or
      GLX_MESA_swap_control.
    - `Common/GL/GLInterface/EGL.{h,cpp}`: OpenGL or OpenGL ES 3.x through EGL (libEGL.so.1)
      on three window systems: X11 (private connection and child window as above,
      EGL_KHR/EXT_platform_x11), Wayland (wl_egl_window from libwayland-egl.so.1 on the
      frontend's wl_surface) and headless (EGL_MESA_platform_surfaceless, else the default
      display; surfaceless contexts, else 1x1 pbuffers). Desktop GL first, ES 3.2/3.1/3.0 when
      that fails or when PreferGLES is set. Core function pointers fall back to
      libOpenGL.so.0/libGL.so.1 or libGLESv2.so.2 where eglGetProcAddress does not give them.
    - `tools/pixelu-wii-test-dol.py`: writes the test DOL used below (also for L5d's CI job).
  - Changed:
    - `Common/GL/GLContext.{h,cpp}`: on Linux (PIXELU_GL_GLX_EGL, set by the Common CMake
      change below) X11 uses GLX, then EGL (EGL first when PreferGLES); Wayland and headless
      use EGL. New virtual `SetSurfaceSize` (no-op except EGL on Wayland, where a surface has
      no size of its own). Windows code paths unchanged.
    - `VideoBackends/OGL/OGLGfx.cpp`: a surface resize passes the presenter's backbuffer size
      to SetSurfaceSize before Update() (ignored by WGL, GLX and EGL on X11).
    - Vulkan: `VulkanLoader.h` includes vulkan_xlib/xcb/wayland.h itself on Linux, with the
      window-system types declared as the opaque handles they are (Display/Window/VisualID
      renamed by macros while the headers are read), so no X11 or Wayland headers are needed
      and no Xlib macros reach the backend; `VulkanEntryPoints.inl` has the six surface entry
      points; `VulkanContext` enables VK_KHR_xlib_surface and/or VK_KHR_xcb_surface for X11
      (one of them required) and VK_KHR_wayland_surface for Wayland, and remembers the enabled
      instance extensions (`IsInstanceExtensionEnabled`); `VKSwapChain` makes an Xlib surface,
      else an XCB one (XGetXCBConnection from libX11-xcb.so.1, looked up at run time), and
      Wayland surfaces. Nothing new is linked: libcommon and libvideovulkan have no undefined
      X11/GL/EGL/Wayland symbols.
  - Needed from another batch's file (not committed; `docs/l5b-handoff.patch`, applies with
    `git apply` on top of these commits): `Common/CMakeLists.txt` (L5a's) builds
    GLInterface/EGL, GLX and X11Window on Linux, finds the X11 and EGL headers
    (libx11-dev, libegl-dev; a clear error otherwise) and defines PIXELU_GL_GLX_EGL. Without
    it the tree still builds and links on Linux, but GL has no context there (as before).
  - Checked here (Ubuntu 24.04 container, Xvfb, Mesa 25.2 llvmpipe and lavapipe):
    - gcc-12 build of dolphin-emu-nogui (L5a's configure, -DENABLE_QT=OFF -DENABLE_LLVM=OFF),
      handoff patch applied: no warnings in the changed files. The changed files also compile
      with clang++-19 -Wall -Wextra without warnings, and EGL.cpp also with an
      eglplatform.h that uses Xlib types (-DUSE_X11, as older headers do by default).
    - Test DOL (tools/pixelu-wii-test-dol.py, 288 + 768 bytes, no Nintendo code): VI 640x480p with the
      XFB at 0x00400000, CP/PI FIFO linked, write-gather pipe; each frame BP writes set the
      clear colour (red = frame counter * 4, green 0xC0, blue 0x40) and trigger an EFB->XFB
      copy with clear. `--triangle` adds a vertex-coloured triangle (CP/XF/TEV setup).
    - In an X11 window under Xvfb: a local test platform for dolphin-emu-nogui (not
      committed; L5d owns the real X11 platform) opened a 640x480 window and passed
      {X11, Display*, window} like a frontend would. Screenshots (`import -window root`) of
      the window, mean R/G/B over 640x480: OpenGL/GLX 4.5 core 0.71/0.69/0.23, Vulkan Xlib
      surface 0.20/0.69/0.23, Vulkan XCB surface (Xlib path disabled for the run)
      0.67/0.69/0.23, OpenGL ES 3.2 through EGL on X11 0.68/0.68/0.23, software renderer
      (its GL window through GLX) 0.22/0.74/0.25: the clear colour (green 0.75, blue 0.25,
      red changing), never black. Worker contexts were made (GLX pbuffers). After resizing
      the window to 800x600 and calling Presenter::ResizeSurface(), the area outside the old
      640x480 is filled with the picture for GLX, EGL and Vulkan.
    - Headless (dolphin-emu-nogui -p headless): OpenGL through EGL_MESA_platform_surfaceless
      (GL 4.5 core) and Vulkan without a surface both render; frame dumps
      (DumpFramesAsImages) of the same frame are pixel-identical between the two (also with
      --triangle; the triangle looks the same in the software renderer, so its odd placement
      is the DOL's, not a backend's).
    - Every run exits 0 within a second of SIGINT.
    - Windows unchanged: the base tree (70b1a3e) and this tree (with the handoff patch) were
      configured as Windows with clang-cl 18 (Ninja, the Windows toolchain file, mingw headers,
      ENABLE_QT=OFF, ENCODE_FRAMEDUMPS=OFF): compile_commands.json is identical for all 1742
      sources and all 59 libraries/programs have the same objects and link lines. The Windows
      code of the changed files differs only by the new no-op virtual and its call in OGLGfx
      (not compiled here: no MSVC STL in the container).
  - CI must confirm (L5d's build-linux job):
    - ubuntu-22.04 packages for the build: `libx11-dev libegl-dev` (headers only; nothing
      linked). For the run: `xvfb mesa-vulkan-drivers libgl1-mesa-dri libegl1 libvulkan1`,
      and `libx11-xcb1` for the XCB path.
    - GLVND's older EGL headers on 22.04 (libegl-dev 1.4.0) compile EGL.cpp (checked here
      only by simulating Xlib-typed native handles).
    - With the X11 platform (L5d) under `xvfb-run -s '-screen 0 1280x1024x24'`:
      `python3 tools/pixelu-wii-test-dol.py t.dol`, then for `-v OGL` and `-v Vulkan`:
      `dolphin-emu-nogui -u $PWD/u -p x11 -v <backend> -e t.dol &`, after 8 s
      `import -window root s.png`, the window area's mean green must be > 0.5
      (`convert s.png -crop 640x480+0+0 -format '%[fx:mean.g]' info:`), then `kill -INT` and
      exit 0. Without the X11 platform the headless check works today:
      `-p headless -v OGL` and `-v Vulkan` with
      `-C Dolphin.Movie.DumpFrames=True` and `[Settings] DumpFramesAsImages = True` in
      `u/Config/GFX.ini` give PNGs in u/Dump/Frames whose mean green is about 0.75.
      (`-C GFX.Settings...=` did not take effect here; GFX.ini did. UICommon's option
      handling, L5d.)
    - Windows: the MSVC build of PixelU-Wii.exe still compiles the changed files.
  - Notes for other batches:
    - L5d (DolphinQt and nogui X11 platform): pass WindowSystemInfo{X11, Display*
      (QNativeInterface::QX11Application::display() or XOpenDisplay), the render window's
      XID as render_window and render_surface}. No XInitThreads is needed for video. Under a
      Wayland session Qt should be forced to xcb (the owner's XWayland decision); for a native
      Wayland frontend, display_connection is the wl_display*, render_surface the wl_surface*,
      and the frontend must call g_presenter->SetBackbuffer(w, h) before
      Presenter::ResizeSurface() since Wayland surfaces have no size (Vulkan and EGL read it).
    - Sys must sit next to dolphin-emu-nogui for the post-processing default shader
      (otherwise a harmless "Post-processing shader not found" error at start).
  - Not done: native Wayland was only built, not run (no compositor in the container).
    Quad-buffered stereo is not offered through EGL (EGL has no stereo config attribute);
    GLX has it.

- L5c log (PixelU-Wii input and sound on Linux, own): keyboard and mouse on
  X11 through a new XInput2 backend, cubeb's PulseAudio and ALSA backends in
  the Externals build (ENABLE_CUBEB is ON on Linux again), SDL game
  controllers built and enabled on Linux, and RetroPixels' Xbox-controller
  mapping (Device = XInput/0/Gamepad) working there through the SDL gamepad.
  Work started from HEAD 70b1a3e on `claude/wiiu-emulator-urpcen`. The new
  code is written from scratch from the X Input Extension 2 protocol and
  Xlib/XKB manuals; Dolphin's Xlib/XInput2 files were read only for the
  names configurations use (source "XInput2", the device named after the
  master pointer, the key names, "Click N", "Cursor/Axis/RelativeMouse X+").
  - New files (original-code header):
    - `InputCommon/ControllerInterface/Xlib/XInput2.h`, `XInput2.cpp`: the
      backend, used when the render window is an X11 window (also under
      XWayland). It opens its own X connection (the frontend's display name),
      asks for XI 2.2, and makes one device per master pointer and its paired
      master keyboard, "XInput2/0/Virtual core pointer" on an ordinary
      desktop; that device is the default device (sort priority) and a
      virtual device, as DInput's keyboard and mouse on Windows.
      - Keys: one input per keysym name, as XKeysymToString writes it for the
        key's unshifted keysym in the first group that has one, with a-z in
        capitals ("A", "Return", "space", "Escape", "KP_Add", "F10",
        "Shift_L"...); keycodes with the same name are one input. The keypad
        digits are named without Num Lock ("KP_End") and also answer to their
        Num Lock name ("KP_1"), which the default hotkeys use. "Alt", "Shift"
        and "Ctrl" combine both sides. The client pointer's keyboard is read
        with XQueryKeymap on every update (no focus needed, like DInput's
        background mode); other master keyboards follow raw key events.
      - Mouse: "Click 1".."Click 32" (X11 numbering: 1 left, 2 middle,
        3 right; 10-32 hidden but mappable) and the cursor from XIQueryPointer
        per seat; "Cursor X/Y" is -1..1 across the render window (size kept
        from ConfigureNotify) times the window input scale, with mouse
        centring (XIWarpPointer) and the TAS-window rule as on Windows.
        "Axis X/Y/Z" (movement per update, averaged with the previous update,
        8 units = full; Z the wheel, one notch = full) and "RelativeMouse
        X/Y/Z" (per input channel) come from raw motion and raw wheel-button
        events selected on the root window; smooth-scrolling valuators are
        used when a device has them (their emulated wheel clicks are then
        skipped), and absolute devices are turned into movement. After 250 ms
        without an update the movement is dropped, as DInput's is.
      - X errors on its own connection (BadWindow when the render window goes
        away) are recorded instead of reaching Xlib's default handler, which
        would end the program; errors of other connections go on to the
        handler that was there before. A window change (docking) only changes
        the window the cursor is measured in.
    - `InputCommon/LinuxCheck/InputCheck.cpp`: `pixelu-wii-input-check`, a
      check program (EXCLUDE_FROM_ALL, `ninja pixelu-wii-input-check`). On an
      X server it opens a window, starts the controllers as the frontends do
      (UICommon::InitControllers), drives the keyboard and mouse with xdotool
      and reads them back from the device and through the default hotkeys,
      then plugs in an SDL virtual gamepad, writes RetroPixels' GCPadMap into
      GCPadNew.ini and reads the emulated GameCube pad 1 (A, main stick,
      L analog) and its rumble. Exit 0 when every check passes.
  - Changed:
    - `Externals/cubeb/CMakeLists.txt`: WASAPI/WinMM only on Windows (same
      sources and defines there); on Linux cubeb_pulse.c and cubeb_alsa.c
      with USE_PULSE/USE_ALSA, options CUBEB_PULSE and CUBEB_ALSA (ON), a
      clear error when pulse/pulseaudio.h or alsa/asoundlib.h is missing.
      cubeb's lazy loading is kept: libpulse.so.0 and libasound.so.2 are
      dlopened when a context is made, so neither is NEEDED by the binary;
      without both, PixelU-Wii falls back to its null output.
    - Top-level `CMakeLists.txt` (L5a's file; the one line the brief asked
      for): `ENABLE_CUBEB` defaults ON on every system again.
    - `InputCommon/CMakeLists.txt`: off Windows, option `ENABLE_X11` (ON):
      FindX11 with Xi required (error naming libx11-dev/libxi-dev), the
      Xlib sources, X11::X11 and X11::Xi, and `HAVE_X11=1` as a PUBLIC
      definition of inputcommon, so core (WiimoteEmu's and FreeLook's
      defaults: "Click 1"/"Click 3") sees it too; the check program.
    - `ControllerInterface.h/.cpp`: `CIFACE_USE_XLIB` with HAVE_X11; the
      XInput2 backend is created before SDL (so the default device exists
      when profiles load).
    - `ControllerInterface/CoreDevice.cpp` (FindDevice, not on Windows): a
      device string "XInput/<n>/Gamepad" with no such device stands for the
      n-th SDL device that has a gamepad mapping (lowest SDL id first, n
      0-3). HasConnectedDevice, the mapping windows and the emulated
      controllers all go through FindDevice.
    - SDL (Externals/SDL): unchanged; it already builds on Linux with the
      joystick, gamepad, haptic, sensor and HIDAPI subsystems (joystick
      drivers: hidapi, linux evdev, virtual; udev for hotplug, D-Bus loaded
      at run time) and ENABLE_SDL is ON on every system. AudioCommon:
      unchanged (CubebStream/CubebUtils already built off Windows).
  - For RetroPixels' "Set up Xbox controller" button (L1's request): on
    Linux keep writing exactly what Windows writes. `Device =
    XInput/0/Gamepad` is an alias here (above), and every input and output
    name of GCPadMap and WiimoteMap exists on an SDL gamepad: "Button A",
    "Button B", "Button X", "Button Y" (SDL's face buttons accept XInput's
    names by position), "Start", "Back", "Shoulder L/R", "Thumb L/R",
    "Trigger L/R", "Left X+/-", "Left Y+/-", "Right X+/-", "Right Y+/-",
    "Pad N/S/W/E", outputs "Motor L", "Motor R" (when the pad can rumble).
    The pad's own name is "SDL/<id>/<SDL's gamepad name>", e.g. "SDL/0/Xbox
    360 Controller" — not fixed, which is why the alias is the stable name.
    So GCPadMap/WiimoteMap need no per-system change; PadState("xbox") keeps
    working.
  - Checked here (Ubuntu 24.04 container, no GPU, no sound card):
    - gcc-12 Release build of dolphin-emu-nogui and pixelu-wii-input-check
      (configure: `-DENABLE_QT=OFF`, cubeb backends "PulseAudio, ALSA (loaded
      at run time)"); no warnings in the new or changed files. The new and
      changed .cpp files also compile with clang++-19 -Wall -Wextra without
      warnings. clang-format (the tree's .clang-format) clean.
    - Xvfb :57 (XI 2.x) with xdotool: pixelu-wii-input-check passes all 82
      checks, three runs in a row: key press/release, right Shift as "Shift",
      cursor 0 in the middle and 0.5 at three quarters, buttons 1 and 3,
      relative movement (24 units right: Axis X+ 1.5, RelativeMouse X+ 24),
      wheel up (Axis Z+), movement settling, hotkeys Stop (Escape) and Full
      screen (Alt+Return), XInput/0/Gamepad → "SDL/0/Virtual test gamepad",
      the emulated GameCube pad reading A, stick right and L from
      RetroPixels' mapping, rumble reaching the pad (both motors), unplug,
      window change and a destroyed render window (no X error exit).
    - Sound with a test DOL (288 bytes, a counting loop, written by a
      throwaway script; no Nintendo code), `dolphin-emu-nogui -v Null
      -C Dolphin.DSP.Backend=Cubeb`:
      - PulseAudio with a null sink: "Cubeb initialized using pulse
        backend", stream started; `pactl list sink-inputs` shows the stream
        (s16le 2ch 48000 Hz, not corked, application.process.id = the
        nogui pid, application.name "Dolphin Emulator").
      - No PulseAudio server, ALSA default = a file plugin: "Cubeb
        initialized using alsa backend", samples written.
      - Neither usable (ALSA default a missing card): "Could not initialize
        backend Cubeb, using No Audio Output instead", the game runs.
      - Each run ends with SIGINT, exit 0.
    - Binary: NEEDED adds libX11.so.6 and libXi.so.6 (no libpulse/libasound);
      the glibc floor is unchanged (GLIBC_2.38 here only from the container,
      as in L5a).
    - Windows unchanged: base tree (70b1a3e) and this tree configured as
      Windows with clang-cl (MSVC mode, Ninja, the toolchain file, mingw
      headers, ENABLE_QT=OFF, ENABLE_LLVM=OFF): compile_commands.json is
      identical for all 1742 sources and the 59 link targets have the same
      inputs and libraries.
  - CI must confirm (L5d's build-linux job):
    - apt on ubuntu-22.04: `libx11-dev libxi-dev libpulse-dev libasound2-dev`
      (plus L5a's list); for the checks `xvfb xdotool pulseaudio
      pulseaudio-utils`.
    - `ninja -C build dolphin-emu-nogui pixelu-wii-input-check`, then under
      `xvfb-run -a -s '-screen 0 1280x1024x24'`:
      `build/Binaries/pixelu-wii-input-check "$RUNNER_TEMP/input-user"`
      exits 0 (it prints one ok/FAIL line per check).
    - Sound: `pulseaudio -D --exit-idle-time=-1 && pactl load-module
      module-null-sink`; nogui with `-C Dolphin.DSP.Backend=Cubeb
      -C Logger.Logs.AUDIO=True -C Logger.Options.Verbosity=4 -e test.dol`;
      after a few seconds `pactl list sink-inputs` lists a stream whose
      application.process.id is nogui's pid, and the log has "Cubeb
      initialized using pulse backend".
    - The 22.04 headers (libpulse 15, alsa-lib 1.2.6, libXi 1.8) compile
      cubeb's backends and XInput2.cpp as here.
  - Only a real desktop can confirm: a physical keyboard and mouse (smooth
    scrolling on a touchpad, a tablet's absolute valuators, mouse centring in
    a game), XWayland (raw events only while the pointer is over an X
    window), PipeWire's PulseAudio server, a real Xbox/PlayStation pad over
    evdev and HIDAPI (hidraw needs the usual udev rules for some pads),
    rumble on hardware, and microphone input (Wii Speak, GameCube mic) through
    cubeb on Pulse/ALSA.
  - For other batches (not changed here):
    - Core (`Core/HW/GCPadEmu.cpp`): the GameCube pad's keyboard defaults are
      Windows names on every system ("`RETURN`", "`UP`", "`DOWN`", "`LEFT`",
      "`RIGHT`"); with the X11 names they would be "`Return`", "`Up`",
      "`Down`", "`Left`", "`Right`" (letters "X", "Z"... already match). Until
      then Start and the main stick have no default key on Linux.
    - L5d: the platform and Qt must pass `WindowSystemType::X11` with the
      Display* and the X window id (as a void*) for the keyboard backend to
      start; nogui's headless platform has no keyboard (as intended). The
      mapping window will show "XInput/0/Gamepad" as the configured device
      even though the device list names the SDL pad.
    - L5d/CI: configure with `-DENABLE_LLVM=OFF` (or without LLVM installed):
      here the host's LLVM 19 was found and dolphin-emu-nogui got NEEDED
      libLLVM.so.19.1, which a desktop does not have.
    - The nogui program now needs libX11/libXi at start-up (inputcommon links
      them); on a machine without any X libraries, configure with
      -DENABLE_X11=OFF.
    - With the Null video backend a game runs faster than real time and the
      mixer logs "Granule Queue has completely filled" (not Linux-specific).
- L5d log (the PixelU-Wii program on Linux, own): the Qt program
  (DolphinQt) builds and runs on Linux as `PixelU-Wii`, docks into
  RetroPixels under the contract of embed_linux.go, keeps its settings in
  PixelU/wii/User through portable.txt, and has a Linux job in
  .github/workflows/pixelu-wii.yml that builds, packages, checks and
  hands the package to bundle-linux.yml. dolphin-emu-nogui gained an X11
  platform. Work started from 9ba8d99. New files carry the original-code
  header; Dolphin was read only to learn what a piece has to do (the
  option names, the config system names, which class the frontends
  expect), never copied.
  - The Qt program (pixelu/wii/Source/Core/DolphinQt):
    - CMake: off Windows the target is still dolphin-emu, the program
      PixelU-Wii. It links X11::xcb (configure stops when xcb is missing
      with ENABLE_X11 on) and Qt D-Bus when Qt has it. windeployqt and
      qt.conf.win are now Windows only (the Windows build is unchanged,
      see "Windows" below).
    - Handles: the render widget's native window goes to the core as
      {WindowSystemType::X11, Display*, X window id}, as before. Qt is
      forced to xcb (QT_QPA_PLATFORM unset), so under Wayland it runs on
      XWayland. Only when there is no DISPLAY but a WAYLAND_DISPLAY does
      Qt stay on Wayland; a resize then also sets the presenter's
      backbuffer (Host::ResizeSurface), since Wayland surfaces report no
      size. That path is untested (no Wayland compositor here).
    - --rp-embed (Main.cpp) is taken off the command line before Qt sees
      it and means PIXELU_EMBED=1, which is what RetroPixels sets today
      (internal/dolphin dolphin.go). Both work.
    - Docking (new QtUtils/X11Docking.cpp/.h, RenderWidget.cpp): in embed
      mode the render window gets _RETROPIXELS_EMBED = 1 (CARDINAL) and,
      before Qt maps it, is reparented into a 1x1 holder window that is
      never mapped. So it is "mapped" for Qt and the renderer (it gets
      resize events, the swap chain is made) but not viewable, and the
      window manager never sees it: no MapRequest, not in
      _NET_CLIENT_LIST. Every 100 ms it looks whether its parent carries
      _RETROPIXELS_DOCK (RetroPixels' pid): then it is docked and stays
      put. After 20 s without that (the Windows deadline) the mark is
      removed and the window goes back to the root window, centred on the
      screen, raised and activated, as an ordinary window.
      _NET_WM_PID and WM_DELETE_WINDOW come from Qt. The xcb calls use
      Qt's own connection (QNativeInterface::QX11Application); without
      xcb (Wayland) docking is off and the window just shows.
      WA_DontShowOnScreen was tried on paper and dropped: Qt then sends
      no resize events to the widget (QWidgetWindow::updateSize), and the
      picture would not follow RetroPixels' layout.
    - QtUtils/SignalDaemon.cpp/.h (new; MainWindow.cpp already included
      it): SIGINT and SIGTERM end the program cleanly (self-pipe and a
      QSocketNotifier, then MainWindow's close path, status 0).
    - Screen saver: besides UICommon's X11 call, the program asks
      org.freedesktop.ScreenSaver for Inhibit/UnInhibit over D-Bus while
      a game runs (MainWindow::UpdateScreenSaverInhibition). Linking Qt
      D-Bus also turns on QtUtils' "show in file manager" through
      org.freedesktop.FileManager1.
    - LinuxCheck/DockCheck.cpp (new): pixelu-wii-dock-check
      (EXCLUDE_FROM_ALL). Modes:
      - plain / nogui: the game window appears, shows the test DOL (mean
        green over 0.5, read back from the screen), SIGINT ends it with 0.
      - dock / rp-embed: a window marked _RETROPIXELS_EMBED appears; for
        3 s it is not viewable and not managed; it is docked as
        embed_linux.go docks (unmap and a synthetic UnmapNotify, save
        set, reparent into a box with _RETROPIXELS_DOCK, resize, map);
        the game shows inside RetroPixels' window; it is still docked
        after the 20 s deadline; the picture follows a resize;
        WM_DELETE_WINDOW stops the game and the program ends with 0.
      - give-up: nobody docks it; after the deadline it is an ordinary,
        unmarked window with the game in it; SIGTERM ends it with 0.
  - UICommon: on Linux, portable.txt next to the program keeps the user
    folder in User/ next to it (PixelU/wii/User, the name RetroPixels'
    internal/dolphin userDirFor expects), checked before the embedded
    ./user rule. The --audio_emulation help no longer prints a literal
    "[%choices]". Discord presence no longer registers its
    discord-<id>:// handler on Linux: discord-rpc did that by writing
    ~/.local/share/applications/discord-455712169795780630.desktop and
    ~/.config/mimeapps.list on every start, even with portable.txt (found
    by the checks here; Windows still registers it, unchanged). CI fails
    when such a file appears.
  - dolphin-emu-nogui: new PlatformX11.cpp, -p x11 (headless stays the
    default; -p lists headless and x11, the help is written out instead
    of "%choices"). It opens its own X connection (XInitThreads first),
    makes a window from the render window settings (size; position only
    when one is saved, -1 means the window manager places it), sets the
    title, WM_CLASS pixelu-wii/PixelU-Wii, _NET_WM_PID,
    WM_CLIENT_MACHINE, WM_DELETE_WINDOW and size hints, fullscreen
    through _NET_WM_STATE, a blank cursor when the cursor is hidden.
    The loop polls the X socket and an eventfd: the close button stops
    the game, ConfigureNotify resizes the presenter's surface, F11 and
    Alt+Return toggle fullscreen; title and size requests from other
    threads are applied there.
  - tools/pixelu-wii-linux-deploy.sh (new, shellcheck clean): the Linux
    counterpart of windeployqt. Copies the program (RUNPATH $ORIGIN/lib)
    and puts next to it lib/ (the Qt libraries it and its plugins need,
    with ICU, and the xcb-util family / xkbcommon-x11 that desktops do
    not always have; RUNPATH $ORIGIN), QtPlugins/ (xcb, SVG image and
    icon engines required; offscreen, Wayland, GTK 3 and XDG portal
    themes when the Qt has them and their libraries load; each finds
    lib/ two folders up) and qt.conf. It then checks that every file
    resolves without LD_LIBRARY_PATH and takes Qt only from the folder.
    Works with aqt's layout and with a distribution's /usr.
  - CI, build-linux in pixelu-wii.yml (the Windows job is byte for byte
    unchanged):
    - ubuntu-22.04 (glibc 2.35 floor), GCC 12, CMake 4.2, Ninja.
    - Qt 6.8.3 linux_gcc_64 by aqtinstall 3.3.0 (qtbase, qtsvg,
      qtwayland, icu), cached. FFmpeg from source with the Windows job's
      build-ffmpeg.sh (PIXELU_FFMPEG_HOST_TEST=1, host GCC), cached.
    - Sys assembled by PixelUAssembleSys.cmake from the sparse Dolphin
      checkout (the same sparse paths as the Windows job), translations
      from Languages/po.
    - Checks: glibc floor and no libLLVM; package contents and
      check_sys_package.py; --version (offscreen) and nothing written to
      the home folder; under Xvfb with a PulseAudio null sink:
      pixelu-wii-input-check, nogui headless start-up (Booting from
      executable, Shutdown complete, status 0), cubeb on the pulse
      backend with a stream of the nogui pid (pactl), the game on screen
      for nogui -p x11 and the Qt program on OpenGL (llvmpipe) and
      Vulkan (lavapipe), then give-up without a window manager and dock
      / rp-embed under openbox.
    - Artifact PixelU-Wii-linux-x64: one PixelU-Wii-linux.tar.gz with
      PixelU/wii/PixelU-Wii inside (tools/linux/package.sh finds it), then
      `gh workflow run bundle-linux.yml`.
  - Checked here (Ubuntu-like container, GCC 12, system Qt 6.4.2 because
    download.qt.io is blocked, so no aqt Qt locally):
    - Configure and build of dolphin-emu, dolphin-emu-nogui,
      pixelu-wii-input-check and pixelu-wii-dock-check; clang-format
      (the repository's style) clean on all new and changed lines.
    - Under Xvfb (and openbox for docking), the same commands as the CI
      steps: input check passed; nogui headless boots the test DOL and
      ends with status 0 and "Shutdown complete"; cubeb on PulseAudio
      with a sink input of the nogui pid; dock-check plain and nogui
      pass on OGL and Vulkan; give-up, dock and rp-embed pass.
    - The deploy script against /usr Qt: the deployed program runs
      (--version, plain on OGL and Vulkan), without -u it makes
      User/ next to itself, and nothing goes to ~/.config or
      ~/.local/share.
    - FFmpeg: build-ffmpeg.sh with PIXELU_FFMPEG_HOST_TEST=1 builds the
      five static libraries; they link into a PIE with -z text (no text
      relocations) and the ffv1, mpeg4, utvideo encoders and the avi
      muxer are there; the configure step reports "FFmpeg found", the
      whole tree builds and links with it, and nogui -p x11 with
      -C Dolphin.Movie.DumpFrames=True writes an AVI (MPEG-4, 640x528)
      to User/Dump/Frames.
    - Windows: a clang-cl Windows configure (mingw headers, a stand-in
      Qt) of 9ba8d99 and of this tree give the same 1944 compile
      commands; build.ninja differs only in the version root, a
      path-hashed depfile name and the order of phony lists; the
      PixelU-Wii.exe link line is the same. GOOS=windows go build and go
      test ./... on Linux pass, gofmt is clean (no Go code changed).
  - Notes:
    - `-C GFX.Settings...` (from L5b's notes) is not a bug: the system is
      called "Graphics" in -C (Config.cpp's names), so
      `-C Graphics.Settings.X=Y`.
    - Qt 6.4's moc cannot parse C++20 concepts in libstdc++ 12 headers
      (<concepts>, through WiimoteControllersWidget.h). Only the local
      build hit this; it was built with an extra moc include that
      defines libstdc++'s visibility macros. Qt 6.8.3 (CI) is not
      affected. Building against a distribution's Qt older than 6.5 needs
      that or a newer Qt.
    - The libstdc++ floor is GCC 12's (GLIBCXX_3.4.30), which Ubuntu
      22.04 and Debian 12 ship; glibc 2.35 in CI.
    - GOOS=windows go vet reports "possible misuse of unsafe.Pointer" in
      winapi_windows.go:87-88 (comCall); that is older than L5d and not
      in its files.
    - pixelu/wii/Externals/pugixml/CMakeLists.txt sets
      PUGIXML_BUILD_DEFINES to OFF, which pugixml turns into a -DOFF
      define on every file that uses it (Windows too). Harmless, but it
      should be set to "" (not an L5d file).
    - For L2b (embed_linux.go): withdraw() waits up to 1 s for the window
      to reach the root window, but a waiting PixelU-Wii window never
      does (it sits in its holder). Windows marked _RETROPIXELS_EMBED
      can skip that wait.
    - A Go test in the main package that docks the real PixelU-Wii
      (as there is for PixelU) would cover the RetroPixels side;
      pixelu-wii-dock-check covers the program's side.
  - Not done:
    - Fullscreen at a chosen resolution (XRandR) and cursor locking or
      confining on Linux.
    - Screen saver inhibition without D-Bus beyond UICommon's X11 call.
    - A native Wayland run (only XWayland is supported and checked).
  - Only CI or a real desktop can confirm: the aqt archive names
    (qtwayland, icu) and that Qt 6.8.3's plugins deploy as described;
    FFmpeg's host build on Ubuntu 22.04 (here a newer distribution);
    lavapipe and llvmpipe on the runner; openbox there; the package on a
    real GNOME/KDE desktop, under XWayland, and docked by RetroPixels
    itself.
