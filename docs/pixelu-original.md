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
| Original (RetroPixels) | 182330 | 69.0% |
| Cemu, untouched | 41741 | 15.8% |
| Cemu, with PixelU changes | 530 | 0.2% |
| PixelU-added files (MPL-2.0 header) | 182 | 0.1% |
| Third-party | 39404 | 14.9% |
| **All** | **264187** | 100% |

| Module | Lines | Original lines | Status |
|---|---:|---:|---|
| (top level: main.cpp, mainLLE.cpp) | 335 | 335 | original |
| audio | 2861 | 1803 | mixed (63% original), 1058 lines third-party |
| Cafe (CafeSystem, GamePatch) | 1559 | 1559 | original |
| Cafe/Account | 696 | 696 | original |
| Cafe/Filesystem | 3355 | 3355 | original |
| Cafe/GameProfile | 375 | 375 | original |
| Cafe/GraphicPack | 2835 | 2835 | original |
| Cafe/HW/ACR | 42 | 42 | original |
| Cafe/HW/AI | 8 | 8 | original |
| Cafe/HW/Common | 129 | 129 | original |
| Cafe/HW/Espresso (core) | 1029 | 1029 | original |
| Cafe/HW/Espresso/Debugger | 2459 | 2459 | original |
| Cafe/HW/Espresso/Interpreter | 3252 | 3252 | original |
| Cafe/HW/Espresso/Recompiler | 7818 | 7818 | original |
| Cafe/HW/Latte/Common | 3130 | 3130 | original |
| Cafe/HW/Latte/Core | 14100 | 14100 | original |
| Cafe/HW/Latte/ISA | 2618 | 2618 | original |
| Cafe/HW/Latte/LatteAddrLib | 1249 | 1249 | original |
| Cafe/HW/Latte/LegacyShaderDecompiler | 11423 | 5976 | mixed (52% original) |
| Cafe/HW/Latte/Renderer | 1028 | 995 | mixed (97% original) |
| Cafe/HW/Latte/Renderer/Metal | 7309 | 0 | Cemu |
| Cafe/HW/Latte/Renderer/OpenGL | 4531 | 4531 | original |
| Cafe/HW/Latte/Renderer/Vulkan | 11861 | 11861 | original |
| Cafe/HW/Latte/ShaderInfo | 70 | 70 | original |
| Cafe/HW/Latte/Transcompiler | 731 | 731 | original |
| Cafe/HW/MMU | 681 | 681 | original |
| Cafe/HW/SI | 132 | 132 | original |
| Cafe/HW/VI | 5 | 5 | original |
| Cafe/IOSU (shared headers) | 91 | 91 | original |
| Cafe/IOSU/ccr_nfc | 335 | 335 | original |
| Cafe/IOSU/fsa | 1043 | 1043 | original |
| Cafe/IOSU/kernel | 678 | 678 | original |
| Cafe/IOSU/legacy | 4410 | 4410 | original |
| Cafe/IOSU/nn | 2200 | 2200 | original |
| Cafe/IOSU/ODM | 139 | 139 | original |
| Cafe/IOSU/PDM | 517 | 517 | original |
| Cafe/OS/common | 565 | 565 | original |
| Cafe/OS/libs (shared headers) | 352 | 352 | original |
| Cafe/OS/libs/avm | 60 | 60 | original |
| Cafe/OS/libs/camera | 266 | 266 | original |
| Cafe/OS/libs/coreinit | 16729 | 14187 | mixed (85% original) |
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
| Cafe/TitleList | 2313 | 2313 | original |
| Cemu/DiscordPresence | 850 | 850 | original |
| Cemu/ExpressionParser | 600 | 600 | original |
| Cemu/FileCache | 535 | 535 | original |
| Cemu/Logging | 430 | 430 | original |
| Cemu/napi | 1986 | 1986 | original |
| Cemu/ncrypto | 921 | 921 | original |
| Cemu/nex | 3410 | 3410 | original |
| Cemu/PPCAssembler | 1893 | 1893 | original |
| Cemu/Tools | 2017 | 0 | Cemu |
| Common | 31625 | 3311 | mixed (10% original), 28314 lines third-party |
| config | 2969 | 2132 | mixed (72% original) |
| gui/interface | 140 | 140 | original |
| gui/wxgui (top level) | 12082 | 12082 | original |
| gui/wxgui/canvas | 372 | 372 | original |
| gui/wxgui/components | 5175 | 0 | Cemu |
| gui/wxgui/debugger | 3894 | 0 | Cemu |
| gui/wxgui/dialogs | 695 | 0 | Cemu |
| gui/wxgui/EmulatedUSBDevices | 943 | 0 | Cemu |
| gui/wxgui/helpers | 546 | 0 | Cemu |
| gui/wxgui/input | 4307 | 0 | Cemu + PixelU change |
| gui/wxgui/windows | 913 | 0 | Cemu |
| gui/wxgui/wxcomponents | 691 | 0 | Cemu |
| imgui | 3183 | 0 | Cemu |
| input (top level) | 1243 | 1061 | mixed (85% original) |
| input/api | 5342 | 5342 | original |
| input/emulated | 2426 | 2426 | original |
| input/motion | 354 | 354 | original |
| resource | 3594 | 0 | Cemu |
| tools | 145 | 0 | Cemu |
| util (top level) | 54 | 54 | original |
| util/boost | 65 | 65 | original |
| util/bootSound | 88 | 88 | original |
| util/ChunkedHeap | 489 | 489 | original |
| util/containers | 4295 | 346 | mixed (8% original), 3949 lines third-party |
| util/crypto | 896 | 478 | mixed (53% original), 418 lines third-party |
| util/DXGIWrapper | 77 | 77 | original |
| util/Fiber | 148 | 148 | original |
| util/helpers | 2333 | 2333 | original |
| util/highresolutiontimer | 171 | 171 | original |
| util/ImageWriter | 123 | 123 | original |
| util/IniParser | 225 | 225 | original |
| util/libusbWrapper | 9 | 9 | original |
| util/math | 391 | 391 | original |
| util/MemMapper | 157 | 157 | original |
| util/ScreenSaver | 45 | 45 | original |
| util/SystemInfo | 236 | 236 | original |
| util/ThreadPool | 14 | 14 | original |
| util/tinyxml2 | 5027 | 0 | third-party |
| util/VirtualHeap | 161 | 161 | original |
| util/Zir | 1610 | 1610 | original |
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
- Batch 7a: all of `coreinit` rewritten as original code (headers and
  sources): the MEM heap tree and lists, the default heap and allocator
  objects, the expanded, frame, unit and block heaps and SysHeap; tick and
  calendar conversion, host and guest alarms and the alarm thread; the guest
  printf behind OSReport/__os_snprintf/COSWarn/OSLogPrintf, console logging
  and the title/process queries; the per-core IOS IPC driver, IPC buffer
  pools, the legacy IOS device path and MCP/UCReadSysConfig; threads, the
  scheduler, thread queues, mutexes, condition variables, semaphores,
  events, fast mutexes, spinlocks, atomics, message queues, MP task queues
  and coroutines; the FS client, command block queue and FSA layer with
  mounting; BSP, system info, overlay arena, HW registers, IM, virtual
  memory reservations, the codegen area, deferred guest callbacks, the
  locked cache, the foreground bucket and clipboard, cache/memory helpers
  and OSScreen; the module entry, title start-up (argc/argv, default heaps
  and __preinit_user), the GHS runtime support (stream locks, errno, EH
  globals, GHS mutexes, __tls_get_addr) and OSDynLoad. Only the generated
  OSScreen font table (coreinit_OSScreen_font.h) is unchanged.
  Exports, argument layouts, return values, guest struct layouts
  (static_asserts), FS/FSA result codes, the export registration order and
  the order of PPC callables and SysAllocator/system-area allocations are
  unchanged, so heap and system-area addresses come out the same for the
  same call sequence. Thread scheduling semantics are unchanged.
  Old-vs-new harnesses (ASan/UBSan) give identical logs, return values and
  guest memory hashes: MEM heaps 30 seeds x 4000 random operations; time
  conversion over 3M samples; the printf family 40 seeds x 3000 random
  formats and argument lists (including va_list state); IPCBufPool 30 seeds
  x 5000 operations; the IPC driver against a fake IOSU 40 seeds x 3000
  requests; the scheduler with real fibers on three cores 300 seeds x 100k
  scripted guest operations; FS/FSA against a fake IOSU with deferred
  completions 300 seeds x 3000 steps; the small modules (locked cache,
  memory mapping, OSScreen, memory helpers, foreground bucket, codegen, IM,
  BSP, overlay arena, HW registers, system info) 60 seeds x 3000 steps; the
  GHS runtime on three fake threads 200 seeds x 3000 steps.
  Fixes and defined behaviour: MEMInitList zeroes the object count;
  MEMGetNthListObject past the end returns null; expanded heap group id and
  attribute bits are stored big endian; the frame heap's allocatable-size
  query ignores the alignment sign and no longer leaks the heap lock; a unit
  heap with block size 0 returns null and memory fill no longer overwrites
  the free link; the block heap tail off-by-one, stale tail block after a
  merge and tail size wrap are fixed; fill mode index 3 or above returns 0.
  Due guest alarms fire earliest first; a one-shot host alarm no longer
  leaks. The guest printf no longer has undefined behaviour on `*` with
  floats and uses a bounded float buffer; OSLaunchTitleByPath no longer
  throws on a malformed title id in the target's app.xml. IPC buffer overflow, bad geometry, bad frees and MCP misuse log
  instead of breaking into the debugger; an IPC buffer pool with block size
  0 no longer divides by zero; when all IPC request slots are in use the
  caller yields until one is free; UCReadSysConfig checks its pointers. The
  mutex contention warning is logged once. FSAChangeMode clamps paths over
  640 characters instead of overflowing; unknown commands in the FS result
  copy-out are ignored. LCDisableDMA's debug leak check no longer reads out
  of bounds. Title arguments beyond 32 entries or the 4 KiB argument
  storage are dropped with a log line instead of overflowing; long
  OSDynLoad module paths no longer overflow a 512 byte buffer. Log texts
  differ throughout. The unused codegen memory-write callback is removed.
  Kept on purpose: MEMSetAllocModeForExpHeap cannot clear the mode;
  unsupported printf specifiers such as %lx still stop formatting; the FS
  command queue leaves the stale back link after popping the head;
  FSAGetFreeSpaceSize and FSAGetDeviceInfo send each other's query type; a
  rejected blocking FS call returns 0; only the top byte of the FS error
  mask is stored; the per-thread TLS table is sized once on first use; an
  OSDynLoad notification for a module that cannot be found stops without
  freeing the info block; the argv string cursor advances past the rest of
  the launch string, so argv addresses stay the same.
- Batch 8b: the Espresso CPU core outside the recompiler rewritten as
  original code: the interpreter (Interpreter/: decoding tables, integer,
  load/store, branch, SPR, FPU, paired-single and HLE handlers, the slim and
  full instruction entry points with BAT/page-table translation and DSI),
  PPCState.h, Const.h, EspressoISA.h, PPCCallback.h, the HLE scheduler
  (PPCScheduler.cpp), the LLE multi-core loop with the boot info block and
  SMDP mailbox (PPCSchedulerLLE.cpp), the guest timer (PPCTimer.cpp), and
  the debugger: Debugger.cpp/h, the GDB remote stub (GDBStub.cpp/h,
  GDBBreakpoints.cpp/h) and DebugSymbolStorage.cpp/h. The recompiler is not
  part of this batch.
  PPCInterpreter_t and every other guest-visible or recompiler-visible
  layout is unchanged (offsets checked), as are all symbols used outside
  the directory, PPCCallback argument placement (registers, register pairs
  for 64-bit values, stack slots), the scheduler quantum and time slicing,
  the LLE boot block (0x01FFF000, 0x40C bytes), the TSC to guest clock
  conversion (128-bit carry), the trap words of the debugger (0x7C020008)
  and the GDB stub (0x7C010008), and the GDB packets, replies and stop
  reasons. Group-63 decoding keeps the old precedence of the A-form opcodes
  (fcmpo executes as fcmpu) and an unknown primary opcode still does not
  advance the instruction pointer.
  Old-vs-new harnesses (ASan/UBSan) give identical results:
  * Interpreter, slim and full (MMU) modes: single-step runs over 20-24
    seeds per mode x 20000 random instructions and sequence runs
    (16-instruction blocks) over 10-11 seeds per mode, comparing all registers, SPRs, CR/XER/FPSCR, reservation
    state, events and memory hashes. The only differences are which NaN
    payload wins when two NaN operands meet, which depends on host operand
    order in both versions. A planted mutation (cntlzw) is reported as a
    real difference. gcov coverage of the interpreter is about 97-100%.
  * ISA decoding over 2M random words, PPCState offsets, callbacks and call
    parameters; the scheduler with a scripted fake CPU over 3000 rounds; the
    LLE loop; the timer with a scripted __rdtsc.
  * The GDB stub over a real loopback socket with a fake guest (threads,
    modules, memory): about 120 packets plus breakpoint, step, watchpoint
    and interrupt flows. Replies are byte-identical apart from the fixes
    below.
  * Debugger breakpoint and patch bookkeeping: 8 seeds x 3000 random
    create/toggle/delete/memory-breakpoint/patch operations, compared on
    guest memory and the breakpoint chains.
  Fixes and defined behaviour:
  * Interpreter: fcmpu writes CR field crfD&~3 (the raw 5-bit field could
    index out of bounds); GQR load/store scales are exact powers of two;
    quantized stores turn NaN into 0 and convert through sint32; fctiw
    honours FPSCR[RN], sets the upper word to 0xFFF80000 and turns NaN
    into 0x80000000; mcrxr fills the whole CR field (LT=SO, GT=OV, EQ=CA);
    add/addi/neg no longer rely on signed overflow (same results); DMA
    copies with memmove; registering an HLE call when the table is full
    returns -1.
  * Debugger: deleting a breakpoint unlinks it first and then rewrites the
    trap, or restores the instruction when it was the last execute
    breakpoint at that address. The old code disabled the first breakpoint
    of the chain instead and could leave a stale trap. A deleted memory
    breakpoint is disarmed. A read+write memory breakpoint request becomes
    a write breakpoint (the old type was uninitialised). Multi-word patches
    keep all their bytes (the old code overflowed a 4-byte buffer and read
    the original instruction for the wrong word). Patch merging uses the
    merged range. The old code used the new patch's own address, which
    overflowed the heap or restored bytes 4 bytes off when a patch touched
    two others. In an overlap the new patch's data wins and the oldest
    original bytes are kept.
  * GDB stub:
    * A breakpoint is re-armed once the thread has executed past it. The
      old stub erased its own restore points right after planting them, so
      every breakpoint fired only once.
    * Stepping a conditional branch uses AA, not LK, to decide whether the
      target is relative; stepping uses the byte-swapped LR of the context.
    * "m" shows all four original bytes behind a trap (the old stub only
      replaced the first byte). "M" over a trap updates the saved
      instruction for every covered byte instead of writing into the trap
      word, and writes invalidate the recompiler range.
    * "G" reads 8 hex digits per GPR and replies OK.
    * "P" accepts 64-bit values. Malformed packets get an empty reply or
      E01 instead of throwing or indexing past the parsed fields.
    * The checksum mismatch is logged instead of asserted.
    * A disconnected client is detected and the stub waits for the next
      one.
    * The destructor stops and joins the server thread. The old one cleared
      the stop flag and could hang.
    * A watchpoint exception without a watchpoint is ignored.
    * "qSupported"/"qAttached" are also answered without arguments.
  Kept on purpose:
  * The interpreter quirks above.
  * Z0 breakpoints stay visible to "m" as the trap word.
  * "qC" answers in decimal.
  * "D" gets an empty reply.
  * Register reads for thread selector -1 concatenate all threads.
  * Persistent breakpoints are disarmed while their thread steps over them,
    so another thread can pass them in that window.
  Removed as unused anywhere in the tree: the interpreter-internal helpers
  (PPCInterpreter_FADD and friends, fcmpu_espresso, the fres lookup table,
  generateDSIException, PPCInterpreter_virtualHLE, the HLE table global and
  debug_lastTranslatedHit), EspressoISA's _decodeForm_* helpers,
  debugger_resume, the two-argument debugger_getFirstBP and the GDB stub's
  internal classes (CommandContext, ExecutionBreakpoint, AccessBreakpoint,
  now Request, CodeTrap and Watchpoint) and its unreachable
  qOffsets/qRcmd branches. Log texts differ throughout.
- Batch 8a: CafeSystem, GamePatch, Account, Filesystem, GameProfile,
  GraphicPack, TitleList and the HW modules ACR, AI, Common, MMU, SI and VI
  rewritten as original code (headers and sources). CafeSystem covers system
  start-up and shutdown, host/MLC folder mounting, preparing a title from the
  title list or a standalone RPX, launch, title queries, override arguments
  and the RPX hashes; the PixelU recorder hooks are unchanged
  (LatteRecorder::Start() right after WindowSystem::NotifyGameLoaded(),
  LatteRecorder::Stop() right after snd_core::reset()). GamePatch keeps the
  same built-in game fixes in the same order, with the signatures stored as
  big-endian words and word masks; a script check confirms every signature
  and mask equals the old byte arrays, and the scan range, 4-byte step and
  HLE registration names (h000000001-4) are the same. Filesystem covers
  fsc, the HostFS, redirect, WUD/WUX, WUA and WUHB devices, the WUHB RomFS
  reader, the WUD/WUX reader, KeyCache and FST. GraphicPack covers rules.txt
  loading, presets, shader and texture/output rules, replacement files,
  activation, the Cemuhook and Cemu patch parsers, symbol and expression
  resolution, code caves and patch apply/undo. TitleList covers title and
  save discovery, meta.xml/app.xml/cos.xml/Aroma ini parsing, the title
  cache and GameInfo. Account covers account.dat, default Miis and
  persistent ids; GameProfile the per-title ini.
  Public interfaces, globals and externs used elsewhere, rules.txt and patch
  grammar, preset quirks, file formats and mount layouts are unchanged, so
  existing graphic packs, profiles, account files and every game format
  (folder, WUD, WUX, WUA, WUHB, NUS) still load.
  Old-vs-new harnesses (ASan/UBSan) give identical results: MMU 300 seeds x
  3000 operations; account.dat 300 seeds; game profiles 40 seeds; title
  list 200 seeds of generated titles, xml files and caches (plus 60
  malformed seeds on the new build only, clean); fsc with HostFS and WUHB
  150 seeds of generated trees, file operations and directory walks (plus
  80 non-strict seeds on the new build only, clean); graphic packs 500
  seeds of generated packs, presets, patch files and fake RPX/RPL modules
  with activate/reload/deactivate and memory hashing (649 packs, 296
  activations, 51 applied patch groups). The remaining differences are the
  fixes below. Changed files and their 333 transitive includers (including
  the wx GUI) compile with g++ and clang++ with no new errors against the
  baseline; clang -Wall -Wextra gives no warnings in the rewritten files.
  Fixes and defined behaviour: MMIO reads without a handler return 0. The
  account utc offset starts at 0; whitespace-only values and keys without
  a terminator are no longer read past the buffer. Title list: malformed
  numbers give 0 instead of throwing, TitleInfo copies keep their location
  uid and no longer leak, failed mounts close their containers, Aroma ini
  files with missing keys and overflowing .wua versions are rejected, and
  the list is locked during removal and the scan-finished callback. fsc:
  unmounting a path without a device returns false, the root can be
  unmounted, directory entry dedupe is bounded and leftover directory
  handles are freed. HostFS directory listing no longer throws, setting the
  file length sets the size, and reads past the end are clamped (the old
  build lost later data on Linux once a read hit EOF; this is the only
  harness difference, 3 of 150 seeds). WUHB entries with an oversized name
  length fall back to the default entry and names stop at 255 characters.
  WUX files with an incomplete index table are rejected and sizes are 64
  bit. FST: 64-bit file offsets, no endless loop on a folder that ends at
  its own index, correct name bounds check, a missing GM volume no longer
  crashes, .app indices and content hash sizes are bounds checked. Graphic
  packs: unresolved symbols are ordered correctly (report order can
  differ), patch lines outside a group report an error instead of crashing,
  callbacks are cleared on unload so a reload no longer registers them
  twice, folder iteration errors no longer throw, and zero-length patches
  skip the copy. The unused XCX critical-section HLE is a correct lock.
  MLC title copies are freed on shutdown. Log texts differ throughout.
  Kept on purpose: preset and variable quirks of the rules.txt parser, the
  patch resolver's pass limit, the Cemuhook grammar, the per-game patch
  order and the start-up initialisation order.
- Batch 10a: the Latte support code under Cafe/HW/Latte rewritten as
  original code: the ISA headers (the LATTEREG register views and the
  LatteContextRegister layout in LatteReg.h, the mm* register index table
  in RegDefines.h, the CF/VTX/ALU instruction views in
  LatteInstructions.h); the surface address library (LatteAddrLib.h/.cpp,
  LatteAddrLib_Coord.cpp: GX2 surface info, mip/slice offsets, bank swap
  width, rotation, linear/micro/macro tiled element addressing and the
  cached macro tiled path) and the texture copy loops in
  AddrLibFastDecode.h; the pipeline cache serializers
  (RegisterSerializer, ShaderSerializer); ShaderInfo (ShaderDescription,
  ShaderInstanceInfo) and the experimental Latte-to-ZpIR transcompiler
  (LatteTC.h/.cpp, LatteTCGenIR.cpp). The two zstd dictionaries
  (s_shaderDict 0xA985 bytes, s_regDataDict 0x5AD2 bytes) are data and are
  byte-identical, only reformatted; both keep external linkage.
  Register and field names, enum values, register offsets
  (static_asserts), instruction field positions, the GX2 surface results
  and the on-disk pipeline cache format are unchanged, so existing shader
  and pipeline caches stay readable and newly written entries are
  byte-identical.
  Old-vs-new harnesses (ASan/UBSan) give identical output: every LATTEREG
  field get/set and helper over 4000 random register files plus all enum
  values and register offsets (1.73M lines); instruction decode of all
  CF/VTX/ALU views over 200k random words (19.3M lines); addrlib over every
  format x tile mode x dimension x AA x 11 sizes x 8 mip levels x
  depth/scan flags, then 200k random surfaces and mip chains (including
  CalculateMipOffset and CalculateMipAndSliceAddr with random slices and
  swizzles), all pixel index tables, bank swap widths and 2M random
  linear/micro/macro/cached element addresses (19.6M lines); the copy
  loops for 9 texel types x 600 random 2D_TILED_THIN1, LINEAR_ALIGNED and
  other surfaces in both directions (surface and linear buffer hashes);
  the serializers over 3000 random register snapshots and 400 random
  shader programs up to 600 KB (serialized bytes identical, round trip
  exact, 4 mutated/truncated inputs each give the same accept/reject and
  result), old-written blobs read by the new code and new-written blobs
  read by the old code; register Store/Load through 50 random register
  files.
  Fixes and defined behaviour: the macro tiled address no longer divides by
  zero when one sample of a thick micro tile exceeds the 2 KiB split size
  (bpp 96/128 THICK with AA); surface info for formats with no bit size is
  computed as LINEAR_GENERAL instead of dividing by zero; a 0 base
  alignment in the mip walk counts as 1; mipIndex <= 0 gives level 0
  instead of reading uninitialised data; rotation for tile modes above 15
  is 0; the bank swap width is 0 for bpp 0 instead of dividing by zero and
  its halving loop stops at 0 instead of spinning forever for pitch 0; the
  cached macro tiled setup computes its shifts with countr_zero instead of
  relying on x86 shift masking of the Linux _BitScanReverse stand-in.
  ShaderDescription::analyzeShaderCode now walks the CF program and
  records exported colour targets (renderTargetWriteMask) instead of
  breaking into the debugger; the transcompiler keeps a single block for
  main(), additionally handles ADD, the 1.0/0.5/1/-1 inline constants,
  PV/PS operands, negated float operands, masked fetch channels, constant
  export swizzles and export bursts, and reports anything else through
  cemu_assert_debug instead of assert_dbg (both are only reachable from
  the unused LatteShader_CompileSeparableVertexShader2). The register
  serializer's boot self-test uses a different test pattern.
  Kept on purpose: register setters do not mask their value;
  LOD_USES_MINOR_AXIS stays at bit 25; setField_Opcode clears only bits
  23-26; SRF_MODE_ALL reads bit 30; the linear surface size applies the
  sample count twice; the display flag has no effect on macro pitch
  alignment; LINEAR_SPECIAL keeps the 32 bit surface size; BC formats keep
  the power-of-two sizing above level 0; the THICK to 1D fallback still
  checks the requested mode; the cached macro tiled address keeps its 32
  bit wrap-around and full-sample slice/macro tile sizes (so it can differ
  from the uncached function for multisampled surfaces, as before).
- Batch 10b: the legacy shader decompiler (Cafe/HW/Latte/LegacyShaderDecompiler)
  rewritten as original code, except the Metal emitter: the analyzer
  (LatteDecompilerAnalyzer.cpp), the decompiler front end and CF/clause
  parsing (LatteDecompiler.cpp), the register data type tracker, the
  shared headers (LatteDecompiler.h, LatteDecompilerInternal.h,
  LatteDecompilerInstructions.h), the GLSL emitter
  (LatteDecompilerEmitGLSL.cpp), its declaration section
  (LatteDecompilerEmitGLSLHeader.hpp) and the vertex attribute decoder
  (LatteDecompilerEmitGLSLAttrDecoder.cpp, now table driven). File names,
  public functions, structs, field layouts and the Instructions.h macros are
  unchanged. The globals other files use are kept: the three Decompile*
  entry points, LatteDecompiler_IsALUTransInstruction (LatteSoftware.cpp),
  the LatteGSCopyShaderParser declarations and the helpers the MSL emitter
  links against (_getVertexShaderOutParamSemanticId,
  _getInputRegisterDataType, _getALUInstructionOutputDataType,
  _isReductionInstruction, LatteDecompiler_getColorOutputIndexFromExportIndex).
  Generated GLSL is byte-identical, including the uniform offset tables,
  the resource mappings and the "predetermined by Cemu" dump marker that
  shader replacements rely on.
  Not rewritten: LatteDecompilerEmitMSL.cpp, LatteDecompilerEmitMSLHeader.hpp
  and LatteDecompilerEmitMSLAttrDecoder.cpp (about 5400 lines). They are only
  built with ENABLE_METAL on macOS and include the Metal renderer headers
  (metal-cpp). Neither a compile check nor an output comparison is possible
  here, so they are left as they are for a batch that can build them.
  Old-vs-new harness: a random shader generator (CF/ALU/TEX/VTX/export/
  streamout/GS ring programs, fetch shaders, context registers, uniform
  modes, both APIs' options) runs the full decompile and hashes the GLSL,
  the output struct and the uniform offsets. Over 20000 release seeds and
  30000 CEMU_DEBUG_ASSERT seeds under ASan/UBSan the output is identical to
  the old code once its two undefined shifts are patched out. The only
  differences from the unpatched old code are the 190 seeds where it
  shifts by a negative amount. g++ and clang++ syntax checks are clean for
  every rewritten file and the files that include its headers;
  object symbol comparison shows only internal symbols dropped.
  Fixes and defined behaviour: reads past the end of the program return
  zero; literal and dst-select data start zeroed; only subroutines that are
  called get parsed; a failed GS copy shader parse sets the error flag and
  the parse result is freed; the uniform usage log checks every buffer;
  colour export index -1 no longer sets bit 31 of pixelColorOutputMask
  (old: 1<<-1); GPR marks with index -1 or 128 and above are ignored (old:
  negative shift); RESINFO/LOD with an out-of-range texture index are
  ignored; the VFETCH range check covers 0x80+16. In the emitter, an
  out-of-range texture unit no longer indexes past the dimension table; a
  REMAPPED constant with no remap entry reads entry 0 instead of
  dereferencing null; an invalid source select prints "_" instead of
  reading past the swizzle table; a memory read with an unknown format
  reads zero components instead of an uninitialised count; a GS with a
  zero vertex item size gets max_vertices 0 instead of a division by zero.
  The source buffer starts at 64 KiB and grows (old: a fixed 12 MiB
  buffer).
  Quirks kept on purpose: the streamout uniforms keep the (A && B) || C
  grouping, so a GS always declares uf_verticesPerInstance; the
  front-facing input is only loaded when R0 is used; the fragCoordScale
  padding slot for old shader replacements; the AMD rg8 attribute
  workaround; the REMAPPED lookup order.
- Batch 9: the PowerPC recompiler rewritten as original code: the front end
  (PPCRecompilerImlGen.cpp, PPCRecompilerImlGenFPU.cpp,
  PPCRecompilerIntermediate.cpp, PPCRecompilerIml.h, the function boundary
  tracker), a new intermediate representation (IML/: instructions, blocks,
  liveness and loop analysis, verifier, dead code removal, debug output),
  a graph colouring register allocator (IMLRegisterAllocator*), the x86-64
  backend (BackendX64/, including its own instruction encoder), the
  AArch64 backend (BackendAArch64/, emitted with xbyak_aarch64) and the
  runtime (PPCRecompiler.cpp/h: jump table, compile queue and worker
  thread, entering and leaving generated code, invalidation). File names
  and every symbol used outside the directory are unchanged:
  PPCRecompiler_init/Shutdown/Enable/Disable, allocateRange,
  invalidateRange, recompileIfUnvisited, attemptEnter(WithoutRecompile),
  the enter/leave stub pointers, ppcRecompilerInstanceData and the jump
  table layout (8 bytes per guest instruction, 4MB steps).
  The IR is new: guest registers are fixed virtual registers (GPRs, FPRs
  as paired doubles, one register per CR bit, CA/OV/SO, LR, CTR, GQRs, the
  lwarx reservation), guest state is written back to PPCInterpreter_t
  after every write that can be observed, and helper calls save only the
  live caller-saved host registers. Instructions the front end does not
  translate are run through the interpreter one at a time.
  Verified with a differential harness that generates random guest
  programs (integer, rotate/mask, load/store incl. string and byte-reversed
  forms, lwarx/stwcx., FPU, paired singles, quantized loads/stores with all
  GQR types, CR logic, branches, loops, bdnz, calls, HLE stubs that return,
  continue, yield or burn cycles, and unsupported opcodes), runs each one
  in the interpreter and in the recompiler, and compares all registers,
  CR/XER/FPSCR, the reservation, LR/CTR and 1MB of data memory:
  * x86-64, ASan/UBSan, g++: 1000 + 4x40000 programs without FTZ,
    4x40000 with MXCSR.FTZ (as set for guest threads), 4x40000 with NaN
    inputs excluded and exact NaN comparison, plus 4x25000 more (2 with,
    2 without FTZ). 0 interpreted steps inside JIT runs. The only
    remaining differences (0-2 per 10000 programs) are which NaN payload
    wins when two NaN operands meet (the interpreter's result depends on
    the host compiler's operand order; the JIT uses frA first like the
    hardware), also when such a NaN is stored or moved through mtfsf.
  * AArch64, UBSan, aarch64-linux-gnu-g++ -std=gnu++20 -O2 under
    qemu-aarch64: 8x1000 + 8x5000 programs without FPCR.FZ, 8x5000 with
    FZ, 8x5000 with FZ, no NaN inputs and exact NaN comparison. Same
    result: only two-NaN payload differences (3 programs in 128000; NaNs
    still appear in the NaN-free runs through fctiw results). ASan does not run under
    qemu-user, so ASan coverage of the shared front end, IR and allocator
    comes from the x86-64 runs.
  * The old recompiler in the same harness (x86-64, 1000 programs) shows
    286 mismatches against the interpreter (reservation, FPSCR[FPCC], CR,
    paired single lanes, memory); those are the old JIT's deviations.
  Syntax checks with g++ and clang++ of all recompiler files and of the
  callers (CafeSystem, coreinit_Thread, coreinit_CodeGen, rpl, elf,
  GraphicPack2PatchesApply, PPCInterpreterOPC, PPCScheduler, Debugger,
  GDBStub, GDBBreakpoints); aarch64 cross g++ and clang
  --target=aarch64 for all recompiler files including the x86-64 backend
  sources that the aarch64 build also compiles. The x86-64 backend calls
  helpers with the Microsoft x64 convention (ATTR_MS_ABI) on every OS.
  Fixes and defined behaviour:
  * Results follow the interpreter bit for bit: lfs widens exactly
    (signaling NaNs stay signaling, denormals are normalised, independent
    of FTZ/FZ), stfs stores without rounding, single precision multiplies
    cut frC to 25 bits, ps_madd rounds the product first, fctiw/fctiwz
    give 0xFFF80000 in the upper word (+1 for a negative input that gives
    0) and 0x80000000 for NaN, divw/divwu give the Espresso results for
    x/0 and 0x80000000/-1, fcmpu/ps_cmpu set FPSCR[FPCC] and VXSNAN,
    ps_cmpo0 sets FPCC only, effective addresses wrap at 32 bits.
  * On AArch64 the multiply-add family is fused like the compiler
    contracts the interpreter's a*c+b. Whether fnmadds/fnmsubs negate
    before or after rounding to single (GCC and clang differ, visible only
    in the sign of an exact zero) is probed once from the interpreter's
    own handler.
  * dcbst and dcbf run through the interpreter (the old JIT skipped them,
    so the data cache flush notification was lost).
  * Compiled function records are freed on invalidation and shutdown (the
    old code leaked them). Generated code memory is never reused, as
    before.
  * AArch64: code is assembled into ordinary memory and copied into one
    RWX arena (MAP_JIT with pthread_jit_write_protect_np on macOS), with
    an instruction cache flush. stwcx. at an unaligned address falls back
    to a plain compare and store instead of faulting on the exclusive
    access.
  Kept on purpose:
  * mftb returns OSGetSystemTime() like the old JIT did.
  * Calls to unimplemented imports (HLE 0xFFD0) cost 500 extra cycles.
  * The two leave stubs are identical code at different addresses; the
    address marks a jump table entry as visited or not.
  Log texts differ throughout.
Batch 11 working notes (Cafe/HW/Latte/Core). The final log entry is at the end.

Done:
- LatteIndices.cpp: original. Harness: 2000 seeds, 402k lines identical
  (renderer reserve/upload/release calls, output buffer hashes, max index,
  host index type and count, cache hits/evictions, invalidation).
  Kept: a 65535-vertex AUTO line loop still reserves 32 bit room while it
  writes 16 bit indices; LE sources of rewritten modes are not converted;
  the 8 slot cache with the first-slot-wins LRU tie rule.
- FetchShader.cpp/.h: original. Harness: 5000 seeds, 983k lines identical
  (all decoded fields, key, vk hash fragment, registry and address lookup
  behaviour, frame-based revalidation).
  Fixes: UnregisterInCache erases its own entry (old code asserted the entry
  was missing and then erased end(), so destroying a registered fetch shader
  crashed); fetches past the end of a bare (old cache layout) program and
  VTX_TC clauses outside the program are ignored instead of reading past the
  buffer; the buffer mask bit for invalid buffer ids uses (id & 31) instead
  of an undefined shift (same value x86 produced).
- LatteCommandProcessor.cpp, LattePM4.h: original. Harness with a logging
  stub renderer: 11000 seeds, 845k lines identical (every renderer call,
  register file hashes, draws incl. the draw-pass fast path, dirty masks,
  waits, mem writes, events, HLE packets, indirect buffers, ring packets).
  Fixes: register writes past the register file are dropped instead of
  writing out of bounds; HLE_SPECIAL_STATE ids >= the state count are
  ignored (old check let id 9 write past the array); DRAW_INDEX_IMMD copies
  at most 0x4000 words; ring packets of 128+ words are no longer copied into
  a 128 word stack buffer; the indirect buffer nesting depth is not capped.
  Dropped: the never-taken SQ_VTX_SEMANTIC_CLEAR special case and the
  LATTE_CP_LOGGING debug dump. Log texts differ.
- LatteTiming.cpp/.h, LatteRingBuffer.cpp/.h: original, same timer
  frequencies, same vsync/flip throttle title list, same allocator rule.
  No harness (timer driven / 10 lines), reviewed by hand.
- LatteTextureLoader.cpp/.h: original. Every decoder class kept (same names,
  singletons, byte sizes, texel counts, quirks such as the 10:10:10:2 to
  RGBA16 placeholders that only clear the first texel of each row, the
  5:6:5 to RGBA8 "v<<3|v>>3" widening and the alpha<<3 dumps of 5:5:5:1).
  Harness: 1500 seeds, 1.27M lines identical (LatteTextureLoader_begin
  fields for random formats/dims/tile modes/mips/swizzles, GetInput
  addresses, every decoder's decode() output incl. untouched bytes,
  decodePixelToRGBA, raw BC1-5 block decoders, BC1_GetPixel, readback
  writes into guest memory). The reference build used Cemu's original
  AddrLibFastDecode.h so the decoders were compared against the real
  original behaviour.
  Fix (in LatteAddrLib/AddrLibFastDecode.h, rewritten in 6499dfb): the
  LINEAR_ALIGNED untile path copied rows with memcpy, which skipped the
  converting assignment of the swapped 5:5:5:1 element types, so linear
  R5G5B5A1 textures were uploaded without their channel swap on the
  swappedRB/swappedOpenGL paths. Such element types are now copied one by
  one again (the harness shows the difference against the 8ffe2a1 header).
  Other differences: the dump buffer is allocated after the decoder check
  (no leak when no decoder exists) and sized exactly; readback log texts.
- LatteBufferCache.cpp: original. The B+ tree interval tree is replaced by
  a std::map of disjoint ranges (same query results and order). Same page
  size, streamout signature pattern written to guest RAM, upload batching,
  dirty interval rules, OOM eviction order and incremental cleanup order.
  The page hash is a new function (only compared internally).
  DC flush pages are queued in a vector and de-duplicated when processed
  (invalidation is order independent). Harness: 2000 seeds, 4.8M lines
  identical (every upload with data hash, copies, streamout copies,
  returned offsets, heap stats, final guest memory hash).
- LatteBufferData.cpp: original. Harness: 300 seeds, 336k lines identical
  (vertex buffer bindings, uniform buffer binds, remapped uniform output,
  cache calls, incremental draw rebinding). Dropped the unused
  rectGenerate4thVertex template.
- LatteTexture.cpp/.h, LatteTextureView.cpp/.h, LatteTextureCache.cpp,
  LatteTextureLegacy.cpp, LatteTextureReadback.cpp, LatteTextureReadbackInfo.h,
  LatteSurfaceCopy.cpp/.h: original. Same texture/view creation rules,
  graphic pack rule handling, mip/slice overlap and data-dependency logic,
  2MB occupancy cells with the same visit order, hash sampling (incl. the
  AVX2 path), stale texture deletion and readback queue. The view lookup
  cache is a map from address to registrations (same first match order).
  Harness (stub renderer, fake graphic pack rules, serial ids, per-op state
  hash, full state dumps): seeds 0-100, 60+1400, 100+1600 and 100+2000
  identical (4.3M to 13.7M lines per run), ASan/UBSan clean.
  Fixes: surface copy no longer uses the source texture after creating the
  destination deleted it (it is looked up again, or the copy is skipped;
  a LatteTextureDeleteGuard reports deletions); a failed destination
  creation returns instead of dereferencing null; deleting a texture drops
  all of its pending readbacks (old code only the first); the AVX2 hash
  stores lanes to an array (clang rejected the out-of-range lane index).
  GetDeleteableTextures and the texture info cache walk the global texture
  list, so their order is deterministic (was unordered_set order). An
  unreachable overlap branch that could loop forever was dropped.
- LatteStreamoutGPU.cpp: original, same ring allocation, range clamp and
  offset update. LatteQuery.cpp, LatteQueryObject.h: original.
  Harness: 3000 seeds, 1.4M lines identical (renderer calls, guest result
  words). Fix: a host query started at an EndOcclusionQuery was pushed to
  the pending list early, so it was processed twice (double counted
  samples, use after destroy, possible endless ForceFinishAll loop). Only
  ended host queries are queued now. With overlapping guest queries the
  old code hit that bug, so that mode was checked on the new code only
  (clean).
- LatteShader.cpp/.h: original. Same base/aux hash formulas (they name the
  shader cache entries and graphic pack shaders), same program hash memo
  quirks (the pixel hash adds the geometry sum from the previous hash; the
  geometry hash uses the vertex shader that was active before the update),
  same shader chains per base hash, same state cache (key, aux variants,
  expiry sweep) and the same compile/cache-write order per stage.
  LatteShaderCache.cpp/.h: original, entry format unchanged (version/stage
  byte, BE hashes, register state, programs; same entry names and extra
  versions), same load loop, compile queue depth, boot images and sound.
  Harness (stub decompiler/renderer/FileCache, both APIs): 420 seeds, 6M
  lines identical, cache stores written by old and new byte identical, and
  each build loading the other's store gives identical results; damaged
  entries are rejected and deleted the same way. ASan/UBSan clean.
  Fixes: UnloadAll clears the active shader pointers (the next geometry
  hash read the freed vertex shader); LatteShader_free also frees a pending
  generated source; a stale state-cache key can no longer loop forever when
  a shader is freed; the boot sound thread is joined on exit (a joinable
  std::thread at static destruction terminates); the loading screen redraw
  timer also advances when the TV ImGui frame could not start (old code
  then swapped buffers after every entry); Windows memory delta is signed.
  Dropped: the dead experimental LatteShader_CompileSeparableVertexShader2
  (it was the only user of the Zir/Transcompiler headers in Core).
- LatteGSCopyShaderParser.cpp, LatteShaderAssembly.h: original (ISA values
  unchanged; seven macros nobody used were dropped). Harness: 200k random
  copy programs, 1.84M lines identical. Fix: fetch clauses that point past
  the program are no longer read out of bounds.
- LatteSoftware.cpp/.h: original (unused CPU vertex interpreter, same
  opcode subset and quirks). Harness: 20000 random programs, 100k lines
  identical. Differences: unsupported attribute formats read as zero
  instead of uninitialised stack; unsupported operands read 0 instead of
  hitting assert_dbg; the CF loop stops at the program end.
- LatteThread.cpp: original. Same start/stop handshake, init order, idle
  frames until a title runs, graphic pack wait and size-optimization check,
  reset registers, OSScreen flips (the four OSScreen buffer textures are
  looked up in the same order) and teardown in LatteThread_Exit. Not
  harness-tested (thread/renderer glue); syntax-checked with its callers.
  Dropped globals nobody used (the OSScreen texture pointers, an unused
  extern, the thread state copies).
- LatteRenderTarget.cpp, LatteCachedFBO.h: original. Same FBO key (built
  from the attachment pointers) and lookup view, same register decoding
  (padded sizes, scissor size hint, swizzle bits), same masks, clears,
  output shader choice, window image area, viewport/scissor math. The
  TV/GamePad swap (isDRCPrimary, Tab / Ctrl+Tab / controller screen
  button) and the recorder hook before the copyToBackbuffer calls are
  kept as they were. Kept quirks: a depth buffer with a valid size but no
  address keeps the previous depth attachment; NotifyTextureDeletion only
  clears color attachments. Harness (stub texture cache/renderer, objects
  in a fixed-address arena so the pointer-based FBO keys are compared
  too): 5000 seeds, 1.39M lines identical; a deliberate one-line change
  is caught. The controller screen button path was not covered (no
  emulated controllers in the harness). prevScissor* and
  hasValidFramebufferAttached are still exported.
- LatteAsyncCommands.cpp/.h: original (variant queue under a mutex; the
  wait now counts commands that are queued or running instead of reading
  the queue without a lock). The forced readback still looks up the
  texture with swizzle 0 as before.
- LattePerformanceMonitor.cpp/.h: original, same fields and slot logic,
  same overlay values (fps over the last two slots, draw calls per frame
  over all slots). Dropped computed values nobody read (cycles per
  second and leave counts); that also removes a possible integer division
  by zero. Harness (fake tick counter): 2M frames, 869k lines identical.
- LatteOverlay.cpp/.h: original. Same panels, order, stacking, texts,
  timings (5s session info, 750ms battery blink, 2.5s compile counters)
  and locking. The three g_compiling_pipelines* globals stay exported.
  The per-core CPU loop is bounded by the vectors it reads.
- Latte.h, LatteConst.h, LatteDraw.h, LatteIndices.h, LatteBufferCache.h:
  original headers, same declarations, values and LatteGPUState_t field
  order.
- LatteRecorder.cpp/.h: these are PixelU's own code (commit 991eb0e), not
  Cemu code. Only the MPL header was swapped for the original-code line;
  the code is unchanged, so its behaviour and output stay identical.
- Batch 12: the renderer layer under `Cafe/HW/Latte/Renderer` rewritten as
  original code (headers and sources), except Metal. 12a: the base
  Renderer, RendererCore, RendererShader and RendererOuputShader (output
  shader sources and the graphic-pack preamble names) and all of OpenGL
  (renderer, textures and views, FBOs, surface copy, shader objects and the
  GL shader cache, streamout, queries, canvas glue). 12b: all of Vulkan:
  the loader and function table (VulkanAPI), instance/device creation with
  the Vulkan 1.1 requirement, queues, swapchain and present (SwapchainInfoVk,
  including the embed/overlay-dock and GamePad window paths), the vsync
  driver, command buffer pool and submission, the memory manager and ring /
  heap allocators (VKRMemoryManager, VKRBase), the shader objects and SPIR-V
  cache (RendererShaderVk), the pipeline compiler and the on-disk pipeline
  cache (VulkanPipelineCompiler, VulkanPipelineStableCache, VKRPipelineInfo),
  textures, views, FBOs and readback (LatteTextureVk, LatteTextureViewVk,
  CachedFBOVk, TextureReadbackVk), surface copy (VulkanSurfaceCopy), draw
  submission, uniforms, descriptor sets and synchronisation
  (VulkanRendererCore), occlusion queries (VulkanQuery), the recorder
  capture (VulkanRecorderCapture) and the macOS surface (CocoaSurface).
  File names, public interfaces and every symbol used outside the renderer
  are unchanged. The Vulkan call order, pNext chains, glslang settings and
  resource limits (verified with memcmp), the internal GLSL texts (rect
  geometry shader, surface copy shaders) and the on-disk formats (SPIR-V
  cache, vkpipeline.bin: version, stage mask, big-endian hashes, register
  state, SHA-256 name) are unchanged, so caches written by either version
  load in the other. The recorder captures the same frames at the same
  points, with the same slot choice, barriers and drop messages.
  Verification: GL through a call-stream logging shim on Mesa llvmpipe,
  seeds 1-40 x 3000 steps: identical call streams apart from the rewritten
  output/copy shader sources, presented images within 1 LSB. Vulkan with a
  headless harness on Mesa lavapipe that drives textures, views, clears,
  copies, readbacks, game shaders with fetch shaders and uniforms,
  descriptor sets, draws in first/continued sequences, queries, present and
  teardown, through a Vulkan logging shim: each group against the previous
  code for seeds 1-8 x 400 steps, comparing the call stream, rendered
  readbacks, the pipeline and SPIR-V cache bytes, and old-cache to
  new-binary cross-loading. All identical.
  Fixes and defined behaviour: the hermite output filter uses a weight form
  (results agree within 1 LSB; Inf/NaN in the source propagate
  differently); unsupported colour formats give an all-UNDEFINED format
  info; the RGB8 screenshot path no longer reads out of bounds;
  VKRObjectSampler::DestroyCache no longer erases while iterating; the
  object reference drop is a single atomic decrement; texture_loadSlice
  and the CachedFBO rendering structs are value-initialised (unused
  attachments get VK_NULL_HANDLE views); the vsync driver state is atomic;
  the surface copy cache destructor is null-safe; the pipeline cache loader
  frees its temporary state when a shader is missing; the (unreachable)
  geometry-stage ALU branch of the incremental uniform upload asserts
  instead of being marked unreachable. GL: unsupported
  formats map to 0; streamout with an unsupported primitive uses
  GL_POINTS; out-of-range semantic ids are guarded; the Linux libEGL
  fallback is fixed.
  In-memory keys use new hash functions over the same inputs: the graphics
  pipeline state hash, the descriptor set hash (it now also mixes in the
  binding slot, so two layouts that differ only in slot position no
  longer collide), the surface copy pipeline key, the blit pipeline and
  descriptor keys, the sampler key and the render pass pipeline hash.
  None of these are written to disk. Debug callback filters are table
  driven. Log texts differ in places.
  Removed (no users): the dead Vulkan benchmark (the empty
  VulkanBenchmarkPrintResults is kept), dlopen_vulkan_loader,
  requestedFormatList, the unused depth-format helpers in surface copy,
  LatteTextureVk_AdjustTextureCompSel, s_syncOnNextDraw. File-local now:
  the rect-emulation geometry shader generator, the integer-format check,
  the uniform staging buffers, the shader compile thread pool and SPIR-V
  cache globals, and the pipeline cache queue and state.
  Kept on purpose: the depth-stencil and rasterizer setup read the global
  context registers, not the passed register set; blend factors 0x0B/0x0C
  map to MAX_ENUM; unsupported logic ops fall back to COPY with logic ops
  enabled; the direct-access vertex binding compares against a binding
  offset that is never updated; the feedback-loop relaxation in sync_inputTexturesChanged
  is unchanged. Metal is not touched. CocoaSurface.mm (Objective-C++) could
  not be compiled here and was reviewed by hand.
- Batch 13b: `util`, the Cemu-authored parts of `Common`, the settings in
  `config` (CemuConfig, ActiveSettings, LaunchSettings, NetworkSettings)
  and the entry points `main.cpp`/`mainLLE.cpp` rewritten as original code.
  util: the chunked, virtual and VHeap heaps, the memory mapper, fibers,
  the thread pool, the high resolution timer, system info, DXGI and
  screensaver wrappers, the boot sound reader, BMP/TGA writers, the INI
  parser, the event service, AES-128 and CRC32, the containers
  (IntervalBucketContainer, LookupTableL3, RangeStore, SmallBitset), the
  helpers (string helpers/parser/buffer, serializer, state hasher, queues,
  semaphores, ring buffer, memory pool, spinlock, enum_array and friends),
  the math types and the whole Zir shader IR (IR, builder, passes,
  scheduler, register allocators, GLSL emitter). Common: precompiled.h and
  precompiled.cpp, betype, MemPtr, StackAllocator, SysAllocator,
  CafeString, enumFlags, platform/version/socket headers, the unix and
  Windows platform layers and FileStream implementations, cpu_features,
  the exception handlers (common, POSIX, Win32) and the ELF symbol table.
  Not rewritten: third-party code (tinyxml2, robin_hood.h,
  flat_hash_map.hpp, md5, unix/date.h, unix/fast_float.h, GLInclude), the
  CMakeLists files, and config/ConfigValue.h and config/XMLConfig.h (not
  in this batch).
  File names, every public symbol, macro and type used elsewhere in the
  tree, and the exported C symbols (gameMeta_getTitleId, the AMD/NVIDIA
  GPU-selection exports) are unchanged. settings.xml is read and written
  with the same element names, value types, defaults and order, so an
  existing settings.xml loads to the same values and saves to the same
  bytes. StackAllocator and SysAllocator keep their sizes, alignment and
  registration order, so guest addresses come out the same.
  The PixelU additions keep their names, parsing and effect: --rp-embed,
  --rp-record/--rp-ffmpeg, --rp-parent (decimal or 0x hex, implies
  --rp-embed), --rp-opengl with LaunchSettings::RPForceOpenGL(), which
  ActiveSettings::GetGraphicsAPI still honours under ENABLE_OPENGL, and
  DefaultControllerProfile_CreateIfMissing() still runs before the input
  profiles are loaded.
  Old-vs-new harnesses (ASan/UBSan) give identical output: util (crypto,
  strings, parser, INI, serializer, containers, heaps, math; 106k lines);
  Zir IR building, passes, register allocation and GLSL emission (69k
  lines); Common (endian types, precompiled helpers, enum flags,
  CafeString, string copies, StackAllocator/SysAllocator guest addresses,
  MemPtr, FileStream); LaunchSettings over 4011 command lines, each in its
  own process (all options including the --rp-* ones); CemuConfig over
  3000 random settings.xml files through Load and Save, comparing the
  loaded values and the saved bytes. A new-only test covers the FileStream
  fixes. g++ and clang++ syntax checks of 320 files across Cafe, input,
  Cemu, util and main, and of the 117 files that include the config
  headers, fail only where the old tree fails too (missing third-party
  headers in this environment); a mingw-target clang check of the Windows
  sources shows the same results as the old tree.
  Fixes and defined behaviour: StringBuf grows instead of truncating; AES
  no longer modifies its input, handles tail blocks safely and its
  function pointers are valid before init; ParseHexString and ToUtf8 are
  bounds checked; GetParentProcess no longer raises SIGTRAP outside
  Windows; StateHasher uses a new hash (only compared within a run);
  MemMapper returns null instead of MAP_FAILED and Windows MEM_RELEASE
  passes size 0; LookupTableL3 frees its tables; VirtualHeap checks the
  first allocation's size, grows its pool and allocateAddr is null-safe;
  Vector3 Rotate* use the original components; timer frequency fixes;
  Fiber sets uc_link to null and on Windows the destructor converts a
  running fiber back to a thread; VHeap frees all blocks and uses loops
  instead of recursion; the regular register allocator works instead of
  trapping; Zir debug print format differs; DXGIWrapper null checks;
  SystemInfo bounds checks; betype operator| returns by value (the old
  one returned a reference to a temporary); libusbWrapper and bmp.h are
  minimal placeholders matching their use; boost/bluetooth.h is a corrected
  Windows-only version; EventService disconnect calls slot.disconnect().
  SysAllocator's string constructor pads to the element count and its
  void* conversion is fixed; CafeString guards empty copies and Length
  checks the bound first. The unix FileStream clears the stream state
  before tell/seek, implements SetEndOfFile, adds readU16/writeU16 and
  ends its va_list; the Windows Flush is a no-op. cpu_features detection
  is rewritten. The ELF symbol table no longer munmaps MAP_FAILED and
  bounds checks its tables; the POSIX backtrace offset is parsed from the
  correct substring; both exception handlers check g_gdbstub; the Win32
  handler no longer leaks a module reference. LaunchSettings help texts
  are reworded (the --rp-* descriptions are unchanged) and the extractor
  messages print their arguments; the legacy network service index
  increment cannot overflow; when the game's gamma is overridden it is
  left out instead of multiplied by 0 (differs only for a non-finite game
  gamma). main.cpp's helper functions are internal. Log texts differ
  in places.
  Kept on purpose: settings.xml reads mtlDevice as an int and writes it as
  a 64-bit value; the DSU port (uint16) is never loaded from settings.xml;
  legacy graphic packs insert the normalized path but store the preset
  under the original one.
- Batch 13a: pixelu/src/Cemu rewritten as original code: the logging
  front end and its asynchronous file writer, the ExpressionParser, the
  FileCache container, the Discord rich presence client, the crypto helpers
  (ncrypto), the online service client (napi: account, ECS/IAS/NUS/CCS SOAP,
  IDBE, version lists), the single line PowerPC assembler and table driven
  disassembler (PPCAssembler, including the ppcAsmTest self tests), and the
  NEX/PRUDP friend service client (nex). Not rewritten: Tools/DownloadManager
  (see below).
  File names, public interfaces and every symbol used outside these folders
  are unchanged. ExpressionParser results, the FileCache file format (read
  and written byte for byte the same), napi request XML, URLs, headers and
  TLS setup, the PPCAssembler output words, error texts and relocations for
  every instruction and syntax variant, and the NEX/PRUDP wire bytes are
  unchanged. PixelU's iosuCrypto_hasSeeprom addition in ncrypto is kept.
  Old-vs-new harnesses (ASan/UBSan) give identical results: the
  ExpressionParser 2.1M expressions on g++ and 13.1M on the clang
  fast_float path; FileCache 300 seeds of 200-1700 random operations with
  reopen, compression and damaged files (final file bytes identical, cross
  reads both ways); the Discord client against a fake IPC server; ncrypto
  200 seeds; napi against a fake libcurl 200 seeds x 250 calls covering
  every log path (ECDSA signatures verified, then masked); PPCAssembler
  200 seeds (every mnemonic in 63 operand shapes with branch hints, 20000
  random and structured words disassembled and reassembled, every
  assembled word disassembled again with bit flips, 20000 random source
  lines, 5000 data directive lines); nex against a scripted NEX server
  behind a fake UDP layer and fake clock (60 seeds x 300 steps: 40
  identical, in the other 20 the old code deadlocks, crashes or traps and
  the new output is identical up to that point; 30 more seeds with error
  answers and truncated lists run on the new code only and finish
  cleanly), plus 40 seeds x 4000 serialisation fuzz cases. All callers
  compile with the same diagnostics on g++ and clang++.
  Fixes and defined behaviour: an empty or whitespace-only expression throws
  instead of reading an empty stack; the FileCache writer thread starts
  lazily, drains on stop and can no longer hang at exit, and a zero sized
  table in the header no longer overflows the heap; the Discord client
  writes valid JSON for party fields, parses READY strings containing braces
  and tries discord-ipc-0..9 on Windows; Logging's waitForFlush waits for
  the batch to be written and returns when no log file is open; napi frees
  its curl header lists, locks the service URL override map and reports a
  TIV without '.' as an XML error instead of reading past the string; the
  PPCAssembler register number parser wraps without signed overflow.
  NEX: login, ticket and account registration calls that get an error
  answer now fail the login instead of waiting forever; a call whose error
  handler queues another call (adding or removing a friend while the
  connection drops) no longer deadlocks the nex thread on the request
  queue lock; a friend list refresh during teardown no longer dereferences
  a null session; updatePreferencesAsync and updateCommentAsync without a
  session report ERR_NOT_CONNECTED to the callback;
  requestGetAllInformation with a callback sends zeroed presence data
  instead of uninitialised memory; malformed or truncated packets, unknown
  PRUDP packet types and call parameters of 7 KB or more are logged and
  dropped or failed instead of hitting assert_dbg or reading out of
  bounds; list and string readers stop at the end of the buffer, so a
  truncated friend list keeps only the entries it holds instead of padding
  the list to the announced count with pid 0 entries or stale entries of
  the previous list; a length placeholder near the end of a buffer is
  patched only inside it; sockets whose bind fails are closed; picking
  port 50000 no longer throws out_of_range; fragment sequence ids wrap as
  u16.
  Kept on purpose: PPCAssembler quirks (no operand swaps, highest priority
  then last row wins when disassembling, '#' starts a comment even inside
  strings, a \x escape ends a string literal, unknown escapes keep the
  backslash, rotate aliases OR their fields without masking, x86 style
  double to integer conversion of data directives); NEX quirks
  (updateMyPresence keeps the caller's game key but sends the foreground
  title, a second incoming request hides an added friend notification, one
  service message is handled per update, empty reliable data packets are
  not queued, the PRUDP random number sequence).
  Log texts differ in places.
  Tools/DownloadManager is still the original Cemu code: it drives account
  login, ticket sync, TMD/content download, verification and installation
  through napi, ncrypto, FST, FileStream, FileCache, the title list and the
  thread pool, and a trustworthy old-vs-new comparison needs a fake title
  server with valid tickets, TMDs and encrypted FST content. It needs its
  own batch.
- Batch 14: all of `input` and `audio` rewritten as original code. Input:
  the InputManager (controller slots, profile load/save including the .txt
  migration, mouse/touch tracking for the GamePad touch screen, the update
  thread), the ControllerFactory, the ControllerBase pipeline (calibration,
  dead zone/range, axis buttons) and the controller state, the motion code
  (Mahony fusion, motion handler, motion sample), the emulated controllers
  (VPAD, Pro, Classic, Wiimote, WPAD and the shared EmulatedController with
  its multi-controller main/extra mappings), and every host API: Keyboard,
  XInput, DirectInput, SDL3 (event thread and sensor fusion), the DSU client
  (messages, UDP reader/writer), GameCube adapter, and real Wii remotes (HID
  report parsing, IR pointer, Nunchuk/Classic/MotionPlus, the hidapi and
  BlueZ L2CAP transports). Audio: IAudioAPI and IAudioInputAPI, and the
  Cubeb, Cubeb input, DirectSound, XAudio 2.8 and XAudio 2.7 backends.
  Unchanged: input/DefaultControllerProfile.* (PixelU's own file, MPL),
  audio/xaudio2_7/ (third party) and the CMake files.
  File names, public interfaces, enum values, button ids, the controller
  profile XML format (users' controllerProfiles/*.xml load the same and save
  in the same layout), the IAudioAPI interface (including GeneralSettings2's
  friend access to s_audioDelay; the recorder still taps snd_core), the
  SDL InitSDL/ShutdownSDL/PumpSDLEvents entry points and the GamePad touch
  from the main window's mouse/touch are unchanged.
  Old-vs-new harnesses (ASan/UBSan) give identical output: profiles,
  migration and emulated controllers 1500 iterations (327669 lines: random
  profile XML round trips, VPAD/KPAD/WPAD reads from random raw states with
  mouse/touch, and Mahony/motion fusion for Pro, Classic and Wiimote, 2334
  motion samples); XInput (fake XInput functions), SDL3 (fake SDL with
  scripted gamepads and sensor events), the DSU client against a local UDP
  server with random, corrupted and unknown packets, and Wii remote HID
  parsing through a scripted fake device (status, memory read replies,
  acknowledgements, every data report, extension detection, IR, rumble/LED
  output reports): 300 XInput iterations, 75 SDL rounds, 300 DSU packets
  and 600 remote reports (25055 lines); audio with a fake cubeb (device
  enumeration, stream setup, FeedBlock/NeedAdditionalBlocks/output callback
  queueing, input recording/ConsumeBlock, play/stop/volume) 300 iterations
  (85054 lines). g++/clang++ syntax checks of every changed file and its
  callers match the pre-rewrite tree; mingw checks of all Windows backends
  pass except XAudio27API.cpp, which fails on the DirectX 2010 header before
  and after and was reviewed by hand.
  Fixes and deliberate differences: profile mapping entries are saved sorted
  by mapping id (the old order was hash order; loading is order
  independent). The Classic Controller's SDL default layout is used for SDL
  pads (it fell through to the XInput layout). The GamePad's right-mouse
  rotation is kept per controller instead of in a function static. SDL
  rumble is not sent to a disconnected pad (it was called with a null
  gamepad). The DSU writer queue owns plain byte copies (the old queue
  deleted derived messages through a base pointer, flagged by ASan), and
  the receive buffer is zeroed per packet so a short packet is checked
  against zeros instead of stale bytes. Wii remote button and 16-bit
  fields are read bytewise (no misaligned loads), the BlueZ candidate map
  uses a strict weak ordering (its comparator returned bacmp, true for both
  orders), an oversized L2CAP write is clamped, and the hidapi probe
  tolerates a missing product string. The GameCube controller sets buttons
  through SetButtonState (the old code called a non-existent buttons.set()
  and could not compile with HAS_GAMECUBE). XAudio2API::GetType returns
  XAudio2 (it returned XAudio27; nothing reads it). XAudio2 frees and
  forgets a DLL without XAudio2Create, and clears the WMI VARIANTs it
  reads; DirectSound checks the initial buffer lock; a device description
  of the wrong backend throws instead of being dereferenced as null. Unused
  structs (BasicIR, ExtendedIR, IRCamera globals), unused friends and the
  four duplicate AI.h includes in DSUController.h were dropped; the cubeb
  output drop message names cubeb instead of direct sound. Window, stream
  and context names ("Cemu Cubeb output" etc.) are unchanged.
- Batch 15a: the top-level wxWidgets GUI rewritten as original code (headers
  and sources, 47 files): interface/WindowSystem.h, wxWindowSystem, wxgui.h,
  wxHelper.h, wxCemuConfig, the OpenGL/Vulkan/Metal canvases and
  IRenderCanvas, LoggingWindow, RPEmbed, CemuApp, MainWindow, PadViewFrame,
  AudioDebuggerWindow, GettingStartedDialog, ChecksumTool, GameUpdateWindow,
  GameProfileWindow, DownloadGraphicPacksWindow,
  DownloadCustomGraphicPackWindow, GraphicPacksWindow2, CemuUpdateWindow,
  TitleManager, MemorySearcherTool and GeneralSettings2. The wxgui
  subfolders other than canvas (components, dialogs, helpers, input,
  debugger, windows, wxcomponents, EmulatedUSBDevices) are unchanged. File names, public interfaces, every symbol other files use,
  window classes and titles, menu and command ids, settings fields and file
  formats are unchanged; the PixelU behaviour is kept exactly: --rp-embed /
  --rp-parent embedding, off-screen waiting and docking (child window or
  window owned by RetroPixels' window, 20 second fallback to a normal
  window), messages shown through RPEmbed::ShowMessage, wxWidgets log output
  routed to log.txt, the PixelU.Screen window message (0 TV, 1 GamePad,
  2 toggle, returns the state), mouse/touch on the canvas as GamePad touch,
  stopping the recorder, the "PixelU" titles and branding, the CemuApp
  start-up and --rp-* handling and no first-run dialog when embedded.
  MainWindow binds its events with Bind (BindEvents at the start of the
  constructor) instead of a static event table, with the same handler order.
  CemuUpdateWindow is now a notice that PixelU is updated together with
  RetroPixels (PixelU never used Cemu's updater) and IsUpdateAvailableAsync
  returns a finished "no update". Cemu branding in user-visible strings now
  says PixelU; the About dialog thanks the Cemu team and contributors instead
  of listing Cemu's Patreon supporters. Translations exist for the unchanged
  strings only.
  Checked with g++, clang++ and mingw clang (MSVC-style flags) against the
  old sources: no new errors (the remaining mingw errors are environment
  only: __uuidof, DirectInput and type_info::raw_name). Old-vs-new harnesses
  (ASan/UBSan) give identical results: wxCemuConfig load/save 3000 random
  configs; PixelU.Screen all messages and states; RPEmbed docking decisions
  (child/owned/host detection, fill-parent moves, message box icons) on a
  fake Win32 window table, 200k setups; ChecksumTool's JSON schema; graphic
  pack filtering and version support 200k cases; TitleManager download list
  entries (kind, status, progress markers), save persistent-id folder names
  and account labels 700k cases; MemorySearcherTool value parsing (600k),
  memory scans of random buffers for all six types, type names, stored
  addresses and saved ini lines; GeneralSettings2 audio API names, text
  scale choices, window remember markers, birthday splitting, country
  filter, vsync/device choices and the missing-files report, plus identical
  sets of stored (54) and applied config fields.
  Fixes and defined behaviour: null checks on the game list and info bar in
  MainWindow; file load paths are taken as UTF-8; PadViewFrame mouse moves
  lock the mouse state's mutex instead of the touch state's. ChecksumTool:
  commit.txt is written only after the checksum archive unpacked, a WUD
  that cannot be opened shows a status text instead of throwing in the
  worker, 30 or more missing files (and nothing damaged) offer the export
  instead of reporting "valid", and the enable event is always sent. The
  graphic pack downloads compute progress in floating point, the
  "connection failed" message box has text and caption the right way
  round, JSON fields are checked before use, an unreadable zip entry is
  skipped, an oversized entry is closed, an unpacking exception becomes
  "Failed to open downloaded ZIP", cancelling no longer shows a result and
  the "cannot be updated while a game is running" box has a plain OK
  button; the never-reachable "Updated graphic packs are available" prompt
  is gone. GraphicPacksWindow2 skips unknown shader types and its hyperlink
  no longer translates an empty string. TitleManager: a save export whose
  folder cannot be listed logs it instead of throwing, a source file that
  cannot be opened is reported once instead of twice, and the download
  callbacks ignore a closed window. MemorySearcherTool: the search thread
  no longer reads controls off the GUI thread, the gauge range is sent
  before progress, filter progress comes in batches, each range's final
  element is searched too, the scan stops when the window goes away,
  invalid table text (address or frozen/edited value) is ignored instead of
  throwing or writing 0, addresses at or above 0x80000000 work on Windows,
  a cancelled edit writes nothing and the edited row is the event's row,
  and the memorySearcher folder is created before saving. GeneralSettings2:
  the "create a new MLC structure?" question is now actually asked,
  deleting an account refreshes the main window's account list, a stored
  vsync index the API does not offer is not passed to the choice, a
  device that cannot be created no longer crashes, the portal device has
  its own tooltip, the missing-files report ends in "..." only when files
  were left out, and the Change/Clear MLC buttons are translatable. Log
  texts differ throughout.
  Kept on purpose: the GamePad view reports the right mouse button from the
  left button's state; MainWindow::IsMenuHidden stays though nothing calls
  it; TitleManager still binds its own (never sent) title-found event and
  exported saves keep the "cemu_meta" marker the importer checks;
  GeneralSettings2::MLCModified stays false (the restart follows anyway);
  Int64 frozen values (decimal) and stored
  addresses (hex) are read exactly as before; the memory searcher's result
  list stops at 5000 hits and only frozen entries keep their value in the
  ini file.
