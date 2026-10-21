// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>

#include <picojson.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/HiresTextures.h"

GraphicsModGroupConfig::GraphicsModGroupConfig(std::string game_id) : m_game_id(std::move(game_id))
{
}

// Copying and moving are member-wise.
GraphicsModGroupConfig::GraphicsModGroupConfig(GraphicsModGroupConfig&&) noexcept = default;
GraphicsModGroupConfig&
GraphicsModGroupConfig::operator=(GraphicsModGroupConfig&&) noexcept = default;
GraphicsModGroupConfig::GraphicsModGroupConfig(const GraphicsModGroupConfig&) = default;
GraphicsModGroupConfig& GraphicsModGroupConfig::operator=(const GraphicsModGroupConfig&) = default;
GraphicsModGroupConfig::~GraphicsModGroupConfig() = default;

void GraphicsModGroupConfig::Load()
{
  const std::string profile = ProfilePath();
  std::set<std::string> from_profile;

  // 1. Mods listed in the profile, with the user's settings applied.
  if (File::Exists(profile))
  {
    picojson::value root;
    std::string error;
    if (!JsonFromFile(profile, &root, &error))
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load graphics mod group json file '{}' due to parse error: {}",
                    profile, error);
      return;
    }
    if (!root.is<picojson::object>())
    {
      ERROR_LOG_FMT(
          VIDEO,
          "Failed to load graphics mod group json file '{}' due to root not being an object!",
          profile);
      return;
    }

    const picojson::value& listed = root.get("mods");
    if (listed.is<picojson::array>())
    {
      for (const picojson::value& entry : listed.get<picojson::array>())
      {
        if (!entry.is<picojson::object>())
          continue;
        const picojson::object& settings = entry.get<picojson::object>();
        std::optional<GraphicsModConfig> mod = GraphicsModConfig::Create(&settings);
        if (!mod)
          continue;
        mod->DeserializeFromProfile(settings);
        from_profile.insert(mod->GetAbsolutePath());
        m_mods.push_back(std::move(*mod));
      }
    }
  }

  // 2. Mods found on disk ("metadata.json" files). Meeting a mod that the profile already
  // listed ends the scan of that directory.
  const auto scan = [&](const std::string& directory, GraphicsModConfig::Source source) {
    for (const std::string& found : Common::DoFileSearch(directory, ".json", true))
    {
      std::string stem;
      SplitPath(found, nullptr, &stem, nullptr);
      if (stem != "metadata")
        continue;

      std::string path = found;
      UnifyPathSeparators(path);
      if (from_profile.contains(path))
        return;

      if (std::optional<GraphicsModConfig> mod = GraphicsModConfig::Create(path, source))
        m_mods.push_back(std::move(*mod));
    }
  };

  for (const std::string& directory :
       GetTextureDirectoriesWithGameId(File::GetUserPath(D_GRAPHICSMOD_IDX), m_game_id))
  {
    scan(directory, GraphicsModConfig::Source::User);
  }
  for (const std::string& directory : GetTextureDirectoriesWithGameId(
           File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR, m_game_id))
  {
    scan(directory, GraphicsModConfig::Source::System);
  }

  // Lower weights first. (Not a stable sort: mods of equal weight keep the standard library's
  // order.)
  std::ranges::sort(m_mods, {}, &GraphicsModConfig::m_weight);
  for (GraphicsModConfig& mod : m_mods)
    m_mods_by_path[mod.GetAbsolutePath()] = &mod;

  ++m_change_count;
}

void GraphicsModGroupConfig::Save() const
{
  const std::string profile = ProfilePath();
  std::ofstream out;
  File::OpenFStream(out, profile, std::ios_base::out);
  if (!out.is_open())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to open graphics mod group json file '{}' for writing", profile);
    return;
  }

  picojson::array mods;
  for (const GraphicsModConfig& mod : m_mods)
  {
    picojson::object settings;
    mod.SerializeToProfile(&settings);
    mods.emplace_back(std::move(settings));
  }
  picojson::object root;
  root.emplace("mods", std::move(mods));
  out << picojson::value{root}.serialize(true);
}

void GraphicsModGroupConfig::SetChangeCount(u32 change_count)
{
  m_change_count = change_count;
}

u32 GraphicsModGroupConfig::GetChangeCount() const
{
  return m_change_count;
}

const std::vector<GraphicsModConfig>& GraphicsModGroupConfig::GetMods() const
{
  return m_mods;
}

std::vector<GraphicsModConfig>& GraphicsModGroupConfig::GetMods()
{
  return m_mods;
}

GraphicsModConfig* GraphicsModGroupConfig::GetMod(std::string_view absolute_path) const
{
  const auto it = m_mods_by_path.find(absolute_path);
  return it != m_mods_by_path.end() ? it->second : nullptr;
}

const std::string& GraphicsModGroupConfig::GetGameID() const
{
  return m_game_id;
}

std::string GraphicsModGroupConfig::ProfilePath() const
{
  return fmt::format("{}{}/{}.json", File::GetUserPath(D_CONFIG_IDX), GRAPHICSMOD_CONFIG_DIR,
                     m_game_id);
}
