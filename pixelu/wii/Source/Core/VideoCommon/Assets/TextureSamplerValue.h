// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include <picojson.h>

#include "VideoCommon/Assets/CustomAssetLibrary.h"

namespace VideoCommon
{
// A texture input of a material: which texture asset to bind and where its sampler state
// comes from.
struct TextureSamplerValue
{
  CustomAssetLibrary::AssetID asset;

  // Asset: use the sampler stored with the texture asset.
  // TextureHash: use whatever sampler the game uses for the texture named by texture_hash.
  enum class SamplerOrigin
  {
    Asset,
    TextureHash
  };
  static std::string ToString(SamplerOrigin sampler_origin);
  SamplerOrigin sampler_origin = SamplerOrigin::Asset;
  std::string texture_hash;

  // Missing fields fall back to defaults, so this always succeeds.
  static bool FromJson(const picojson::object& json, TextureSamplerValue* data);
  static void ToJson(picojson::object* obj, const TextureSamplerValue& data);
};
}  // namespace VideoCommon
