#!/bin/sh
# Builds RetroPixels' Supermodel (Sega Model 3 arcade emulator) from the
# pinned upstream commit plus the patches in patches/ (see README.txt).
#
#   sh supermodel/build.sh fetch            get the source and apply the patches
#   sh supermodel/build.sh linux   <outdir> build for Linux (needs SDL2, SDL2_net, GL/GLU and zlib dev packages)
#   sh supermodel/build.sh windows <outdir> cross-build supermodel.exe for 64-bit Windows with MinGW-w64
#   sh supermodel/build.sh check            only check the pins and that the patches apply
#
# <outdir>/Supermodel gets the program and the files it needs next to it
# (Config/Games.xml, Config/Music.xml, Assets/), its licence, what was
# changed, and the source it was built from (Supermodel-source.tar.gz).
# SM_WORK sets the working folder (default: supermodel/build, not committed).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
. "$here/pins.sh"
work=${SM_WORK:-$here/build}
src="$work/src"
jobs=$(nproc 2>/dev/null || echo 2)

log() { printf '== %s\n' "$*"; }

fetch() {
	mkdir -p "$work"
	if [ ! -d "$src/.git" ]; then
		rm -rf "$src"
		git init -q "$src"
		git -C "$src" remote add origin "$SUPERMODEL_REPO"
	fi
	if [ "$(git -C "$src" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$SUPERMODEL_COMMIT" ]; then
		log "fetching Supermodel $SUPERMODEL_COMMIT"
		git -C "$src" fetch -q --depth 1 origin "$SUPERMODEL_COMMIT"
		git -C "$src" -c advice.detachedHead=false checkout -q -f FETCH_HEAD
	fi
	got=$(git -C "$src" rev-parse HEAD)
	[ "$got" = "$SUPERMODEL_COMMIT" ] || { echo "got commit $got, wanted $SUPERMODEL_COMMIT" >&2; exit 1; }
	# Start from the clean upstream commit each time, then patch.
	git -C "$src" reset -q --hard
	git -C "$src" clean -q -fdx -e obj -e obj64 -e bin -e bin64
	for p in "$here"/patches/*.patch; do
		log "applying $(basename "$p")"
		git -C "$src" apply --whitespace=nowarn "$p"
	done
}

# download <url> <sha256> <file>
download() {
	if [ -f "$3" ] && echo "$2  $3" | sha256sum -c - >/dev/null 2>&1; then
		return
	fi
	log "downloading $1"
	curl -fsSL --retry 3 -o "$3.part" "$1"
	echo "$2  $3.part" | sha256sum -c - >/dev/null || { echo "SHA-256 mismatch for $1" >&2; rm -f "$3.part"; exit 1; }
	mv "$3.part" "$3"
}

# Pre-generates the 68K emulator's sources with the build machine's own C
# compiler (the Makefile would build its generator for Windows and then
# couldn't run it).
musashi() {
	obj="$src/$1"
	mkdir -p "$obj"
	cc -o "$obj/m68kmake.exe" "$src/Src/CPU/68K/Musashi/m68kmake.c"
	(cd "$src" && "$obj/m68kmake.exe" "$1" Src/CPU/68K/Musashi/m68k_in.c >/dev/null)
}

windows_deps() {
	deps="$work/deps"
	prefix="$deps/x86_64-w64-mingw32"
	stamp="$prefix/.pins-$SDL2_SHA256-$SDL2_NET_SHA256-$ZLIB_SHA256"
	[ -f "$stamp" ] && return
	mkdir -p "$deps/dl"
	download "$SDL2_URL" "$SDL2_SHA256" "$deps/dl/SDL2-mingw.tar.gz"
	download "$SDL2_NET_URL" "$SDL2_NET_SHA256" "$deps/dl/SDL2_net-mingw.tar.gz"
	download "$ZLIB_URL" "$ZLIB_SHA256" "$deps/dl/zlib.tar.gz"
	rm -rf "$prefix" "$deps/x"
	mkdir -p "$prefix" "$deps/x"
	tar -xzf "$deps/dl/SDL2-mingw.tar.gz" -C "$deps/x"
	tar -xzf "$deps/dl/SDL2_net-mingw.tar.gz" -C "$deps/x"
	tar -xzf "$deps/dl/zlib.tar.gz" -C "$deps/x"
	cp -R "$deps/x/SDL2-$SDL2_VERSION/x86_64-w64-mingw32/." "$prefix/"
	cp -R "$deps/x/SDL2_net-$SDL2_NET_VERSION/x86_64-w64-mingw32/." "$prefix/"
	# sdl2-config names the folder SDL was built in; point it here.
	sed -i "s|^prefix=.*|prefix=$prefix|" "$prefix/bin/sdl2-config"
	chmod +x "$prefix/bin/sdl2-config"
	log "building zlib $ZLIB_VERSION"
	make -s -C "$deps/x/zlib-$ZLIB_VERSION" -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a >/dev/null
	cp "$deps/x/zlib-$ZLIB_VERSION/zlib.h" "$deps/x/zlib-$ZLIB_VERSION/zconf.h" "$prefix/include/"
	cp "$deps/x/zlib-$ZLIB_VERSION/libz.a" "$prefix/lib/"
	# Windows headers named in another case than MinGW-w64's own (Windows
	# file names ignore case, Linux ones don't).
	echo '#include <xinput.h>' > "$prefix/include/XInput.h"
	# The Makefile calls plain "windres".
	mkdir -p "$deps/shim"
	printf '#!/bin/sh\nexec x86_64-w64-mingw32-windres "$@"\n' > "$deps/shim/windres"
	chmod +x "$deps/shim/windres"
	rm -rf "$deps/x"
	touch "$stamp"
}

# package <program> <outdir>
package() {
	# Absolute, because the source tarball is written from inside $src.
	mkdir -p "$2"
	out="$(cd "$2" && pwd)/Supermodel"
	rm -rf "$out"
	mkdir -p "$out/Config" "$out/Assets"
	cp "$1" "$out/"
	cp "$src/Config/Games.xml" "$src/Config/Music.xml" "$out/Config/"
	cp "$src/Assets/"*.bmp "$out/Assets/" 2>/dev/null || true
	cp "$src/Docs/LICENSE.txt" "$out/LICENSE.txt"
	cp "$src/Docs/README.txt" "$out/README.txt"
	cp "$src/Docs/RetroPixels.txt" "$out/RetroPixels.txt"
	version=$(cd "$src" && make -s -f Makefiles/Makefile.UNIX version 2>/dev/null || echo "0.3a-git-$(git rev-parse --short HEAD)")
	{
		echo "Supermodel $version, modified for RetroPixels (see RetroPixels.txt)"
		echo "Upstream: $SUPERMODEL_REPO commit $SUPERMODEL_COMMIT"
		echo "Licence: GNU General Public License v3 or later (LICENSE.txt)"
		echo "Source: Supermodel-source.tar.gz in this folder (upstream commit with RetroPixels' patches applied)"
		[ "${3:-}" = windows ] && echo "Built with MinGW-w64 ($(x86_64-w64-mingw32-gcc-posix -dumpversion)), SDL $SDL2_VERSION, SDL_net $SDL2_NET_VERSION, zlib $ZLIB_VERSION (statically linked)"
		echo "No games, ROMs or BIOS files are included."
	} > "$out/VERSION.txt"
	# The exact source of this build (GPL): upstream plus the patches, without
	# the documentation pictures.
	(cd "$src" && git ls-files --cached --others --exclude-standard | grep -v '^Docs/Images/' |
		tar -czf "$out/Supermodel-source.tar.gz" --transform 's|^|Supermodel/|' -T -)
	log "packaged into $out"
	ls -la "$out"
}

case "${1:-}" in
check)
	fetch
	log "patches apply cleanly to $SUPERMODEL_COMMIT"
	;;
fetch)
	fetch
	;;
linux)
	[ $# -ge 2 ] || { echo "usage: $0 linux <outdir>" >&2; exit 2; }
	fetch
	musashi obj
	log "building for Linux"
	make -C "$src" -s -f Makefiles/Makefile.UNIX -j"$jobs"
	package "$src/bin/supermodel" "$2" linux
	;;
windows)
	[ $# -ge 2 ] || { echo "usage: $0 windows <outdir>" >&2; exit 2; }
	command -v x86_64-w64-mingw32-g++-posix >/dev/null || { echo "needs MinGW-w64 (apt install g++-mingw-w64-x86-64-posix)" >&2; exit 1; }
	fetch
	windows_deps
	musashi obj64
	prefix="$work/deps/x86_64-w64-mingw32"
	log "building supermodel.exe for Windows"
	PATH="$prefix/bin:$work/deps/shim:$PATH" make -C "$src" -s -f Makefiles/Makefile.Win32 -j"$jobs" \
		CC="x86_64-w64-mingw32-gcc-posix -I$prefix/include" \
		CXX="x86_64-w64-mingw32-g++-posix -I$prefix/include" \
		LD="x86_64-w64-mingw32-g++-posix -L$prefix/lib"
	exe="$src/bin64/supermodel.exe"
	x86_64-w64-mingw32-objdump -p "$exe" | grep -i "DLL Name" | sort
	package "$exe" "$2" windows
	;;
*)
	sed -n '2,15p' "$0"
	exit 2
	;;
esac
