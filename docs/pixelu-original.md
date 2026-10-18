# PixelU: replacing Cemu code with original code

PixelU (in `pixelu/`) started as a fork of the Cemu Wii U emulator (MPL-2.0).
The goal is for PixelU to become entirely RetroPixels' own code, one piece at
a time, like the ship of Theseus: every step leaves an emulator that builds,
runs and keeps every feature.

## Method

MPL-2.0 applies file by file. A file written from scratch is new code with no
MPL obligations; a Cemu file that we only edit stays Cemu (MPL) code.

For each file we replace:

1. Read the Cemu file **only for its interface**: what the rest of PixelU
   calls (names in its header, `GetModule()`, init functions), the Wii U
   functions it registers (`cafeExportRegister`, `cafeExportRegisterFunc`,
   `osLib_addFunction`) and the results games get back from them.
2. Write the file again from our own knowledge of the Wii U system libraries
   (WiiUBrew and the wut headers are fine API references). Do not copy Cemu's
   code, comments, structure or internal names; keep only the interface other
   files and games depend on.
3. Behaviour stays the same from a game's point of view: same exports, same
   return values, same data written back. Fixing an obvious gap (for example a
   copy mode Cemu did not implement) is fine; changing what working games see
   is not.
4. Start the file with this header and **no** MPL notice (no license has been
   chosen for original code yet):

   ```cpp
   // Original code for RetroPixels (PixelU). Not derived from Cemu.
   ```

5. Keep the same file name so CMake lists do not change, compile-check
   it (`g++ -std=c++20 -fsyntax-only -include Common/precompiled.h -I. -ICafe
   -I../dependencies -I../dependencies/Vulkan-Headers/include -DENABLE_VULKAN
   -DVK_NO_PROTOTYPES` from `pixelu/src`), let the Windows CI build
   (`.github/workflows/pixelu.yml`) confirm it, and run
   `python3 tools/pixelu-original.py` to update the table below.

Order of work: small, self-contained HLE libraries first, then the pieces the
user's games (DuckTales Remastered, Mighty No. 9, ...) rely on most, last of
all the CPU/GPU core.

## Status

Status values: **original** = rewritten from scratch; **Cemu** = Cemu's file
unchanged; **Cemu + PixelU change** = Cemu's file with our edits (still Cemu
code); **mixed** = some files of the module rewritten; **third-party** =
bundled libraries that are neither Cemu's nor ours.

<!-- table:start -->
Totals (lines in pixelu/src, h/hpp/c/cpp/mm/S files):

| Kind | Lines | Share |
|---|---:|---:|
| Original (RetroPixels) | 1769 | 0.5% |
| Cemu, untouched | 278333 | 82.7% |
| Cemu, with PixelU changes | 15907 | 4.7% |
| PixelU-added files (MPL-2.0 header) | 1940 | 0.6% |
| Third-party | 38766 | 11.5% |
| **All** | **336715** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| (top level: main.cpp, mainLLE.cpp) | 324 | 0 | Cemu + PixelU change |
| audio | 3050 | 0 | Cemu, 1058 lines third-party |
| Cafe (CafeSystem, GamePatch) | 1843 | 0 | Cemu + PixelU change |
| Cafe/Account | 780 | 0 | Cemu |
| Cafe/Filesystem | 4429 | 0 | Cemu |
| Cafe/GameProfile | 464 | 0 | Cemu |
| Cafe/GraphicPack | 3437 | 0 | Cemu |
| Cafe/HW/ACR | 68 | 0 | Cemu |
| Cafe/HW/AI | 19 | 0 | Cemu |
| Cafe/HW/Common | 211 | 0 | Cemu |
| Cafe/HW/Espresso (core) | 1089 | 0 | Cemu |
| Cafe/HW/Espresso/Debugger | 2772 | 0 | Cemu |
| Cafe/HW/Espresso/Interpreter | 6641 | 0 | Cemu |
| Cafe/HW/Espresso/Recompiler | 24605 | 0 | Cemu |
| Cafe/HW/Latte/Common | 5959 | 0 | Cemu |
| Cafe/HW/Latte/Core | 20296 | 0 | Cemu + PixelU change |
| Cafe/HW/Latte/ISA | 3062 | 0 | Cemu |
| Cafe/HW/Latte/LatteAddrLib | 2206 | 0 | Cemu |
| Cafe/HW/Latte/LegacyShaderDecompiler | 13807 | 0 | Cemu |
| Cafe/HW/Latte/Renderer | 1236 | 0 | Cemu + PixelU change |
| Cafe/HW/Latte/Renderer/Metal | 7309 | 0 | Cemu |
| Cafe/HW/Latte/Renderer/OpenGL | 5862 | 0 | Cemu + PixelU change |
| Cafe/HW/Latte/Renderer/Vulkan | 14432 | 0 | Cemu + PixelU change |
| Cafe/HW/Latte/ShaderInfo | 73 | 0 | Cemu |
| Cafe/HW/Latte/Transcompiler | 1067 | 0 | Cemu |
| Cafe/HW/MMU | 808 | 0 | Cemu |
| Cafe/HW/SI | 165 | 0 | Cemu |
| Cafe/HW/VI | 11 | 0 | Cemu |
| Cafe/IOSU (shared headers) | 107 | 0 | Cemu |
| Cafe/IOSU/ccr_nfc | 437 | 0 | Cemu |
| Cafe/IOSU/fsa | 1257 | 0 | Cemu |
| Cafe/IOSU/kernel | 809 | 0 | Cemu |
| Cafe/IOSU/legacy | 5530 | 0 | Cemu |
| Cafe/IOSU/nn | 2454 | 0 | Cemu |
| Cafe/IOSU/ODM | 166 | 0 | Cemu |
| Cafe/IOSU/PDM | 537 | 0 | Cemu |
| Cafe/OS/common | 634 | 0 | Cemu |
| Cafe/OS/libs (shared headers) | 374 | 0 | Cemu |
| Cafe/OS/libs/avm | 60 | 60 | original |
| Cafe/OS/libs/camera | 266 | 266 | original |
| Cafe/OS/libs/coreinit | 19977 | 0 | Cemu |
| Cafe/OS/libs/dmae | 174 | 174 | original |
| Cafe/OS/libs/drmapp | 53 | 53 | original |
| Cafe/OS/libs/erreula | 598 | 0 | Cemu + PixelU change |
| Cafe/OS/libs/gx2 | 7994 | 0 | Cemu |
| Cafe/OS/libs/h264_avc | 3097 | 0 | Cemu |
| Cafe/OS/libs/mic | 493 | 0 | Cemu |
| Cafe/OS/libs/nfc | 2020 | 0 | Cemu |
| Cafe/OS/libs/nlibcurl | 1708 | 0 | Cemu |
| Cafe/OS/libs/nlibnss | 65 | 65 | original |
| Cafe/OS/libs/nn_ac | 363 | 0 | Cemu |
| Cafe/OS/libs/nn_acp | 407 | 0 | Cemu |
| Cafe/OS/libs/nn_act | 813 | 0 | Cemu |
| Cafe/OS/libs/nn_aoc | 182 | 182 | original |
| Cafe/OS/libs/nn_boss | 1360 | 0 | Cemu |
| Cafe/OS/libs/nn_ccr | 50 | 50 | original |
| Cafe/OS/libs/nn_cmpt | 67 | 67 | original |
| Cafe/OS/libs/nn_ec | 49 | 49 | original |
| Cafe/OS/libs/nn_fp | 847 | 0 | Cemu |
| Cafe/OS/libs/nn_idbe | 158 | 158 | original |
| Cafe/OS/libs/nn_ndm | 119 | 119 | original |
| Cafe/OS/libs/nn_nfp | 1354 | 0 | Cemu |
| Cafe/OS/libs/nn_nim | 326 | 0 | Cemu |
| Cafe/OS/libs/nn_olv | 4486 | 0 | Cemu |
| Cafe/OS/libs/nn_pdm | 66 | 66 | original |
| Cafe/OS/libs/nn_save | 864 | 0 | Cemu |
| Cafe/OS/libs/nn_sl | 121 | 121 | original |
| Cafe/OS/libs/nn_spm | 126 | 126 | original |
| Cafe/OS/libs/nn_temp | 65 | 65 | original |
| Cafe/OS/libs/nn_uds | 64 | 64 | original |
| Cafe/OS/libs/nsyshid | 6921 | 0 | Cemu |
| Cafe/OS/libs/nsyskbd | 84 | 84 | original |
| Cafe/OS/libs/nsysnet | 2328 | 0 | Cemu |
| Cafe/OS/libs/ntag | 763 | 0 | Cemu |
| Cafe/OS/libs/padscore | 1109 | 0 | Cemu |
| Cafe/OS/libs/proc_ui | 970 | 0 | Cemu |
| Cafe/OS/libs/snd_core | 5601 | 0 | Cemu + PixelU change |
| Cafe/OS/libs/snd_user | 1476 | 0 | Cemu |
| Cafe/OS/libs/swkbd | 730 | 0 | Cemu |
| Cafe/OS/libs/sysapp | 721 | 0 | Cemu |
| Cafe/OS/libs/TCL | 222 | 0 | Cemu |
| Cafe/OS/libs/vpad | 1319 | 0 | Cemu |
| Cafe/OS/libs/zlib125 | 411 | 0 | Cemu |
| Cafe/OS/RPL | 3366 | 0 | Cemu |
| Cafe/TitleList | 2597 | 0 | Cemu |
| Cemu/DiscordPresence | 846 | 0 | Cemu |
| Cemu/ExpressionParser | 725 | 0 | Cemu |
| Cemu/FileCache | 762 | 0 | Cemu |
| Cemu/Logging | 415 | 0 | Cemu |
| Cemu/napi | 2450 | 0 | Cemu |
| Cemu/ncrypto | 1203 | 0 | Cemu |
| Cemu/nex | 4220 | 0 | Cemu |
| Cemu/PPCAssembler | 4106 | 0 | Cemu |
| Cemu/Tools | 2017 | 0 | Cemu |
| Common | 31891 | 0 | Cemu, 28314 lines third-party |
| config | 3005 | 0 | Cemu + PixelU change |
| gui/interface | 126 | 0 | Cemu |
| gui/wxgui (top level) | 14868 | 0 | Cemu + PixelU change |
| gui/wxgui/canvas | 389 | 0 | Cemu + PixelU change |
| gui/wxgui/components | 5175 | 0 | Cemu |
| gui/wxgui/debugger | 3894 | 0 | Cemu |
| gui/wxgui/dialogs | 695 | 0 | Cemu |
| gui/wxgui/EmulatedUSBDevices | 943 | 0 | Cemu |
| gui/wxgui/helpers | 546 | 0 | Cemu |
| gui/wxgui/input | 4307 | 0 | Cemu + PixelU change |
| gui/wxgui/windows | 913 | 0 | Cemu |
| gui/wxgui/wxcomponents | 691 | 0 | Cemu |
| imgui | 3183 | 0 | Cemu |
| input (top level) | 1479 | 0 | Cemu + PixelU change |
| input/api | 5997 | 0 | Cemu |
| input/emulated | 2832 | 0 | Cemu + PixelU change |
| input/motion | 540 | 0 | Cemu |
| resource | 3594 | 0 | Cemu |
| tools | 145 | 0 | Cemu |
| util (top level) | 66 | 0 | Cemu |
| util/boost | 109 | 0 | Cemu |
| util/bootSound | 71 | 0 | Cemu |
| util/ChunkedHeap | 657 | 0 | Cemu |
| util/containers | 4318 | 0 | Cemu, 3949 lines third-party |
| util/crypto | 1659 | 0 | Cemu, 418 lines third-party |
| util/DXGIWrapper | 75 | 0 | Cemu |
| util/Fiber | 122 | 0 | Cemu |
| util/helpers | 2710 | 0 | Cemu |
| util/highresolutiontimer | 191 | 0 | Cemu |
| util/ImageWriter | 160 | 0 | Cemu |
| util/IniParser | 306 | 0 | Cemu |
| util/libusbWrapper | 122 | 0 | Cemu |
| util/math | 635 | 0 | Cemu |
| util/MemMapper | 156 | 0 | Cemu |
| util/ScreenSaver | 72 | 0 | Cemu |
| util/SystemInfo | 265 | 0 | Cemu |
| util/ThreadPool | 14 | 0 | Cemu |
| util/tinyxml2 | 5027 | 0 | third-party |
| util/VirtualHeap | 194 | 0 | Cemu |
| util/Zir | 2489 | 0 | Cemu |
<!-- table:end -->

## Log

- Batch 1: HLE libraries `avm`, `camera`, `dmae`, `drmapp`, `nlibnss`,
  `nn_aoc`, `nn_ccr`, `nn_cmpt`, `nn_ec`, `nn_idbe`, `nn_ndm`, `nn_pdm`,
  `nn_sl`, `nn_spm`, `nn_temp`, `nn_uds`, `nsyskbd` rewritten as original code
  (headers and sources). `TCL` skipped: it is the GPU command ring every game
  draws through.
