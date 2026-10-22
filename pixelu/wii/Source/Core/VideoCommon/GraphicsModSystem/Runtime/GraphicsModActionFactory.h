// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string_view>

#include <picojson.h>

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

namespace GraphicsModActionFactory
{
// Builds the action a feature names ("print", "skip", "move", "scale", "custom_pipeline").
// Returns null for unknown names or invalid action data.
std::unique_ptr<GraphicsModAction> Create(std::string_view name, const picojson::value& json_data,
                                          std::shared_ptr<VideoCommon::CustomAssetLibrary> library);
}  // namespace GraphicsModActionFactory
