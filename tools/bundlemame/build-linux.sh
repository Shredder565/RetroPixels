#!/bin/sh
# Builds MAME for Linux from the MAME team's official source code of one
# release, unchanged, and lays it out as a folder that bundlemame -os linux
# puts into the RetroPixels download (MAME/MAME 0.xxx). Used by
# .github/workflows/mame-linux.yml on ubuntu-22.04; it runs the same way on
# any Linux machine with the packages that workflow installs.
#
#   sh tools/bundlemame/build-linux.sh -s mame-src.tar.gz -t mame0289 [options] STAGE
#
#   -s FILE    MAME's source (GitHub's .tar.gz of the release tag; bundlemame
#              -resolve -source saves it), or an unpacked source folder, used
#              in place (local checks)
#   -t TAG     the release tag (mame0289)
#   -S SHA     the source's SHA-256 (checked, and written to the build info)
#   -w DIR     work folder (default ./build-mame): sources, SDL, MAME's objects
#   -j N       parallel jobs (default: the number of CPUs)
#   -m FILES   build only these driver sources (MAME's SOURCES=, for a quick
#              check of the flags; the result isn't for the download)
#   -g         configure only: generate MAME's makefiles, build nothing of MAME
#   -y         use the system's SDL2 and SDL2_ttf instead of building the pinned
#              ones (local checks only; such a build isn't for the download)
#
# STAGE gets: mame, lib/ (SDL2, SDL2_ttf), MAME's data folders as in the
# MAME team's Windows release (artwork, bgfx, ctrlr, hash, ini, keymaps,
# language, plugins, roms, samples, docs/legal), COPYING, and
# RETROPIXELS-BUILD.txt (version=, tag=, source_sha256=, how it was built).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
. "$here/linux-pins.sh"

src_tgz= tag= src_sha= work=./build-mame jobs= sources= genie_only= system_sdl=
while getopts s:t:S:w:j:m:gy opt; do
	case $opt in
	s) src_tgz=$OPTARG ;;
	t) tag=$OPTARG ;;
	S) src_sha=$OPTARG ;;
	w) work=$OPTARG ;;
	j) jobs=$OPTARG ;;
	m) sources=$OPTARG ;;
	g) genie_only=1 ;;
	y) system_sdl=1 ;;
	*) sed -n '2,30p' "$0" >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] && [ -n "$src_tgz" ] && [ -n "$tag" ] || { sed -n '2,30p' "$0" >&2; exit 2; }
stage=$1
case "$tag" in mame0[0-9][0-9][0-9] | mame0[0-9][0-9]) ;; *) echo "bad tag $tag" >&2; exit 2 ;; esac
version=0.$(echo "$tag" | sed 's/^mame0*//')
[ -n "$jobs" ] || jobs=$(nproc)
mkdir -p "$work"
work=$(cd "$work" && pwd)
if [ -d "$src_tgz" ]; then
	src_tgz=$(cd "$src_tgz" && pwd)
else
	src_tgz=$(cd "$(dirname "$src_tgz")" && pwd)/$(basename "$src_tgz")
fi
log() { echo "== $*"; }

# ---- MAME's source ----
if [ -d "$src_tgz" ]; then
	src=$src_tgz
	src_sha=
	log "MAME $version ($tag), source folder $src (a local check)"
else
	got=$(sha256sum "$src_tgz" | cut -d' ' -f1)
	if [ -n "$src_sha" ] && [ "$got" != "$src_sha" ]; then
		echo "MAME's source is sha256 $got, expected $src_sha" >&2
		exit 1
	fi
	src_sha=$got
	log "MAME $version ($tag), source sha256 $src_sha"
	src=$work/mame
fi
if [ -n "$src_sha" ] && [ "$(cat "$src/.rp-source" 2>/dev/null || true)" != "$src_sha" ]; then
	log "unpacking MAME's source"
	rm -rf "$src"
	mkdir -p "$src"
	tar -xzf "$src_tgz" -C "$src" --strip-components=1
	echo "$src_sha" >"$src/.rp-source"
fi
grep -q "BARE_BUILD_VERSION \"$version\"" "$src/makefile" || {
	echo "the source isn't MAME $version (its makefile says $(sed -n 's/.*BARE_BUILD_VERSION "\([0-9.]*\)".*/\1/p' "$src/makefile" | head -n 1))" >&2
	exit 1
}

# ---- SDL2 and SDL2_ttf ----
# fetch URL SHA256 "OWNER/REPO TAG" FILE: download and check (an empty pin is
# checked against the SHA-256 GitHub lists for the release file).
fetch() {
	if [ -f "$5" ] && [ -n "$2" ] && echo "$2  $5" | sha256sum -c - >/dev/null 2>&1; then
		return
	fi
	log "downloading $1"
	curl -fsSL --retry 3 -o "$5.part" "$1"
	sum=$(sha256sum "$5.part" | cut -d' ' -f1)
	want=$2
	if [ -z "$want" ]; then
		repo=${3% *} rel=${3#* }
		want=$(gh api "repos/$repo/releases/tags/$rel" --jq ".assets[] | select(.name == \"$(basename "$1")\") | .digest" 2>/dev/null | sed 's/^sha256://' || true)
		[ -n "$want" ] || { echo "no SHA-256 pinned for $1 and GitHub lists none: pin $sum in linux-pins.sh after checking it" >&2; rm -f "$5.part"; exit 1; }
		echo "::warning::$(basename "$1") isn't pinned; it matches GitHub's SHA-256 $sum (pin it in tools/bundlemame/linux-pins.sh)"
	fi
	[ "$sum" = "$want" ] || { echo "SHA-256 mismatch for $1: $sum, expected $want" >&2; rm -f "$5.part"; exit 1; }
	mv "$5.part" "$5"
}

prefix=$work/sdl-prefix
sdl_note="SDL2 $SDL2_VERSION and SDL2_ttf $SDL2_TTF_VERSION, built from their release sources"
if [ -n "$system_sdl" ]; then
	sdl_note="the system's SDL2 $(pkg-config --modversion sdl2) and SDL2_ttf $(pkg-config --modversion SDL2_ttf) (a local check, not for the download)"
elif [ "$(cat "$prefix/.rp-sdl" 2>/dev/null || true)" != "$SDL2_VERSION $SDL2_TTF_VERSION" ]; then
	mkdir -p "$work/dl"
	fetch "$SDL2_URL" "$SDL2_SHA256" "$SDL2_RELEASE" "$work/dl/SDL2-$SDL2_VERSION.tar.gz"
	fetch "$SDL2_TTF_URL" "$SDL2_TTF_SHA256" "$SDL2_TTF_RELEASE" "$work/dl/SDL2_ttf-$SDL2_TTF_VERSION.tar.gz"
	rm -rf "$prefix" "$work/sdl-src"
	mkdir -p "$work/sdl-src"
	tar -xzf "$work/dl/SDL2-$SDL2_VERSION.tar.gz" -C "$work/sdl-src"
	tar -xzf "$work/dl/SDL2_ttf-$SDL2_TTF_VERSION.tar.gz" -C "$work/sdl-src"
	log "building SDL2 $SDL2_VERSION"
	# Every back end is loaded when SDL starts (the *_SHARED defaults), so
	# the library needs only libc and libm.
	cmake -S "$work/sdl-src/SDL2-$SDL2_VERSION" -B "$work/sdl-build" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
		-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF
	cmake --build "$work/sdl-build" -j "$jobs"
	cmake --install "$work/sdl-build"
	log "building SDL2_ttf $SDL2_TTF_VERSION"
	# The system's FreeType (on every desktop); no HarfBuzz (MAME doesn't use it).
	cmake -S "$work/sdl-src/SDL2_ttf-$SDL2_TTF_VERSION" -B "$work/ttf-build" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_PREFIX_PATH="$prefix" -DBUILD_SHARED_LIBS=ON -DSDL2TTF_VENDORED=OFF \
		-DSDL2TTF_HARFBUZZ=OFF -DSDL2TTF_SAMPLES=OFF
	cmake --build "$work/ttf-build" -j "$jobs"
	cmake --install "$work/ttf-build"
	rm -rf "$work/sdl-build" "$work/ttf-build" "$work/sdl-src"
	echo "$SDL2_VERSION $SDL2_TTF_VERSION" >"$prefix/.rp-sdl"
fi
if [ -z "$system_sdl" ]; then
	export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
	[ "$(pkg-config --modversion sdl2)" = "$SDL2_VERSION" ] || { echo "pkg-config doesn't find the built SDL2" >&2; exit 1; }
fi

# ---- MAME ----
subtarget=$MAME_SUBTARGET
[ -z "$sources" ] || subtarget=rpcheck
# The program is named after the subtarget: mame for all of MAME; a SOURCES=
# build names it after its subtarget alone (rpcheck).
if [ "$subtarget" = mame ] || [ -n "$sources" ]; then binary=$subtarget; else binary=mame$subtarget; fi
# - the same machines and options as the MAME team's release (OPTIMIZE=3,
#   no debug symbols); no Qt debugger (MAME's own debugger window needs Qt)
# - SDL_INI_PATH: mame.ini is looked for in MAME's own folder only, as on
#   Windows, not in ~/.mame, so a distribution's MAME settings there (with
#   their own plugin and hash folders) don't mix into this one
# - PulseAudio for sound (PipeWire systems have pipewire-pulse), so the
#   program doesn't need libpipewire; MIDI and PortAudio go through ALSA
# - PRECOMPILE=0: no precompiled header, so ccache can keep every object
#   between builds (a new MAME release then mostly comes from the cache)
# - the version reads "0.289 (mame0289)", as the MAME team's builds do
set -- SUBTARGET="$subtarget" TOOLS=0 USE_QTDEBUG=0 NO_USE_PIPEWIRE=1 NOWERROR=1 PRECOMPILE=0 \
	OPTIMIZE=3 SYMBOLS=0 PYTHON_EXECUTABLE=python3 IGNORE_GIT=1 NEW_GIT_VERSION="$tag" \
	SDL_INI_PATH='.;ini;ini/presets'
[ -z "$sources" ] || set -- "$@" SOURCES="$sources"
flags=$*
cd "$src"
if [ -n "$genie_only" ]; then
	log "configuring MAME: $flags"
	make "$@" REGENIE=1 "build/projects/sdl/$binary/gmake-linux/Makefile"
	test -f "build/projects/sdl/$binary/gmake-linux/Makefile"
	log "configured: build/projects/sdl/$binary/gmake-linux"
	exit 0
fi
log "building MAME ($jobs jobs): $flags"
make -j "$jobs" "$@"
test -x "$binary"

# ---- the folder ----
log "laying out $stage"
rm -rf "$stage"
mkdir -p "$stage/lib" "$stage/docs"
cp "$binary" "$stage/mame"
strip --strip-unneeded "$stage/mame"
if [ -z "$system_sdl" ]; then
	cp -L "$prefix/lib/libSDL2-2.0.so.0" "$prefix/lib/libSDL2_ttf-2.0.so.0" "$stage/lib/"
	strip --strip-unneeded "$stage/lib/"*.so.0
	tar -xzOf "$work/dl/SDL2-$SDL2_VERSION.tar.gz" "SDL2-$SDL2_VERSION/LICENSE.txt" >"$stage/lib/LICENSE-SDL2.txt"
	tar -xzOf "$work/dl/SDL2_ttf-$SDL2_TTF_VERSION.tar.gz" "SDL2_ttf-$SDL2_TTF_VERSION/LICENSE.txt" >"$stage/lib/LICENSE-SDL2_ttf.txt"
	patchelf --set-rpath '$ORIGIN' "$stage/lib/libSDL2_ttf-2.0.so.0"
fi
patchelf --set-rpath '$ORIGIN/lib' "$stage/mame"
for d in artwork bgfx ctrlr hash ini keymaps language plugins roms samples; do
	[ -d "$d" ] && cp -R "$d" "$stage/$d"
done
find "$stage/language" -name '*.po' -delete
cp -R docs/legal "$stage/docs/legal"
cp COPYING "$stage/COPYING"
[ -f uismall.bdf ] && cp uismall.bdf "$stage/"

# Each program and library may need glibc $GLIBC_MAX at most.
bins=$(find "$stage/mame" "$stage/lib" -maxdepth 1 -type f \( -name mame -o -name "*.so.0" \))
# shellcheck disable=SC2086 # (no spaces in these names)
need=$(objdump -T $bins | grep -o "GLIBC_[0-9.]*" | sed "s/GLIBC_//" | sort -Vu | tail -n 1)
log "needs glibc $need (at most $GLIBC_MAX)"
[ "$(printf '%s\n%s\n' "$need" "$GLIBC_MAX" | sort -V | tail -n 1)" = "$GLIBC_MAX" ] || {
	echo "the build needs glibc $need, newer than $GLIBC_MAX" >&2
	exit 1
}
log "libraries it needs:"
objdump -p $bins | grep NEEDED | sort -u

cat >"$stage/RETROPIXELS-BUILD.txt" <<EOF
version=$version
tag=$tag
source=https://github.com/mamedev/mame/archive/refs/tags/$tag.tar.gz
source_sha256=$src_sha
subtarget=$subtarget
make=$flags
compiler=$(${CC:-gcc} --version | head -n 1)
glibc_max=$need
sdl=$sdl_note
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
# MAME $version for Linux, built by RetroPixels from the MAME team's official
# source code of release $tag, unchanged, with tools/bundlemame/build-linux.sh
# (make $flags). lib/ holds SDL2 and SDL2_ttf (zlib licence, see their
# LICENSE files there). MAME is licensed under the GNU GPL, version 2 or later
# (COPYING); its source code: https://github.com/mamedev/mame/tree/$tag
EOF
"$stage/mame" -version
log "done: $stage"
