#!/bin/sh
# Makes the RetroPixels download for Linux (x86_64):
#
#   sh tools/linux/package.sh [-v VERSION] [-e ENGINE=PATH]... [-n NOTES] OUTDIR
#
# OUTDIR/RetroPixels-VERSION/ gets the same layout as the Windows download:
#   RetroPixels (RetroPixels Native is Windows-only),
#   PixelU/PixelU, PixelU/wii/PixelU-Wii, MAME/MAME 0.xxx/mame,
#   Supermodel/supermodel, README.txt, CHANGELOG.txt, licences, VERSION.txt,
#   RetroPixels.desktop and retropixels.png (once tools/linux has them),
# and is packed as OUTDIR/RetroPixels-VERSION.tar.gz (a workflow artifact
# zip loses the executable bits, a tar.gz keeps them), with a copy of
# VERSION.txt as OUTDIR/VERSION.txt.
#
#   -v VERSION       e.g. 2.21.0-linux-test12 (default: main.go's appVersion
#                    followed by -linux-dev)
#   -e ENGINE=PATH   an engine's Linux build, a .tar.gz or a folder, for
#                    ENGINE supermodel, pixelu, pixelu-wii or mame. Each is
#                    optional: what isn't given is left out and VERSION.txt
#                    says so. For supermodel, pixelu and pixelu-wii the
#                    folder holding the program (supermodel, PixelU,
#                    PixelU-Wii) is found inside and becomes Supermodel/,
#                    PixelU/ and PixelU/wii/. For mame the contents go into
#                    MAME/ as they are (MAME 0.xxx/mame, laid out by
#                    tools/bundlemame).
#   -n NOTES         a text file whose lines are listed in VERSION.txt under
#                    "Engine builds used" (bundle-linux.yml writes which run
#                    each engine came from).
#
# Environment: BRANCH, REPO (owner/name) and COMMIT go in VERSION.txt when
# set (CI sets them; the defaults come from git). GLIBC_MAX (default 2.35)
# is the newest glibc symbol version any program or library may need.
# Needs: go, tar, gzip, readelf and objdump (binutils).
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
GLIBC_MAX=${GLIBC_MAX:-2.35}

usage() { sed -n '2,36p' "$0" >&2; exit 2; }
log() { printf '== %s\n' "$*"; }
die() { printf 'package.sh: %s\n' "$*" >&2; exit 1; }

app_version=$(sed -n 's/.*appVersion = "\(.*\)"/\1/p' "$root/main.go")
[ -n "$app_version" ] || die "no appVersion in main.go"
ver="$app_version-linux-dev"
notes=""
engines=""
while getopts v:e:n: opt; do
	case "$opt" in
	v) ver=$OPTARG ;;
	e)
		name=${OPTARG%%=*}
		path=${OPTARG#*=}
		case "$name" in
		supermodel | pixelu | pixelu-wii | mame) ;;
		*) die "unknown engine $name (supermodel, pixelu, pixelu-wii or mame)" ;;
		esac
		[ "$name" != "$OPTARG" ] && [ -e "$path" ] || die "-e $OPTARG: no such file or folder"
		engines="$engines $name=$(cd "$(dirname "$path")" && pwd)/$(basename "$path")"
		;;
	n) notes=$(cd "$(dirname "$OPTARG")" && pwd)/$(basename "$OPTARG") ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
case "$ver" in
*/* | "" | .*) die "bad version $ver" ;;
esac
mkdir -p "$1"
out=$(cd "$1" && pwd)
name="RetroPixels-$ver"
dir="$out/$name"
rm -rf "$dir" "$out/$name.tar.gz"
mkdir -p "$dir"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---- RetroPixels itself (static, no glibc floor) ----
log "building RetroPixels $ver"
(
	cd "$root"
	export GOOS=linux GOARCH=amd64 CGO_ENABLED=0
	go build -trimpath -ldflags "-s -w" -o "$dir/RetroPixels" .
	# RetroPixels Native is Windows-only: it is not built for Linux (owner's
	# call, 2026-10-21).
)

# ---- engines ----

# unpack <tar.gz or folder> <into>
unpack() {
	mkdir -p "$2"
	if [ -d "$1" ]; then
		cp -a "$1/." "$2/"
	else
		tar -xzf "$1" -C "$2"
	fi
}

# engine_dir <unpacked folder> <program name>: the folder holding the program.
engine_dir() {
	found=$(find "$1" -type f -name "$2" -perm -u+x | awk '{ print length($0) " " $0 }' | sort -n | head -n 1 | cut -d' ' -f2-)
	[ -n "$found" ] || die "no executable $2 in the $2 build (is it a Linux build, with its exec bits?)"
	dirname "$found"
}

have() { case " $engines " in *" $1="*) return 0 ;; esac; return 1; }
path_of() { for e in $engines; do [ "${e%%=*}" = "$1" ] && echo "${e#*=}"; done; }

if have supermodel; then
	log "Supermodel"
	unpack "$(path_of supermodel)" "$tmp/supermodel"
	src=$(engine_dir "$tmp/supermodel" supermodel)
	mkdir -p "$dir/Supermodel"
	cp -a "$src/." "$dir/Supermodel/"
	for f in RetroPixels.txt LICENSE.txt Supermodel-source.tar.gz Config/Games.xml; do
		[ -f "$dir/Supermodel/$f" ] || die "Supermodel/$f is missing"
	done
fi
if have pixelu; then
	log "PixelU"
	unpack "$(path_of pixelu)" "$tmp/pixelu"
	src=$(engine_dir "$tmp/pixelu" PixelU)
	mkdir -p "$dir/PixelU"
	cp -a "$src/." "$dir/PixelU/"
fi
if have pixelu-wii; then
	log "PixelU-Wii"
	unpack "$(path_of pixelu-wii)" "$tmp/pixelu-wii"
	src=$(engine_dir "$tmp/pixelu-wii" PixelU-Wii)
	rm -rf "$dir/PixelU/wii"
	mkdir -p "$dir/PixelU/wii"
	cp -a "$src/." "$dir/PixelU/wii/"
fi
if have mame; then
	log "MAME"
	unpack "$(path_of mame)" "$dir/MAME"
	find "$dir/MAME" -type f -name mame -perm -u+x | grep -q . || die "no executable mame in the MAME build"
fi

# ---- documents ----
cp "$root/README.txt" "$root/CHANGELOG.txt" "$root/LICENSE.txt" "$root/THIRD-PARTY-NOTICES.txt" "$dir/"
cp "$root/pixelu/LICENSE.txt" "$dir/LICENSE-MPL-2.0.txt"
cp "$root/wiiu/testdata/demo.rpx" "$dir/RetroPixels Wii U demo.rpx"
for f in RetroPixels.desktop retropixels.png; do
	if [ -f "$root/tools/linux/$f" ]; then
		cp "$root/tools/linux/$f" "$dir/$f"
	fi
done
if [ -f "$dir/RetroPixels.desktop" ]; then
	chmod 0755 "$dir/RetroPixels.desktop"
fi

branch=${BRANCH:-$(git -C "$root" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)}
commit=${COMMIT:-$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)}
repo=${REPO:-}
{
	echo "RetroPixels $ver for Linux (x86_64) (test build, not a release)"
	echo "Built $(date -u +%F) from branch $branch ($(echo "$commit" | cut -c1-7))"
	echo
	echo "Unpack it anywhere and start ./RetroPixels (it opens in its own window; it"
	echo "needs a Chromium-based browser: Chrome, Chromium, Edge or Brave, otherwise"
	echo "the default browser opens the page). Wayland desktops run it on XWayland."
	echo "Needs glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora 36 and later),"
	echo "OpenGL, and zenity or kdialog for the file dialogs. Settings go to"
	echo "~/.config/RetroPixels, the app window's data to ~/.local/share/RetroPixels."
	echo
	echo "RetroPixels is Copyright Shredder565, all rights reserved (LICENSE.txt). Third-party components keep their own licences (THIRD-PARTY-NOTICES.txt)."
	echo
	echo "Everything plays inside RetroPixels. Its engines are in the folders next to it:"
	if have pixelu; then
		echo "- PixelU/PixelU: Wii U (original engine rewritten from a Cemu start; Cemu's game profiles and translations are MPL-2.0, LICENSE-MPL-2.0.txt)"
	else
		echo "- PixelU (Wii U): not in this build yet (its Linux build isn't ready)"
	fi
	if have pixelu-wii; then
		echo "- PixelU/wii/PixelU-Wii: GameCube and Wii (original engine rewritten from a Dolphin start, GPL-2.0-or-later, PixelU/wii/license.txt)"
	else
		echo "- PixelU-Wii (GameCube and Wii): not in this build yet (its Linux build isn't ready)"
	fi
	if have mame; then
		echo "- MAME: arcade games (built from the MAME team's official release source, GPL-2.0-or-later)"
	else
		echo "- MAME: not in this build yet (its Linux build isn't ready); a MAME installed on the system can be used"
	fi
	if have supermodel; then
		echo "- Supermodel/supermodel: Sega Model 3 (Star Wars Trilogy Arcade). Supermodel by The Supermodel Team, modified for RetroPixels; GPL-3.0-or-later, Supermodel/LICENSE.txt"
		echo "  Source: Supermodel/Supermodel-source.tar.gz (the exact source built); RetroPixels' changes: Supermodel/RetroPixels.txt"
		echo "  Bundled libraries in Supermodel/lib (SDL2, SDL2_net, GLU), their licences in Supermodel/lib/LICENSES"
	else
		echo "- Supermodel (Sega Model 3): not in this build"
	fi
	echo "- RetroPixels Native is Windows-only and is not part of the Linux download"
	if [ -n "$notes" ] && [ -s "$notes" ]; then
		echo "Engine builds used:"
		sed 's/^/  /' "$notes"
	fi
	echo
	if [ -n "$repo" ]; then
		echo "Source: https://github.com/$repo/tree/$branch"
	fi
	echo "PixelU-Wii source is in pixelu/wii; docs/pixelu-wii-original.md explains the rewrite and lists the third-party parts and kept Dolphin data."
	echo "No games, keys, BIOS or firmware are included."
} >"$dir/VERSION.txt"

# ---- checks ----
log "checking the programs"
find "$dir" -type f | sort >"$tmp/files"
: >"$tmp/elf"
while IFS= read -r f; do
	if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then
		echo "$f" >>"$tmp/elf"
	fi
done <"$tmp/files"
for p in RetroPixels Supermodel/supermodel PixelU/PixelU PixelU/wii/PixelU-Wii; do
	if [ -f "$dir/$p" ] && [ ! -x "$dir/$p" ]; then
		die "$p is not executable"
	fi
done
[ -x "$dir/RetroPixels" ] || die "RetroPixels was not built"

# What a desktop has (docs/linux-plan.md, "Expected on the system"); a
# library a program needs that is neither here nor bundled with it fails.
system_lib() {
	case "$1" in
	ld-linux-x86-64.so.2 | linux-vdso.so.1 | libc.so.6 | libm.so.6 | libpthread.so.0 | libdl.so.2 | librt.so.1 | \
		libresolv.so.2 | libutil.so.1 | libanl.so.1 | libstdc++.so.6 | libgcc_s.so.1 | libatomic.so.1 | libz.so.1 | \
		libGL.so.1 | libEGL.so.1 | libGLX.so.0 | libOpenGL.so.0 | libGLESv2.so.2 | libGLdispatch.so.0 | libvulkan.so.1 | \
		libgbm.so.1 | libdrm.so.2 | libX11.so.6 | libX11-xcb.so.1 | libXext.so.6 | libXrandr.so.2 | libXi.so.6 | \
		libXcursor.so.1 | libXfixes.so.3 | libXinerama.so.1 | libXrender.so.1 | libXss.so.1 | libXxf86vm.so.1 | \
		libSM.so.6 | libICE.so.6 | libxcb*.so.* | libxkbcommon.so.0 | libxkbcommon-x11.so.0 | libwayland-*.so.0 | libwayland-egl.so.1 | \
		libasound.so.2 | libpulse.so.0 | libpulse-simple.so.0 | libudev.so.1 | libdbus-1.so.3 | libzstd.so.1 | \
		libgtk-3.so.0 | libgdk-3.so.0 | libglib-2.0.so.0 | libgobject-2.0.so.0 | libgio-2.0.so.0 | libgmodule-2.0.so.0 | \
		libgthread-2.0.so.0 | libpango-1.0.so.0 | libpangocairo-1.0.so.0 | libpangoft2-1.0.so.0 | libcairo.so.2 | \
		libcairo-gobject.so.2 | libgdk_pixbuf-2.0.so.0 | libatk-1.0.so.0 | libharfbuzz.so.0 | libfontconfig.so.1 | \
		libfreetype.so.6 | libpng16.so.16) return 0 ;;
	esac
	return 1
}

: >"$tmp/problems"
glibc_newest=""
while IFS= read -r f; do
	rel=${f#"$dir"/}
	readelf -d "$f" >"$tmp/dyn" 2>/dev/null || true
	grep -q '(NEEDED)' "$tmp/dyn" || continue # static (the Go programs) or no dynamic section
	v=$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
	if [ -n "$v" ]; then
		if [ "$(printf '%s\n%s\n' "$v" "$GLIBC_MAX" | sort -V | tail -n 1)" != "$GLIBC_MAX" ]; then
			echo "$rel needs glibc $v (newest allowed: $GLIBC_MAX)" >>"$tmp/problems"
		fi
		glibc_newest=$(printf '%s\n%s\n' "$v" "$glibc_newest" | sort -V | tail -n 1)
	fi
	fdir=$(dirname "$f")
	paths=$(sed -n 's/.*(R\(UN\)\{0,1\}PATH).*\[\(.*\)\]/\2/p' "$tmp/dyn" | tr ':' '\n' | sed "s|\$ORIGIN|$fdir|; s|\${ORIGIN}|$fdir|")
	for lib in $(sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' "$tmp/dyn"); do
		system_lib "$lib" && continue
		ok=""
		# Folder names may have spaces ("MAME 0.281"): one per line.
		set -f
		oldifs=$IFS
		IFS='
'
		for p in $fdir $paths; do
			if [ -e "$p/$lib" ]; then
				ok=1
				break
			fi
		done
		IFS=$oldifs
		set +f
		# A plugin needing a library its program has already loaded (by the
		# same name, from the program's own lib folder) is fine too.
		if [ -z "$ok" ] && [ "$rel" != "${rel#*/}" ] && find "$dir/${rel%%/*}" -name "$lib" | grep -q .; then
			ok=1
		fi
		[ -n "$ok" ] || echo "$rel needs $lib, which is neither bundled nor on every desktop" >>"$tmp/problems"
	done
done <"$tmp/elf"
if [ -s "$tmp/problems" ]; then
	cat "$tmp/problems" >&2
	die "the package would not run everywhere it should"
fi
echo "programs and libraries: $(wc -l <"$tmp/elf"), newest glibc symbol needed: ${glibc_newest:-none (all static)}"

# ---- pack ----
log "packing $name.tar.gz"
tar -C "$out" --owner=0 --group=0 --numeric-owner -czf "$out/$name.tar.gz" "$name"
cp "$dir/VERSION.txt" "$out/VERSION.txt"
ls -la "$out"
cat "$dir/VERSION.txt"
