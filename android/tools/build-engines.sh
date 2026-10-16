#!/bin/sh
# Builds the RetroPixels emulator engine (cmd/retropixels-android) for the
# four Android CPUs into android/app/src/main/jniLibs/<abi>/librpengine.so,
# as the app's buildEngine Gradle task does. Needs only Go (no cgo, no NDK):
# arm64 as an Android program, the others as static Linux programs, which
# Android runs too.
#
#   sh android/tools/build-engines.sh [version]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
go=${GO:-go}
version=${1:-$(sed -n 's/.*appVersion = "\(.*\)"/\1/p' "$repo/main.go" 2>/dev/null || true)}
[ -n "$version" ] || version=$(sed -n 's/^version=//p' "$repo/android/version.properties" 2>/dev/null || true)
[ -n "$version" ] || version=dev
out="$repo/android/app/src/main/jniLibs"
cd "$repo"
for t in "arm64-v8a android arm64 " "armeabi-v7a linux arm 7" "x86_64 linux amd64 " "x86 linux 386 "; do
    set -- $t
    abi=$1 goos=$2 goarch=$3 goarm=${4:-}
    mkdir -p "$out/$abi"
    # 16 KB-aligned segments, for x86_64 devices with 16 KB memory pages
    # (Go already aligns arm64 and arm to 64 KB)
    align=
    [ "$abi" = x86_64 ] && align=" -R 0x4000"
    if [ -n "$goarm" ]; then
        CGO_ENABLED=0 GOOS=$goos GOARCH=$goarch GOARM=$goarm "$go" build -trimpath -ldflags "-s -w$align -X main.version=$version" \
            -o "$out/$abi/librpengine.so" ./cmd/retropixels-android
    else
        env -u GOARM CGO_ENABLED=0 GOOS=$goos GOARCH=$goarch "$go" build -trimpath -ldflags "-s -w$align -X main.version=$version" \
            -o "$out/$abi/librpengine.so" ./cmd/retropixels-android
    fi
    echo "$abi: $(wc -c < "$out/$abi/librpengine.so") bytes"
done
