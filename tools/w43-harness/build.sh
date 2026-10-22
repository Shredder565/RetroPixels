#!/bin/bash
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# Builds the W43 harness: build.sh <out_dir> [source_root]
#   out_dir      where objects and the w43h binary go (scratch space; use one out_dir per
#                source_root, objects are reused when newer than their source)
#   source_root  the Source/Core directory holding the device files to test
#                (default: pixelu/wii/Source/Core of this checkout)
# Needs g++ 13 or later (C++23). Run: <out_dir>/w43h --dir <scratch_dir> [--seed N] [--cases N]
# W43_INC may name extra include directories (colon separated) holding the third-party
# headers fmt/, picojson.h and mbedtls/ when the system does not provide them.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build.sh <out_dir> [source_root]}
SRC=${2:-$HERE/../../pixelu/wii/Source/Core}
SRC=$(cd "$SRC" && pwd)
mkdir -p "$OUT/obj"

INC=(-I"$SRC" -I"$HERE/shim")
IFS=: read -r -a extra <<< "${W43_INC:-}"
for d in "${extra[@]}"; do
  if [ -n "$d" ]; then INC+=(-I"$d"); fi
done

CXX=${CXX:-g++}
# W43_OPT overrides the optimisation level; equal hashes at -O0 and -O2 are a cheap UB check.
FLAGS=(-std=c++23 "${W43_OPT:--O1}" -g0 -DFMT_HEADER_ONLY -ftrivial-auto-var-init=zero -w)

DEVICES=(
  Core/HW/DVD/AMMediaboard.cpp
  Core/HW/SI/SI_DeviceAMBaseboard.cpp
  Core/HW/Triforce/JVSIO.cpp
  Core/HW/Triforce/IOPorts.cpp
  Core/HW/Triforce/ICCardReader.cpp
  Core/HW/Triforce/DeckReader.cpp
  Core/HW/Triforce/FZeroAX.cpp
  Core/HW/Triforce/MarioKartGP.cpp
  Core/HW/Triforce/Touchscreen.cpp
  Core/HW/MagCard/MagneticCardReader.cpp
  Core/HW/MagCard/C1231BR.cpp
  Core/HW/MagCard/C1231LR.cpp
  Core/HW/EXI/EXI_DeviceModem.cpp
  Core/HW/EXI/EXI_DeviceAGP.cpp
  Core/HW/EXI/EXI_DeviceBaseboard.cpp
)
BASE=(
  Core/HW/Triforce/SerialDevice.cpp
  Core/HW/EXI/Modem/TAPServerModem.cpp
  Common/IOFile.cpp
  Common/DirectIOFile.cpp
  Common/FileUtil.cpp
  Common/StringUtil.cpp
  Common/Network.cpp
  Common/Random.cpp
  Common/CommonFuncs.cpp
  Common/SocketContext.cpp
  Common/JsonUtil.cpp
  Common/Config/Config.cpp
  Common/Config/Layer.cpp
  Common/Config/ConfigInfo.cpp
)

# Objects are only reused when the compiler, flags, includes and source root are unchanged.
STAMP="$CXX ${FLAGS[*]} ${INC[*]}"
if [ "$(cat "$OUT/obj/stamp" 2>/dev/null || true)" != "$STAMP" ]; then
  rm -f "$OUT"/obj/*.o
  printf '%s' "$STAMP" > "$OUT/obj/stamp"
fi

objs=()
compile() {
  local src=$1 obj=$2
  objs+=("$obj")
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    "$CXX" "${FLAGS[@]}" "${INC[@]}" -c "$src" -o "$obj"
  fi
}
for f in "${DEVICES[@]}" "${BASE[@]}"; do
  # A file the new tree folds into another one may be missing; that is fine.
  if [ -f "$SRC/$f" ]; then
    compile "$SRC/$f" "$OUT/obj/$(echo "$f" | tr / _ | sed 's/\.cpp$/.o/')"
  fi
done
compile "$HERE/fakes.cpp" "$OUT/obj/fakes.o"
compile "$HERE/harness.cpp" "$OUT/obj/harness.o"

WRAP=()
for s in socket close bind connect listen accept recv send setsockopt getsockopt poll ioctl; do
  WRAP+=(-Wl,--wrap="$s")
done
"$CXX" "${objs[@]}" "${WRAP[@]}" -o "$OUT/w43h"
echo "built $OUT/w43h"
