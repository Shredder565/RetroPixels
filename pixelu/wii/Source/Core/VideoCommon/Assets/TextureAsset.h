// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <fmt/format.h>
#include <picojson.h>

#include "Common/EnumFormatter.h"
#include "VideoCommon/Assets/CustomAsset.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/RenderState.h"

namespace VideoCommon
{
// A texture together with how it is meant to be sampled. The JSON metadata names the texture
// type and, for 2D textures, optional wrap and filter modes.
struct TextureAndSamplerData
{
  static bool FromJson(const CustomAssetLibrary::AssetID& asset_id, const picojson::object& json,
                       TextureAndSamplerData* data);
  static void ToJson(picojson::object* obj, const TextureAndSamplerData& data);
  AbstractTextureType type;
  CustomTextureData texture_data;
  SamplerState sampler;
};

// Image data only.
class TextureAsset final : public CustomLoadableAsset<CustomTextureData>
{
public:
  using CustomLoadableAsset::CustomLoadableAsset;

private:
  CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID& asset_id) override;
};

// Image data plus type and sampler.
class TextureAndSamplerAsset final : public CustomLoadableAsset<TextureAndSamplerData>
{
public:
  using CustomLoadableAsset::CustomLoadableAsset;

private:
  CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID& asset_id) override;
};
}  // namespace VideoCommon
