# What the Linux MAME is built with, besides MAME's own source (read by
# build-linux.sh). MAME itself is the MAME team's official release that the
# Windows download has (bundlemame -resolve picks it, the same way), from
# GitHub's archive of its tag; its SHA-256 goes into RETROPIXELS-BUILD.txt.
#
# Each download below is checked against its SHA-256 before it's used. An
# empty pin is checked against the SHA-256 GitHub lists for that release file
# instead (and the build prints the value to pin here).

# Build machine: GitHub's ubuntu-22.04 runner (glibc 2.35, GCC 11.4), so the
# result runs on glibc 2.35 and later. MAME needs GCC 11 or later.
GLIBC_MAX=${GLIBC_MAX:-2.35} # (a local check on a newer system can raise it)

# Which machines: all of MAME, as in the MAME team's Windows release. (MAME
# no longer has an "arcade" subtarget: it went with the source tree's
# reorganisation into manufacturer folders, and src/mame has no arcade.flt
# any more.)
MAME_SUBTARGET=mame

# SDL2 and SDL2_ttf, MAME's Linux window, input, sound and UI font library,
# built from their source tarballs as shared libraries and bundled in the
# MAME folder's lib/ (MAME finds them by RPATH=$ORIGIN/lib). SDL loads X11,
# Wayland, PulseAudio, PipeWire and ALSA itself when it starts, so the
# bundled copy runs on any desktop. The SDL2 release RetroPixels' engines
# were tested with.
SDL2_VERSION=2.30.12
SDL2_URL=https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-2.30.12.tar.gz
SDL2_SHA256=ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea
SDL2_RELEASE="libsdl-org/SDL release-2.30.12"

SDL2_TTF_VERSION=2.24.0
SDL2_TTF_URL=https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz
# (GitHub lists no SHA-256 for this file: this is what its download had on
# the MAME for Linux workflow, and the one Homebrew pins for it.)
SDL2_TTF_SHA256=0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd
SDL2_TTF_RELEASE="libsdl-org/SDL_ttf release-2.24.0"
