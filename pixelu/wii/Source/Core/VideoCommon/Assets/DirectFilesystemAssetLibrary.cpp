// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"

#include <optional>
#include <system_error>
#include <vector>

#include <fmt/std.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/System.h"
#include "VideoCommon/Assets/MaterialAsset.h"
#include "VideoCommon/Assets/MeshAsset.h"
#include "VideoCommon/Assets/ShaderAsset.h"
#include "VideoCommon/Assets/TextureAsset.h"
#include "VideoCommon/Assets/TextureAssetUtils.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/Resources/CustomResourceManager.h"

namespace VideoCommon
{
namespace
{
using AssetID = CustomAssetLibrary::AssetID;
using LoadInfo = CustomAssetLibrary::LoadInfo;

std::size_t TotalLevelBytes(const CustomTextureData& data)
{
  std::size_t bytes = 0;
  for (const auto& slice : data.m_slices)
  {
    for (const auto& level : slice.m_levels)
      bytes += level.data.size();
  }
  return bytes;
}

// The file's size, or nothing (after logging) when it cannot be determined. `what` names the
// file in the message.
std::optional<std::size_t> SizeOf(const AssetID& asset_id, const std::filesystem::path& path,
                                  std::string_view what)
{
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error - failed to get {} file size with error '{}'!", asset_id,
                  what, ec);
    return std::nullopt;
  }
  return static_cast<std::size_t>(size);
}

// Parses a JSON file whose root must be an object. The two `lead` strings are spliced into
// the error messages after "error - ".
bool ReadJsonObjectFile(const AssetID& asset_id, const std::filesystem::path& path,
                        std::string_view parse_lead, std::string_view root_lead,
                        picojson::value* root)
{
  const std::string file = PathToString(path);
  std::string error;
  if (!JsonFromFile(file, root, &error))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' error - {}failed to load the json file '{}', due to parse error: {}",
                  asset_id, parse_lead, file, error);
    return false;
  }
  if (!root->is<picojson::object>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' error - {}failed to load the json file '{}', due to root not being "
                  "an object!",
                  asset_id, root_lead, file);
    return false;
  }
  return true;
}

bool ReadMetadata(const AssetID& asset_id, const std::filesystem::path& path, picojson::value* root)
{
  return ReadJsonObjectFile(asset_id, path, " ", " ", root);
}

// Looks up a required named file of an asset.
const std::filesystem::path* Require(const Assets::AssetMap& files, const std::string& name)
{
  const auto it = files.find(name);
  return it != files.end() ? &it->second : nullptr;
}
}  // namespace

CustomAssetLibrary::LoadInfo
DirectFilesystemAssetLibrary::LoadRasterSurfaceShader(const AssetID& asset_id,
                                                      RasterSurfaceShaderData* data)
{
  // A shader is its metadata plus one source file per stage.
  const Assets::AssetMap files = FilesOf(asset_id);
  if (files.size() != 3)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have three files mapped!", asset_id);
    return {};
  }

  const std::filesystem::path* metadata = Require(files, "metadata");
  if (metadata == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a metadata entry mapped!", asset_id);
    return {};
  }
  const std::filesystem::path* vertex = Require(files, "vertex_shader");
  if (vertex == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a vertex shader entry mapped!", asset_id);
    return {};
  }
  const std::filesystem::path* pixel = Require(files, "pixel_shader");
  if (pixel == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a pixel shader entry mapped!", asset_id);
    return {};
  }

  const auto metadata_size = SizeOf(asset_id, *metadata, "shader metadata");
  if (!metadata_size)
    return {};
  const auto vertex_size = SizeOf(asset_id, *vertex, "vertex shader source");
  if (!vertex_size)
    return {};
  const auto pixel_size = SizeOf(asset_id, *pixel, "pixel shader source");
  if (!pixel_size)
    return {};

  if (!File::ReadFileToString(PathToString(*vertex), data->vertex_source))
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error -  failed to load the vertex shader file '{}',",
                  asset_id, PathToString(*vertex));
    return {};
  }
  if (!File::ReadFileToString(PathToString(*pixel), data->pixel_source))
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error -  failed to load the pixel shader file '{}',", asset_id,
                  PathToString(*pixel));
    return {};
  }

  picojson::value root;
  if (!ReadMetadata(asset_id, *metadata, &root))
    return {};
  if (!RasterSurfaceShaderData::FromJson(asset_id, root.get<picojson::object>(), data))
    return {};

  // Includes resolve next to the pixel shader first, then in the built-in shader library.
  const std::string builtin_shaders =
      File::GetSysDirectory() + GRAPHICSMOD_DIR + "/Builtin" + "/Shaders";
  data->shader_includer = std::make_unique<VideoCommon::ShaderIncluder>(
      PathToString(pixel->parent_path()), builtin_shaders);

  return LoadInfo{*metadata_size + *vertex_size + *pixel_size};
}

CustomAssetLibrary::LoadInfo DirectFilesystemAssetLibrary::LoadMaterial(const AssetID& asset_id,
                                                                        MaterialData* data)
{
  const Assets::AssetMap files = FilesOf(asset_id);
  if (files.size() != 1)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error - material expected to have one file mapped!", asset_id);
    return {};
  }
  const std::filesystem::path& path = files.begin()->second;

  const auto size = SizeOf(asset_id, path, "material");
  if (!size)
    return {};

  picojson::value root;
  if (!ReadJsonObjectFile(asset_id, path, " material ", "material ", &root))
    return {};

  if (!MaterialData::FromJson(asset_id, root.get<picojson::object>(), data))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' error -  material failed to load the json file '{}', as material "
                  "json could not be parsed!",
                  asset_id, PathToString(path));
    return {};
  }

  return LoadInfo{*size};
}

CustomAssetLibrary::LoadInfo DirectFilesystemAssetLibrary::LoadMesh(const AssetID& asset_id,
                                                                    MeshData* data)
{
  // A mesh is the binary mesh file plus its metadata.
  const Assets::AssetMap files = FilesOf(asset_id);
  if (files.size() != 2)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have two files mapped!", asset_id);
    return {};
  }

  const std::filesystem::path* metadata = Require(files, "metadata");
  const std::filesystem::path* mesh = Require(files, "mesh");
  if (metadata == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a metadata entry mapped!", asset_id);
    return {};
  }
  if (mesh == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a mesh entry mapped!", asset_id);
    return {};
  }

  const auto metadata_size = SizeOf(asset_id, *metadata, "mesh metadata");
  if (!metadata_size)
    return {};
  const auto mesh_size = SizeOf(asset_id, *mesh, "mesh");
  if (!mesh_size)
    return {};

  const std::string mesh_file = PathToString(*mesh);
  File::IOFile file(mesh_file, "rb");
  if (!file.IsOpen())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error - failed to open mesh file '{}'!", asset_id, mesh_file);
    return {};
  }

  std::vector<u8> contents(static_cast<std::size_t>(file.GetSize()));
  file.ReadBytes(contents.data(), contents.size());
  if (!MeshData::FromDolphinMesh(contents, data))
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error -  failed to load the mesh file '{}'!", asset_id,
                  mesh_file);
    return {};
  }

  picojson::value root;
  if (!ReadMetadata(asset_id, *metadata, &root))
    return {};
  if (!MeshData::FromJson(asset_id, root.get<picojson::object>(), data))
    return {};

  return LoadInfo{*metadata_size + *mesh_size};
}

CustomAssetLibrary::LoadInfo DirectFilesystemAssetLibrary::LoadTexture(const AssetID& asset_id,
                                                                       CustomTextureData* data)
{
  const Assets::AssetMap files = FilesOf(asset_id);
  if (files.empty())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error - raw texture expected to have one or two files mapped!",
                  asset_id);
    return {};
  }

  const std::filesystem::path* texture = Require(files, "texture");
  if (texture == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a texture entry mapped!", asset_id);
    return {};
  }

  if (!LoadTextureDataFromFile(asset_id, *texture, AbstractTextureType::Texture_2D, data) ||
      !PurgeInvalidMipsFromTextureData(asset_id, data))
  {
    return {};
  }
  return LoadInfo{TotalLevelBytes(*data)};
}

CustomAssetLibrary::LoadInfo DirectFilesystemAssetLibrary::LoadTexture(const AssetID& asset_id,
                                                                       TextureAndSamplerData* data)
{
  // The metadata file is optional.
  const Assets::AssetMap files = FilesOf(asset_id);
  if (files.empty() || files.size() > 2)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' error - raw texture expected to have one or two files mapped!",
                  asset_id);
    return {};
  }

  const std::filesystem::path* metadata = Require(files, "metadata");
  const std::filesystem::path* texture = Require(files, "texture");
  if (texture == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' expected to have a texture entry mapped!", asset_id);
    return {};
  }

  std::size_t metadata_size = 0;
  if (metadata == nullptr)
  {
    // Without metadata it is a plain 2D texture sampled linearly.
    data->sampler = RenderState::GetLinearSamplerState();
    data->type = AbstractTextureType::Texture_2D;
  }
  else
  {
    const auto size = SizeOf(asset_id, *metadata, "texture metadata");
    if (!size)
      return {};
    metadata_size = *size;

    picojson::value root;
    if (!ReadMetadata(asset_id, *metadata, &root))
      return {};
    if (!TextureAndSamplerData::FromJson(asset_id, root.get<picojson::object>(), data))
      return {};
  }

  if (!LoadTextureDataFromFile(asset_id, *texture, data->type, &data->texture_data) ||
      !PurgeInvalidMipsFromTextureData(asset_id, &data->texture_data))
  {
    return {};
  }
  return LoadInfo{TotalLevelBytes(data->texture_data) + metadata_size};
}

void DirectFilesystemAssetLibrary::SetAssetIDMapData(const AssetID& asset_id,
                                                     VideoCommon::Assets::AssetMap asset_path_map)
{
  Assets::AssetMap old_files;
  {
    std::lock_guard guard(m_files_mutex);
    old_files = m_files_by_asset[asset_id];
  }

  {
    // Re-point the reverse lookup used for change notifications.
    std::lock_guard guard(m_owners_mutex);
    for (const auto& entry : old_files)
      m_asset_by_file.erase(PathToString(entry.second));
    for (const auto& entry : asset_path_map)
      m_asset_by_file[PathToString(entry.second)] = asset_id;
  }

  std::lock_guard guard(m_files_mutex);
  m_files_by_asset[asset_id] = std::move(asset_path_map);
}

void DirectFilesystemAssetLibrary::PathModified(std::string_view path)
{
  std::lock_guard guard(m_owners_mutex);
  const auto it = m_asset_by_file.find(path);
  if (it == m_asset_by_file.end())
    return;
  Core::System::GetInstance().GetCustomResourceManager().MarkAssetDirty(it->second);
}

Assets::AssetMap DirectFilesystemAssetLibrary::FilesOf(const AssetID& asset_id) const
{
  std::lock_guard guard(m_files_mutex);
  const auto it = m_files_by_asset.find(asset_id);
  return it != m_files_by_asset.end() ? it->second : Assets::AssetMap{};
}
}  // namespace VideoCommon
