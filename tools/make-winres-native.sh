#!/bin/sh
# Builds cmd/retropixels-native/rsrc_windows_amd64.syso: the program icon
# (winres/icon.ico), a manifest (sharp on high-DPI screens, modern buttons)
# and the file details Windows shows in Properties for RetroPixels-Native.exe,
# with the version from main.go. Run it after changing the icon or the version:
#   sh tools/make-winres-native.sh
set -e
cd "$(dirname "$0")/.."
v=$(sed -n 's/.*appVersion = "\(.*\)"/\1/p' main.go)
go run github.com/tc-hib/go-winres@v0.3.3 simply --arch amd64 --manifest gui \
  --out cmd/retropixels-native/rsrc \
  --icon winres/icon.ico --product-name "RetroPixels Native" --file-description "RetroPixels Native" \
  --original-filename RetroPixels-Native.exe --copyright "© 2026 RetroPixels" \
  --product-version "$v.0" --file-version "$v.0"
