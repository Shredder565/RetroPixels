#!/bin/sh
# Builds and signs the RetroPixels APK without Gradle or the Android Gradle
# Plugin, from the Android SDK's command-line tools alone (Ubuntu:
# apt-get install android-sdk-platform-23 android-sdk-build-tools apksigner):
# javac against Android 6's android.jar (so only calls every supported phone
# has are used, unless guarded), dx, aapt, zipalign and apksigner (the owner's key
# from RP_KEYSTORE, else the debug key). A check that the app compiles and packs; Android Studio or
# ./gradlew assembleRelease make the real build.
#
#   sh android/tools/build-apk.sh [out.apk]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
proj=$(cd "$here/.." && pwd)
repo=$(cd "$proj/.." && pwd)
sdk=${ANDROID_SDK:-/usr/lib/android-sdk}
jar=${ANDROID_JAR:-$sdk/platforms/android-23/android.jar}
bt=${BUILD_TOOLS:-$(ls -d "$sdk"/build-tools/[0-9]* | sort -V | tail -1)}
out=${1:-$proj/app/build/local/RetroPixels.apk}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

version=$(sed -n 's/.*appVersion = "\(.*\)"/\1/p' "$repo/main.go" 2>/dev/null || true)
if [ -z "$version" ] && [ -f "$proj/version.properties" ]; then
    version=$(sed -n 's/^version=//p' "$proj/version.properties")
fi
[ -n "$version" ] || version=0.0.0
code=$(echo "$version" | awk -F. '{ printf "%d", $1 * 10000 + $2 * 100 + $3 }')
echo "RetroPixels $version (version code $code)"

libs="$proj/app/src/main/jniLibs"
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    if [ ! -f "$libs/$abi/librpengine.so" ]; then
        echo "building the engines"
        sh "$here/build-engines.sh" "$version"
        break
    fi
done

src="$proj/app/src/main"
# the manifest as the Gradle plugin would complete it
sed -e 's|<manifest xmlns:android="http://schemas.android.com/apk/res/android">|<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="io.github.shredder565.retropixels">|' \
    -e 's|<application|<application android:extractNativeLibs="true"|' "$src/AndroidManifest.xml" > "$work/AndroidManifest.xml"

mkdir -p "$work/gen" "$work/classes" "$work/apk"
aapt package -f -m -J "$work/gen" -M "$work/AndroidManifest.xml" -S "$src/res" -I "$jar"
find "$src/java" "$work/gen" -name '*.java' > "$work/sources"
javac -nowarn -Xlint:-options -encoding UTF-8 -source 8 -target 8 -bootclasspath "$jar" -d "$work/classes" @"$work/sources"
"$bt/dx" --dex --min-sdk-version=24 --output="$work/apk/classes.dex" "$work/classes"
aapt package -f -M "$work/AndroidManifest.xml" -S "$src/res" -I "$jar" \
    --min-sdk-version 24 --target-sdk-version 36 --version-code "$code" --version-name "$version" \
    -F "$work/unsigned.apk"
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    mkdir -p "$work/apk/lib/$abi"
    cp "$libs/$abi/librpengine.so" "$work/apk/lib/$abi/"
done
(cd "$work/apk" && aapt add "$work/unsigned.apk" classes.dex lib/*/librpengine.so > /dev/null)
zipalign -f -p 4 "$work/unsigned.apk" "$work/aligned.apk"
mkdir -p "$(dirname "$out")"
# Signed with the owner's key when RP_KEYSTORE (and RP_KEYSTORE_PASSWORD,
# RP_KEY_ALIAS, RP_KEY_PASSWORD) are set, else with this computer's Android
# debug key (~/.android/debug.keystore, made here if missing): no key is kept
# in the source.
if [ -n "${RP_KEYSTORE:-}" ]; then
    apksigner sign --ks "$RP_KEYSTORE" --ks-pass env:RP_KEYSTORE_PASSWORD --key-pass env:RP_KEY_PASSWORD \
        --ks-key-alias "$RP_KEY_ALIAS" --min-sdk-version 24 --out "$out" "$work/aligned.apk"
else
    dbg="$HOME/.android/debug.keystore"
    if [ ! -f "$dbg" ]; then
        mkdir -p "$HOME/.android"
        keytool -genkeypair -keystore "$dbg" -storepass android -keypass android -alias androiddebugkey \
            -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
    fi
    echo "no RP_KEYSTORE: signing with the debug key $dbg (for testing only)"
    apksigner sign --ks "$dbg" --ks-pass pass:android --key-pass pass:android \
        --ks-key-alias androiddebugkey --min-sdk-version 24 --out "$out" "$work/aligned.apk"
fi
apksigner verify --min-sdk-version 24 "$out"
echo "$out: $(wc -c < "$out") bytes"
aapt dump badging "$out" | grep -E "^(package|sdkVersion|targetSdkVersion|application-label|launchable-activity|native-code)"
