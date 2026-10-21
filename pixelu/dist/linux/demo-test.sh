#!/bin/sh
# Original code for RetroPixels (PixelU).
#
# CI check of the Linux build (.github/workflows/pixelu.yml): runs the packaged PixelU
# (package/PixelU/PixelU) on RetroPixels' own demo, wiiu/testdata/demo.rpx, under Xvfb through the
# docking tests in internal/pixelu/screen_linux_test.go, with Vulkan on lavapipe ("vulkan") or OpenGL
# on llvmpipe ("opengl"). Screenshots and PixelU's log land in shots/<renderer>/; every screenshot
# must have some brightness (the demo draws), a black one fails.
# Run from the repository root: sh pixelu/dist/linux/demo-test.sh vulkan|opengl
set -eu

renderer=$1
root=$(pwd)
copy="${RUNNER_TEMP:-/tmp}/pixelu-$renderer"
shots="$root/shots/$renderer"
rm -rf "$copy"
cp -r package/PixelU "$copy" # its own portable folder for each run
mkdir -p "$shots"

args="-g $root/wiiu/testdata/demo.rpx"
case "$renderer" in
vulkan)
	for f in /usr/share/vulkan/icd.d/lvp_icd*.json; do
		VK_ICD_FILENAMES=$f
	done
	export VK_ICD_FILENAMES
	;;
opengl)
	args="--rp-opengl $args"
	export LIBGL_ALWAYS_SOFTWARE=1
	;;
*)
	echo "usage: $0 vulkan|opengl" >&2
	exit 2
	;;
esac

status=0
RETRO_X11_REQUIRE=1 RETRO_PIXELU_BIN="$copy/PixelU" RETRO_PIXELU_ARGS="$args" RETRO_PIXELU_SHOTS="$shots" \
	go test ./internal/pixelu -run 'SetScreen|PixelU' -v -count=1 -timeout 15m || status=1
find "$copy" -name log.txt -exec cp {} "$shots/" \; 2>/dev/null || true
if [ "$status" != 0 ] && [ -f "$shots/log.txt" ]; then
	echo "--- end of PixelU's log.txt (last test) ---"
	tail -n 80 "$shots/log.txt"
fi

found=0
for f in "$shots"/*.png; do
	[ -e "$f" ] || continue
	found=1
	mean=$(convert "$f" -colorspace Gray -format '%[fx:mean]' info:)
	echo "$f: mean brightness $mean"
	if ! awk -v m="$mean" 'BEGIN { exit !(m > 0.01) }'; then
		echo "::error::$f is black"
		status=1
	fi
done
if [ "$found" = 0 ]; then
	echo "::error::no screenshots"
	status=1
fi
exit $status
