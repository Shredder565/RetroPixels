// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

namespace DolphinTool
{
// "dolphin-tool convert": rewrites a disc image as ISO, GCZ, WIA or RVZ.
int ConvertCommand(const std::vector<std::string>& args);
}  // namespace DolphinTool
