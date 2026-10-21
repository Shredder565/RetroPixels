# Exactly what RetroPixels' Supermodel is built from (read by build.sh).
# Every download is checked against its SHA-256 before it is used.

# Supermodel itself: the upstream commit the patches in patches/ apply to.
SUPERMODEL_REPO=https://github.com/trzy/Supermodel.git
SUPERMODEL_COMMIT=24d2ffcfc7f14229337f05f4920fe26b56633d9d # 2026-07-26 "Fix initializer order (#325)", the 0.3a-20260726-git-24d2ffc build

# Windows build only (cross-compiled with MinGW-w64 on Ubuntu 24.04,
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
