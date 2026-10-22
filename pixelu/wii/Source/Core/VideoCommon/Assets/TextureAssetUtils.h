// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <filesystem>

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/TextureAsset.h"
#include "VideoCommon/TextureConfig.h"

namespace VideoCommon
{
// Loads a .dds or .png texture, plus any "<name>_mip<N>.<ext>" files next to it as extra mip
// levels.
bool LoadTextureDataFromFile(const CustomAssetLibrary::AssetID& asset_id,
                             const std::filesystem::path& asset_path, AbstractTextureType type,
                             CustomTextureData* data);

// Checks that the data has exactly one slice with at least one level. Size mismatches against
// the native texture only produce warnings.
bool ValidateTextureData(const CustomAssetLibrary::AssetID& asset_id, const CustomTextureData& data,
                         u32 native_width, u32 native_height);

// Drops mip levels whose size does not follow from the level above. Fails when levels of one
// slice have different formats.
bool PurgeInvalidMipsFromTextureData(const CustomAssetLibrary::AssetID& asset_id,
                                     CustomTextureData* data);
}  // namespace VideoCommon
