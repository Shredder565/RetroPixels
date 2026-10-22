// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <picojson.h>

#include "VideoCommon/Assets/CustomAsset.h"
#include "VideoCommon/ShaderCompileUtils.h"
#include "VideoCommon/TextureConfig.h"

class ShaderCode;

namespace VideoCommon
{
// One uniform input a custom shader declares.
struct ShaderProperty
{
  // Colours are three or four floats but are edited as colours.
  struct RGB
  {
    std::array<float, 3> value;
  };

  struct RGBA
  {
    std::array<float, 4> value;
  };

  using Value = std::variant<s32, std::array<s32, 2>, std::array<s32, 3>, std::array<s32, 4>, float,
                             std::array<float, 2>, std::array<float, 3>, std::array<float, 4>, bool,
                             RGB, RGBA>;

  // The type names accepted in JSON ("int" ... "bool").
  static std::span<const std::string_view> GetValueTypeNames();
  // A zero value of the named type (an int 0 for an unknown name).
  static Value GetDefaultValueFromTypeName(std::string_view name);
  // Declares the property as a uniform, padded to a full four-component slot.
  static void WriteAsShaderCode(ShaderCode& shader_source, const ShaderProperty& property);

  Value default_value;
  std::string name;
  std::string description;
};

// A shader for rasterized surfaces: its source, the uniforms it exposes and the samplers it
// reads.
struct RasterSurfaceShaderData
{
  RasterSurfaceShaderData() = default;
  RasterSurfaceShaderData(const RasterSurfaceShaderData&) = delete;
  RasterSurfaceShaderData(RasterSurfaceShaderData&&) = default;
  ~RasterSurfaceShaderData() = default;
  RasterSurfaceShaderData& operator=(const RasterSurfaceShaderData&) = delete;
  RasterSurfaceShaderData& operator=(RasterSurfaceShaderData&&) = default;

  static bool FromJson(const CustomAssetLibrary::AssetID& asset_id, const picojson::object& json,
                       RasterSurfaceShaderData* data);
  static void ToJson(picojson::object& obj, const RasterSurfaceShaderData& data);

  // Each property's name is the identifier the shader source uses for it.
  std::vector<ShaderProperty> uniform_properties;
  std::string vertex_source;
  std::string pixel_source;

  struct SamplerData
  {
    AbstractTextureType type;
    std::string name;

    bool operator==(const SamplerData&) const = default;
  };
  std::vector<SamplerData> samplers;

  // Resolves #include directives in the sources.
  std::unique_ptr<VideoCommon::ShaderIncluder> shader_includer;
};

class RasterSurfaceShaderAsset final : public CustomLoadableAsset<RasterSurfaceShaderData>
{
public:
  using CustomLoadableAsset::CustomLoadableAsset;

private:
  CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID& asset_id) override;
};
}  // namespace VideoCommon
