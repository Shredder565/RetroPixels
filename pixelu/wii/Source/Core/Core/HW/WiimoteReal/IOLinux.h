// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// PixelU-Wii does not build the BlueZ backend; on Linux remotes are found through hidapi, so the
// scanner slot of a BlueZ backend finds nothing.

#include "Core/HW/WiimoteReal/IODummy.h"

namespace WiimoteReal
{
using WiimoteScannerLinux = WiimoteScannerDummy;
}
