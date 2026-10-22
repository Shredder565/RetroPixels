// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/ConfigLoaders/BaseConfigLoader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigLoaders/IsSettingSaveable.h"
#include "Core/ConfigManager.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTBase.h"
#include "Core/SysConf.h"
#include "Core/System.h"

namespace ConfigLoaders
{
namespace
{
// While a title runs, the emulated system menu (or game) owns SYSCONF: the host side must neither
// read nor overwrite it. Both the flag and every SYSCONF access are guarded by this lock.
struct SysconfOwnership
{
  std::recursive_mutex lock;
  bool guest_owns = false;
};

SysconfOwnership& Ownership()
{
  static SysconfOwnership ownership;
  return ownership;
}

// The INI file each config system is stored in. Session settings never touch the disk, and
// SYSCONF / GameSettingsOnly settings are stored elsewhere, so none of them is listed. The order
// (that of Config::System) is the order the files are loaded in.
constexpr std::array<std::pair<Config::System, unsigned int>, 9> SYSTEM_INI_FILES{{
    {Config::System::Main, F_DOLPHINCONFIG_IDX},
    {Config::System::GCPad, F_GCPADCONFIG_IDX},
    {Config::System::WiiPad, F_WIIPADCONFIG_IDX},
    {Config::System::GCKeyboard, F_GCKEYBOARDCONFIG_IDX},
    {Config::System::GFX, F_GFXCONFIG_IDX},
    {Config::System::Logger, F_LOGGERCONFIG_IDX},
    {Config::System::DualShockUDPClient, F_DUALSHOCKUDPCLIENTCONFIG_IDX},
    {Config::System::FreeLook, F_FREELOOKCONFIG_IDX},
    {Config::System::Achievements, F_RETROACHIEVEMENTSCONFIG_IDX},
}};

// SYSCONF entries are named "<section>.<key>", e.g. "IPL.LNG".
std::string SysconfEntryName(const Config::Location& location)
{
  std::string name = location.section;
  name += '.';
  name += location.key;
  return name;
}

// IPL.SADR is a large blob (0x1008 bytes) of which the config only mirrors the first word, the
// country code.
constexpr size_t SADR_ENTRY_SIZE = 0x1008;

// Settings that must never be taken from the global INI even if they are present in it.
bool IsBlockedFromBaseIni(const Config::Location& location)
{
  return location == Config::MAIN_MEMORY_CARD_SIZE.GetLocation();
}

void ReadSysconfInto(Config::Layer* layer)
{
  std::lock_guard guard(Ownership().lock);
  if (Ownership().guest_owns)
    return;

  IOS::HLE::Kernel kernel;
  SysConf sysconf{kernel.GetFS()};

  for (const Config::SYSCONFSetting& setting : Config::SYSCONF_SETTINGS)
  {
    const auto read_one = [&](const auto* info) {
      const Config::Location location = info->GetLocation();
      const std::string name = SysconfEntryName(location);
      switch (setting.type)
      {
      case SysConf::Entry::Type::Long:
        layer->Set(location, sysconf.GetData<u32>(name, info->GetDefaultValue()));
        break;
      case SysConf::Entry::Type::Byte:
        layer->Set(location, sysconf.GetData<u8>(name, static_cast<u8>(info->GetDefaultValue())));
        break;
      case SysConf::Entry::Type::BigArray:
      {
        u32 country = info->GetDefaultValue();
        if (const SysConf::Entry* entry = sysconf.GetEntry(name))
        {
          const size_t copied = std::min<size_t>(entry->bytes.size(), sizeof(country));
          std::memcpy(&country, entry->bytes.data(), copied);
        }
        layer->Set(location, country);
        break;
      }
      default:
        break;
      }
    };
    std::visit(read_one, setting.config_info);
  }
}

void ReadIniFilesInto(Config::Layer* layer)
{
  for (const auto& [system, file_index] : SYSTEM_INI_FILES)
  {
    Common::IniFile ini;
    ini.Load(File::GetUserPath(file_index));

    for (const Common::IniFile::Section& section : ini.GetSections())
    {
      const std::string& section_name = section.GetName();
      for (const auto& [key, value] : section.GetValues())
      {
        Config::Location location{system, section_name, key};
        if (!IsBlockedFromBaseIni(location))
          layer->Set(location, value);
      }
    }
  }
}

// Whether a setting is written by the INI saver (SYSCONF ones are saved separately).
bool BelongsInIniFile(Config::System system)
{
  switch (system)
  {
  case Config::System::SYSCONF:
  case Config::System::Session:
  case Config::System::GameSettingsOnly:
    return false;
  default:
    return true;
  }
}

void WriteIniFilesFrom(const Config::Layer& layer)
{
  std::map<Config::System, Common::IniFile> files;
  for (const auto& [system, file_index] : SYSTEM_INI_FILES)
    files[system].Load(File::GetUserPath(file_index));

  for (const auto& [location, value] : layer.GetLayerMap())
  {
    if (!BelongsInIniFile(location.system))
      continue;

    const auto file = files.find(location.system);
    if (file == files.end())
    {
      ERROR_LOG_FMT(COMMON, "Config can't map system '{}' to an INI file!",
                    Config::GetSystemName(location.system));
      continue;
    }
    if (!IsSettingSaveable(location))
      continue;

    Common::IniFile& ini = file->second;
    if (!value)
      ini.DeleteKey(location.section, location.key);
    else
      ini.GetOrCreateSection(location.section)->Set(location.key, *value);
  }

  for (const auto& [system, file_index] : SYSTEM_INI_FILES)
    files[system].Save(File::GetUserPath(file_index));
}

class BaseIniLayerLoader final : public Config::ConfigLayerLoader
{
public:
  BaseIniLayerLoader() : ConfigLayerLoader(Config::LayerType::Base) {}

  void Load(Config::Layer* layer) override
  {
    // SYSCONF first; the INI files can't contain SYSCONF settings anyway.
    ReadSysconfInto(layer);
    ReadIniFilesInto(layer);
  }

  void Save(Config::Layer* layer) override
  {
    SaveToSYSCONF(layer->GetLayer(), SkipIfControlledByGuest::Yes);
    WriteIniFilesFrom(*layer);
  }
};
}  // namespace

void TransferSYSCONFControlToGuest()
{
  std::lock_guard guard(Ownership().lock);
  ASSERT(!Ownership().guest_owns);
  Ownership().guest_owns = true;
}

void TransferSYSCONFControlFromGuest(WriteBackChangedValues write_back_changed_values)
{
  std::lock_guard guard(Ownership().lock);
  ASSERT(Ownership().guest_owns);
  Ownership().guest_owns = false;

  if (write_back_changed_values != WriteBackChangedValues::Yes)
    return;

  // The guest may have changed SYSCONF while it owned it. Those changes are kept, except for
  // settings that some higher layer (a movie, NetPlay, a game INI, ...) overrides: the guest saw
  // the temporary value there, so the user's base value is put back instead.
  //
  // Read what SYSCONF holds now into a scratch layer. A loader of its own is used so that the
  // scratch layer isn't saved when it goes away.
  Config::Layer current_sysconf(Config::LayerType::Base);
  GenerateBaseConfigLoader()->Load(&current_sysconf);

  for (const Config::SYSCONFSetting& setting : Config::SYSCONF_SETTINGS)
  {
    std::visit(
        [&current_sysconf](const auto* info) {
          const bool not_overridden =
              Config::GetActiveLayerForConfig(*info) == Config::LayerType::Base;
          if (not_overridden)
            Config::SetBase(*info, current_sysconf.Get(*info));
        },
        setting.config_info);
  }

  SaveToSYSCONF(Config::LayerType::Base, SkipIfControlledByGuest::No);
}

void SaveToSYSCONF(Config::LayerType layer, SkipIfControlledByGuest skip,
                   std::function<bool(const Config::Location&)> predicate)
{
  std::lock_guard guard(Ownership().lock);
  if (Ownership().guest_owns && skip == SkipIfControlledByGuest::Yes)
    return;

  IOS::HLE::Kernel kernel;
  SysConf sysconf{kernel.GetFS()};

  for (const Config::SYSCONFSetting& setting : Config::SYSCONF_SETTINGS)
  {
    const auto write_one = [&](const auto* info) {
      const Config::Location& location = info->GetLocation();
      if (predicate && !predicate(location))
        return;

      const std::string name = SysconfEntryName(location);
      const auto value = Config::Get(layer, *info);
      switch (setting.type)
      {
      case SysConf::Entry::Type::Long:
        sysconf.SetData<u32>(name, setting.type, value);
        break;
      case SysConf::Entry::Type::Byte:
        sysconf.SetData<u8>(name, setting.type, static_cast<u8>(value));
        break;
      case SysConf::Entry::Type::BigArray:
      {
        SysConf::Entry* entry = sysconf.GetOrAddEntry(name, setting.type);
        if (entry->bytes.size() < SADR_ENTRY_SIZE)
          entry->bytes.resize(SADR_ENTRY_SIZE);
        const u32 word = value;
        std::memcpy(entry->bytes.data(), &word, sizeof(word));
        break;
      }
      default:
        break;
      }
    };
    std::visit(write_one, setting.config_info);
  }

  // Always mark the configuration as done (IPL.CB = 0) so the system menu doesn't start its
  // first-time setup, and keep the Bluetooth section consistent.
  sysconf.SetData<u32>("IPL.CB", SysConf::Entry::Type::Long, 0);
  IOS::HLE::RestoreBTInfoSection(&sysconf);
  sysconf.Save();
}

std::unique_ptr<Config::ConfigLayerLoader> GenerateBaseConfigLoader()
{
  return std::make_unique<BaseIniLayerLoader>();
}
}  // namespace ConfigLoaders
