# Exactly what RetroPixels' Supermodel is built from (read by build.sh).
# Every download is checked against its SHA-256 before it is used.

# Supermodel itself: the upstream commit the patches in patches/ apply to.
SUPERMODEL_REPO=https://github.com/trzy/Supermodel.git
SUPERMODEL_COMMIT=24d2ffcfc7f14229337f05f4920fe26b56633d9d # 2026-07-26 "Fix initializer order (#325)", the 0.3a-20260726-git-24d2ffc build

# Windows build (cross-compiled with MinGW-w64 on Ubuntu 24.04,
# gcc-mingw-w64 13.2.0-6ubuntu1+26.1, posix threads):
SDL2_VERSION=2.30.12
SDL2_URL=https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-devel-2.30.12-mingw.tar.gz
SDL2_SHA256=dddafbf0705a8cbe1f97c69680886ce147ed4d38f1d04e03523681298e081b0f

SDL2_NET_VERSION=2.2.0
SDL2_NET_URL=https://github.com/libsdl-org/SDL_net/releases/download/release-2.2.0/SDL2_net-devel-2.2.0-mingw.tar.gz
SDL2_NET_SHA256=369829e06c509d5e001fabdcbe006ff3efa934f3825dc0ae1b076f5ce9c183c4

ZLIB_VERSION=1.3.1
ZLIB_URL=https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

# Linux build only (built on Ubuntu 22.04, so it runs on glibc 2.35 and
# later): the same SDL2 and SDL2_net releases from their source tarballs,
# built as shared libraries that go in Supermodel/lib next to the program.
SDL2_SRC_URL=https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-2.30.12.tar.gz
SDL2_SRC_SHA256=ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea

SDL2_NET_SRC_URL=https://github.com/libsdl-org/SDL_net/releases/download/release-2.2.0/SDL2_net-2.2.0.tar.gz
SDL2_NET_SRC_SHA256=4e4a891988316271974ff4e9585ed1ef729a123d22c08bd473129179dc857feb

# RetroPixels for Android (build.sh android): the same SDL2 source as the
# Linux build (SDL2_SRC_*, built for each ABI with SDL's own CMake files),
# zlib from the NDK, and this NDK unless ANDROID_NDK_HOME / ANDROID_NDK names
# one already installed (CI installs this one).
ANDROID_NDK_VERSION=r27c
ANDROID_NDK_REVISION=27.2.12479018
ANDROID_NDK_URL=https://dl.google.com/android/repository/android-ndk-r27c-linux.zip
ANDROID_NDK_SHA256=59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc
ANDROID_MIN_SDK=24
ANDROID_ABIS="arm64-v8a armeabi-v7a x86_64 x86"
