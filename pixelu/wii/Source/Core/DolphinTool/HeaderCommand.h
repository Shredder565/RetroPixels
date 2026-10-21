// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

namespace DolphinTool
{
// "dolphin-tool header": prints container and game details of a disc image.
int HeaderCommand(const std::vector<std::string>& args);
}  // namespace DolphinTool
