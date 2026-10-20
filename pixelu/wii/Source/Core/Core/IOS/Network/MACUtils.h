// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Network.h"

namespace IOS::Net
{
// The console's wireless MAC address from the config. A missing or unreadable one is replaced
// by a newly generated address, which is saved.
Common::MACAddress GetMACAddress();
}  // namespace IOS::Net
