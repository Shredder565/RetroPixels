#!/bin/sh
# Builds RetroPixels' Supermodel (Sega Model 3 arcade emulator) from its
# code in Src/ (RetroPixels' copy, being rewritten) and the rest of the
# pinned upstream tree plus the patches in patches/ (see README.txt).
#
#   sh supermodel/build.sh fetch            get the source and apply the patches
#   sh supermodel/build.sh linux   <outdir> build for Linux, with the pinned SDL2 and SDL2_net built from
#                                           source and bundled in lib/ (needs cmake, GL/GLU, zlib and SDL's
#                                           X11/Wayland/PulseAudio/ALSA dev packages; see supermodel.yml)
#   sh supermodel/build.sh windows <outdir> cross-build supermodel.exe for 64-bit Windows with MinGW-w64
#   sh supermodel/build.sh android <outdir> build libsupermodel.so and libSDL2.so for RetroPixels for Android
#                                           (every ABI in ANDROID_ABIS, or SM_ANDROID_ABIS) with the NDK in
#                                           ANDROID_NDK_HOME / ANDROID_NDK or the pinned one; see below
#   sh supermodel/build.sh gles <outdir>    build supermodel-gles, the Android build's OpenGL ES renderer as a
#                                           Linux program (system SDL2 and Mesa's GLES), for testing on a PC
#   sh supermodel/build.sh check            only check the pins and that the patches apply
#
# <outdir>/Supermodel gets the program and the files it needs next to it
# (Config/Games.xml, Config/Music.xml, Assets/), its licence, what was
# changed, and the source it was built from (Supermodel-source.tar.gz).
# On Linux also lib/ with the bundled libraries (found through the
# program's RUNPATH $ORIGIN/lib) and their licences in lib/LICENSES.
# SM_WORK sets the working folder (default: supermodel/build, not committed).
#
# android puts in <outdir>: jniLibs/<abi>/libsupermodel.so and libSDL2.so,
# assets/supermodel/ (Config/Games.xml, Config/Music.xml, Assets/, the
# licences and VERSION.txt) and java/org/libsdl/app/ (SDL's Android Java
# code, from the same SDL source); the app's Gradle build uses that folder
# (android/app/src/main/supermodel).
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
	# The emulator's code is RetroPixels' copy in supermodel/Src, which is
	# being rewritten (docs/supermodel-original.md); upstream only gives the
	# rest of the tree (Makefiles, Config, Assets, Docs).
	rm -rf "$src/Src"
	cp -R "$here/Src" "$src/Src"
	git -C "$src" add -A
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

# The pinned SDL2 and SDL2_net as shared libraries in $work/deps/linux.
# SDL loads X11, Wayland, PulseAudio, PipeWire, ALSA and udev itself when it
# starts (by their sonames), so only their headers are needed here, and the
# program uses whichever of them the player's system has.
linux_deps() {
	deps="$work/deps"
	prefix="$deps/linux"
	stamp="$prefix/.pins-$SDL2_SRC_SHA256-$SDL2_NET_SRC_SHA256"
	[ -f "$stamp" ] && return
	command -v cmake >/dev/null || { echo "needs cmake" >&2; exit 1; }
	mkdir -p "$deps/dl"
	download "$SDL2_SRC_URL" "$SDL2_SRC_SHA256" "$deps/dl/SDL2-src.tar.gz"
	download "$SDL2_NET_SRC_URL" "$SDL2_NET_SRC_SHA256" "$deps/dl/SDL2_net-src.tar.gz"
	rm -rf "$prefix" "$deps/lx"
	mkdir -p "$prefix" "$deps/lx"
	tar -xzf "$deps/dl/SDL2-src.tar.gz" -C "$deps/lx"
	tar -xzf "$deps/dl/SDL2_net-src.tar.gz" -C "$deps/lx"
	log "building SDL $SDL2_VERSION"
	cmake -S "$deps/lx/SDL2-$SDL2_VERSION" -B "$deps/lx/sdl-build" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
		-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF \
		-DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_ALSA=ON \
		-DSDL_OPENGL=ON -DSDL_HIDAPI=ON >"$deps/lx/sdl-cmake.log" || { cat "$deps/lx/sdl-cmake.log"; exit 1; }
	cmake --build "$deps/lx/sdl-build" -j"$jobs" >/dev/null
	cmake --install "$deps/lx/sdl-build" >/dev/null
	# What Supermodel needs on a desktop must be in it: X11 (Wayland desktops
	# run it on XWayland), PulseAudio (PipeWire desktops serve it too) and
	# ALSA for sound, Linux joysticks and force feedback, udev for hot plug.
	conf="$prefix/include/SDL2/SDL_config.h"
	for d in SDL_VIDEO_DRIVER_X11 SDL_VIDEO_OPENGL_GLX SDL_AUDIO_DRIVER_PULSEAUDIO SDL_AUDIO_DRIVER_ALSA \
		SDL_JOYSTICK_LINUX SDL_HAPTIC_LINUX HAVE_LIBUDEV_H SDL_VIDEO_DRIVER_X11_XINPUT2; do
		grep -q "^#define $d 1" "$conf" || { echo "SDL was built without $d (a dev package is missing)" >&2; exit 1; }
	done
	grep -E '^#define SDL_(VIDEO_DRIVER|AUDIO_DRIVER)_[A-Z0-9_]+ 1' "$conf" | sed 's/^#define /  /; s/ 1$//'
	log "building SDL_net $SDL2_NET_VERSION"
	cmake -S "$deps/lx/SDL2_net-$SDL2_NET_VERSION" -B "$deps/lx/net-build" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
		-DBUILD_SHARED_LIBS=ON -DSDL2NET_SAMPLES=OFF -DSDL2NET_INSTALL=ON \
		'-DCMAKE_INSTALL_RPATH=$ORIGIN' >"$deps/lx/net-cmake.log" || { cat "$deps/lx/net-cmake.log"; exit 1; }
	cmake --build "$deps/lx/net-build" -j"$jobs" >/dev/null
	cmake --install "$deps/lx/net-build" >/dev/null
	mkdir -p "$prefix/licenses"
	cp "$deps/lx/SDL2-$SDL2_VERSION/LICENSE.txt" "$prefix/licenses/SDL2.txt"
	cp "$deps/lx/SDL2_net-$SDL2_NET_VERSION/LICENSE.txt" "$prefix/licenses/SDL2_net.txt"
	rm -rf "$deps/lx"
	touch "$stamp"
}

# linux_libs <program> <folder>: the bundled libraries in <folder>/lib.
linux_libs() {
	prefix="$work/deps/linux"
	mkdir -p "$2/lib/LICENSES"
	cp -L "$prefix/lib/libSDL2-2.0.so.0" "$prefix/lib/libSDL2_net-2.0.so.0" "$2/lib/"
	cp "$prefix/licenses/SDL2.txt" "$prefix/licenses/SDL2_net.txt" "$2/lib/LICENSES/"
	# GLU (gluPerspective in the legacy 3D engine) is not on every desktop:
	# the build machine's Mesa GLU goes with it.
	glu=$(ldd "$1" | awk '$1 == "libGLU.so.1" { print $3 }')
	[ -n "$glu" ] && [ -f "$glu" ] || { echo "libGLU.so.1 not found" >&2; exit 1; }
	cp -L "$glu" "$2/lib/libGLU.so.1"
	for c in /usr/share/doc/libglu1-mesa/copyright /usr/share/licenses/glu/LICENSE; do
		if [ -f "$c" ]; then
			cp "$c" "$2/lib/LICENSES/GLU.txt"
			break
		fi
	done
	[ -f "$2/lib/LICENSES/GLU.txt" ] || { echo "GLU licence not found" >&2; exit 1; }
	chmod 0644 "$2/lib/"*.so*
	strip --strip-unneeded "$2/lib/"*.so*
	# Every library the program loads is bundled or one a desktop has.
	LD_LIBRARY_PATH='' ldd "$2/supermodel" | tee "$work/ldd.txt"
	if grep -q 'not found' "$work/ldd.txt"; then
		echo "a library is missing" >&2
		exit 1
	fi
	for l in libSDL2-2.0.so.0 libSDL2_net-2.0.so.0 libGLU.so.1; do
		grep -q "$l => $2/lib/$l" "$work/ldd.txt" || { echo "$l is not loaded from lib/" >&2; exit 1; }
	done
}

# The NDK: the one ANDROID_NDK_HOME or ANDROID_NDK names (its version is
# shown; the pinned one is ANDROID_NDK_REVISION), else the pinned one,
# downloaded into $work/deps. Sets $ndk.
android_ndk() {
	for d in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK:-}"; do
		if [ -n "$d" ] && [ -f "$d/build/cmake/android.toolchain.cmake" ]; then
			ndk=$d
			rev=$(sed -n 's/^Pkg.Revision *= *//p' "$d/source.properties" 2>/dev/null || true)
			log "using the NDK in $d (${rev:-version unknown})"
			[ "$rev" = "$ANDROID_NDK_REVISION" ] || log "note: the pinned NDK is $ANDROID_NDK_VERSION ($ANDROID_NDK_REVISION)"
			return
		fi
	done
	deps="$work/deps"
	ndk="$deps/android-ndk-$ANDROID_NDK_VERSION"
	[ -f "$ndk/build/cmake/android.toolchain.cmake" ] && return
	command -v unzip >/dev/null || { echo "needs unzip" >&2; exit 1; }
	mkdir -p "$deps/dl"
	download "$ANDROID_NDK_URL" "$ANDROID_NDK_SHA256" "$deps/dl/android-ndk-$ANDROID_NDK_VERSION.zip"
	log "unpacking the NDK"
	rm -rf "$ndk"
	unzip -q "$deps/dl/android-ndk-$ANDROID_NDK_VERSION.zip" -d "$deps"
	[ -f "$ndk/build/cmake/android.toolchain.cmake" ] || { echo "the NDK zip didn't hold $ndk" >&2; exit 1; }
	rm -f "$deps/dl/android-ndk-$ANDROID_NDK_VERSION.zip"
}

# The pinned SDL2 source in $work/deps/android-sdl. Sets $sdlsrc.
android_sdl() {
	deps="$work/deps"
	sdlsrc="$deps/android-sdl/SDL2-$SDL2_VERSION"
	stamp="$deps/android-sdl/.pins-$SDL2_SRC_SHA256"
	[ -f "$stamp" ] && return
	mkdir -p "$deps/dl"
	download "$SDL2_SRC_URL" "$SDL2_SRC_SHA256" "$deps/dl/SDL2-src.tar.gz"
	rm -rf "$deps/android-sdl"
	mkdir -p "$deps/android-sdl"
	tar -xzf "$deps/dl/SDL2-src.tar.gz" -C "$deps/android-sdl"
	touch "$stamp"
}

# The version string the desktop builds carry (save states and NVRAM name it).
sm_version() {
	(cd "$src" && make -s -f Makefiles/Makefile.UNIX version 2>/dev/null) || echo "0.3a-git-$(git -C "$src" rev-parse --short HEAD)"
}

# android_build <outdir>
android_build() {
	command -v cmake >/dev/null || { echo "needs cmake" >&2; exit 1; }
	gen=
	command -v ninja >/dev/null && gen="-G Ninja"
	mkdir -p "$1"
	out=$(cd "$1" && pwd)
	version=$(sm_version)
	rm -rf "$out/jniLibs" "$out/assets" "$out/java"
	for abi in ${SM_ANDROID_ABIS:-$ANDROID_ABIS}; do
		b="$work/android-build/$abi"
		log "building for Android $abi"
		# shellcheck disable=SC2086
		cmake -S "$here/android" -B "$b" $gen -DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
			-DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$ANDROID_MIN_SDK" -DANDROID_STL=c++_static \
			-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
			-DSM_TREE="$src" -DSM_SDL_SRC="$sdlsrc" -DSM_VERSION="$version" >"$work/android-cmake-$abi.log" ||
			{ cat "$work/android-cmake-$abi.log"; exit 1; }
		cmake --build "$b" -j"$jobs" --target supermodel SDL2
		mkdir -p "$out/jniLibs/$abi"
		cp "$b/libsupermodel.so" "$out/jniLibs/$abi/"
		cp "$(find "$b/sdl" -maxdepth 1 -name libSDL2.so | head -n 1)" "$out/jniLibs/$abi/libSDL2.so"
	done
	a="$out/assets/supermodel"
	mkdir -p "$a/Config" "$a/Assets"
	cp "$src/Config/Games.xml" "$src/Config/Music.xml" "$a/Config/"
	cp "$src/Assets/"*.bmp "$a/Assets/"
	cp "$src/Docs/LICENSE.txt" "$a/LICENSE.txt"
	cp "$src/Docs/RetroPixels.txt" "$a/RetroPixels.txt"
	cp "$sdlsrc/LICENSE.txt" "$a/SDL2-LICENSE.txt"
	{
		echo "Supermodel $version, modified for RetroPixels"
		echo "Upstream: $SUPERMODEL_REPO commit $SUPERMODEL_COMMIT; RetroPixels' Src/ and patches from https://github.com/shredder565/retropixels (supermodel/)"
		echo "Licence: GNU General Public License v3 or later (LICENSE.txt)"
		echo "Built with the Android NDK ($(sed -n 's/^Pkg.Revision *= *//p' "$ndk/source.properties" 2>/dev/null)), SDL $SDL2_VERSION (SDL2-LICENSE.txt, zlib licence) and the NDK's zlib, for Android $ANDROID_MIN_SDK and later, OpenGL ES 3.2"
		echo "No games, ROMs or BIOS files are included."
	} > "$a/VERSION.txt"
	mkdir -p "$out/java/org/libsdl/app"
	cp "$sdlsrc/android-project/app/src/main/java/org/libsdl/app/"*.java "$out/java/org/libsdl/app/"
	echo "$SDL2_SRC_SHA256 $SUPERMODEL_COMMIT $version" > "$out/.built"
	log "Android libraries in $out"
	find "$out/jniLibs" -name '*.so' -exec ls -la {} +
}

# package <program> <outdir>
package() {
	# Absolute, because the source tarball is written from inside $src.
	mkdir -p "$2"
	out="$(cd "$2" && pwd)/Supermodel"
	rm -rf "$out"
	mkdir -p "$out/Config" "$out/Assets"
	cp "$1" "$out/"
	if [ "${3:-}" = linux ]; then
		linux_libs "$1" "$out"
	fi
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
		echo "Source: Supermodel-source.tar.gz in this folder (RetroPixels' Src/ in the upstream tree, with the patches applied)"
		[ "${3:-}" = linux ] && echo "Built on $(. /etc/os-release && echo "$PRETTY_NAME") with GCC $(gcc -dumpfullversion), SDL $SDL2_VERSION and SDL_net $SDL2_NET_VERSION (lib/, zlib licence), GLU from Mesa (lib/libGLU.so.1, SGI Free Software License B); needs glibc 2.35 or later and OpenGL"
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
	linux_deps
	musashi obj
	prefix="$work/deps/linux"
	log "building for Linux"
	# The pinned SDL from $prefix, found at run time in lib/ next to the
	# program ($$ is make's $, the quotes keep the shell off $ORIGIN).
	PATH="$prefix/bin:$PATH" make -C "$src" -s -f Makefiles/Makefile.UNIX -j"$jobs" \
		PLATFORM_LDFLAGS="-L$prefix/lib -lSDL2 -lSDL2_net -lGL -lGLU -lz -lm -lstdc++ -lpthread -Wl,--enable-new-dtags,-rpath,'\$\$ORIGIN/lib'"
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
android)
	[ $# -ge 2 ] || { echo "usage: $0 android <outdir>" >&2; exit 2; }
	fetch
	musashi obj
	android_ndk
	android_sdl
	android_build "$2"
	;;
gles)
	[ $# -ge 2 ] || { echo "usage: $0 gles <outdir>" >&2; exit 2; }
	fetch
	musashi obj
	log "building supermodel-gles"
	cmake -S "$here/android" -B "$work/gles-build" -DCMAKE_BUILD_TYPE=Release -DSM_HOST_GLES=ON \
		-DSM_TREE="$src" -DSM_VERSION="$(sm_version)" >"$work/gles-cmake.log" || { cat "$work/gles-cmake.log"; exit 1; }
	cmake --build "$work/gles-build" -j"$jobs"
	package "$work/gles-build/supermodel-gles" "$2"
	;;
*)
	sed -n '2,32p' "$0"
	exit 2
	;;
esac
