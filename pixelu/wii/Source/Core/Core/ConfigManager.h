// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
class IniFile;
}

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace DiscIO
{
enum class Language;
enum class Platform;
enum class Region;
struct Partition;
class Volume;
}  // namespace DiscIO

namespace IOS::ES
{
class TMDReader;
}  // namespace IOS::ES

struct BootParameters;

// Game ID used while nothing is running.
static constexpr std::string_view DEFAULT_GAME_ID = "00000000";

// Session-wide state that is not a Config setting: what is running (IDs,
// title, revision, region) and the paths and flags chosen at boot.
struct SConfig
{
  // Boot and JIT switches set by the front end.
  bool bBootToPause = false;
  bool bJITNoBlockCache = false;
  bool bJITNoBlockLinking = false;

  // NetPlay: copy the host's Wii saves into the session.
  bool bCopyWiiSaveNetplay = true;

  // Region of the running game, after fallbacks.
  DiscIO::Region m_region;

  std::string m_strBootROM;
  std::string m_strSRAM;
  // Name used for the debugger's per-game files (map files and so on).
  std::string m_debugger_game_id;

  // Set while a disc launched from the game list runs; only the ES ticket
  // view workaround reads it.
  bool m_disc_booted_from_game_list = false;

  const std::string GetGameID() const;
  const std::string GetGameTDBID() const;
  const std::string GetGameIDElfDol() const;

  // IDs to look for custom textures under, best first: "<elf/dol id>-<game id>"
  // and the game ID for an ELF/DOL, otherwise just the game ID.
  const std::vector<std::string> GetGameIDsForTextures() const;
  // The first of GetGameIDsForTextures().
  const std::string GetGameIDForTextures() const;

  const std::string GetTitleName() const;
  const std::string GetTitleDescription() const;
  u64 GetTitleID() const;
  u16 GetRevision() const;
  u32 GetSimulatedMemorySize() const;
  void ResetRunningGameMetadata();
  void SetRunningGameMetadata(const DiscIO::Volume& volume, const DiscIO::Partition& partition);
  void SetRunningGameMetadata(const IOS::ES::TMDReader& tmd, DiscIO::Platform platform);
  void SetRunningGameMetadata(const std::string& game_id);

  void SetElfDolID(const std::string& game_id);

  // A title was started directly (not through ES): reload map files, HLE
  // hooks, patches and, for GameCube, textures.
  static void OnTitleDirectlyBooted(const Core::CPUThreadGuard& guard);

  // ES switched titles (Wii only): reload controller settings and textures.
  static void OnESTitleChanged();

  void LoadDefaults();
  static std::string MakeGameID(std::string_view file_name);
  bool SetPathsAndGameMetadata(Core::System& system, const BootParameters& boot);
  DiscIO::Language GetCurrentLanguage(bool wii) const;
  DiscIO::Language GetLanguageAdjustedForRegion(bool wii, DiscIO::Region region) const;
  std::string GetGameTDBImageRegionCode(bool wii, DiscIO::Region region) const;

  Common::IniFile LoadDefaultGameIni() const;
  Common::IniFile LoadLocalGameIni() const;
  Common::IniFile LoadGameIni() const;

  static Common::IniFile LoadDefaultGameIni(std::string_view id, std::optional<u16> revision);
  static Common::IniFile LoadLocalGameIni(std::string_view id, std::optional<u16> revision);
  static Common::IniFile LoadGameIni(std::string_view id, std::optional<u16> revision);

  SConfig(const SConfig&) = delete;
  SConfig& operator=(const SConfig&) = delete;
  SConfig(SConfig&&) = delete;
  SConfig& operator=(SConfig&&) = delete;

  // Write or read Dolphin.ini and the other config files.
  void SaveSettings();
  void LoadSettings();

  // Deletes the config files and clears every config layer.
  static void ResetAllSettings();

  // The single instance, valid between Init() and Shutdown().
  static SConfig& GetInstance() { return *s_instance; }
  static void Init();
  static void Shutdown();

private:
  SConfig();
  ~SConfig();

  static void ReloadTextures(Core::System& system);

  void SetRunningGameMetadata(const std::string& game_id, const std::string& gametdb_id,
                              u64 title_id, u16 revision, DiscIO::Region region,
                              u32 simulated_memory_size);

  static SConfig* s_instance;

  // Guards the running-game fields below; recursive because the setters call each other.
  mutable std::recursive_mutex m_metadata_mutex;
  std::string m_game_id;
  std::string m_elf_dol_id;
  std::string m_gametdb_id;
  std::string m_title_name;
  std::string m_title_description;
  u64 m_title_id = 0;
  u16 m_revision = 0;
  // MEM1 size the game asked for in its header (0 for the default).
  u32 m_simulated_memory_size = 0;
};
