// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/CustomPipeline.h"

void CustomPipeline::UpdatePixelData(const VideoCommon::CustomAssetLibrary*, std::span<const u32>,
                                     const VideoCommon::CustomAssetLibrary::AssetID&)
{
  // Intentionally empty: materials are resolved by the resource manager instead.
}
