// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>
#include <variant>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/Assets/CustomAsset.h"
#include "VideoCommon/Assets/TextureSamplerValue.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/RenderState.h"

class ShaderCode;

namespace VideoCommon
{
// One uniform value supplied by a material.
struct MaterialProperty
{
  // Writes the value into a uniform buffer (one 16-byte slot) and advances `buffer`.
  static void WriteToMemory(u8*& buffer, const MaterialProperty& property);
  // Bytes WriteToMemory uses for the property.
  static std::size_t GetMemorySize(const MaterialProperty& property);
  using Value =
      std::variant<s32, std::array<s32, 2>, std::array<s32, 3>, std::array<s32, 4>, float,
                   std::array<float, 2>, std::array<float, 3>, std::array<float, 4>, bool>;
  Value m_value;
};

// A material supplies the inputs of a shader asset: uniform values, textures and optional
// render state overrides. It may chain to another material.
struct MaterialData
{
  static bool FromJson(const CustomAssetLibrary::AssetID& asset_id, const picojson::object& json,
                       MaterialData* data);
  static void ToJson(picojson::object* obj, const MaterialData& data);
  CustomAssetLibrary::AssetID shader_asset;
  CustomAssetLibrary::AssetID next_material_asset;
  std::vector<MaterialProperty> properties;

  std::optional<CullMode> cull_mode;
  std::optional<DepthState> depth_state;
  std::optional<BlendingState> blending_state;

  std::vector<TextureSamplerValue> textures;
};

// Holds MaterialData; pairing it with the named shader is left to whoever uses the asset.
class MaterialAsset final : public CustomLoadableAsset<MaterialData>
{
public:
  using CustomLoadableAsset::CustomLoadableAsset;

private:
  CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID& asset_id) override;
};

}  // namespace VideoCommon
