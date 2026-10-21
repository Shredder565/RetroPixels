// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/TextureAssetUtils.h"

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace VideoCommon
{
namespace
{
using Level = CustomTextureData::ArraySlice::Level;

// Appends levels from "<stem>_mip<N><ext>" files, starting at N = current level count, until
// the next file does not exist.
bool AppendMipFiles(const std::filesystem::path& asset_path, CustomTextureData::ArraySlice* slice)
{
  if (slice == nullptr) [[unlikely]]
    return false;

  std::string directory;
  std::string stem;
  std::string extension;
  SplitPath(PathToString(asset_path), &directory, &stem, &extension);

  std::string kind = extension;
  Common::ToLower(&kind);
  const bool is_dds = kind == ".dds";
  const bool is_png = kind == ".png";

  for (auto mip = static_cast<u32>(slice->m_levels.size());; ++mip)
  {
    const std::string mip_name = fmt::format("{}_mip{}", stem, mip);
    const std::string mip_path = directory + mip_name + extension;
    if (!File::Exists(mip_path))
      return true;

    if (!is_dds && !is_png)
    {
      ERROR_LOG_FMT(VIDEO, "Custom mipmap '{}' has unsupported extension", mip_name);
      return false;
    }

    Level level;
    const bool loaded =
        is_dds ? LoadDDSTexture(&level, mip_path, mip) : LoadPNGTexture(&level, mip_path);
    if (!loaded)
    {
      ERROR_LOG_FMT(VIDEO, "Custom mipmap '{}' failed to load", mip_name);
      return false;
    }
    slice->m_levels.push_back(std::move(level));
  }
}

CustomTextureData::ArraySlice& FirstSlice(CustomTextureData* data)
{
  if (data->m_slices.empty())
    data->m_slices.emplace_back();
  return data->m_slices.front();
}
}  // namespace

bool LoadTextureDataFromFile(const CustomAssetLibrary::AssetID& asset_id,
                             const std::filesystem::path& asset_path, AbstractTextureType type,
                             CustomTextureData* data)
{
  std::string ext = PathToString(asset_path.extension());
  Common::ToLower(&ext);

  if (ext == ".dds")
  {
    if (!LoadDDSTexture(data, PathToString(asset_path)))
    {
      ERROR_LOG_FMT(VIDEO, "Asset '{}' error - could not load dds texture!", asset_id);
      return false;
    }
    // Extra mip files only ever extend the first slice.
    return AppendMipFiles(asset_path, &FirstSlice(data));
  }

  if (ext == ".png")
  {
    // A PNG holds one 2D image; arrays and cube maps need DDS.
    if (type != AbstractTextureType::Texture_2D)
    {
      ERROR_LOG_FMT(VIDEO, "Asset '{}' error - PNG is not supported for texture type '{}'!",
                    asset_id, type);
      return false;
    }

    CustomTextureData::ArraySlice& slice = FirstSlice(data);
    if (slice.m_levels.empty())
      slice.m_levels.emplace_back();

    if (!LoadPNGTexture(&slice.m_levels.front(), PathToString(asset_path)))
    {
      ERROR_LOG_FMT(VIDEO, "Asset '{}' error - could not load png texture!", asset_id);
      return false;
    }
    return AppendMipFiles(asset_path, &slice);
  }

  ERROR_LOG_FMT(VIDEO, "Asset '{}' error - extension '{}' unknown!", asset_id, ext);
  return false;
}

bool ValidateTextureData(const CustomAssetLibrary::AssetID& asset_id, const CustomTextureData& data,
                         u32 native_width, u32 native_height)
{
  switch (data.m_slices.size())
  {
  case 0:
    ERROR_LOG_FMT(VIDEO,
                  "Texture data can't be validated for asset '{}' because no data was available.",
                  asset_id);
    return false;
  case 1:
    break;
  default:
    ERROR_LOG_FMT(
        VIDEO,
        "Texture data can't be validated for asset '{}' because it has more slices than expected.",
        asset_id);
    return false;
  }

  const auto& levels = data.m_slices.front().m_levels;
  if (levels.empty())
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Texture data can't be validated for asset '{}' because first slice has no data available.",
        asset_id);
    return false;
  }

  // Both checks below are warnings only: such textures have always been accepted.
  const Level& top = levels.front();
  const bool same_aspect = top.width * native_height == top.height * native_width;
  if (!same_aspect)
  {
    WARN_LOG_FMT(VIDEO,
                 "Invalid texture data size {}x{} for asset '{}'. The aspect differs "
                 "from the native size {}x{}.",
                 top.width, top.height, asset_id, native_width, native_height);
  }

  const bool has_native_size = native_width != 0 && native_height != 0;
  if (has_native_size && (top.width % native_width != 0 || top.height % native_height != 0))
  {
    WARN_LOG_FMT(VIDEO,
                 "Invalid texture data size {}x{} for asset '{}'. Please use an integer "
                 "upscaling factor based on the native size {}x{}.",
                 top.width, top.height, asset_id, native_width, native_height);
  }

  return true;
}

bool PurgeInvalidMipsFromTextureData(const CustomAssetLibrary::AssetID& asset_id,
                                     CustomTextureData* data)
{
  for (std::size_t slice_index = 0; slice_index < data->m_slices.size(); ++slice_index)
  {
    auto& levels = data->m_slices[slice_index].m_levels;
    if (levels.empty())
      continue;

    // Find the first level that breaks the halving chain; it and everything after it go.
    u32 expect_w = levels.front().width;
    u32 expect_h = levels.front().height;
    std::size_t keep = levels.size();
    for (std::size_t mip = 1; mip < levels.size(); ++mip)
    {
      if (expect_w == 1 && expect_h == 1)
      {
        ERROR_LOG_FMT(VIDEO,
                      "Custom game texture {} has too many 1x1 mipmaps for slice {}. Skipping "
                      "extra levels.",
                      asset_id, slice_index);
        keep = mip;
        break;
      }

      expect_w = std::max(expect_w / 2, 1u);
      expect_h = std::max(expect_h / 2, 1u);
      const Level& level = levels[mip];
      if (level.width != expect_w || level.height != expect_h)
      {
        ERROR_LOG_FMT(VIDEO,
                      "Invalid custom game texture size {}x{} for texture asset {}. Slice {} with "
                      "mipmap level {} must be {}x{}.",
                      level.width, level.height, asset_id, slice_index, mip, expect_w, expect_h);
        keep = mip;
        break;
      }
    }
    levels.resize(keep);

    const AbstractTextureFormat format = levels.front().format;
    const bool mixed =
        std::ranges::any_of(levels, [format](const Level& l) { return l.format != format; });
    if (mixed)
    {
      ERROR_LOG_FMT(
          VIDEO, "Custom game texture {} has inconsistent formats across mip levels for slice {}.",
          asset_id, slice_index);
      return false;
    }
  }

  return true;
}
}  // namespace VideoCommon
