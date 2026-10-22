// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <span>

#include "Common/CommonTypes.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

// Placeholder for per-draw custom pipeline state; updating it currently does nothing.
struct CustomPipeline
{
  void UpdatePixelData(const VideoCommon::CustomAssetLibrary* library,
                       std::span<const u32> texture_units,
                       const VideoCommon::CustomAssetLibrary::AssetID& material_to_load);
};
