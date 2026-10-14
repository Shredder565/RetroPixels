#!/bin/sh
# Builds rsrc_windows_amd64.syso: the program icon (winres/icon.ico) and the
# file details Windows shows in Properties, with the version from main.go.
# Run it after changing the icon or the version:  sh tools/make-winres.sh
set -e
cd "$(dirname "$0")/.."
v=$(sed -n 's/.*appVersion = "\(.*\)"/\1/p' main.go)
go run github.com/tc-hib/go-winres@v0.3.3 simply --arch amd64 --manifest none \
  --icon winres/icon.ico --product-name RetroPixels --file-description RetroPixels \
  --original-filename RetroPixels.exe --copyright "© 2026 RetroPixels" \
  --product-version "$v.0" --file-version "$v.0"
