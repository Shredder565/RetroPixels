#!/bin/bash
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# Puts the Qt runtime next to a Linux build of PixelU-Wii, so the folder runs
# on any x86-64 desktop with glibc 2.35+ without Qt installed (the Linux
# counterpart of windeployqt in pixelu/wii/Source/Core/DolphinQt/CMakeLists.txt):
#
#   tools/pixelu-wii-linux-deploy.sh <PixelU-Wii program> <Qt folder> <output folder>
#
#   <Qt folder>   the Qt the program was built with, e.g.
#                 pixelu/wii/third_party/Qt/6.8.3/gcc_64 (aqtinstall's
#                 linux_gcc_64 build) or /usr for a system Qt (local checks)
#
# The output folder gets:
#   PixelU-Wii     the program (copied unless it is already there), RUNPATH $ORIGIN/lib
#   lib/           the Qt libraries it and the plugins below need, plus the few
#                  X11 helper libraries Qt's xcb plugin needs that desktops do
#                  not always have (XCB_UTIL_LIBS below); RUNPATH $ORIGIN
#   QtPlugins/     platforms (xcb, offscreen; the Wayland ones when the Qt has them),
#                  platform themes (GTK 3, XDG desktop portal), SVG images and icons
#   qt.conf        tells Qt its plugins are in QtPlugins (as on Windows)
#
# Everything else comes from the system, as on every desktop: glibc,
# libstdc++, libX11/libxcb, libGL/libEGL/libvulkan, fontconfig/freetype,
# GLib, D-Bus, libxkbcommon, zlib (docs/linux-plan.md, "Expected on the
# system"). Libraries are found with ldd (which runs the dynamic loader on
# the program; only use it on programs you built). Needs patchelf.
#
# Exit status 0 when every library of the program and of every copied plugin
# resolves from the output folder or the system.
#
# $ORIGIN in single quotes is meant literally (it is the loader's, not the shell's).
# shellcheck disable=SC2016

set -euo pipefail

die() {
	echo "pixelu-wii-linux-deploy: $*" >&2
	exit 1
}

[ $# -eq 3 ] || die "usage: $0 <PixelU-Wii program> <Qt folder> <output folder>"
program=$(realpath "$1")
qt=$(realpath "$2")
mkdir -p "$3"
out=$(realpath "$3")
if [ ! -f "$program" ] || [ ! -x "$program" ]; then die "$1 is not an executable file"; fi
# aqtinstall's layout (lib/, plugins/), else a distribution's Qt in /usr.
qt_lib="$qt/lib"
qt_plugins="$qt/plugins"
if [ ! -d "$qt_plugins/platforms" ] && [ -d "$qt/lib/x86_64-linux-gnu/qt6/plugins/platforms" ]; then
	qt_lib="$qt/lib/x86_64-linux-gnu"
	qt_plugins="$qt/lib/x86_64-linux-gnu/qt6/plugins"
fi
[ -d "$qt_plugins/platforms" ] || die "$2 has no plugins/platforms folder (not a Qt folder?)"
command -v patchelf > /dev/null || die "patchelf is needed (apt install patchelf)"

# Plugins, relative to the Qt plugin folder. Required ones must be there.
REQUIRED_PLUGINS=(
	platforms/libqxcb.so
	imageformats/libqsvg.so
	iconengines/libqsvgicon.so
)
# Copied when this Qt has them. offscreen runs --help/--version without a
# display (QT_QPA_PLATFORM=offscreen); the Wayland platform is only used where
# no X server runs (DolphinQt/Main.cpp); the themes give native dialogs and fonts.
OPTIONAL_PLUGINS=(
	platforms/libqoffscreen.so
	platforms/libqwayland-generic.so
	platforms/libqwayland-egl.so
	wayland-shell-integration
	wayland-decoration-client
	wayland-graphics-integration-client
	platformthemes/libqgtk3.so
	platformthemes/libqxdgdesktopportal.so
)
# Libraries Qt's xcb plugin needs that are not on every desktop (the xcb-util
# family, XInput and XKB for xcb, xkbcommon's X11 part): bundled from the
# build machine. Matched by soname.
XCB_UTIL_LIBS=(
	libxcb-cursor.so.0
	libxcb-icccm.so.4
	libxcb-image.so.0
	libxcb-keysyms.so.1
	libxcb-render-util.so.0
	libxcb-util.so.1
	libxcb-xinput.so.0
	libxcb-xkb.so.1
	libxkbcommon-x11.so.0
)

mkdir -p "$out/lib" "$out/QtPlugins"
target="$out/PixelU-Wii"
if [ "$program" != "$target" ]; then
	cp -f "$program" "$target"
fi
chmod 755 "$target"

# The program finds lib/ through its RUNPATH (CMake sets $ORIGIN/lib).
runpath=$(patchelf --print-rpath "$target")
case ":$runpath:" in
*':$ORIGIN/lib:'*) ;;
*)
	echo "RUNPATH was '$runpath'; setting \$ORIGIN/lib"
	patchelf --set-rpath '$ORIGIN/lib' "$target"
	;;
esac

# ---- plugins ----
copied_plugins=()
copy_plugin() { # <relative path> <required: 1/0>
	local from="$qt_plugins/$1"
	if [ ! -e "$from" ]; then
		[ "$2" = 1 ] && die "Qt has no plugin $1 ($from)"
		echo "plugin $1: not in this Qt, left out"
		return 0
	fi
	mkdir -p "$out/QtPlugins/$(dirname "$1")"
	cp -RL "$from" "$out/QtPlugins/$(dirname "$1")/"
	if [ -d "$from" ]; then
		while IFS= read -r f; do copied_plugins+=("$f"); done < <(find "$out/QtPlugins/$1" -name '*.so' -type f)
	else
		copied_plugins+=("$out/QtPlugins/$1")
	fi
	echo "plugin $1"
}
for p in "${REQUIRED_PLUGINS[@]}"; do copy_plugin "$p" 1; done
for p in "${OPTIONAL_PLUGINS[@]}"; do copy_plugin "$p" 0; done

# A plugin whose own libraries are missing on the build machine (the GTK
# theme without GTK 3, say) is left out rather than shipped broken.
unloadable() { # <file>: prints the missing libraries
	LD_LIBRARY_PATH="$qt_lib" ldd "$1" 2> /dev/null | awk '/=> not found/ { print $1 }'
}
kept_plugins=()
for f in "${copied_plugins[@]}"; do
	missing=$(unloadable "$f")
	if [ -n "$missing" ]; then
		rel=${f#"$out/QtPlugins/"}
		case " ${REQUIRED_PLUGINS[*]} " in
		*" $rel "*) die "plugin $rel needs libraries this machine does not have: $missing" ;;
		esac
		echo "plugin $rel: needs ${missing//$'\n'/ } (not on this machine), left out"
		rm -f "$f"
	else
		kept_plugins+=("$f")
		# Every plugin is QtPlugins/<kind>/<file>: it finds Qt in lib/ two folders up
		# (aqt's plugins already say so; a distribution's say nothing).
		patchelf --set-rpath '$ORIGIN/../../lib' "$f"
	fi
done

# ---- libraries ----
is_xcb_util() {
	local n
	for n in "${XCB_UTIL_LIBS[@]}"; do [ "$1" = "$n" ] && return 0; done
	return 1
}
declare -A have=()
queue=("$target" "${kept_plugins[@]}")
while [ ${#queue[@]} -gt 0 ]; do
	file=${queue[0]}
	queue=("${queue[@]:1}")
	while read -r name path; do
		[ -n "${have[$name]:-}" ] && continue
		# Qt's own libraries (and the ICU that Qt's build ships), and the X11 helpers.
		case "$name" in
		libQt6* | libicu*) ;;
		*) is_xcb_util "$name" || continue ;;
		esac
		have[$name]=1
		cp -fL "$path" "$out/lib/$name"
		chmod 644 "$out/lib/$name"
		# Each finds its neighbours in lib/ (Qt's own already say $ORIGIN).
		patchelf --set-rpath '$ORIGIN' "$out/lib/$name"
		queue+=("$out/lib/$name")
	done < <(LD_LIBRARY_PATH="$qt_lib" ldd "$file" | awk '$2 == "=>" && $3 ~ /^\// { print $1, $3 }')
done

cat > "$out/qt.conf" << 'EOF'
; Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
; Shipped as qt.conf next to PixelU-Wii: Qt's plugins are in QtPlugins.
[Paths]
Plugins = ./QtPlugins
EOF

# ---- check: everything resolves without LD_LIBRARY_PATH ----
bad=0
while IFS= read -r f; do
	missing=$(env -u LD_LIBRARY_PATH ldd "$f" 2> /dev/null | awk '/=> not found/ { print $1 }')
	if [ -n "$missing" ]; then
		echo "${f#"$out/"}: not found: ${missing//$'\n'/ }" >&2
		bad=1
	fi
	# Qt's libraries and plugins must come from the folder, never from a system Qt.
	# (A plugin's paths come out as QtPlugins/<kind>/../../lib/..., hence realpath.)
	outside=$(env -u LD_LIBRARY_PATH ldd "$f" 2> /dev/null | awk '$1 ~ /^libQt6/ && $3 ~ /^\// { print $3 }' |
		while IFS= read -r lib; do realpath "$lib"; done | grep -v "^$out/lib/" || true)
	if [ -n "$outside" ]; then
		echo "${f#"$out/"}: takes Qt from outside the folder:" >&2
		env -u LD_LIBRARY_PATH ldd "$f" | grep libQt6 >&2
		bad=1
	fi
done < <(
	echo "$target"
	find "$out/lib" "$out/QtPlugins" -name '*.so*' -type f
)
[ $bad = 0 ] || die "some libraries do not resolve (see above)"

echo "lib/: $(find "$out/lib" -type f | wc -l) libraries, QtPlugins/: $(find "$out/QtPlugins" -name '*.so' | wc -l) plugins"
ls "$out/lib"
