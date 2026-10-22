// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/ConfigLoaders/GameConfigLoader.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigLoaders/IsSettingSaveable.h"

namespace ConfigLoaders
{
std::vector<std::string> GetGameIniFilenames(std::string_view id, std::optional<u16> revision)
{
  // Least specific first, so that later files override earlier ones.
  std::vector<std::string> names;
  if (id.empty())
    return names;

  // Only a real six-character game ID has a meaningful system letter (one per Virtual Console
  // platform) and a meaningful three-letter region-independent prefix.
  const bool is_game_id = id.size() == 6;
  if (is_game_id)
  {
    names.push_back(fmt::format("{}.ini", id.substr(0, 1)));
    names.push_back(fmt::format("{}.ini", id.substr(0, 3)));
  }

  names.push_back(fmt::format("{}.ini", id));
  if (revision.has_value())
    names.push_back(fmt::format("{}r{}.ini", id, revision.value()));
  return names;
}

namespace
{
// Game INIs written before the layered config existed use their own section/key names for a few
// settings. These are only translated; new settings use "[System.Section]" headers instead.
struct LegacyKey
{
  std::string_view ini_section;
  std::string_view ini_key;
  Config::Location location;
};

const std::vector<LegacyKey>& LegacyKeys()
{
  static const std::vector<LegacyKey> keys = [] {
    std::vector<LegacyKey> list{
        {"Core", "GameCubeLanguage", Config::MAIN_GC_LANGUAGE.GetLocation()},
        {"Core", "HLE_BS2", Config::MAIN_SKIP_IPL.GetLocation()},
        {"Core", "PAL60", Config::SYSCONF_PAL60.GetLocation()},
        {"Core", "ProgressiveScan", Config::SYSCONF_PROGRESSIVE_SCAN.GetLocation()},
        {"Wii", "Language", Config::SYSCONF_LANGUAGE.GetLocation()},
        {"Wii", "Widescreen", Config::SYSCONF_WIDESCREEN.GetLocation()},
        {"Controls", "WiimoteSourceBB", Config::WIIMOTE_BB_SOURCE.GetLocation()},
    };
    // Pad types 0-3 and Wii Remote sources 0-3.
    for (int i = 0; i < 4; ++i)
    {
      static constexpr std::array<std::string_view, 4> PAD_KEYS{"PadType0", "PadType1", "PadType2",
                                                                "PadType3"};
      list.push_back({"Controls", PAD_KEYS[static_cast<size_t>(i)],
                      Config::GetInfoForSIDevice(i).GetLocation()});
    }
    for (int i = 0; i < 4; ++i)
    {
      static constexpr std::array<std::string_view, 4> SOURCE_KEYS{
          "WiimoteSource0", "WiimoteSource1", "WiimoteSource2", "WiimoteSource3"};
      list.push_back({"Controls", SOURCE_KEYS[static_cast<size_t>(i)],
                      Config::GetInfoForWiimoteSource(i).GetLocation()});
    }
    return list;
  }();
  return keys;
}

// Legacy section names that map a whole section onto one system and section.
struct LegacySection
{
  std::string_view ini_section;
  Config::System system;
  std::string_view section;
};

constexpr std::array<LegacySection, 10> LEGACY_SECTIONS{{
    {"Core", Config::System::Main, "Core"},
    {"DSP", Config::System::Main, "DSP"},
    {"Display", Config::System::Main, "Display"},
    {"Video", Config::System::GFX, "GameSpecific"},
    {"Video_Hardware", Config::System::GFX, "Hardware"},
    {"Video_Settings", Config::System::GFX, "Settings"},
    {"Video_Enhancements", Config::System::GFX, "Enhancements"},
    {"Video_Stereoscopy", Config::System::GFX, "Stereoscopy"},
    {"Video_Hacks", Config::System::GFX, "Hacks"},
    {"Controls", Config::System::GameSettingsOnly, "Controls"},
}};

// The INI names compare case-sensitively; locations compare the way Config::Location does.

const LegacyKey* FindLegacyKey(std::string_view section, std::string_view key)
{
  for (const LegacyKey& entry : LegacyKeys())
  {
    if (entry.ini_section == section && entry.ini_key == key)
      return &entry;
  }
  return nullptr;
}

const LegacySection* FindLegacySection(std::string_view section)
{
  for (const LegacySection& entry : LEGACY_SECTIONS)
  {
    if (entry.ini_section == section)
      return &entry;
  }
  return nullptr;
}

// Splits a "<System>.<Section>" header. The section ends at a second dot, if any. A header
// without a dot, or with nothing after the first dot, isn't of this form.
std::optional<Config::Location> ParseSystemSectionHeader(std::string_view header,
                                                         const std::string& key)
{
  const size_t dot = header.find('.');
  if (dot == std::string_view::npos || dot + 1 == header.size())
    return std::nullopt;

  const std::string_view rest = header.substr(dot + 1);
  const std::string_view section = rest.substr(0, rest.find('.'));
  const std::optional<Config::System> system =
      Config::GetSystemFromName(std::string(header.substr(0, dot)));
  if (!system)
    return std::nullopt;
  return Config::Location{*system, std::string(section), key};
}

// Where a game INI entry goes, or nothing for an entry that can't be placed.
std::optional<Config::Location> LocationOfIniEntry(const std::string& section,
                                                   const std::string& key)
{
  if (const LegacyKey* legacy = FindLegacyKey(section, key))
    return legacy->location;

  if (const LegacySection* legacy = FindLegacySection(section))
    return Config::Location{legacy->system, std::string(legacy->section), key};

  if (auto location = ParseSystemSectionHeader(section, key))
    return location;

  WARN_LOG_FMT(CORE, "Unknown game INI option in section {}: {}", section, key);
  return std::nullopt;
}

// The inverse: the section and key a setting is written under in a game INI.
std::pair<std::string, std::string> IniEntryOfLocation(const Config::Location& location)
{
  for (const LegacyKey& legacy : LegacyKeys())
  {
    if (legacy.location == location)
      return {std::string(legacy.ini_section), std::string(legacy.ini_key)};
  }
  for (const LegacySection& legacy : LEGACY_SECTIONS)
  {
    if (legacy.system == location.system && legacy.section == location.section)
      return {std::string(legacy.ini_section), location.key};
  }
  return {fmt::format("{}.{}", Config::GetSystemName(location.system), location.section),
          location.key};
}

// Game INIs can pick controller profiles: "<Kind>Profile<N> = <name>" in a Controls section
// loads Profiles/<Folder>/<name>.ini as the configuration of controller N.
struct ProfileKind
{
  std::string_view key_prefix;
  std::string_view folder;
  Config::System system;
};

constexpr std::array<ProfileKind, 3> PROFILE_KINDS{{
    {"Pad", "GCPad", Config::System::GCPad},
    {"GBA", "GBA", Config::System::GCPad},
    {"Wiimote", "Wiimote", Config::System::WiiPad},
}};

class GameIniLayerLoader final : public Config::ConfigLayerLoader
{
public:
  GameIniLayerLoader(Config::LayerType layer, std::string id, u16 revision)
      : ConfigLayerLoader(layer), m_game_id(std::move(id)), m_revision(revision)
  {
  }

  void Load(Config::Layer* layer) override
  {
    // The global (shipped) INIs live in the Sys folder, the user's own ones in the user folder.
    const bool shipped = layer->GetLayer() == Config::LayerType::GlobalGame;
    const std::string folder = shipped ? File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP :
                                         File::GetUserPath(D_GAMESETTINGS_IDX);

    Common::IniFile merged;
    for (const std::string& name : GetGameIniFilenames(m_game_id, m_revision))
      merged.Load(folder + name, true);

    for (const Common::IniFile::Section& section : merged.GetSections())
      ApplySection(layer, section);

    ApplyControllerProfiles(layer);
  }

  void Save(Config::Layer* layer) override
  {
    // Only the user's own game INI is ever written.
    if (layer->GetLayer() != Config::LayerType::LocalGame)
      return;

    const std::string& folder = File::GetUserPath(D_GAMESETTINGS_IDX);
    Common::IniFile merged;
    for (const std::string& name : GetGameIniFilenames(m_game_id, m_revision))
      merged.Load(folder + name, true);

    for (const auto& [location, value] : layer->GetLayerMap())
    {
      if (location.system == Config::System::Session || !IsSettingSaveable(location))
        continue;

      const auto [section, key] = IniEntryOfLocation(location);
      if (value)
        merged.GetOrCreateSection(section)->Set(key, *value);
      else
        merged.DeleteKey(section, key);
    }

    // Prefer an existing revision-specific INI. Otherwise write the plain per-game INI; the
    // broader ones (prefix, system letter) are shared with other games and are never written.
    const std::string revision_path = fmt::format("{}{}r{}.ini", folder, m_game_id, m_revision);
    if (File::Exists(revision_path))
      merged.Save(revision_path);
    else
      merged.Save(fmt::format("{}{}.ini", folder, m_game_id));
  }

private:
  static void ApplySection(Config::Layer* layer, const Common::IniFile::Section& section)
  {
    const std::string& name = section.GetName();
    for (const auto& [key, value] : section.GetValues())
    {
      const std::optional<Config::Location> location = LocationOfIniEntry(name, key);
      if (!location || location->system == Config::System::Session)
        continue;
      // A "[System.]" style header with an empty key leaves nothing to address.
      if (location->section.empty() && location->key.empty())
        continue;
      layer->Set(*location, value);
    }
  }

  void ApplyControllerProfiles(Config::Layer* layer) const
  {
    // The all-zero ID is used when no game is running.
    if (m_game_id == "00000000")
      return;

    for (const ProfileKind& kind : PROFILE_KINDS)
    {
      for (char slot = '1'; slot <= '4'; ++slot)
      {
        const Config::Location choice{kind.system, "Controls",
                                      fmt::format("{}Profile{}", kind.key_prefix, slot)};
        const std::optional<std::string> profile_name = layer->Get<std::string>(choice);
        if (!profile_name)
          continue;

        const std::string profile_path = fmt::format(
            "{}Profiles/{}/{}.ini", File::GetUserPath(D_CONFIG_IDX), kind.folder, *profile_name);
        if (!File::Exists(profile_path))
        {
          PanicAlertFmtT("Selected controller profile does not exist");
          continue;
        }

        Common::IniFile profile;
        profile.Load(profile_path);
        const std::string controller_section = fmt::format("{}{}", kind.folder, slot);
        for (const auto& [key, value] : profile.GetOrCreateSection("Profile")->GetValues())
          layer->Set(Config::Location{kind.system, controller_section, key}, value);
      }
    }
  }

  const std::string m_game_id;
  const u16 m_revision;
};
}  // namespace

std::unique_ptr<Config::ConfigLayerLoader> GenerateGlobalGameConfigLoader(const std::string& id,
                                                                          u16 revision)
{
  return std::make_unique<GameIniLayerLoader>(Config::LayerType::GlobalGame, id, revision);
}

std::unique_ptr<Config::ConfigLayerLoader> GenerateLocalGameConfigLoader(const std::string& id,
                                                                         u16 revision)
{
  return std::make_unique<GameIniLayerLoader>(Config::LayerType::LocalGame, id, revision);
}
}  // namespace ConfigLoaders
