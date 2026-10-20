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
| Original (RetroPixels) | 63967 | 14.8% |
| Dolphin, untouched | 359457 | 82.9% |
| Dolphin, with PixelU changes | 4224 | 1.0% |
| PixelU-added files (no original mark) | 0 | 0.0% |
| Third-party | 5763 | 1.3% |
| **All** | **433411** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| Core/AudioCommon | 3219 | 0 | Dolphin |
| Core/Common (top level) | 24949 | 24949 | original |
| Core/Common/Assembler | 3802 | 3802 | original |
| Core/Common/Config | 909 | 909 | original |
| Core/Common/Crypto | 1202 | 1202 | original |
| Core/Common/Debug | 437 | 437 | original |
| Core/Common/GL | 9096 | 0 | Dolphin, 5672 lines third-party |
| Core/Common/Logging | 581 | 581 | original |
| Core/Core (top level) | 25099 | 0 | Dolphin + PixelU change |
| Core/Core/Boot | 2768 | 0 | Dolphin |
| Core/Core/Config | 2427 | 0 | Dolphin |
| Core/Core/ConfigLoaders | 1202 | 0 | Dolphin |
| Core/Core/Debugger | 3402 | 0 | Dolphin |
| Core/Core/DSP (top level) | 5220 | 0 | Dolphin |
| Core/Core/DSP/Interpreter | 4457 | 0 | Dolphin |
| Core/Core/DSP/Jit | 7632 | 0 | Dolphin |
| Core/Core/FifoPlayer | 2133 | 0 | Dolphin |
| Core/Core/HLE | 1350 | 0 | Dolphin |
| Core/Core/HW (top level) | 11546 | 0 | Dolphin |
| Core/Core/HW/DSPHLE | 7731 | 0 | Dolphin |
| Core/Core/HW/DSPLLE | 573 | 0 | Dolphin |
| Core/Core/HW/DVD | 5354 | 0 | Dolphin |
| Core/Core/HW/EXI | 8449 | 0 | Dolphin, 91 lines third-party |
| Core/Core/HW/GCMemcard | 3590 | 0 | Dolphin |
| Core/Core/HW/HSP | 1053 | 0 | Dolphin |
| Core/Core/HW/MagCard | 1368 | 0 | Dolphin |
| Core/Core/HW/SI | 3611 | 0 | Dolphin |
| Core/Core/HW/Triforce | 3620 | 0 | Dolphin |
| Core/Core/HW/WiimoteCommon | 1052 | 0 | Dolphin |
| Core/Core/HW/WiimoteEmu | 8117 | 0 | Dolphin |
| Core/Core/HW/WiimoteReal | 2579 | 0 | Dolphin |
| Core/Core/IOS (top level) | 3685 | 0 | Dolphin |
| Core/Core/IOS/Crypto | 250 | 0 | Dolphin |
| Core/Core/IOS/DI | 975 | 0 | Dolphin |
| Core/Core/IOS/ES | 5238 | 0 | Dolphin |
| Core/Core/IOS/FS | 2808 | 0 | Dolphin |
| Core/Core/IOS/Network | 8383 | 0 | Dolphin |
| Core/Core/IOS/SDIO | 885 | 0 | Dolphin |
| Core/Core/IOS/STM | 208 | 0 | Dolphin |
| Core/Core/IOS/USB (top level) | 2597 | 0 | Dolphin |
| Core/Core/IOS/USB/Bluetooth | 8741 | 0 | Dolphin |
| Core/Core/IOS/USB/Emulated | 5232 | 0 | Dolphin |
| Core/Core/IOS/USB/OH0 | 576 | 0 | Dolphin |
| Core/Core/IOS/USB/USB_HID | 584 | 0 | Dolphin |
| Core/Core/IOS/USB/USB_VEN | 194 | 0 | Dolphin |
| Core/Core/IOS/WFS | 1346 | 0 | Dolphin |
| Core/Core/PowerPC (top level) | 9959 | 9959 | original |
| Core/Core/PowerPC/CachedInterpreter | 1083 | 1083 | original |
| Core/Core/PowerPC/Interpreter | 4390 | 4390 | original |
| Core/Core/PowerPC/Jit64 | 10326 | 0 | Dolphin |
| Core/Core/PowerPC/Jit64Common | 2549 | 0 | Dolphin |
| Core/Core/PowerPC/JitCommon | 2294 | 0 | Dolphin |
| Core/Core/PowerPC/SignatureDB | 781 | 781 | original |
| Core/DiscIO | 15874 | 15874 | original |
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
| Core/VideoBackends/Null | 557 | 0 | Dolphin |
| Core/VideoBackends/OGL | 5742 | 0 | Dolphin |
| Core/VideoBackends/Software | 6734 | 0 | Dolphin |
| Core/VideoBackends/Vulkan | 10213 | 0 | Dolphin |
| Core/VideoCommon (top level) | 51142 | 0 | Dolphin |
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
| W2 | Common: types, bits, containers, strings, files, threads, config, logging | 13403 | 13403 | original |
| W3 | Common: platform, memory, network, crypto, file formats | 8998 | 8998 | original |
| W4 | Common: x64 emitter, disassemblers, PowerPC assembler | 9479 | 9479 | original |
| W5 | DiscIO: blob formats and compression | 7584 | 7584 | original |
| W6 | DiscIO: volumes, filesystems, verifier, Riivolution, NAND import | 8290 | 8290 | original |
| W7 | PowerPC: CPU state, MMU, caches, analyst, tables, symbols, breakpoints, GDB stub | 10740 | 10740 | original |
| W8 | PowerPC: interpreter and cached interpreter | 5473 | 5473 | original |
| W9 | PowerPC: JIT common and Jit64 | 15169 | 0 | Dolphin |
| W10 | HW: memory map, MMIO, CPU/PI/MI/VI/AI/DSP interfaces, timers, IPC, CoreTiming | 9898 | 0 | Dolphin |
| W11 | HW: DVD and GameCube memory cards | 8944 | 0 | Dolphin |
| W12 | HW: EXI devices (memory card slots, BBA, mic, modem, IPL) | 8358 | 0 | Dolphin |
| W13 | HW: SI, pads, keyboard, GBA link, HSP, Triforce, MagCard | 11839 | 0 | Dolphin |
| W14 | HW: Wii Remote (common, emulated, real) | 12108 | 0 | Dolphin |
| W15 | DSP: core, interpreter, LLE glue | 10311 | 0 | Dolphin |
| W16 | DSP: x64 JIT | 7632 | 0 | Dolphin |
| W17 | DSP: HLE (AX, Zelda, GBA, CARD, ROM ucodes) | 7731 | 0 | Dolphin |
| W18 | IOS: kernel, ES, FS, DI, SDIO, STM, WFS, crypto | 15395 | 0 | Dolphin |
| W19 | IOS: network (SO, KD/WC24, NCD, WD, SSL) and USB host devices | 12334 | 0 | Dolphin |
| W20 | IOS: Bluetooth and emulated USB devices (Skylanders, Infinity, keyboard, mic) | 13973 | 0 | Dolphin |
| W21 | Core: boot, config, config loaders, HLE, debugger, FIFO player | 13282 | 0 | Dolphin |
| W22 | Core: Core.cpp, System, state saves, movies, Wii root, title DB, hotkeys | 10095 | 0 | Dolphin + PixelU change |
| W23 | Core: cheats, patches, NetPlay, achievements, analytics, USB utils | 14044 | 0 | Dolphin |
| W24 | VideoCommon: GX state and command processing (BP/CP/XF, FIFO, PE, CPU cull) | 12077 | 0 | Dolphin |
| W25 | VideoCommon: vertex loaders and vertex manager | 4968 | 0 | Dolphin |
| W26 | VideoCommon: texture cache and decoders | 9127 | 0 | Dolphin |
| W27 | VideoCommon: shader generators and shader managers | 9946 | 0 | Dolphin |
| W28 | VideoCommon: renderer abstraction, framebuffers, shader cache, present, post-processing | 11210 | 0 | Dolphin |
| W29 | VideoCommon: OSD, frame dumping, performance, assets, graphics mods, resources | 13335 | 0 | Dolphin |
| W30 | Video backends: Software and Null | 7291 | 0 | Dolphin |
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
