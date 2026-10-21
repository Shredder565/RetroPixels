// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Boot flow: the UI calls BootManager::BootCore, which calls Core::Init, which
// starts the emulation thread (Core.cpp), which calls CBoot::BootUp (Boot.cpp).

#include "Core/BootManager.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Sram.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

#include "DiscIO/Enums.h"

namespace BootManager
{
namespace
{
// A movie recorded from a blank memory card replays against one: delete the
// movie's GameCube card image and GCI folder before it starts.
void ClearMovieMemoryCards(Core::System& system)
{
  const Movie::MovieManager& movie = system.GetMovie();
  if (!movie.IsPlayingInput() || !movie.IsConfigSaved())
    return;

  const std::string& gc_user_dir = File::GetUserPath(D_GCUSER_IDX);
  for (const ExpansionInterface::Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    if (!movie.IsUsingMemcard(slot) || !movie.IsStartingFromClearSave() || system.IsWii())
      continue;

    const char slot_letter = slot == ExpansionInterface::Slot::A ? 'A' : 'B';
    const std::string card_image = gc_user_dir + fmt::format("Movie{}.raw", slot_letter);
    if (File::Exists(card_image))
      File::Delete(card_image);

    const std::string gci_folder = gc_user_dir + "Movie";
    if (File::Exists(gci_folder))
      File::DeleteDirRecursively(gci_folder);
  }
}

// The SYSCONF country code the System Menu of each region ships with.
std::optional<u32> HomeCountryCode(DiscIO::Region region)
{
  switch (region)
  {
  case DiscIO::Region::NTSC_J:
    return 0x01;  // Japan
  case DiscIO::Region::NTSC_U:
    return 0x31;  // United States
  case DiscIO::Region::PAL:
    return 0x6c;  // Switzerland
  case DiscIO::Region::NTSC_K:
    return 0x88;  // South Korea
  default:
    return std::nullopt;
  }
}

// Games can misbehave or crash with a language or country from another
// region, so unless the user overrides it, use ones that match the game.
void MatchLanguageAndCountryToRegion(const Core::System& system, const SConfig& config)
{
  if (Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS))
    return;

  const DiscIO::Region region = config.m_region;
  Config::SetCurrent(Config::MAIN_GC_LANGUAGE,
                     DiscIO::ToGameCubeLanguage(config.GetLanguageAdjustedForRegion(false, region)));

  if (!system.IsWii())
    return;

  const auto wii_language =
      static_cast<u32>(config.GetLanguageAdjustedForRegion(true, region));
  if (Config::Get(Config::SYSCONF_LANGUAGE) != wii_language)
    Config::SetCurrent(Config::SYSCONF_LANGUAGE, wii_language);

  const auto country = static_cast<u8>(Config::Get(Config::SYSCONF_COUNTRY));
  if (DiscIO::SysConfCountryToRegion(country) == region)
    return;
  if (const std::optional<u32> home_country = HomeCountryCode(region))
    Config::SetCurrent(Config::SYSCONF_COUNTRY, *home_country);
}

// Triforce games expect the arcade baseboard on the serial and EXI buses and
// are all 4:3. Each default only applies when no layer above Base sets it
// (a game INI can opt out).
void ApplyTriforceDefaults()
{
  const auto only_base_sets = [](const auto& setting) {
    return Config::GetActiveLayerForConfig(setting) == Config::LayerType::Base;
  };

  if (only_base_sets(Config::MAIN_SERIAL_PORT_1))
    Config::SetCurrent(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::Baseboard);

  if (only_base_sets(Config::GetInfoForSIDevice(0)))
  {
    Config::SetCurrent(Config::GetInfoForSIDevice(0),
                       SerialInterface::SIDevices::SIDEVICE_AM_BASEBOARD);
  }

  // Mario Kart Arcade GP trips the widescreen heuristic.
  if (only_base_sets(Config::GFX_SUGGESTED_ASPECT_RATIO))
    Config::SetCurrent(Config::GFX_SUGGESTED_ASPECT_RATIO, AspectMode::ForceStandard);
}

// Sets up the NAND root for a Wii session and pushes the session's settings
// into its SYSCONF.
void PrepareWiiRoot()
{
  const bool deterministic = Core::WantsDeterminism();
  Core::InitializeWiiRoot(deterministic);

  if (!deterministic)
  {
    // The real NAND: keep a backup in case emulation never ends cleanly.
    Core::BackupWiiSettings();
    ConfigLoaders::SaveToSYSCONF(Config::LayerType::Meta,
                                 ConfigLoaders::SkipIfControlledByGuest::No);
    return;
  }

  // A temporary NAND for movies and NetPlay: only write what the movie or
  // NetPlay layers (or anything above them) decide.
  ConfigLoaders::SaveToSYSCONF(Config::LayerType::Meta, ConfigLoaders::SkipIfControlledByGuest::No,
                               [](const Config::Location& location) {
                                 return Config::GetActiveLayerForConfig(location) >=
                                        Config::LayerType::Movie;
                               });
}
}  // namespace

bool BootCore(Core::System& system, std::unique_ptr<BootParameters> boot,
              const WindowSystemInfo& wsi)
{
  if (!boot)
    return false;

  SConfig& config = SConfig::GetInstance();
  if (!config.SetPathsAndGameMetadata(system, *boot))
    return false;

  ClearMovieMemoryCards(system);

  if (NetPlay::IsNetPlayRunning())
  {
    const NetPlay::NetSettings* netplay = boot->boot_session_data.GetNetplaySettings();
    if (!netplay)
      return false;
    Config::AddLayer(ConfigLoaders::GenerateNetPlayConfigLoader(*netplay));
    config.bCopyWiiSaveNetplay = netplay->savedata_load;
  }

  MatchLanguageAndCountryToRegion(system, config);

  // PAL60 crashes some NTSC Wii titles (Doc Louis's Punch-Out!!, 1942 on Virtual Console).
  if (system.IsWii() && DiscIO::IsNTSC(config.m_region) && Config::Get(Config::SYSCONF_PAL60))
    Config::SetCurrent(Config::SYSCONF_PAL60, false);

  // Disc timing is not emulated for Riivolution-patched discs yet.
  if (!boot->riivolution_patches.empty())
    Config::SetCurrent(Config::MAIN_FAST_DISC_SPEED, true);

  if (system.IsTriforce())
    ApplyTriforceDefaults();

  system.Initialize();
  Core::UpdateWantDeterminism(system, /*initial*/ true);
  ConfigLoaders::TransferSYSCONFControlToGuest();

  if (system.IsWii())
    PrepareWiiRoot();

  AchievementManager::GetInstance().CloseGame();

  // A GameCube disc boots through the IPL unless the user skips it.
  const bool through_ipl = !system.IsWii() && !Config::Get(Config::MAIN_SKIP_IPL) &&
                           std::holds_alternative<BootParameters::Disc>(boot->parameters);
  if (!through_ipl)
    return Core::Init(system, std::move(boot), wsi);

  auto& disc = std::get<BootParameters::Disc>(boot->parameters);
  auto ipl_boot = std::make_unique<BootParameters>(
      BootParameters::IPL{config.m_region, std::move(disc)}, std::move(boot->boot_session_data));
  return Core::Init(system, std::move(ipl_boot), wsi);
}

void RestoreConfig()
{
  Core::ShutdownWiiRoot();

  // ShutdownWiiRoot has already forgotten the temporary root by now, so in
  // practice the second branch always runs (as it always has): the
  // setting.txt backup, if any, is put back and SYSCONF changes made by the
  // guest are written back to the user's settings.
  if (Core::WiiRootIsTemporary())
  {
    ConfigLoaders::TransferSYSCONFControlFromGuest(ConfigLoaders::WriteBackChangedValues::No);
  }
  else
  {
    Core::RestoreWiiSettings(Core::RestoreReason::EmulationEnd);
    ConfigLoaders::TransferSYSCONFControlFromGuest(ConfigLoaders::WriteBackChangedValues::Yes);
  }

  Config::ClearCurrentRunLayer();
  for (const Config::LayerType layer :
       {Config::LayerType::Movie, Config::LayerType::Netplay, Config::LayerType::GlobalGame,
        Config::LayerType::LocalGame})
  {
    Config::RemoveLayer(layer);
  }
  SConfig::GetInstance().ResetRunningGameMetadata();
}
}  // namespace BootManager
