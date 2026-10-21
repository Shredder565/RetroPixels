# Supermodel in RetroPixels (Star Wars Trilogy Arcade)

RetroPixels' Supermodel tab plays one game: Star Wars Trilogy Arcade (Sega
and LucasArts, 1998, Sega Model 3 Step 2.1). It runs on Supermodel, the
Sega Model 3 emulator (https://github.com/trzy/Supermodel, GPL-3.0-or-later).
RetroPixels builds it from upstream commit 24d2ffc, the commit of the
official "supermodel-0.3a-20260726-git-24d2ffc" Windows build, plus the
patches in `supermodel/patches/`.

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

## What RetroPixels changes (supermodel/patches/0001-retropixels.patch)

- **Player folder.** `SUPERMODEL_HOME` (RetroPixels sets it to
  `%APPDATA%\RetroPixels\Supermodel`) holds Config, NVRAM, Saves, Screenshots
  and Log. Games.xml and Music.xml still come from the exe's folder, so the
  install folder stays read-only. Non-ASCII paths fall back to their short
  8.3 names on Windows.
- **Recording.** `-rp-record=DIR -rp-ffmpeg=PATH` sends each frame to ffmpeg
  (glReadPixels of the back buffer) and writes the sound as a WAV.
  RetroPixels' recorder then makes the usual .mp4 with the black bars
  cropped.
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
- **Keyboard (as on the MAME tab).** 5 coin, 1 start, arrow keys, Ctrl/A fire,
  Alt/S event, F2 test menu, 9 service, Esc leaves.
- **Sound.** MusicVolume 70 for both sets.
- **Command line.** `-input-system=sdlgamepad`, `-force-feedback` when the
  drive board ROM is there, and a wide-screen option.
  - Docked (the default, needs the patched build): `-window -borderless`,
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
