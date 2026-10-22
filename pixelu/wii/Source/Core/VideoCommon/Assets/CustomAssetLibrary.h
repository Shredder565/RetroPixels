// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

namespace VideoCommon
{
class CustomTextureData;
struct MaterialData;
struct MeshData;
struct RasterSurfaceShaderData;
struct TextureAndSamplerData;

// Source of custom asset data. Each Load function fills the given structure for one asset and
// reports how many bytes it read; how and where the data is stored is up to the implementation.
class CustomAssetLibrary
{
public:
  // Names one asset within a library.
  using AssetID = std::string;

  struct LoadInfo
  {
    // Zero means nothing was loaded.
    std::size_t bytes_loaded = 0;
  };

  virtual ~CustomAssetLibrary() = default;

  // A texture together with its sampler state and texture type.
  virtual LoadInfo LoadTexture(const AssetID& asset_id, TextureAndSamplerData* data) = 0;

  // Just the texture image data.
  virtual LoadInfo LoadTexture(const AssetID& asset_id, CustomTextureData* data) = 0;

  // A shader applied to rasterized surfaces.
  virtual LoadInfo LoadRasterSurfaceShader(const AssetID& asset_id,
                                           RasterSurfaceShaderData* data) = 0;

  virtual LoadInfo LoadMaterial(const AssetID& asset_id, MaterialData* data) = 0;

  virtual LoadInfo LoadMesh(const AssetID& asset_id, MeshData* data) = 0;
};
}  // namespace VideoCommon
