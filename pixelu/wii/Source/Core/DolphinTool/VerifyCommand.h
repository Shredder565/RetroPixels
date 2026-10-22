// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

namespace DolphinTool
{
// "dolphin-tool verify": checks a disc image and prints its digests.
int VerifyCommand(const std::vector<std::string>& args);
}  // namespace DolphinTool
