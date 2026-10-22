// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include "Common/CommonTypes.h"

namespace Common
{
// Writes a blank FAT32 SD card image of `disk_size` MiB (8 MiB to 32 GiB) to `filename`.
// On failure nothing is left behind.
bool SDCardCreate(u64 disk_size /*in MB*/, const std::string& filename);
}  // namespace Common
