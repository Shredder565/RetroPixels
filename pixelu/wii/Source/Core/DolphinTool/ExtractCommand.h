// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

namespace DolphinTool
{
// "dolphin-tool extract": lists or extracts a disc image's partitions and files.
int Extract(const std::vector<std::string>& args);
}  // namespace DolphinTool
