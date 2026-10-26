RetroPixels' Model 3 engine (Supermodel for RetroPixels)
========================================================

The engine behind RetroPixels' Supermodel tab, which plays Star Wars Trilogy
Arcade (Sega Model 3). It began as a copy of Supermodel, the Sega Model 3
emulator by the Supermodel team (https://github.com/trzy/Supermodel), and
has been rewritten as RetroPixels' own code (docs/supermodel-original.md).
Nothing from upstream is fetched, built or shipped any more.

  Src/          the engine's code, RetroPixels' own except Src/Pkgs (GLEW,
                Khronos headers, Dear ImGui, minizip, minimp3, TinyXML-2:
                permissive, see THIRD-PARTY-NOTICES.txt). The 68000 (sound
                board, DSB, net board) is Src/CPU/68K/M68000.cpp, written
                from RetroPixels' Go 68000 (cores/m68k).
  Makefile      the desktop build: Linux (default) or TARGET=windows with
                MinGW-w64 (see its header)
  android/      the Android build (CMake, run by build.sh android) and the
                OpenGL ES test program for Linux (build.sh gles)
  Config/       Games.xml: the two Star Wars Trilogy Arcade ROM sets,
                swtrilgy and swtrilgya
  Assets/       the crosshair pictures (tools/supermodel-crosshairs.py)
  tests/m68k/   tests of the 68000 (with cores/m68k's Go tests)
  pins.sh       the exact SDL2, SDL2_net, zlib and Android NDK releases,
                each with its SHA-256
  build.sh      builds and packages:
                  sh supermodel/build.sh linux   out      Linux build (SDL2 and
                                                         SDL2_net built from source)
                  sh supermodel/build.sh windows out      Windows build
                                                         (MinGW-w64 cross build)
                  sh supermodel/build.sh android out      Android libraries
                  sh supermodel/build.sh gles    out      OpenGL ES test build
                  sh supermodel/build.sh test             68000 wrapper test
                The result is out/Supermodel: supermodel(.exe),
                Config/Games.xml, Assets/, LICENSE.txt,
                THIRD-PARTY-NOTICES.txt and VERSION.txt. The Linux build
                also has lib/: libSDL2-2.0.so.0, libSDL2_net-2.0.so.0 and
                Mesa's libGLU.so.1 with their licences in lib/LICENSES,
                found through the program's RUNPATH ($ORIGIN/lib), so
                nothing needs to be installed.

Testing the 68000:
  go test ./cores/m68k -run Supermodel
compiles the C++ core and runs every opcode with random operands, and 400
random programs with exceptions and interrupts, in both the C++ and the Go
68000, comparing registers, flags, cycles and every bus access; it also
runs tests/m68k/wrapper.cpp (contexts, interrupts, STOP, save states).

.github/workflows/supermodel.yml builds it: Windows (artifact
Supermodel-windows-x64) and Linux on Ubuntu 22.04, so it runs on glibc 2.35 and
later (artifact Supermodel-linux-x64: Supermodel-linux.tar.gz, started headless
under Xvfb as a check). .github/workflows/bundle-test.yml puts the Windows build
in the RetroPixels test download as Supermodel\supermodel.exe, and
.github/workflows/bundle-linux.yml the Linux one in the Linux download as
Supermodel/supermodel.

Licence
-------
Copyright (c) 2025-2026 Shredder565. All rights reserved: RetroPixels'
LICENSE.txt. The third-party libraries in it keep their own (permissive)
licences, listed in THIRD-PARTY-NOTICES.txt, section 4. Builds published
before the engine became entirely RetroPixels' own were distributed under
the GNU GPL with their source and stay available under it to those who
received them. No ROMs, BIOS files or other game data are part of this
folder, the build or the download.

What RetroPixels adds
---------------------
Flight stick (Src/Inputs/InputSystem.cpp): RP_FLIGHTSTICK ("curve=N,dead=N,smooth=N", curve
and dead in percent and smooth in milliseconds, or "off"; RetroPixels sets
it) makes a gamepad's thumbstick aim like a flight stick: the two axes of a
stick are read together, with a round dead zone, the round gate stretched
to a square one (so the corners can be reached), a cubic response curve
(fine near the centre, full at the edge), and time-based smoothing like a
yoke's weight and spring (jitter doesn't jump the aim, and letting go
glides back to the centre). Only full-range stick axes are shaped.

Other features (Src/OSD and Src/Inputs):
- SUPERMODEL_HOME: when this environment variable names a folder, the
  player's own files go there: Config/Supermodel.ini, NVRAM/, Saves/ (save
  states), Log/Supermodel.log and Screenshots/. RetroPixels sets it to
  %APPDATA%\RetroPixels\Supermodel, so nothing is written next to the program.
  Games.xml and Assets/ are still read from the program's folder (an
  optional Config/Music.xml there maps MPEG tracks to audio files).
  Folder names outside the Windows code page are opened by their short name.
- The game window follows its size when another program resizes it: once the
  size has been the same for 3 frames the renderers are set up again at that
  size, as when switching to full screen. RetroPixels docks the game into
  its own window and sizes it to fit, so the picture always fills it (4:3,
  or wider with -wide-screen) instead of staying 496x384 in a corner.
- -desktop-res: full screen at the desktop's own resolution, instead of
  changing the display mode to -res (496x384 by default).
- -rp-record=<dir> -rp-ffmpeg=<ffmpeg.exe>: records the game for RetroPixels'
  Recordings: the picture, read from the window each frame and encoded by
  ffmpeg as it plays (x264, <dir>\Frames\supermodel_framedump<N>.mkv, a new
  file when the size changes) and the sound (<dir>\Audio\supermodel_dspdump.wav,
  44.1 kHz stereo, its header kept valid as it grows). One frame of video per
  emulated frame at 60 fps, against the 735 samples of sound the engine makes
  per frame, so picture and sound stay in step. Nothing is recorded while
  the game is paused. RetroPixels turns the files into one .mp4 when the
  game ends, as it does for its other emulators.
- -rp-fast-boot: runs the game's start-up unseen, so it opens ready for coins.
- The help text and the log say which RetroPixels change level this is.

Settings RetroPixels gives the engine (internal/supermodel):
Supermodel.ini with Xbox controller and keyboard controls for Star Wars
Trilogy (SDL game controller input, so every pad has the same layout), the
music a little quieter than the sound effects, and on the command line the
window or full screen mode, the wide screen choice and force feedback when
the ROM set has the force feedback board's program. See docs/supermodel.md.
