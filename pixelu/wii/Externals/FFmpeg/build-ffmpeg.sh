#!/bin/bash
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# Builds the FFmpeg libraries PixelU-Wii links for frame dumps, from FFmpeg's
# own source (Externals/sources.json, entry "ffmpeg"; fetch it with
# `python Externals/fetch.py --group ffmpeg`).
#
#   build-ffmpeg.sh <ffmpeg source folder> <install folder>
#
# On Windows this runs in an MSYS2 shell that has make, nasm and the MSVC
# x64 environment (cl, link, lib) on PATH; the workflow sets that up. The
# result in <install folder> is what PIXELU_WII_FFMPEG_DIR points at:
#   include/lib<name>/*.h, lib/<name>.lib (static, linked into PixelU-Wii.exe:
#   no FFmpeg DLL is shipped), FFmpeg's licence files and BUILD-INFO.txt.
#
# What is built: avcodec, avformat, avutil, swresample and swscale, with only
# what frame dumping needs: the AVI muxer, the FFV1, MPEG-4 part 2 and Ut Video
# encoders and the file protocol. GPL and version 3 parts are allowed, so the
# libraries are GPL-3.0-or-later, which PixelU-Wii (GPL-2.0-or-later) allows.
#
# PIXELU_FFMPEG_HOST_TEST=1 builds the same configuration with the host's own
# compiler instead of MSVC (for checking the options off Windows).

set -euo pipefail

if [ $# -ne 2 ]; then
  echo "usage: $0 <ffmpeg source folder> <install folder>" >&2
  exit 2
fi
src=$(cd "$1" && pwd)
mkdir -p "$2"
prefix=$(cd "$2" && pwd)
if [ ! -f "$src/libavcodec/avcodec.c" ]; then
  echo "$src is not an FFmpeg source folder" >&2
  exit 1
fi

if [ "${PIXELU_FFMPEG_HOST_TEST:-}" = 1 ]; then
  # This FFmpeg's GCC inline assembly no longer assembles with current binutils
  # (MSVC builds have no inline assembly anyway).
  platform=(--disable-inline-asm --disable-autodetect)
  command -v nasm > /dev/null || platform+=(--disable-x86asm)
  threads=()  # w32threads only exists on Windows; the host's default threads
else
  # MSYS2's coreutils has its own link.exe; MSVC's must come first.
  cl_dir=$(dirname "$(command -v cl)")
  export PATH="$cl_dir:$PATH"
  # /MD: the DLL C runtime, as PixelU-Wii uses; /GS-: as PixelU-Wii's release build.
  platform=(--toolchain=msvc --target-os=win64 --arch=x86_64 --extra-cflags="-MD -GS-")
  threads=(--disable-pthreads --enable-w32threads)
  # Autodetection stays on, so Windows' own parts are found as they should be
  # (bcrypt for random seeds, the D3D11VA and DXVA2 hardware contexts), but
  # nothing a runner may happen to have installed is picked up.
  platform+=(--disable-zlib --disable-bzlib --disable-lzma --disable-iconv --disable-sdl2
             --disable-vulkan --disable-cuda-llvm --disable-ffnvcodec --disable-amf
             --disable-mediafoundation --disable-schannel)
fi

options=(
  --enable-gpl --enable-version3
  --enable-static --disable-shared
  --disable-programs --disable-doc
  --disable-avdevice --disable-avfilter --disable-postproc
  --disable-network
  "${threads[@]}"
  --disable-everything
  --disable-encoders --disable-muxers --disable-hwaccels --disable-parsers --disable-protocols
  --enable-muxer=avi
  --enable-encoder=ffv1 --enable-encoder=mpeg4 --enable-encoder=utvideo
  --enable-protocol=file
)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
rm -rf "$prefix"/include "$prefix"/lib
cd "$build"
echo "configure ${platform[*]} ${options[*]}"
"$src/configure" --prefix="$prefix" "${platform[@]}" "${options[@]}"
make -j"$(nproc 2> /dev/null || echo 4)"
make install

rm -rf "$prefix/share" "$prefix/lib/pkgconfig"
if [ "${PIXELU_FFMPEG_HOST_TEST:-}" != 1 ]; then
  # FFmpeg names its MSVC static libraries libNAME.a; CMake looks for NAME.lib.
  for a in "$prefix"/lib/lib*.a; do
    base=$(basename "$a" .a)
    mv "$a" "$prefix/lib/${base#lib}.lib"
  done
fi
for f in LICENSE.md COPYING.GPLv2 COPYING.GPLv3 COPYING.LGPLv2.1 COPYING.LGPLv3; do
  cp "$src/$f" "$prefix/$f"
done
{
  echo "FFmpeg $(grep -o '"[^"]*"' "$prefix/include/libavutil/ffversion.h" | tr -d '"') built for PixelU-Wii"
  echo "configure ${platform[*]} ${options[*]}"
} > "$prefix/BUILD-INFO.txt"
ls -l "$prefix/lib"
