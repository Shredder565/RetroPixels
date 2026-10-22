// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/ConfigManager.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/VariantUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/DefaultLocale.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/Host.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"
#include "Core/TitleDatabase.h"
#include "Core/WC24PatchEngine.h"

#include "VideoCommon/HiresTextures.h"

#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeWad.h"

SConfig* SConfig::s_instance;

namespace
{
using MetadataLock = std::lock_guard<std::recursive_mutex>;

// Loads each of the game's INI files (most generic first) from `directory`
// into `ini`, later files overriding earlier ones.
void MergeGameInis(Common::IniFile& ini, const std::string& directory, std::string_view id,
                   std::optional<u16> revision)
{
  for (const std::string& file_name : ConfigLoaders::GetGameIniFilenames(id, revision))
    ini.Load(directory + file_name, true);
}

std::string SysGameSettingsDir()
{
  return File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP;
}
}  // namespace

// --- Lifetime -------------------------------------------------------------------

SConfig::SConfig()
{
  LoadDefaults();
  LoadSettings();
}

SConfig::~SConfig()
{
  SaveSettings();
}

void SConfig::Init()
{
  s_instance = new SConfig;
}

void SConfig::Shutdown()
{
  delete s_instance;
  s_instance = nullptr;
}

void SConfig::SaveSettings()
{
  NOTICE_LOG_FMT(BOOT, "Saving settings to {}", File::GetUserPath(F_DOLPHINCONFIG_IDX));
  Config::Save();
}

void SConfig::LoadSettings()
{
  INFO_LOG_FMT(BOOT, "Loading Settings from {}", File::GetUserPath(F_DOLPHINCONFIG_IDX));
  Config::Load();
}

void SConfig::ResetAllSettings()
{
  // Callbacks fire once, at the end.
  Config::ConfigChangeCallbackGuard batch;

  for (const unsigned int file_index :
       {F_DOLPHINCONFIG_IDX, F_GFXCONFIG_IDX, F_LOGGERCONFIG_IDX, F_DUALSHOCKUDPCLIENTCONFIG_IDX,
        F_FREELOOKCONFIG_IDX, F_RETROACHIEVEMENTSCONFIG_IDX, F_WIISYSCONF_IDX})
  {
    File::Delete(File::GetUserPath(file_index));
  }

  for (const Config::LayerType type : Config::SEARCH_ORDER)
  {
    if (const std::shared_ptr<Config::Layer> layer = Config::GetLayer(type))
      layer->DeleteAllKeys();
  }

  Config::OnConfigChanged();
}

void SConfig::LoadDefaults()
{
  bBootToPause = false;

  Core::System& system = Core::System::GetInstance();
  system.SetIsWii(false);
  system.SetIsTriforce(false);

  ResetRunningGameMetadata();
}

// --- Running game metadata --------------------------------------------------------

const std::string SConfig::GetGameID() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_game_id;
}

const std::string SConfig::GetGameTDBID() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_gametdb_id;
}

const std::string SConfig::GetGameIDElfDol() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_elf_dol_id;
}

const std::vector<std::string> SConfig::GetGameIDsForTextures() const
{
  MetadataLock lock(m_metadata_mutex);
  std::vector<std::string> ids;
  if (!m_elf_dol_id.empty())
    ids.push_back(fmt::format("{}-{}", m_elf_dol_id, m_game_id));
  ids.push_back(m_game_id);
  return ids;
}

const std::string SConfig::GetGameIDForTextures() const
{
  return GetGameIDsForTextures().front();
}

const std::string SConfig::GetTitleName() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_title_name;
}

const std::string SConfig::GetTitleDescription() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_title_description;
}

u64 SConfig::GetTitleID() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_title_id;
}

u16 SConfig::GetRevision() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_revision;
}

u32 SConfig::GetSimulatedMemorySize() const
{
  MetadataLock lock(m_metadata_mutex);
  return m_simulated_memory_size;
}

void SConfig::ResetRunningGameMetadata()
{
  MetadataLock lock(m_metadata_mutex);
  SetRunningGameMetadata(std::string(DEFAULT_GAME_ID), "", 0, 0, DiscIO::Region::Unknown, 0);
  SetElfDolID("");
}

void SConfig::SetRunningGameMetadata(const DiscIO::Volume& volume,
                                     const DiscIO::Partition& partition)
{
  MetadataLock lock(m_metadata_mutex);

  // The volume's own getters (no partition) describe its game partition.
  if (partition == volume.GetGamePartition())
  {
    SetRunningGameMetadata(volume.GetGameID(), volume.GetGameTDBID(),
                           volume.GetTitleID().value_or(0), volume.GetRevision().value_or(0),
                           volume.GetRegion(), volume.GetSimulatedMemorySize());
    return;
  }

  SetRunningGameMetadata(volume.GetGameID(partition), volume.GetGameTDBID(partition),
                         volume.GetTitleID(partition).value_or(0),
                         volume.GetRevision(partition).value_or(0), volume.GetRegion(),
                         volume.GetSimulatedMemorySize());
}

void SConfig::SetRunningGameMetadata(const IOS::ES::TMDReader& tmd, DiscIO::Platform platform)
{
  MetadataLock lock(m_metadata_mutex);
  const u64 title_id = tmd.GetTitleId();

  // ES launches disc games with only a TMD. For those the disc header is the
  // better source (its revision can differ from the TMD's), so let the DVD
  // interface fill the metadata in when it has that disc.
  const bool from_disc =
      platform != DiscIO::Platform::WiiWAD &&
      Core::System::GetInstance().GetDVDInterface().UpdateRunningGameMetadata(title_id);
  if (from_disc)
    return;

  SetRunningGameMetadata(tmd.GetGameID(), tmd.GetGameTDBID(), title_id, tmd.GetTitleVersion(),
                         tmd.GetRegion(), 0);
}

void SConfig::SetRunningGameMetadata(const std::string& game_id)
{
  MetadataLock lock(m_metadata_mutex);
  SetRunningGameMetadata(game_id, "", 0, 0, DiscIO::Region::Unknown, 0);
}

void SConfig::SetRunningGameMetadata(const std::string& game_id, const std::string& gametdb_id,
                                     u64 title_id, u16 revision, DiscIO::Region region,
                                     u32 simulated_memory_size)
{
  MetadataLock lock(m_metadata_mutex);

  // The region is not part of what counts as a different game.
  const bool same_game = m_game_id == game_id && m_gametdb_id == gametdb_id &&
                         m_title_id == title_id && m_revision == revision &&
                         m_simulated_memory_size == simulated_memory_size;

  m_game_id = game_id;
  m_gametdb_id = gametdb_id;
  m_title_id = title_id;
  m_revision = revision;
  m_simulated_memory_size = simulated_memory_size;

  // Six-character disc IDs name the debugger files; otherwise the title ID does.
  if (game_id.size() == 6)
    m_debugger_game_id = game_id;
  else if (title_id != 0)
    m_debugger_game_id = fmt::format("{:08X}_{:08X}", static_cast<u32>(title_id >> 32),
                                     static_cast<u32>(title_id));
  else
    m_debugger_game_id.clear();

  if (same_game)
    return;

  if (game_id == DEFAULT_GAME_ID)
  {
    m_title_name.clear();
    m_title_description.clear();
    return;
  }

  AchievementManager::GetInstance().CloseGame();

  Core::System& system = Core::System::GetInstance();
  {
    const Core::TitleDatabase names;
    const DiscIO::Language language = GetLanguageAdjustedForRegion(system.IsWii(), region);
    m_title_name = names.GetTitleName(m_gametdb_id, language);
    m_title_description = names.Describe(m_gametdb_id, language);
  }
  NOTICE_LOG_FMT(CORE, "Active title: {}", m_title_description);
  Host_TitleChanged();

  const bool active = Core::IsRunningOrStarting(system);
  if (active)
    Core::UpdateTitle(system);

  Config::AddLayer(ConfigLoaders::GenerateGlobalGameConfigLoader(game_id, revision));
  Config::AddLayer(ConfigLoaders::GenerateLocalGameConfigLoader(game_id, revision));

  if (active)
    DolphinAnalytics::Instance().ReportGameStart();
}

void SConfig::SetElfDolID(const std::string& game_id)
{
  MetadataLock lock(m_metadata_mutex);
  m_elf_dol_id = game_id;
}

// --- Title change hooks --------------------------------------------------------------

void SConfig::OnESTitleChanged()
{
  Core::System& system = Core::System::GetInstance();
  Pad::LoadConfig();
  Keyboard::LoadConfig();
  // With Bluetooth passthrough the Wii Remotes are real ones.
  if (system.IsWii() && !Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    Wiimote::LoadConfig();

  ReloadTextures(system);
}

void SConfig::OnTitleDirectlyBooted(const Core::CPUThreadGuard& guard)
{
  Core::System& system = guard.GetSystem();
  if (!Core::IsRunningOrStarting(system))
    return;

  if (system.GetPPCSymbolDB().LoadMapOnBoot(guard))
    Host_PPCSymbolsChanged();
  HLE::Reload(system);
  PatchEngine::Reload(system);
  WC24PatchEngine::Reload();

  // Wii titles get their textures from OnESTitleChanged.
  if (!system.IsWii())
    ReloadTextures(system);
}

void SConfig::ReloadTextures(Core::System& system)
{
  Pad::GenerateDynamicInputTextures();
  Keyboard::GenerateDynamicInputTextures();
  if (system.IsWii() && !Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    Wiimote::GenerateDynamicInputTextures();

  HiresTexture::Update();
}

// --- Boot ---------------------------------------------------------------------

std::string SConfig::MakeGameID(std::string_view file_name)
{
  // "ID-" plus the file name without its last extension.
  const size_t dot = file_name.rfind('.');
  return "ID-" + std::string(file_name.substr(0, dot));
}

bool SConfig::SetPathsAndGameMetadata(Core::System& system, const BootParameters& boot)
{
  system.SetIsMIOS(false);
  m_disc_booted_from_game_list = false;

  const auto from_disc = [&](const BootParameters::Disc& disc) {
    const DiscIO::Platform platform = disc.volume->GetVolumeType();
    m_region = disc.volume->GetRegion();
    system.SetIsWii(platform == DiscIO::Platform::WiiDisc);
    system.SetIsTriforce(disc.volume->GetVolumeType() == DiscIO::Platform::Triforce);
    m_disc_booted_from_game_list = true;
    SetRunningGameMetadata(*disc.volume, disc.volume->GetGamePartition());
    return true;
  };

  const auto from_executable = [&](const BootParameters::Executable& executable) {
    if (!executable.reader->IsValid())
      return false;

    m_region = DiscIO::Region::Unknown;
    system.SetIsWii(executable.reader->IsWii());

    SplitPath(executable.path, nullptr, &m_debugger_game_id, nullptr);

    // The game ID comes from the file name, whatever kind of slashes the path uses.
    std::string path = executable.path;
    std::ranges::replace(path, '\\', '/');
    const std::string id = MakeGameID(PathToFileName(path));
    SetRunningGameMetadata(id);
    SetElfDolID(id);

    Host_TitleChanged();
    return true;
  };

  const auto from_wad = [&](const DiscIO::VolumeWAD& wad) {
    const IOS::ES::TMDReader& tmd = wad.GetTMD();
    if (!tmd.IsValid())
    {
      PanicAlertFmtT("This WAD is not valid.");
      return false;
    }
    if (!IOS::ES::IsChannel(tmd.GetTitleId()))
    {
      PanicAlertFmtT("This WAD is not bootable.");
      return false;
    }

    m_region = tmd.GetRegion();
    system.SetIsWii(true);
    SetRunningGameMetadata(tmd, DiscIO::Platform::WiiWAD);
    return true;
  };

  const auto from_nand = [&](const BootParameters::NANDTitle& title) {
    IOS::HLE::Kernel kernel;
    const IOS::ES::TMDReader tmd = kernel.GetESCore().FindInstalledTMD(title.id);
    if (!tmd.IsValid() || !IOS::ES::IsChannel(title.id))
    {
      PanicAlertFmtT("This title cannot be booted.");
      return false;
    }

    m_region = tmd.GetRegion();
    system.SetIsWii(true);
    SetRunningGameMetadata(tmd, DiscIO::Platform::WiiWAD);
    return true;
  };

  const auto from_ipl = [&](const BootParameters::IPL& ipl) {
    m_region = ipl.region;
    system.SetIsWii(false);
    Host_TitleChanged();
    return true;
  };

  const auto from_fifo_log = [&](const BootParameters::DFF& dff) {
    const std::unique_ptr<FifoDataFile> log(FifoDataFile::Load(dff.dff_path, true));
    if (!log)
      return false;

    m_region = DiscIO::Region::NTSC_U;
    system.SetIsWii(log->GetIsWii());

    const std::string& id = log->GetGameId();
    if (id == DEFAULT_GAME_ID)
      Host_TitleChanged();
    else
      SetRunningGameMetadata(id);
    return true;
  };

  const bool ok = std::visit(
      overloaded{from_disc, from_executable, from_wad, from_nand, from_ipl, from_fifo_log},
      boot.parameters);
  if (!ok)
    return false;

  if (m_region == DiscIO::Region::Unknown)
    m_region = Config::Get(Config::MAIN_FALLBACK_REGION);
  // Triforce boards always count as development hardware.
  if (system.IsTriforce())
    m_region = DiscIO::Region::DEV;

  const std::string region_dir = Config::GetDirectoryForRegion(Config::ToGameCubeRegion(m_region));
  m_strSRAM = File::GetUserPath(F_GCSRAM_IDX);
  m_strBootROM = Config::GetBootROMPath(region_dir);
  return true;
}

// --- Languages and regions ------------------------------------------------------

DiscIO::Language SConfig::GetCurrentLanguage(bool wii) const
{
  const DiscIO::Language language =
      wii ? static_cast<DiscIO::Language>(Config::Get(Config::SYSCONF_LANGUAGE)) :
            DiscIO::FromGameCubeLanguage(Config::Get(Config::MAIN_GC_LANGUAGE));

  const bool in_range =
      language >= DiscIO::Language::Japanese && language <= DiscIO::Language::Unknown;
  return in_range ? language : DiscIO::Language::Unknown;
}

DiscIO::Language SConfig::GetLanguageAdjustedForRegion(bool wii, DiscIO::Region region) const
{
  using DiscIO::Language;
  using DiscIO::Region;

  const Language language = GetCurrentLanguage(wii);

  if (!wii)
  {
    // NTSC-K is Wii-only; a GameCube game with that region is treated as Japanese.
    if (region == Region::NTSC_K)
      region = Region::NTSC_J;
    // GameCube SRAM stores English and Japanese as the same value.
    if (region == Region::NTSC_J && language == Language::English)
      return Language::Japanese;
  }

  if (Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS))
    return language;

  switch (region)
  {
  case Region::NTSC_J:
    return Language::Japanese;
  case Region::NTSC_U:
  {
    // American Wii systems also offer French and Spanish.
    const bool allowed = language == Language::English ||
                         (wii && (language == Language::French || language == Language::Spanish));
    return allowed ? language : Language::English;
  }
  case Region::PAL:
  {
    const bool european = language >= Language::English && language <= Language::Dutch;
    return european ? language : Language::English;
  }
  case Region::NTSC_K:
    return Language::Korean;
  default:
    return language;
  }
}

std::string SConfig::GetGameTDBImageRegionCode(bool wii, DiscIO::Region region) const
{
  using DiscIO::Language;

  switch (region)
  {
  case DiscIO::Region::NTSC_J:
    // Taiwanese releases are NTSC-J too; GameTDB tells them apart by the
    // 'W' fourth character of the ID.
    return (m_game_id.size() >= 4 && m_game_id[3] == 'W') ? "ZH" : "JA";
  case DiscIO::Region::NTSC_U:
    return "US";
  case DiscIO::Region::NTSC_K:
    return "KO";
  case DiscIO::Region::PAL:
    switch (GetCurrentLanguage(wii))
    {
    case Language::German:
      return "DE";
    case Language::French:
      return "FR";
    case Language::Spanish:
      return "ES";
    case Language::Italian:
      return "IT";
    case Language::Dutch:
      return "NL";
    default:
      return "EN";
    }
  default:
    return "EN";
  }
}

// --- Game INI files -------------------------------------------------------------

Common::IniFile SConfig::LoadDefaultGameIni() const
{
  return LoadDefaultGameIni(GetGameID(), m_revision);
}

Common::IniFile SConfig::LoadLocalGameIni() const
{
  return LoadLocalGameIni(GetGameID(), m_revision);
}

Common::IniFile SConfig::LoadGameIni() const
{
  return LoadGameIni(GetGameID(), m_revision);
}

Common::IniFile SConfig::LoadDefaultGameIni(std::string_view id, std::optional<u16> revision)
{
  Common::IniFile ini;
  MergeGameInis(ini, SysGameSettingsDir(), id, revision);
  return ini;
}

Common::IniFile SConfig::LoadLocalGameIni(std::string_view id, std::optional<u16> revision)
{
  Common::IniFile ini;
  MergeGameInis(ini, File::GetUserPath(D_GAMESETTINGS_IDX), id, revision);
  return ini;
}

Common::IniFile SConfig::LoadGameIni(std::string_view id, std::optional<u16> revision)
{
  // The user's settings override the bundled ones.
  Common::IniFile ini;
  MergeGameInis(ini, SysGameSettingsDir(), id, revision);
  MergeGameInis(ini, File::GetUserPath(D_GAMESETTINGS_IDX), id, revision);
  return ini;
}
