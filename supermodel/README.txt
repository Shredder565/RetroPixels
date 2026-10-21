Supermodel for RetroPixels
==========================

Supermodel is the Sega Model 3 arcade emulator by Bart Trzynadlowski, Nik
Henson, Ian Curtis and the Supermodel team (https://github.com/trzy/Supermodel,
http://www.supermodel3.com). RetroPixels uses it for its Supermodel tab, which
plays Star Wars Trilogy Arcade.

This folder holds RetroPixels' changes to Supermodel, not a copy of it:

  pins.sh       the upstream commit (24d2ffc, 2026-07-26, the same code as the
                official "supermodel-0.3a-20260726-git-24d2ffc" Windows build)
                and the exact SDL2, SDL2_net and zlib releases, each with its
                SHA-256: the MinGW builds for Windows, the source tarballs of
                the same SDL2 and SDL2_net releases for Linux
  patches/      RetroPixels' changes, applied in order with "git apply"
  build.sh      fetches the pinned commit, applies the patches and builds:
                  sh supermodel/build.sh check            patches apply?
                  sh supermodel/build.sh linux   out      Linux build (SDL2 and
                                                         SDL2_net built from source)
                  sh supermodel/build.sh windows out      Windows build
                                                         (MinGW-w64 cross build)
                The result is out/Supermodel: supermodel(.exe), Config/Games.xml,
                Config/Music.xml, Assets/, LICENSE.txt, README.txt,
                RetroPixels.txt (what was changed), VERSION.txt and
                Supermodel-source.tar.gz (the exact source it was built from).
                The Linux build also has lib/: libSDL2-2.0.so.0,
                libSDL2_net-2.0.so.0 and Mesa's libGLU.so.1 with their
                licences in lib/LICENSES, found through the program's RUNPATH
                ($ORIGIN/lib), so nothing needs to be installed.
  COPYING       the GNU General Public License, version 3

.github/workflows/supermodel.yml builds it: Windows (artifact
Supermodel-windows-x64) and Linux on Ubuntu 22.04, so it runs on glibc 2.35 and
later (artifact Supermodel-linux-x64: Supermodel-linux.tar.gz, started headless
under Xvfb as a check). .github/workflows/bundle-test.yml puts the Windows build
in the RetroPixels test download as Supermodel\supermodel.exe, and
.github/workflows/bundle-linux.yml the Linux one in the Linux download as
Supermodel/supermodel.

Licence
-------
Supermodel is free software under the GNU General Public License, version 3
or (at your option) any later version (COPYING). The patches are changes to
Supermodel and are under the same licence; the result is a modified version
of Supermodel, marked as such in Docs/RetroPixels.txt, in its log and help
text, and in every changed place in the source ("RetroPixels"). The download
includes the complete corresponding source (Supermodel-source.tar.gz).
No ROMs, BIOS files or other game data are part of this folder, the build or
the download.

What the patches change (0001-retropixels.patch)
-------------------------------------------------
- SUPERMODEL_HOME: when this environment variable names a folder, the
  player's own files go there: Config/Supermodel.ini, NVRAM/, Saves/ (save
  states), Log/Supermodel.log and Screenshots/. RetroPixels sets it to
  %APPDATA%\RetroPixels\Supermodel, so nothing is written next to the program.
  Games.xml, Music.xml and Assets/ are still read from the program's folder.
  Folder names outside the Windows code page are opened by their short name.
- The game window follows its size when another program resizes it: once the
  size has been the same for 3 frames the renderers are set up again at that
  size, as Supermodel does when switching to full screen. RetroPixels docks
  the game into its own window and sizes it to fit, so the picture always
  fills it (4:3, or wider with -wide-screen) instead of staying 496x384 in a
  corner.
- -desktop-res: full screen at the desktop's own resolution, instead of
  changing the display mode to -res (496x384 by default).
- -rp-record=<dir> -rp-ffmpeg=<ffmpeg.exe>: records the game for RetroPixels'
  Recordings: the picture, read from the window each frame and encoded by
  ffmpeg as it plays (x264, <dir>\Frames\supermodel_framedump<N>.mkv, a new
  file when the size changes) and the sound (<dir>\Audio\supermodel_dspdump.wav,
  44.1 kHz stereo, its header kept valid as it grows). One frame of video per
  emulated frame at 60 fps, against the 735 samples of sound Supermodel makes
  per frame, so picture and sound stay in step. Nothing is recorded while
  the game is paused. RetroPixels turns the files into one .mp4 when the
  game ends, as it does for its other emulators.
- The help text and the log say which RetroPixels change level this is.

Settings RetroPixels gives Supermodel (internal/supermodel, not a patch):
Supermodel.ini with Xbox controller and keyboard controls for Star Wars
Trilogy (SDL game controller input, so every pad has the same layout), the
music a little quieter than the sound effects, and on the command line the
window or full screen mode, the wide screen choice and force feedback when
the ROM set has the force feedback board's program. See docs/supermodel.md.
