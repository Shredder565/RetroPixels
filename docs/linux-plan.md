# RetroPixels for Linux: plan

The project owner asked for "a linux version" of RetroPixels with every
feature kept (2026-10-20). This file is the survey of what is Windows-only
today, the target, the batches that get there, and how each batch is
checked on a Linux container without a GPU. Batch L0 (this survey, this
plan and the first Go pieces) is done; every later batch is one worker's
job and names the files it owns, so batches that don't depend on each
other can run at the same time without touching the same files.

## Target

- **Machine:** x86_64 Linux desktops (X11, or Wayland with XWayland, which
  every mainstream Wayland desktop has). No ARM64: PixelU's and PixelU-Wii's
  JITs are x86-64 only (the ARM64 JIT was pruned in PixelU-Wii W1).
- **Baseline:** glibc 2.35 and later, i.e. everything is built on the
  `ubuntu-22.04` runner. That covers Ubuntu 22.04+, Debian 12+, Linux Mint
  21+, Fedora 36+, openSUSE Leap 15.5+/Tumbleweed, Arch, SteamOS 3.5+.
  The Go programs are static (`CGO_ENABLED=0`) and have no glibc floor at
  all; the C++ engines do. Each engine job checks its floor with
  `objdump -T <binary and bundled .so> | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1`
  and fails above 2.35.
- **Expected on the system** (present on every desktop install, so not
  bundled): glibc, libstdc++ (GCC 11's or newer), X11/XCB client libraries,
  libGL/libEGL and libvulkan (Mesa or the GPU vendor's driver),
  PulseAudio or PipeWire (libpulse), ALSA, GTK 3 (PixelU's wxWidgets UI),
  fontconfig/freetype, D-Bus. A Chromium-based browser (Chrome, Chromium,
  Edge or Brave) for the app window; without one the default browser opens
  the page. `zenity` or `kdialog` for file dialogs.
- **Bundled:** everything else: Qt 6.8.3 (PixelU-Wii, official
  `linux_gcc_64` build through aqtinstall, with the xcb platform plugin and
  the Wayland plugin), SDL2/SDL2_net (Supermodel, the versions pinned in
  supermodel/pins.sh), the vcpkg libraries of PixelU (linked statically, as
  on Windows), ffmpeg (downloaded on first use, as on Windows, or the
  system's). Bundled `.so` files sit in each engine's `lib/` and the
  engines are linked with `RPATH=$ORIGIN/lib`, so nothing is installed.
- **Package:** a folder `RetroPixels-<ver>-linux-testN/` with the same
  layout as the Windows download (`RetroPixels`, `RetroPixels-Native`,
  `PixelU/PixelU`, `PixelU/wii/PixelU-Wii`, `MAME/MAME 0.xxx/mame`,
  `Supermodel/supermodel`, README.txt, CHANGELOG.txt, licences,
  VERSION.txt, plus `RetroPixels.desktop` and `retropixels.png`), packed as
  `RetroPixels-<ver>-linux-testN.tar.gz`. A workflow artifact zip loses the
  executable bits, so the artifact `RetroPixels-<ver>-linux-testN` holds the
  `.tar.gz` and a copy of VERSION.txt next to it. Never a GitHub release.
  An AppImage is a later, optional extra (see the decisions below): it
  can't hold the engines' writable portable folders, which RetroPixels and
  the engines rely on.
- **Settings and data:** `os.UserConfigDir()` is already
  `~/.config/RetroPixels` (config.json, mame.json). What Windows keeps in
  `%LOCALAPPDATA%\RetroPixels` (the browser profile with the page's save
  states, the downloaded ffmpeg, downloaded MAMEs) goes to
  `$XDG_DATA_HOME/RetroPixels` (`~/.local/share/RetroPixels`), `dataDir()`
  in platform_linux.go. Engines stay portable (their data next to them),
  exactly as on Windows.

## Survey: the Go app (RetroPixels, the web app)

`GOOS=linux go build ./...` and `go test ./...` already passed before L0:
every Windows piece has a `_windows.go` file and a `!windows` stub, and the
stubs were written for running the server's tests. What each needs:

| Piece | Windows today | Linux | Batch |
|---|---|---|---|
| App window (`openAppWindow`) | Edge/Chrome `--app` with its own profile, `rundll32` + message box fallback | Chrome/Chromium/Edge/Brave `--app`, own profile, `--class=RetroPixels`; `xdg-open` fallback | **L0 done** (platform_linux.go) |
| Error box (`showError`) | MessageBoxW | zenity/kdialog, and stderr | **L0 done** |
| File and folder dialogs | PowerShell WinForms | zenity or kdialog (filters converted, "All files" added); `RETRO_TEST_PICK(_DIR)` answer them in tests | **L0 done** |
| Open folder | explorer.exe | xdg-open | **L0 done** |
| Is a program running | tasklist; Toolhelp32 (internal/dolphin) | /proc (exe link and argv[0]; "X" counts for "X.exe"; Wine found too) | **L0 done** |
| .7z unpack (internal/dolphin `systemUnpack7z`) | Windows 11 tar.exe, else RetroPixels' own 7-Zip reader (sevenzip/) | 7zz/7z/7za or bsdtar when installed, else the same own reader (main's `systemUnpack7z` is unused on every system) | **L0 done** (internal/dolphin/unpack_linux.go) |
| Single instance | /ping on port 47821 | same code, already portable | — |
| Window icon (appicon_windows.go) | ExtractIconExW + WM_SETICON | `--class=RetroPixels` + `StartupWMClass` in the .desktop file + the page favicon; no code | L9 |
| Tray / menus | none in the web app (the page is the UI) | — | — |
| Controllers | the page's Gamepad API (Chrome) | Chrome's Gamepad API works on Linux (evdev, rumble included) | — |
| Docking engine windows (embed_windows.go: child window for Dolphin/MAME, owned overlay for PixelU/Supermodel, kept below the game bar, Stop = WM_CLOSE, `pixeluScreen`) | user32 | X11: own small X11 protocol client (no cgo), reparent / transient overlay, `_NET_WM_PID` to find windows, `WM_DELETE_WINDOW`; on Wayland everything runs under XWayland | L2a, L2b |
| Per-game volume (dolvolume_windows.go) | Core Audio session of the engine's pid | PulseAudio/PipeWire sink-input of that pid (`pactl`, `application.process.id`) | L3 |
| Recording (recorder.go, internal/*/record.go) | engines dump frames and sound, ffmpeg makes the .mp4; ffmpeg.exe found/downloaded (BtbN win64 build); `lowPriority` = BELOW_NORMAL | same flow; `ffmpeg` (no .exe) next to the app, in `dataDir()/ffmpeg`, or on PATH; download BtbN's `linux64-gpl` build; `lowPriority` via `nice -n 10`; add `h264_vaapi` to the encoder order | L8 |
| Hidden console windows (`hiddenCommand`, `hideConsole`) | CREATE_NO_WINDOW | nothing to do (stubs are right) | — |
| Engine program names and folders | `PixelU.exe`, `PixelU-Wii.exe`, `supermodel.exe`, `mame*.exe`, `%APPDATA%`, `Downloads\…` | `PixelU`, `PixelU-Wii`, `supermodel` (internal/supermodel already accepts `supermodel` off Windows), `mame`; also `/usr/games/mame`, `/usr/bin/mame` | L1a (PixelU, PixelU-Wii, Supermodel), L7 (MAME) |
| Vulkan check (internal/pixelu `CheckVulkan`) | vulkan-1.dll | libvulkan.so.1 + an ICD json in the Vulkan loader's folders | L4 |
| TV / GamePad screen switch (`SetScreen`, `pixeluScreen`) | window message to PixelU's HWND | ClientMessage to PixelU's X11 window (same mode numbers) | L4 (engine + internal/pixelu/screen_linux.go), L2b (caller) |
| Wording in the page ("RetroPixels.exe", "PixelU.exe", "File Explorer") | Windows wording | `window.RETRO_DESKTOP.os` from main.go; the page words it per system | L1b |
| Opening games by double-click | only RetroPixels Native (assoc_windows.go, registry) | .desktop file with MimeType + `xdg-mime` | L10 (web app has none today), L11 (Native) |
| Go Wii U emulator's host GPU (wiiu/latte_d3d_windows.go, wiiu/d3d) | Direct3D 11 | none yet: the software renderer (hostgpu_other.go) is used, which is slower | L12 (optional) |
| RetroPixels Native (cmd/retropixels-native, 24 `_windows.go` files, ~8600 lines: Win32 window and menus, GDI drawing, waveOut sound, XInput, registry file types, dialogs, docking) | Win32 | a Linux front end: window + menus + dialogs, ALSA/Pulse sound, evdev controllers, .desktop file types, X11 docking | L11a–L11d |

The rest of the app (the N64, arcade and Wii U hosts over WebSocket, the
Archipelago bridge, settings, the web page, all of `cores/`, `n64/`,
`wiiu/` with its amd64 JIT that already has a Linux file) is portable Go and
already tested on Linux by CI.

## Survey: the engines

**PixelU (Wii U, pixelu/, MPL-2.0).** The Cemu-derived tree kept its Linux
code: CMake still configures on Linux (GTK 3, optional Wayland), audio is
cubeb (Pulse/ALSA back ends in the submodule) and SDL3, video is Vulkan or
OpenGL with Xlib/XCB/Wayland surfaces. Only Cemu's `dist/linux` desktop
files and AppImage script were dropped (24c4a16; not needed). What is
missing is RetroPixels' own part on Linux: `RPEmbed` (`--rp-embed` keeps the
window hidden until RetroPixels docks it; `--rp-parent` is Windows only),
the screen-switch message, `--rp-record` paths, and a Linux CI build with
vcpkg's `x64-linux` triplet. No pruned code needs to come back. Risks:
untested Linux build after 1600 lines of rewrite log; Cemu needs Vulkan
features lavapipe may lack (OpenGL 4.5 on llvmpipe is the fallback check).

**PixelU-Wii (GameCube/Wii, pixelu/wii, GPL-2.0+, now fully original
code).** W1 deleted the Linux sources: `MemArenaUnix.cpp`,
`ConsoleListenerNix.cpp`, `UnixUtil.h`, the EGL/EGLX11/GLX11Window GL
interfaces, evdev/Xlib/Pipes input, `AlsaSoundStream`/`PulseAudioStream`,
UICommon's `X11Utils`/`DBusUtils`, DolphinQt's `SignalDaemon`,
`MemoryWatcher`, `TAP_Unix`, `TraversalServer`. The rewritten files keep
their POSIX branches (MemoryUtil, DynamicLibrary, Thread, MemTools…), but
the rewritten Vulkan back end creates only Win32 surfaces, and the GL back
end has only WGL. Cubeb (with its Pulse/ALSA back ends) and SDL are still
there, so audio and controllers need no new back end; a keyboard/mouse
back end is needed for the hotkeys and keyboard play. Since the owner's
goal is 100% original code, the missing pieces are **written as our own
code** (clean-room: learn from Dolphin what they must do, never copy),
not restored from Dolphin. Third-party libraries come from their own
projects as today (Externals/sources.json, Qt's official build).

**MAME.** RetroPixels ships the MAME team's official Windows release
unchanged. There is no official Linux binary, so the Linux download builds
the same release from MAME's own source tarball (which bundlemame already
saves) on the `ubuntu-22.04` runner, `SUBTARGET=arcade` unless the owner
wants all of MAME (see decisions), cached by version so it is built once
per MAME release. The in-app "Download MAME" button can't fetch a Linux
build; on Linux it offers the bundled one and the system's `mame` package.

**Supermodel.** Already builds and starts headless on Linux in
supermodel.yml (`linux-check` job, Xvfb + llvmpipe). Missing: a packaged
Linux artifact with SDL2/SDL2_net bundled from the pinned sources, and
checking RetroPixels' patch's Windows-only parts (`FileSystemPath`, window
placement for docking) on Linux.

**ffmpeg.** BtbN's FFmpeg-Builds publishes `ffmpeg-master-latest-linux64-gpl`
(static); same source as the Windows download.

## Batches

Each batch: one worker, its own files, its checks, small commits. "Owns"
lists files or folders only that batch changes; a batch that needs a
change in another batch's file asks for it instead. `docs/linux-plan.md`
is updated only by the coordinator (each worker reports its result).

| Batch | What | Owns | Needs |
|---|---|---|---|
| **L0** ✓ | Survey, this plan; Linux app window, dialogs, open folder, /proc, 7z in the Go app | platform_linux.go, internal/dolphin/{proc,unpack}_linux.go (+ tests) | — |
| **L1a** | Engine names and folders on Linux: `PixelU`, `PixelU-Wii`, `supermodel` without .exe; `Downloads/…` finding; pick-dialog filters; Linux Dolphin user dir; Wii keys scan roots | internal/pixelu/pixelu.go, internal/pixelu/wii.go, internal/supermodel/supermodel.go, internal/dolphin/{config,find,wiikeys_other}.go (+ `_linux.go` splits), internal/oldname, pixelu.go, supermodel.go, dolphin.go | L0 |
| **L1b** | The page on Linux: `window.RETRO_DESKTOP.os`, per-system wording and file filters in the page | web/index.html (wording only), main.go (one line) | — |
| **L2a** | `internal/x11`: our own X11 protocol client in pure Go (connect over the Unix socket, MIT-MAGIC-COOKIE auth, atoms, QueryTree, GetProperty, ReparentWindow, Configure/Map/Unmap, SendEvent, events) | internal/x11/** | — |
| **L2b** | Docking on Linux: `embed_linux.go` (child for PixelU-Wii/MAME, transient overlay for PixelU/Supermodel, kept below the game bar, focus, Stop via WM_DELETE_WINDOW then SIGTERM), `stopEmbeddedGame`, `pixeluScreen` caller; on Wayland Chrome and the engines are started for XWayland | embed_linux.go, embed_other.go (tag), platform_linux.go (browser flags) | L2a, L0 |
| **L3** | Per-game volume through PulseAudio/PipeWire (`pactl`, sink inputs of the engine's process tree) | dolvolume_linux.go, dolvolume_other.go (tag) | — |
| **L4** | PixelU on Linux: vcpkg `x64-linux` build in pixelu.yml (new `build-linux` job on ubuntu-22.04), RPEmbed Linux branch (hidden until docked, `--rp-parent <X11 id>`), screen switch by ClientMessage, record paths; artifact `PixelU-linux-x64` (tar.gz inside) | pixelu/** (Linux branches), .github/workflows/pixelu.yml, internal/pixelu/{screen,vulkan}_linux.go | — (L2a for its X11 tests) |
| **L5a** | PixelU-Wii Linux platform code (own): Unix memory arena (memfd + mmap), console listener, CMake lists for Linux; `dolphin-emu-nogui` links and starts with the Null back ends | pixelu/wii/Source/Core/Common/**, pixelu/wii/Source/Core/Core/** (platform files only), pixelu/wii/CMakeLists.txt | — |
| **L5b** | PixelU-Wii video on Linux (own): Vulkan Xlib/XCB and Wayland surfaces, GLX and EGL GL interfaces | pixelu/wii/Source/Core/VideoBackends/**, pixelu/wii/Source/Core/Common/GL/** | L5a |
| **L5c** | PixelU-Wii input and sound on Linux (own): X11 keyboard/mouse back end (XInput2), cubeb with Pulse/ALSA enabled in the Externals build; SDL for controllers (already there) | pixelu/wii/Source/Core/InputCommon/**, pixelu/wii/Source/Core/AudioCommon/**, pixelu/wii/Externals/** (cubeb/SDL options) | L5a |
| **L5d** | PixelU-Wii app on Linux: DolphinQt Linux bits, `--rp-embed` on Linux, pixelu-wii.yml `build-linux` job (aqt `linux_gcc_64` Qt 6.8.3, own script to copy Qt libs and plugins with `$ORIGIN` rpath); artifact `PixelU-Wii-linux-x64` | pixelu/wii/Source/Core/DolphinQt/**, pixelu/wii/Source/Core/UICommon/**, .github/workflows/pixelu-wii.yml, tools/pixelu-wii-linux-deploy.sh | L5a, L5b, L5c |
| **L6** | Supermodel Linux package: `linux` job in supermodel.yml makes `Supermodel-linux-x64` with SDL2/SDL2_net from pins.sh bundled; patch checked/fixed for Linux docking | supermodel/**, .github/workflows/supermodel.yml | — |
| **L7** | MAME on Linux: `mame-linux.yml` builds the bundled release from source (cached by version); bundlemame `-os linux` places it; internal/mame finds `mame` (no .exe, `/usr/games`), Linux install/download behaviour | tools/bundlemame/**, internal/mame/**, mame.go, .github/workflows/mame-linux.yml | — |
| **L8** | ffmpeg and recording on Linux: names, download of the linux64 build, `nice`, `h264_vaapi` | internal/ffmpeg/**, ffmpeg_install.go, recorder.go (finder and encoder list only), recorder_linux.go, recorder_other.go (tag) | — |
| **L9** | Linux download: `bundle-linux.yml` (ubuntu-22.04): Go tests, static builds of RetroPixels and RetroPixels-Native, engines from their latest successful Linux artifacts (each optional until its batch lands, listed in VERSION.txt), glibc check, `RetroPixels-<ver>-linux-testN.tar.gz` + VERSION.txt; smoke test under Xvfb | .github/workflows/bundle-linux.yml, tools/linux/package.sh, tools/linux/smoke.sh | L0; engines as they land |
| **L10** | Desktop integration: `RetroPixels.desktop` + icon, "Add RetroPixels to the applications menu" (writes to `~/.local/share/applications` and the hicolor icon folder), file types for double-click opening once the web app takes a game path | tools/linux/RetroPixels.desktop, tools/linux/retropixels.png, desktop_linux.go | L0 |
| **L11a** | Native: sound (ALSA or PulseAudio) and controllers (evdev in pure Go, rumble with FF_RUMBLE) | cmd/retropixels-native/{sound,pad}_linux.go | — |
| **L11b** | Native: window, drawing, menus, dialogs, drag and drop (toolkit per decision 3) | cmd/retropixels-native/{app,ui_*,win}_linux.go | L11a |
| **L11c** | Native: libraries and settings windows (GameCube/Wii, MAME, PixelU, controls, save states, Archipelago) | cmd/retropixels-native/*_library_linux.go, dialogs | L11b |
| **L11d** | Native: docking, file types, autorecord; main_other.go becomes `!windows && !linux` | cmd/retropixels-native/{dock,assoc,autorecord,main}_linux.go, main_other.go | L11b, L2a |
| **L12** (optional) | A Vulkan host GPU for the Go Wii U emulator (today D3D11 only; Linux uses the software renderer) | wiiu/latte_vk_linux.go, wiiu/vk/**, wiiu/hostgpu_other.go | — |

Order: L1a, L1b, L2a, L3, L4, L5a, L6, L7, L8, L10, L11a can all start at
once. Then L2b (after L2a), L5b and L5c (after L5a), then L5d; L11b–d in
order. L9 starts early with the Go programs only and picks up each engine
artifact when its batch is done. The Linux download is "complete" when
L1–L8 and L9–L10 are done; L12 follows.

**Owner decisions, 2026-10-21:** RetroPixels Native is not built for Linux
(L11a–L11d are dropped; Native stays Windows-only). MAME for Linux
(mame-linux.yml) runs by hand only and is built last, after everything else.

## Checking on a Linux container without a GPU

- **Display:** Xvfb (`xvfb-run -a -s '-screen 0 1280x1024x24'`); Wayland
  paths are only built, and run under XWayland on real machines.
- **OpenGL:** Mesa llvmpipe (`libgl1-mesa-dri`, `LIBGL_ALWAYS_SOFTWARE=1`),
  OpenGL 4.5 core: enough for PixelU's and PixelU-Wii's GL back ends and
  Supermodel.
- **Vulkan:** Mesa lavapipe (`mesa-vulkan-drivers`, `VK_ICD_FILENAMES` →
  `lvp_icd.x86_64.json`), Vulkan 1.3; `vulkaninfo --summary` in the log.
  Where a back end needs an extension lavapipe lacks, the batch says so and
  checks that back end's start-up and the OpenGL path instead.
- **Sound:** PulseAudio or PipeWire started for the job with a null sink
  (`pulseaudio -D --exit-idle-time=-1 && pactl load-module module-null-sink`);
  cubeb and SDL then open a real stream; L3 checks volumes with
  `pactl list sink-inputs`. Without a server, the engines' null output.
- **Controllers:** no /dev/uinput in containers, so: SDL's virtual joystick
  in the engines' own tests, the Gamepad API faked in the page tests, and
  evdev parsing tested against recorded event bytes (L11a).
- **The app window:** Chrome (on the ubuntu runner already) under Xvfb with
  `--remote-debugging-port`; the smoke test (L9) checks /ping, that the page
  loads and checks in (`/api/heartbeat`), that closing the window ends the
  app, and that a second start opens another window on the first copy.
- **Docking:** a test program (Go, using internal/x11) that maps a window
  with `_NET_WM_PID`; L2b's test docks it and checks the tree with
  QueryTree and `xwininfo -tree`, its geometry, and Stop.
- **Engines without games:** no Nintendo files are ever used. PixelU:
  `wiiu/testdata/demo.rpx` (RetroPixels' own demo) boots, draws and makes
  sound; a screenshot (`import -window root`) must not be all black.
  PixelU-Wii: `dolphin-emu-nogui` start-up with the Null
  back ends, and a tiny test DOL written for this (with PixelU-Wii's own
  PowerPC assembler in Common) that clears the screen through the GX FIFO.
  Supermodel: as today (`-print-games`, `-print-gl-info`, no-ROM exit).
  MAME: `mame -validate` and `-listxml` for one driver.
- **Go:** `go test ./...` on Linux as today, plus the new `_linux_test.go`
  files; `GOOS=windows go build ./...` and `go vet` for Windows must stay
  clean in every batch.

## CI workflows

- **ci.yml** (exists, ubuntu-latest): already runs `go test ./...` on
  Linux. Add `GOOS=linux go vet ./...` and `GOOS=windows go build ./...`
  so both systems stay building (L9).
- **pixelu.yml:** new `build-linux` job next to `build-windows`
  (ubuntu-22.04, vcpkg `x64-linux` with the same binary cache scheme,
  GTK 3 dev packages), headless demo.rpx run, artifact `PixelU-linux-x64`
  (`PixelU-linux.tar.gz` inside, keeps exec bits). (L4)
- **pixelu-wii.yml:** new `build-linux` job (ubuntu-22.04, Externals from
  sources.json, Qt 6.8.3 `linux_gcc_64` by aqtinstall, cached), the test DOL,
  nogui start-up, artifact `PixelU-Wii-linux-x64`. (L5d)
- **supermodel.yml:** `linux-check` becomes `linux`, which also uploads
  `Supermodel-linux-x64`. (L6)
- **mame-linux.yml** (new): builds MAME 0.xxx from source when its cache
  for that version is empty (`timeout-minutes: 340`, ccache), uploads
  `MAME-linux-x64`. (L7)
- **bundle-linux.yml** (new, ubuntu-22.04): on push to the branch and when
  an engine's Linux build finishes (they start it, as they start
  bundle-test.yml today): tests, builds, engines, glibc check, package,
  smoke test, artifact `RetroPixels-<ver>-linux-testN` with the `.tar.gz`
  and VERSION.txt, `retention-days: 14`. bundle-test.yml (Windows) stays as
  it is. (L9)

## Decisions for the project owner

1. **Package format.** Default: tar.gz of a portable folder (inside the
   workflow artifact zip). AppImage later only if wanted; it would have to
   keep the engines' data outside the image.
2. **Wayland.** Default: run the app window and the engines under XWayland
   on Wayland desktops, so docking works everywhere (Wayland has no way to
   put one program's window inside another's). Alternative: native Wayland
   with engines in their own windows (no docking there).
3. **RetroPixels Native's toolkit.** Default: GTK 3 through our own small
   cgo bindings (menus, dialogs, drag and drop come with it; GTK is on
   every desktop). Alternatives: pure Go on internal/x11 with self-drawn
   menus (no cgo, much more code), or SDL2. Or: no Native on Linux at first
   (the web app has every game); default is to port it, last.
4. **MAME on Linux.** Default: build the same official MAME release from
   source, `SUBTARGET=arcade` (all arcade drivers, which is what RetroPixels
   uses MAME for; a much faster build). Alternative: full MAME, or rely on
   the distribution's `mame` package.
5. **PixelU-Wii Linux code.** Default: new original code for every pruned
   piece (the rewrite's goal). Alternative: restore Dolphin's files (faster,
   but Dolphin code again).
6. **glibc floor.** Default 2.35 (ubuntu-22.04 builders). 2.31 would need
   an older container image with a newer compiler.
7. **Wii U in the page on Linux** (the Go emulator): software rendering
   until the optional L12 Vulkan back end. Default: L12 later, after the
   download is complete.
