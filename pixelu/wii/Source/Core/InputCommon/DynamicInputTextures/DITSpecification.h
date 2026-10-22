// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include <picojson.h>

#include "InputCommon/DynamicInputTextures/DITData.h"

namespace InputCommon::DynamicInputTextures
{
// Reads the textures of a version 1 pack description into input_textures. base_path is the
// pack's folder and json_file is only used in log messages. Returns false (after logging why)
// on the first problem; textures read before it stay in input_textures.
bool ProcessSpecificationV1(picojson::value& root, std::vector<Data>& input_textures,
                            const std::string& base_path, const std::string& json_file);
}  // namespace InputCommon::DynamicInputTextures
