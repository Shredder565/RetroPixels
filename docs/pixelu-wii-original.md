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
`ninja core` and so on compile one library at a time.

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
| Original (RetroPixels) | 0 | 0.0% |
| Dolphin, untouched | 468212 | 97.9% |
| Dolphin, with PixelU changes | 4224 | 0.9% |
| PixelU-added files (no original mark) | 0 | 0.0% |
| Third-party | 5763 | 1.2% |
| **All** | **478199** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| Core/AudioCommon | 3833 | 0 | Dolphin |
| Core/Common (top level) | 37655 | 0 | Dolphin |
| Core/Common/Assembler | 5585 | 0 | Dolphin |
| Core/Common/Config | 865 | 0 | Dolphin |
| Core/Common/Crypto | 1459 | 0 | Dolphin |
| Core/Common/Debug | 420 | 0 | Dolphin |
| Core/Common/GL | 10079 | 0 | Dolphin, 5672 lines third-party |
| Core/Common/Logging | 713 | 0 | Dolphin |
| Core/Core (top level) | 25261 | 0 | Dolphin + PixelU change |
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
| Core/Core/HW/EXI | 8744 | 0 | Dolphin, 91 lines third-party |
| Core/Core/HW/GCMemcard | 3590 | 0 | Dolphin |
| Core/Core/HW/HSP | 1053 | 0 | Dolphin |
| Core/Core/HW/MagCard | 1368 | 0 | Dolphin |
| Core/Core/HW/SI | 3611 | 0 | Dolphin |
| Core/Core/HW/Triforce | 3620 | 0 | Dolphin |
| Core/Core/HW/WiimoteCommon | 1052 | 0 | Dolphin |
| Core/Core/HW/WiimoteEmu | 8117 | 0 | Dolphin |
| Core/Core/HW/WiimoteReal | 3145 | 0 | Dolphin |
| Core/Core/IOS (top level) | 3685 | 0 | Dolphin |
| Core/Core/IOS/Crypto | 250 | 0 | Dolphin |
| Core/Core/IOS/DI | 975 | 0 | Dolphin |
| Core/Core/IOS/ES | 5238 | 0 | Dolphin |
| Core/Core/IOS/FS | 2808 | 0 | Dolphin |
| Core/Core/IOS/Network | 8483 | 0 | Dolphin |
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
| Core/Core/PowerPC/JitArm64 | 12529 | 0 | Dolphin |
| Core/Core/PowerPC/JitArmCommon | 44 | 0 | Dolphin |
| Core/Core/PowerPC/JitCommon | 2294 | 0 | Dolphin |
| Core/Core/PowerPC/SignatureDB | 755 | 0 | Dolphin |
| Core/DiscIO | 17619 | 0 | Dolphin |
| Core/DolphinQt (top level) | 15091 | 0 | Dolphin + PixelU change |
| Core/DolphinQt/Achievements | 1273 | 0 | Dolphin |
| Core/DolphinQt/Config | 18978 | 0 | Dolphin |
| Core/DolphinQt/Debugger | 14683 | 0 | Dolphin |
| Core/DolphinQt/EmulatedUSB | 367 | 0 | Dolphin |
| Core/DolphinQt/FIFO | 1371 | 0 | Dolphin |
| Core/DolphinQt/GameList | 2605 | 0 | Dolphin |
| Core/DolphinQt/InfinityBase | 403 | 0 | Dolphin |
| Core/DolphinQt/NetPlay | 3201 | 0 | Dolphin |
| Core/DolphinQt/QtUtils | 1493 | 0 | Dolphin |
| Core/DolphinQt/Settings | 4742 | 0 | Dolphin |
| Core/DolphinQt/SkylanderPortal | 1499 | 0 | Dolphin |
| Core/DolphinQt/TAS | 1979 | 0 | Dolphin |
| Core/DolphinTool | 1341 | 0 | Dolphin |
| Core/InputCommon (top level) | 2259 | 0 | Dolphin |
| Core/InputCommon/ControllerEmu | 3592 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface (top level) | 1863 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Android | 1242 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/DInput | 1112 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/DualShockUDPClient | 978 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/evdev | 982 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/ForceFeedback | 337 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Pipes | 235 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Quartz | 468 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/SDL | 1311 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/SteamDeck | 335 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Touch | 361 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/WGInput | 807 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Wiimote | 2020 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Win32 | 139 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/XInput | 350 | 0 | Dolphin |
| Core/InputCommon/ControllerInterface/Xlib | 665 | 0 | Dolphin |
| Core/InputCommon/ControlReference | 2333 | 0 | Dolphin |
| Core/InputCommon/DynamicInputTextures | 477 | 0 | Dolphin |
| Core/UICommon | 4641 | 0 | Dolphin |
| Core/UpdaterCommon | 903 | 0 | Dolphin |
| Core/VideoBackends/D3D | 3633 | 0 | Dolphin |
| Core/VideoBackends/D3D12 | 5320 | 0 | Dolphin |
| Core/VideoBackends/D3DCommon | 1094 | 0 | Dolphin |
| Core/VideoBackends/Metal | 4618 | 0 | Dolphin |
| Core/VideoBackends/Null | 557 | 0 | Dolphin |
| Core/VideoBackends/OGL | 5742 | 0 | Dolphin |
| Core/VideoBackends/Software | 6734 | 0 | Dolphin |
| Core/VideoBackends/Vulkan | 10213 | 0 | Dolphin |
| Core/VideoCommon (top level) | 52049 | 0 | Dolphin |
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

W1 is different: it **deletes** code no Windows x64 build compiles (the
ARM64 JIT and emitter, Metal, and the Android, macOS, Linux and BSD
platform backends) together with their entries in the CMake lists, instead
of rewriting it. That gives up building PixelU-Wii for those platforms,
which CI does not do; if that is not wanted, those files become batches of
their own instead. Each removal must be checked against the CMake
conditions (for example `IOhidapi` and `SteamDeck` are built on Windows
too, so they are not in W1).

<!-- batches:start -->
| Batch | Scope | Lines | Original lines | Status |
|---|---|---:|---:|---|
| W1 | Prune: code no Windows x64 build compiles (ARM64 JIT, Metal, Android, macOS, Linux backends) | 31970 | 0 | Dolphin |
| W2 | Common: types, bits, containers, strings, files, threads, config, logging | 15822 | 0 | Dolphin |
| W3 | Common: platform, memory, network, crypto, file formats | 10308 | 0 | Dolphin |
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
| W14 | HW: Wii Remote (common, emulated, real) | 11973 | 0 | Dolphin |
| W15 | DSP: core, interpreter, LLE glue | 10311 | 0 | Dolphin |
| W16 | DSP: x64 JIT | 7632 | 0 | Dolphin |
| W17 | DSP: HLE (AX, Zelda, GBA, CARD, ROM ucodes) | 7731 | 0 | Dolphin |
| W18 | IOS: kernel, ES, FS, DI, SDIO, STM, WFS, crypto | 15395 | 0 | Dolphin |
| W19 | IOS: network (SO, KD/WC24, NCD, WD, SSL) and USB host devices | 12434 | 0 | Dolphin |
| W20 | IOS: Bluetooth and emulated USB devices (Skylanders, Infinity, keyboard, mic) | 13973 | 0 | Dolphin |
| W21 | Core: boot, config, config loaders, HLE, debugger, FIFO player | 13282 | 0 | Dolphin |
| W22 | Core: Core.cpp, System, state saves, movies, Wii root, title DB, hotkeys | 10257 | 0 | Dolphin + PixelU change |
| W23 | Core: cheats, patches, NetPlay, achievements, analytics, USB utils | 14044 | 0 | Dolphin |
| W24 | VideoCommon: GX state and command processing (BP/CP/XF, FIFO, PE, CPU cull) | 12077 | 0 | Dolphin |
| W25 | VideoCommon: vertex loaders and vertex manager | 4968 | 0 | Dolphin |
| W26 | VideoCommon: texture cache and decoders | 9463 | 0 | Dolphin |
| W27 | VideoCommon: shader generators and shader managers | 9946 | 0 | Dolphin |
| W28 | VideoCommon: renderer abstraction, framebuffers, shader cache, present, post-processing | 11210 | 0 | Dolphin |
| W29 | VideoCommon: OSD, frame dumping, performance, assets, graphics mods, resources | 13335 | 0 | Dolphin |
| W30 | Video backends: Software and Null | 7291 | 0 | Dolphin |
| W31 | Video backends: OpenGL (with Common/GL) | 9166 | 0 | Dolphin |
| W32 | Video backends: Vulkan | 10213 | 0 | Dolphin |
| W33 | Video backends: D3D11, D3D12, D3DCommon | 10047 | 0 | Dolphin |
| W34 | InputCommon: controller emulation, control references, dynamic input textures | 8661 | 0 | Dolphin |
| W35 | InputCommon: ControllerInterface (DInput, XInput, WGInput, SDL, DSU, Wii Remote, Steam Deck) | 9252 | 0 | Dolphin |
| W36 | AudioCommon, UICommon, UpdaterCommon, DolphinTool, PCH | 9744 | 0 | Dolphin |
| W37 | DolphinQt: main window, menus, host, settings, render widget, hotkeys, Qt utils | 11063 | 0 | Dolphin + PixelU change |
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
