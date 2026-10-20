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
| Original (RetroPixels) | 8998 | 2.0% |
| Dolphin, untouched | 425224 | 95.7% |
| Dolphin, with PixelU changes | 4224 | 1.0% |
| PixelU-added files (no original mark) | 0 | 0.0% |
| Third-party | 5763 | 1.3% |
| **All** | **444209** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| Core/AudioCommon | 3219 | 0 | Dolphin |
| Core/Common (top level) | 29392 | 7359 | mixed (25% original) |
| Core/Common/Assembler | 5585 | 0 | Dolphin |
| Core/Common/Config | 865 | 0 | Dolphin |
| Core/Common/Crypto | 1202 | 1202 | original |
| Core/Common/Debug | 437 | 437 | original |
| Core/Common/GL | 9096 | 0 | Dolphin, 5672 lines third-party |
| Core/Common/Logging | 620 | 0 | Dolphin |
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
| Core/Core/PowerPC (top level) | 11150 | 0 | Dolphin |
| Core/Core/PowerPC/CachedInterpreter | 1020 | 0 | Dolphin |
| Core/Core/PowerPC/Interpreter | 6120 | 0 | Dolphin |
| Core/Core/PowerPC/Jit64 | 10326 | 0 | Dolphin |
| Core/Core/PowerPC/Jit64Common | 2549 | 0 | Dolphin |
| Core/Core/PowerPC/JitCommon | 2294 | 0 | Dolphin |
| Core/Core/PowerPC/SignatureDB | 755 | 0 | Dolphin |
| Core/DiscIO | 17619 | 0 | Dolphin |
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
| W2 | Common: types, bits, containers, strings, files, threads, config, logging | 15729 | 0 | Dolphin |
| W3 | Common: platform, memory, network, crypto, file formats | 8998 | 8998 | original |
| W4 | Common: x64 emitter, disassemblers, PowerPC assembler | 13374 | 0 | Dolphin |
| W5 | DiscIO: blob formats and compression | 8943 | 0 | Dolphin |
| W6 | DiscIO: volumes, filesystems, verifier, Riivolution, NAND import | 8676 | 0 | Dolphin |
| W7 | PowerPC: CPU state, MMU, caches, analyst, tables, symbols, breakpoints, GDB stub | 11905 | 0 | Dolphin |
| W8 | PowerPC: interpreter and cached interpreter | 7140 | 0 | Dolphin |
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
