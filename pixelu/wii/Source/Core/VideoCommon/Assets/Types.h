// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace VideoCommon::Assets
{
// Maps a role name inside an asset (for example "metadata" or "texture") to the file holding it.
using AssetMap = std::map<std::string, std::filesystem::path>;
}  // namespace VideoCommon::Assets
