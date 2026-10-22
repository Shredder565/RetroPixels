// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// PixelU-Wii has no Android build, so there is no Android (DolphinBar over JNI) backend; the
// scanner slot it would take finds nothing.

#include "Core/HW/WiimoteReal/IODummy.h"

namespace WiimoteReal
{
using WiimoteScannerAndroid = WiimoteScannerDummy;
}
