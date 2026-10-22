// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/HiresTextures.h"

#include <array>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/System.h"
#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr std::string_view kTexturePrefix = TextureInfo::format_prefix;
constexpr std::string_view kArbitraryMipsMarker = "_arb";

// Everything known about the packs of the running game.
struct PackState
{
  // Textures already created (and, when preloading, already loaded), by id. Declared first so
  // that it is destroyed last, after the asset library.
  std::unordered_map<std::string, std::shared_ptr<HiresTexture>> textures;
  // Every texture id the packs provide, and whether it has hand-made mip levels.
  std::unordered_map<std::string, bool> available;
  // Maps ids to files, and watches the pack folders for edits.
  std::shared_ptr<VideoCommon::DirectFilesystemAssetLibrary> library =
      std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();
};

PackState s_packs;

struct PackMatch
{
  std::string id;
  bool has_arbitrary_mipmaps;
};

// Pack file names may replace either hash with '$' to match any texture or any palette. The
// exact name is preferred, then the any-palette form, then the any-texture form.
std::optional<PackMatch> FindPackEntry(const TextureInfo& texture_info)
{
  if (s_packs.available.empty())
    return std::nullopt;

  const TextureInfo::NameDetails name = texture_info.CalculateTextureName();
  const std::array<std::string, 3> candidates = {
      name.GetFullName(),
      fmt::format("{}_{}_$_{}", name.base_name, name.texture_name, name.format_name),
      fmt::format("{}_${}_{}", name.base_name, name.tlut_name, name.format_name),
  };
  for (const std::string& candidate : candidates)
  {
    const auto found = s_packs.available.find(candidate);
    if (found != s_packs.available.end())
      return PackMatch{candidate, found->second};
  }
  return std::nullopt;
}

// Turns a pack file stem into a texture id, removing (the last) "_arb" marker. Returns whether
// a marker was found.
bool StripArbitraryMipsMarker(std::string& stem)
{
  const size_t marker = stem.rfind(kArbitraryMipsMarker);
  if (marker == std::string::npos)
    return false;
  stem.erase(marker, kArbitraryMipsMarker.size());
  return true;
}

// Registers every texture file in one pack folder. Returns false if any id was already known
// (from this or an earlier folder); the first file seen for an id wins.
bool IndexPackFolder(const std::string& folder)
{
  s_packs.library->Watch(folder);

  static constexpr std::array<std::string_view, 2> kImageExtensions = {".png", ".dds"};
  bool all_new = true;
  for (const std::string& path : Common::DoFileSearch(folder, kImageExtensions, true))
  {
    std::string id;
    SplitPath(path, nullptr, &id, nullptr);
    if (!id.starts_with(kTexturePrefix))
      continue;

    const bool arbitrary_mips = StripArbitraryMipsMarker(id);
    if (!s_packs.available.try_emplace(id, arbitrary_mips).second)
    {
      all_new = false;
      continue;
    }

    // A texture asset is a single file, so the map has just one entry under any name.
    s_packs.library->SetAssetIDMapData(
        id, std::map<std::string, std::filesystem::path>{{"texture", StringToPath(path)}});

    if (g_ActiveConfig.bCacheHiresTextures)
    {
      auto texture = std::make_shared<HiresTexture>(arbitrary_mips, std::move(id));
      static_cast<void>(texture->LoadTexture());
      s_packs.textures.try_emplace(texture->GetId(), std::move(texture));
    }
  }
  return all_new;
}
}  // namespace

void HiresTexture::Update()
{
  if (!g_ActiveConfig.bHiresTextures)
  {
    Clear();
    return;
  }

  const std::string root = File::GetUserPath(D_HIRESTEXTURES_IDX);
  const std::vector<std::string> ids = SConfig::GetInstance().GetGameIDsForTextures();
  for (const std::string& folder : GetTextureDirectoriesForFirstMatchingGameId(root, ids))
  {
    if (!IndexPackFolder(folder))
      ERROR_LOG_FMT(VIDEO, "One or more textures at path '{}' were already inserted", folder);
  }

  const std::vector<std::string> shown_ids = SConfig::GetInstance().GetGameIDsForTextures();
  const std::string game = fmt::format("{}", fmt::join(shown_ids, "' or '"));
  const bool preloaded = g_ActiveConfig.bCacheHiresTextures;
  std::string message =
      preloaded ?
          fmt::format("Preloading '{}' custom textures for '{}'", s_packs.textures.size(), game) :
          fmt::format("Found '{}' custom textures for '{}'", s_packs.available.size(), game);
  OSD::AddMessage(std::move(message), 10000);
}

void HiresTexture::Clear()
{
  s_packs.textures.clear();
  s_packs.available.clear();
  // A fresh library drops all id mappings and folder watches.
  s_packs.library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();
}

void HiresTexture::Shutdown()
{
  Clear();
}

std::shared_ptr<HiresTexture> HiresTexture::Search(const TextureInfo& texture_info)
{
  std::optional<PackMatch> match = FindPackEntry(texture_info);
  if (!match || match->id.empty())
    return nullptr;

  if (const auto known = s_packs.textures.find(match->id); known != s_packs.textures.end())
    return known->second;

  auto texture = std::make_shared<HiresTexture>(match->has_arbitrary_mipmaps, std::move(match->id));
  if (g_ActiveConfig.bCacheHiresTextures)
    s_packs.textures.try_emplace(texture->GetId(), texture);
  return texture;
}

HiresTexture::HiresTexture(bool has_arbitrary_mipmaps, std::string id)
    : m_has_arbitrary_mipmaps(has_arbitrary_mipmaps), m_id(std::move(id))
{
}

VideoCommon::TextureDataResource* HiresTexture::LoadTexture() const
{
  auto& resources = Core::System::GetInstance().GetCustomResourceManager();
  return resources.GetTextureDataFromAsset(m_id, s_packs.library);
}

std::set<std::string> GetTextureDirectoriesWithGameId(const std::string& root_directory,
                                                      const std::string& game_id)
{
  std::set<std::string> folders;
  const std::string short_id = game_id.substr(0, 3);

  // A folder named after the full id, or failing that the region-free three-character id.
  if (const std::string full = root_directory + game_id; File::Exists(full))
  {
    folders.insert(full);
  }
  else if (const std::string region_free = root_directory + short_id; File::Exists(region_free))
  {
    folders.insert(region_free);
  }

  // Marker files: "<root>/<top folder>/.../<id>.txt" selects "<root>/<top folder>".
  for (const std::string& marker : Common::DoFileSearch(root_directory, ".txt", true))
  {
    std::string stem;
    SplitPath(marker, nullptr, &stem, nullptr);
    if (stem != game_id && stem != short_id && stem != "all")
      continue;

    const std::string relative = marker.substr(root_directory.size());
    folders.insert(root_directory + relative.substr(0, relative.find(DIR_SEP_CHR)));
  }
  return folders;
}

std::set<std::string>
GetTextureDirectoriesForFirstMatchingGameId(const std::string& root_directory,
                                            const std::vector<std::string>& game_ids)
{
  for (const std::string& id : game_ids)
  {
    std::set<std::string> folders = GetTextureDirectoriesWithGameId(root_directory, id);
    if (!folders.empty())
      return folders;
  }
  return {};
}
