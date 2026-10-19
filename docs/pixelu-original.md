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
| Original (RetroPixels) | 48986 | 15.3% |
| Cemu, untouched | 214182 | 67.0% |
| Cemu, with PixelU changes | 15096 | 4.7% |
| PixelU-added files (MPL-2.0 header) | 1940 | 0.6% |
| Third-party | 39404 | 12.3% |
| **All** | **319608** | 100% |

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
| Cafe/IOSU (shared headers) | 91 | 91 | original |
| Cafe/IOSU/ccr_nfc | 335 | 335 | original |
| Cafe/IOSU/fsa | 1043 | 1043 | original |
| Cafe/IOSU/kernel | 678 | 678 | original |
| Cafe/IOSU/legacy | 4405 | 4405 | original |
| Cafe/IOSU/nn | 2200 | 2200 | original |
| Cafe/IOSU/ODM | 139 | 139 | original |
| Cafe/IOSU/PDM | 517 | 517 | original |
| Cafe/OS/common | 565 | 565 | original |
| Cafe/OS/libs (shared headers) | 352 | 352 | original |
| Cafe/OS/libs/avm | 60 | 60 | original |
| Cafe/OS/libs/camera | 266 | 266 | original |
| Cafe/OS/libs/coreinit | 19977 | 0 | Cemu |
| Cafe/OS/libs/dmae | 174 | 174 | original |
| Cafe/OS/libs/drmapp | 53 | 53 | original |
| Cafe/OS/libs/erreula | 514 | 514 | original |
| Cafe/OS/libs/gx2 | 6269 | 6269 | original |
| Cafe/OS/libs/h264_avc | 2291 | 2291 | original |
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
| Cafe/OS/libs/TCL | 216 | 216 | original |
| Cafe/OS/libs/vpad | 636 | 636 | original |
| Cafe/OS/libs/zlib125 | 306 | 306 | original |
| Cafe/OS/RPL | 2525 | 2525 | original |
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
- Batch 6: `TCL` (the GPU command ring) and all of `gx2` rewritten as
  original code (headers and sources): command buffers and display lists,
  surfaces and addrlib wrappers, render state, textures and samplers,
  shaders, draws, clears, queries, stream out, events and the vsync/flip
  thread, GX2R resources, the allocator, context state, render targets, scan
  buffers and swapping, surface copies, tiling apertures and the module
  registration. Exports, argument layouts, return values, guest struct
  layouts (static_asserts) and the export registration order are unchanged.
  Every PM4 packet and register value is the same byte for byte, including
  the HLE swap, scan-buffer copy and surface copy packets that PixelU's
  GamePad screen toggle and the recorder read. Surface size, alignment and
  tile mode results are unchanged. An old-vs-new harness (ASan/UBSan, 106
  randomised actions over the whole GX2 API, random pools, display lists,
  surfaces of every format/tile mode/dimension, apertures and software
  copies) produces identical command streams, guest memory and return values
  for every seed.
  Fixes and defined behaviour: float-to-integer conversions for sampler LOD,
  clear colours and point size are defined now and keep the x86 results,
  except that a negative point size gives 0 instead of 0xFFFF; an unknown
  vertex attribute format uses fetch format 0 instead of reading past the
  format table; GX2DrawIndexedImmediateEx with an unknown index type is
  ignored (it used an uninitialised count); GX2ClearDepthStencilEx with no
  depth buffer is ignored instead of crashing; GX2CalcTVSize with an invalid
  mode no longer reads past the resolution table; a tiling aperture on a mip
  level beyond 13 no longer reads unrelated host memory for the level
  address; overlapping 2D-tiled software copies use memmove; mip shifts of 32
  or more wrap like the x86 shift. The disabled addrlib self-test and the
  surface copy benchmark are removed (their hooks stay and do nothing).

- Batch 7b: `OS/common` (OSCommon/OSUtil export marshalling and the
  PPC varargs helpers), the shared nn headers (`nn_common.h`,
  `nn_client_service.h`), the RPL loader (`rpl.cpp`, `elf.cpp`, COSModule,
  rpl_structs, symbol storage, debug symbols) and all of IOSU rewritten as
  original code: the kernel (message queues, timers, device registration,
  IPC routing), /dev/fsa, /dev/ccr_nfc, /dev/odm, PDM play statistics, the nn
  service base classes, the nn boss service and its shared types, and the
  legacy devices (ioctl queues, mcp, nim, crypto, acp, act, fpd). Finally
  `h264_avc` (the h264.rpl exports, the ih264d-based AVC backend, which still
  wraps dependencies/ih264d, and the stream parser). Exports, the order in
  which exports are registered, argument layouts, return values, guest
  struct layouts (with new static_asserts), IOSU result codes, handle and
  queue id formats, IPC wire formats and the mlc folder and file layout
  (saveinfo.xml, fad.db, play statistics, account.dat) are unchanged.
  PixelU-specific behaviour is kept: the BotW h264 title quirks, and the
  account, friend and boss behaviour that PixelU's online features rely on.
  Old-vs-new harnesses (ASan/UBSan) gave these results:
  * RPL loader: generated RPLs, seeds 1-400 valid, give identical memory
    images, export tables and symbol resolution. A real RPX is also
    identical. Corrupt-file seeds 1000-1150 differ only where the new
    loader now rejects bad input cleanly.
  * IOSU kernel, nn service, nfc and ODM: harness identical over seeds
    1-60 plus 80 earlier seeds.
  * FSA path resolution: 3M fuzz cases, no mismatches.
  * PDM 300/300, crypto 150/150 (base64 over 200k cases), ACP 300/300,
    ACT 300/300, FPD 150/150 and boss 80/80 seeds identical.
  * h264 decoder: 11 generated streams × seeds 1-40, plus the parser API
    fuzz × seeds 1-60, all identical.
  * The only mismatches seen were timing races in a hang that the old code
    has too (buffered mode with a multi-slice stream): both builds produce
    the same set of outputs when the case is rerun.
  Every changed file and its callers pass a g++ and clang++ syntax check;
  the only failures (GraphicPack2Patches, the wx GUI files,
  ActiveSettings.cpp) are present in the original tree too.
  Fixes and defined behaviour:
  * RPL loader:
    * A section header table with the wrong entry size, or a compressed
      section smaller than 4 bytes, fails the load instead of asserting
      or reading out of bounds.
    * The patch CRC skips compressed sections whose data lies outside the
      file.
    * A TLS relocation against a non-TLS symbol, and an SDA21 relocation
      against a register other than r2/r13, log the problem and are
      skipped.
    * FindModuleOrHLEExport with an unknown handle returns 0 instead of
      dereferencing null.
    * Inflated section data is zero-initialised.
    * Library names over 63 characters are truncated.
    * The HLE stub cache is keyed by the full name instead of a 64-bit hash
      (this differs only on a hash collision).
    * Dependency records and replaced debug comments are freed.
  * IOSU kernel and services:
    * Destroying a message queue wakes its waiters.
    * IPCSimpleService forgets a closed client instead of keeping a
      dangling pointer.
    * FSA WriteFile with element size 0 returns OK instead of dividing by
      zero.
    * The ccr_nfc shutdown guard actually works (the old `< 0` test was on
      an unsigned value).
  * Legacy devices:
    * The crypto private-key buffer is sized to the file instead of a fixed
      4096-byte stack buffer.
    * An empty or missing value in an ACP XML file reads as 0 instead of
      crashing or storing garbage.
    * ACT treats slot 0 and slots above 12 as "no account"; its time zone
      string is bounded and NUL-terminated, and its strings are copied
      bounded.
    * FPD clears exactly `count` entries for screen names and basic info
      (the old code cleared 11 entries, or 4 bytes); the GetMyComment
      result starts zeroed; NNIDs are copied bounded; notification pruning
      is locked.
  * h264:
    * ParseSPS starts from a cleared state (the old code read an
      uninitialised flag).
    * The aligned allocator returns null when allocation fails instead of
      a garbage pointer.
    * The codec reset and core-count requests are zero-initialised.
  Quirks kept on purpose:
  * RPL loader: module handle and TLS counters survive UnloadAll; the TLS
    start is the unrelocated vaddr; the last matching module's small-data
    bases win; HLE data imports use the raw library name.
  * ODM still does not destroy its queue at shutdown.
  * PDM creates play statistics without creating their directory.
  * NIM: the package info title id is stored in host order.
  * ACPGetTitleSaveDirEx still reports the final count when the user
    directory iterator fails.
  * The boss service keeps its parameter read order, and TitleStop does
    not stop the task thread.
  * h264: list-1 reordering still counts and writes like list 0, and the
    BotW titles still get a 1080-line size and an unchecked 256-byte memory
    requirement.
  The h264 parser API (h264Parser_*) has no callers outside h264_avc and
  is kept for interface compatibility.
