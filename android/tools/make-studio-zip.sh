#!/bin/sh
# Packs RetroPixels-Android-Studio-<version>.zip: the Android Studio project
# with the emulator engine prebuilt for the four Android CPUs (run
# build-engines.sh first), plus the engine's Go sources, so the project
# builds in Android Studio with or without Go installed.
#
#   sh android/tools/make-studio-zip.sh [output-dir]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
go=${GO:-go}
outdir=${1:-.}
mkdir -p "$outdir"
outdir=$(cd "$outdir" && pwd)
version=$(sed -n 's/.*appVersion = "\(.*\)"/\1/p' "$repo/main.go")
[ -n "$version" ] || { echo "no appVersion in main.go" >&2; exit 1; }
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    [ -f "$repo/android/app/src/main/jniLibs/$abi/librpengine.so" ] || {
        echo "the engine for $abi isn't built: run sh android/tools/build-engines.sh first" >&2
        exit 1
    }
done

name=RetroPixels-Android-Studio-$version
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
top="$stage/$name"
mkdir -p "$top"

# the project, without what a build or Android Studio leaves behind
(cd "$repo" && tar -cf - --exclude=build --exclude=.gradle --exclude=.idea --exclude=.kotlin --exclude='*.iml' \
    --exclude=local.properties --exclude=.cxx --exclude=captures android) | (cd "$top" && tar -xf -)
# the version, for the build without ../main.go
printf 'version=%s\n' "$version" > "$top/android/version.properties"

# the engine's Go sources: its packages' non-test files and go.mod
cp "$repo/go.mod" "$top/"
[ ! -f "$repo/go.sum" ] || cp "$repo/go.sum" "$top/"
(cd "$repo" && "$go" list -deps -f '{{if not .Standard}}{{.Dir}}{{end}}' ./cmd/retropixels-android) | while read -r dir; do
    [ -n "$dir" ] || continue
    rel=${dir#"$repo"/}
    mkdir -p "$top/$rel"
    for f in "$dir"/*.go; do
        case $f in *_test.go) continue ;; esac
        cp "$f" "$top/$rel/"
    done
done

cat > "$top/OPEN-ME.txt" <<NOTE
RetroPixels $version for Android: the Android Studio project

In Android Studio, choose File > Open and pick the "android" folder in here
(not this folder). Let Gradle sync, then Build > Build App Bundle(s) / APK(s)
> Build APK(s), or press Run with a phone connected.

The emulator engine (app/src/main/jniLibs/*/librpengine.so) is already built
for every Android CPU. If Go (go.dev) is installed, the build rebuilds it from
the Go sources in this folder (cmd/retropixels-android and the packages it
uses); without Go it uses the ones already there.

No signing key is included. Android Studio signs what it builds with your
computer's debug key, which is fine for testing; to publish, sign with your
own key (see the signing notes in app/build.gradle). An APK signed with a
different key can't install over one already on the phone: export your
battery saves first, uninstall, then install.

No games, BIOS files or chip programs are included.
NOTE

rm -f "$outdir/$name.zip"
(cd "$stage" && zip -qrX "$outdir/$name.zip" "$name")
echo "$outdir/$name.zip: $(wc -c < "$outdir/$name.zip") bytes"
