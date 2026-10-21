// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/Assets/TextureAsset.h"
#include "VideoCommon/Assets/Types.h"
#include "VideoCommon/Assets/WatchableFilesystemAssetLibrary.h"

namespace VideoCommon
{
// Asset library backed by plain files. Each asset id is registered with a map of named files
// (for example "texture" and "metadata"); edits to any registered file mark its asset dirty.
class DirectFilesystemAssetLibrary final : public WatchableFilesystemAssetLibrary
{
public:
  LoadInfo LoadTexture(const AssetID& asset_id, TextureAndSamplerData* data) override;
  LoadInfo LoadTexture(const AssetID& asset_id, CustomTextureData* data) override;
  LoadInfo LoadRasterSurfaceShader(const AssetID& asset_id, RasterSurfaceShaderData* data) override;
  LoadInfo LoadMaterial(const AssetID& asset_id, MaterialData* data) override;
  LoadInfo LoadMesh(const AssetID& asset_id, MeshData* data) override;

  // Registers (or replaces) the files making up an asset. Which names are expected depends on
  // the kind of asset: a texture needs "texture" and may have "metadata", a mesh needs "mesh"
  // and "metadata", and so on.
  void SetAssetIDMapData(const AssetID& asset_id, Assets::AssetMap asset_path_map);

private:
  void PathModified(std::string_view path) override;

  // A copy of the files registered for the id (empty if none).
  Assets::AssetMap FilesOf(const AssetID& asset_id) const;

  mutable std::mutex m_files_mutex;
  std::map<AssetID, Assets::AssetMap> m_files_by_asset;

  mutable std::mutex m_owners_mutex;
  std::map<std::string, AssetID, std::less<>> m_asset_by_file;
};
}  // namespace VideoCommon
