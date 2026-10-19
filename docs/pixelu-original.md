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
| Original (RetroPixels) | 27360 | 8.4% |
| Cemu, untouched | 241166 | 74.2% |
| Cemu, with PixelU changes | 15096 | 4.6% |
| PixelU-added files (MPL-2.0 header) | 1940 | 0.6% |
| Third-party | 39404 | 12.1% |
| **All** | **324966** | 100% |

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
| Cafe/OS/libs/erreula | 514 | 514 | original |
| Cafe/OS/libs/gx2 | 7994 | 0 | Cemu |
| Cafe/OS/libs/h264_avc | 3097 | 0 | Cemu |
| Cafe/OS/libs/mic | 397 | 397 | original |
| Cafe/OS/libs/nfc | 1081 | 1081 | original |
| Cafe/OS/libs/nlibcurl | 1243 | 1243 | original |
| Cafe/OS/libs/nlibnss | 65 | 65 | original |
| Cafe/OS/libs/nn_ac | 313 | 313 | original |
| Cafe/OS/libs/nn_acp | 304 | 304 | original |
| Cafe/OS/libs/nn_act | 568 | 568 | original |
| Cafe/OS/libs/nn_aoc | 182 | 182 | original |
| Cafe/OS/libs/nn_boss | 1145 | 1145 | original |
| Cafe/OS/libs/nn_ccr | 50 | 50 | original |
| Cafe/OS/libs/nn_cmpt | 67 | 67 | original |
| Cafe/OS/libs/nn_ec | 49 | 49 | original |
| Cafe/OS/libs/nn_fp | 736 | 736 | original |
| Cafe/OS/libs/nn_idbe | 158 | 158 | original |
| Cafe/OS/libs/nn_ndm | 119 | 119 | original |
| Cafe/OS/libs/nn_nfp | 850 | 850 | original |
| Cafe/OS/libs/nn_nim | 241 | 241 | original |
| Cafe/OS/libs/nn_olv | 2887 | 2887 | original |
| Cafe/OS/libs/nn_pdm | 66 | 66 | original |
| Cafe/OS/libs/nn_save | 566 | 566 | original |
| Cafe/OS/libs/nn_sl | 121 | 121 | original |
| Cafe/OS/libs/nn_spm | 126 | 126 | original |
| Cafe/OS/libs/nn_temp | 65 | 65 | original |
| Cafe/OS/libs/nn_uds | 64 | 64 | original |
| Cafe/OS/libs/nsyshid | 4944 | 4306 | mixed (87% original), 638 lines third-party |
| Cafe/OS/libs/nsyskbd | 84 | 84 | original |
| Cafe/OS/libs/nsysnet | 1374 | 1374 | original |
| Cafe/OS/libs/ntag | 623 | 623 | original |
| Cafe/OS/libs/padscore | 737 | 737 | original |
| Cafe/OS/libs/proc_ui | 868 | 868 | original |
| Cafe/OS/libs/snd_core | 3818 | 3818 | original |
| Cafe/OS/libs/snd_user | 1030 | 1030 | original |
| Cafe/OS/libs/swkbd | 613 | 613 | original |
| Cafe/OS/libs/sysapp | 435 | 435 | original |
| Cafe/OS/libs/TCL | 222 | 0 | Cemu |
| Cafe/OS/libs/vpad | 636 | 636 | original |
| Cafe/OS/libs/zlib125 | 306 | 306 | original |
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
| config | 3015 | 0 | Cemu + PixelU change |
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
- Batch 2: HLE libraries `erreula`, `mic`, `nn_ac`, `nn_acp`, `nn_act`,
  `nn_fp`, `nn_nim`, `ntag`, `swkbd`, `sysapp`, `zlib125` rewritten as
  original code (headers and sources). The erreula embedded-mode auto-answer
  for online error codes (102/103/115/118) is kept. Small fixes: zlib125 keeps
  each stream's zlib state in host memory (no more patching zlib internals in
  game memory), the mic test tone runs at the real sample rate, a failing host
  microphone no longer quits the emulator, ntag opens /dev/ccr_nfc once
  instead of on every tag operation.
- Batch 3: HLE libraries `nfc`, `nn_nfp` (amiibo), `nn_boss`, `nlibcurl`,
  `nsysnet` and `nsyshid` (including the Skylanders, Infinity and Dimensions
  portals and the libusb backend) rewritten as original code. Exports, guest
  struct layouts and the portal API the GUI uses are unchanged. The bundled
  G.721 codec in `nsyshid/g721/` is counted as third-party. `nn_olv` (Miiverse,
  about 4.5k lines of mirrored guest class layouts) is left for a later batch.
  Fixes: curl `CONTENT_TYPE` getinfo queried the wrong field,
  `curl_multi_info_read` returned the wrong handle and a little-endian count,
  easy handles no longer default to stdin/stdout when the game sets no
  read/write callback, the Dimensions password generator no longer reads past
  its buffer, the Infinity/Dimensions configuration descriptors are no longer
  corrupt, portal queues are locked, and libusb devices claim their interfaces
  once instead of on every transfer.
- Batch 4: HLE libraries `nn_save`, `proc_ui`, `padscore`, `vpad` and
  `snd_user` (snd_user and snduser2) rewritten as original code (headers and
  sources), plus `ax_aux.cpp` and `ax_multivoice.cpp` of `snd_core`. Exports,
  guest struct layouts, save paths and the save folder layout are unchanged.
  The GamePad touch conversion matches the old code for every raw value. The
  rest of `snd_core` (voices, ADPCM/PCM decoding, resampling, mixing, the
  output thread and the PixelU recorder hook in `ax_out.cpp`) is still Cemu's
  code; it is the audio engine every game goes through and is left for a
  batch of its own, together with `nn_olv`.
  Fixes: MotionPlus data reached games byte-swapped (KPAD MotionPlus structs
  are big endian now); KPADGetUnifiedWpadStatus reads every data format;
  WPADGetInfoAsync reports a missing controller as -1; padscore, vpad and
  snd_user drop the previous title's callbacks and state on each boot;
  VPADSetTPCalibrationParam stores both offsets; vpad checks the channel
  number everywhere; MIX DPL2 no longer silences front-left (it clears the unused
  centre and LFE slots instead) and 6-channel aux sends route their LFE to their own bus; the
  AXFX stub callbacks clear every channel; ProcUISetCallbackStackSize no longer
  drops registered callbacks and callbacks of equal priority run in
  registration order; nn_save application box 4 returns NOT_FOUND instead of
  an uninitialised path; AXAcquireMultiVoice no longer leaks voices when it
  runs out part way through.
- Batch 5: the rest of `snd_core` (`ax.h`, `ax_internal.h`, `ax_voice.cpp`,
  `ax_mix.cpp`, `ax_ist.cpp`, `ax_out.cpp`, `ax_exports.cpp`: voices, ADPCM
  and PCM decoding, resampling, envelopes, filters, bus mixing, aux, the
  32-to-48 kHz upsampler, final-mix callbacks, remix matrices, the frame
  thread and host output) and all of `nn_olv` (Miiverse, including the offline
  post archive) rewritten as original code. Exports, guest struct layouts
  (voice parameter blocks are checked byte for byte with static_asserts) and
  the PixelU recorder hook in `ax_out.cpp` are unchanged. An old-vs-new
  harness (ASan/UBSan, random voices, offsets, ADPCM, SRC, envelopes, filters,
  mixes, aux and final-mix callbacks, remix, 32/48 kHz, sndcore1/2, mono,
  stereo and surround) produces byte-identical parameter blocks, TV and
  GamePad output and recorder data for every scenario that avoids the fixes
  below. The nn_olv community-code and TGA checks match the old code on 13M
  random inputs.
  snd_core fixes: ADPCM prediction no longer overflows 32 bits; a one-shot
  ADPCM voice ending in its first partial frame no longer repeats its last
  sample past the buffer; PCM8 voices without sample-rate conversion decode
  instead of mixing uninitialised data; freeing a voice twice no longer
  corrupts the free list; AXUserEnd removes the caller's entry wherever it
  is; the second GamePad's mix masks, upsampler offsets and final-mix data
  are in their own slots (GamePad channels 2/3 at 32 kHz were overwritten
  before); each device decides its own upsample stage; the GamePad remix
  uses the GamePad matrix; AXGetDeviceFinalOutput no longer byte-swaps
  twice; AXGetVoiceLoopCount no longer divides by zero on short loops;
  invalid AXInit parameters, remix queries and voice priorities return errors
  instead of asserting.
  nn_olv fixes: community data returned by the upload calls is big endian like
  the downloaded data; output sizes of GetBodyMemo and GetAppData are big
  endian; oversized app data and icons are clamped to their buffers; a
  topic list with more than 10 topics no longer writes past the list;
  negative UTC offsets give the right param-pack value; a post that fails to
  parse in the offline archive is skipped instead of looping forever; the
  offline archive keeps the requested community id; the "post app" stub
  thread returns properly.
