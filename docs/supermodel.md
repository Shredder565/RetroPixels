# Supermodel in RetroPixels (Star Wars Trilogy Arcade)

RetroPixels' Supermodel tab plays one game: Star Wars Trilogy Arcade (Sega
and LucasArts, 1998, Sega Model 3 Step 2.1). It runs on Supermodel, the
Sega Model 3 emulator (https://github.com/trzy/Supermodel, GPL-3.0-or-later).
RetroPixels builds it from upstream commit 24d2ffc, the commit of the
official "supermodel-0.3a-20260726-git-24d2ffc" Windows build, with the
emulator code from `supermodel/Src/` (upstream's with RetroPixels' changes,
being rewritten as original code: `docs/supermodel-original.md`).

## How Supermodel 0.3a-24d2ffc runs the game

- **ROM sets.** `Config/Games.xml` describes the sets. Supermodel finds each
  file in the zip by CRC-32, not by its name. Two export versions are
  playable:
  - `swtrilgy` (Revision A) is the parent set: 4 CROM, 12 banked CROM,
    16 VROM, the sound program and samples, and the MPEG (DSB) program and
    music.
  - `swtrilgya` (the first export version) is a clone that replaces only the
    4 CROM files (epr-21379..21382).

  A split `swtrilgya.zip` needs `swtrilgy.zip` in the same folder. A merged
  one plays alone. `internal/supermodel/roms.go` holds the CRC tables, so
  RetroPixels can tell in advance whether a zip will play and, if not, which
  files are missing.
- **Drive board.** `epr-21119.ic8` (CRC 65082B14) is the program of the
  force-feedback joystick's Z80. It is optional:
  - Force feedback off (the default): Supermodel disables the drive board,
    its reads return FF, and the game stops at "WAIT SETUP THE FEEDBACK
    STICK". Supermodel's README says to set the stick to U/R in the test
    menu.
  - `-force-feedback` with the ROM present: the real Z80 program runs and
    answers the game. RetroPixels turns it on only when the set has the ROM.
- **Known issues (upstream README).**
  - Pressing Start before the 3D Darth Vader attract scene can crash the
    emulated PowerPC.
  - Credits are kept in NVRAM. After you leave with credits, the next start
    goes straight to stage select. Alt+N clears NVRAM.
  - The music board is louder than the sound effects.
- **Inputs.** SWT uses the analog joystick group: AnalogJoyX/Y, Trigger,
  Event, and Trigger2/Event2 for the second set.
  - Upstream's sdlgamepad defaults put Start and Coin on JOY1_BUTTON9/10.
    With SDL's game controller mapping those are the stick clicks, which is
    wrong for an Xbox pad.
  - The official Windows build ships no Supermodel.ini, so its defaults are
    what a player gets.
- **Files.** The official build reads and writes Config, NVRAM, Saves, Log
  and so on relative to the current folder. So it has to be started from its
  own folder, and the player's files end up next to the exe.
- **Video and sound.** It renders with OpenGL 4.1 (legacy or new 3D engine)
  into a top-level SDL window, paced at RefreshRate (60 Hz by default; the
  real Model 3 is 57.524 Hz), with 44.1 kHz stereo audio. It is a console
  program: started directly, it opens a console window next to the game
  window.

## What RetroPixels changes (in supermodel/Src)

- **Player folder.** `SUPERMODEL_HOME` (RetroPixels sets it to
  `%APPDATA%\RetroPixels\Supermodel`) holds Config, NVRAM, Saves, Screenshots
  and Log. Games.xml and Music.xml still come from the exe's folder, so the
  install folder stays read-only. Non-ASCII paths fall back to their short
  8.3 names on Windows.
- **Recording.** `-rp-record=DIR -rp-ffmpeg=PATH` sends each frame to ffmpeg
  (glReadPixels of the back buffer) and writes the sound as a WAV.
  RetroPixels' recorder then makes the usual .mp4 with the black bars
  cropped.
- **Fast boot.** `-rp-fast-boot=N` runs the game's first N seconds (58
  frames each) unthrottled, with nothing drawn or swapped, the sound muted
  and the recording paused. RetroPixels passes 30 (`FastBootSeconds`), which
  covers Star Wars' "WAIT SETUP THE FEEDBACK LEVER" calibration and the Sega
  logo, so the game appears on its attract mode, past the point where
  coins can crash it. Pause ends it early.
- **Resolution.** `-desktop-res` makes fullscreen use the desktop's
  resolution. A borderless window that is resized (when it's docked in
  RetroPixels) re-creates the renderers at the new size, so the picture is
  never stretched from a small framebuffer.
- **Credits.** Docs/RetroPixels.txt is the GPL modification notice, and the
  log shows the `retropixels-1` patch level.

## RetroPixels' defaults for the game

These live in `internal/supermodel/ini.go` and `GameArgs`.

- **Settings file.** RetroPixels writes `Supermodel.ini` before each game
  while its first line is RetroPixels' marker. Remove the line to keep your
  own settings.
- **Xbox controller.**
  - Left stick: the yoke. "Invert stick Y" is an option.
  - RT or A: fire. RB or B: event button.
  - Start: start. Back: coin. Back + Start: leave.
- **Mouse, at the same time as the controller.** The pointer's place in the
  game window is the yoke's (centre = centre; not flipped by "Invert stick
  Y"). Left button fire, right button event, middle button second trigger.
  Whichever of the mouse and the stick moved last steers (a change in
  `Src/Inputs/MultiInputSource.cpp`, used for "A,B" mappings that include a
  mouse pointer axis).
- **Keyboard (as on the MAME tab).** 5 coin, 1 start, arrow keys, Ctrl/A fire,
  Alt/S event, F2 test menu, 9 service, Esc leaves.
- **Sound.** MusicVolume 70 for both sets.
- **Command line.** `-input-system=xinput` on Windows (Xbox controllers read
  directly, with rumble, focused or not) and `sdlgamepad` elsewhere, `-force-feedback` when the
  drive board ROM is there, and a wide-screen option.
  - Fullscreen in its own window is the default (owner, 2026-10-21).
  - Docked ("Play inside RetroPixels", needs the patched build): `-window -borderless`,
    placed off-screen until RetroPixels docks it as an owned overlay window,
    like PixelU, because an OpenGL child window of the WebView doesn't show.
  - Not docked: `-fullscreen -desktop-res`, or `-window -res=992,768`.
- **Start-up.** It runs from its own folder with no console window
  (CREATE_NO_WINDOW). Its output is captured, so when it fails in the first
  20 s the tab shows why (missing parent set, missing files, no OpenGL 4.1,
  no sound device).
- **One game at a time,** started through `startProcess` like the other
  emulators. Stop game closes it.
- **Finding the ROM.** The first time the tab opens, RetroPixels looks for
  the game once in the background. It searches the MAME ROM folders, the
  bundled ROM folders, the Supermodel and RetroPixels folders, the PixelU and
  Dolphin folders, and Downloads, Desktop, Documents and ROMs, 3 levels deep,
  and prefers a copy that plays. It remembers the result (`SupermodelROM` in
  the config). "Choose the game file…" picks it by hand. One click on the
  tile then starts the game.

## Building

`supermodel/build.sh` builds Supermodel:

- `sh supermodel/build.sh windows OUT` cross-builds `supermodel.exe` with
  MinGW-w64 (posix threads) on Ubuntu 24.04, against SDL2 2.30.12, SDL2_net
  2.2.0 and zlib 1.3.1. All three are pinned by SHA-256 in
  `supermodel/pins.sh`, and the exe links statically.
- `sh supermodel/build.sh linux OUT` is the Linux build CI starts headless.

- `sh supermodel/build.sh android OUT` builds RetroPixels for Android's
  copy (`supermodel/android/CMakeLists.txt`, once per ABI with the NDK in
  `ANDROID_NDK_HOME`, else the pinned r27c): `libsupermodel.so` and the
  pinned SDL2 as `libSDL2.so`, Games.xml / Music.xml / Assets, and SDL's
  Java code. CI's android job builds it into
  `android/app/src/main/supermodel`, where the app's Gradle build (and
  `android/tools/build-apk.sh`) pick it up; without it the APK builds
  without the game and its screen says so.
- `sh supermodel/build.sh gles OUT` builds the same OpenGL ES code as a
  Linux program (`supermodel-gles`, system SDL2 and Mesa's GLES) to test the
  renderer on a PC.

The Android build differs from the desktop one only behind build options:
`SUPERMODEL_GLES` (an OpenGL ES 3.2 context; `Src/Graphics/GLES/GL/glew.h`
stands in for GLEW, shaders become `#version 320 es`, line-of-sight depth and
stencil are read through a small draw, the crosshair uploads RGBA; only the
new 3D engine, without quad rendering), `SUPERMODEL_NO_NET` (no network
board or outputs: `Network/NoNetBoard.h`), `SUPERMODEL_NO_GUI` (no settings
window) and `__ANDROID__` (`Src/OSD/Android`: SDL's hints, the touch
controls as a virtual game controller, the console log in `Log/`, and the
recorder, which hands each frame's picture, read back without waiting, and
its sound to the app's GameRecorder). The app (`StarWarsActivity`,
`SupermodelActivity` in `android/app/src/supermodel`) starts it in a process
of its own with `-fullscreen -desktop-res -no-wide-screen
-input-system=sdlgamepad -rp-fast-boot=30` (and `-force-feedback` when the
set has the drive board program), and writes a Supermodel.ini that maps any
controller (`JOY_`), the touch controls being one.

`.github/workflows/supermodel.yml` runs both builds and checks the exe:

- It is a CUI program and imports only DLLs that come with Windows.
- It contains RetroPixels' changes.
- The package includes the GPL text and `Supermodel-source.tar.gz` (the
  exact source built).

`bundle-test.yml` calls it and adds the package as `Supermodel\` next to
`MAME\`, with a line in VERSION.txt.

No ROM, BIOS or key is in the repository or the download.

## Still to confirm (CI or Windows)

- **CI:** `supermodel.yml` passes (Windows job and PE checks, Linux headless
  check), and the bundle has `Supermodel\supermodel.exe`.
- **On Windows with the ROM:**
  - The tile starts the game, docked or fullscreen, with no console window.
  - The Xbox mapping works.
  - NVRAM lands in `%APPDATA%\RetroPixels\Supermodel\NVRAM`.
  - With the drive board ROM and `-force-feedback`, the game goes past the
    feedback-stick screen.
  - Resizing while docked keeps the picture sharp.
  - Recording makes an .mp4.
- **Android:** CI's android job builds `libsupermodel.so` with the NDK
  (not built where this was written: no NDK there; the same sources were
  built as `supermodel-gles` and every shader program compiled and linked
  under Mesa's OpenGL ES 3.2). Still to try on a phone with the ROM: the
  game starts and runs at full speed, touch and controller aim, the
  recording, and leaving with Back.
