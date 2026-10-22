// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/UICommon.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <shlobj.h>

#include <wil/resource.h>
#endif

#include <fmt/format.h>

#include "Common/Common.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/LogManager.h"
#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/STM/STM.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"

#include "UICommon/DiscordPresence.h"

#include "VideoCommon/Statistics.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace UICommon
{
namespace
{
Config::ConfigChangedCallbackID s_config_callback;
Common::HookableEvent<> s_flush_requested;

void MakeFolder(unsigned int user_path_index, const char* subfolder = "")
{
  File::CreateFullPath(File::GetUserPath(user_path_index) + subfolder);
}

// Points `index` at a configured folder; an empty setting keeps the default.
void OverrideFolder(unsigned int index, std::string configured)
{
  if (!configured.empty())
    File::SetUserPath(index, std::move(configured));
}

// Applies the folder settings that can change while running.
void ApplyFolderSettings()
{
  File::SetUserPath(D_WIIROOT_IDX, Config::Get(Config::MAIN_FS_PATH));

  OverrideFolder(D_LOAD_IDX, Config::Get(Config::MAIN_LOAD_PATH));
  for (const unsigned int index :
       {D_HIRESTEXTURES_IDX, D_RIIVOLUTION_IDX, D_GRAPHICSMOD_IDX, D_DYNAMICINPUT_IDX})
  {
    MakeFolder(index);
  }

  OverrideFolder(D_DUMP_IDX, Config::Get(Config::MAIN_DUMP_PATH));
  for (const unsigned int index : {D_DUMPAUDIO_IDX, D_DUMPDSP_IDX, D_DUMPSSL_IDX, D_DUMPFRAMES_IDX,
                                   D_DUMPOBJECTS_IDX, D_DUMPTEXTURES_IDX, D_DUMPDEBUG_IDX,
                                   D_DUMPDEBUG_BRANCHWATCH_IDX, D_DUMPDEBUG_JITBLOCKS_IDX})
  {
    MakeFolder(index);
  }

  OverrideFolder(D_RESOURCEPACK_IDX, Config::Get(Config::MAIN_RESOURCEPACK_PATH));

  if (const std::string wfs = Config::Get(Config::MAIN_WFS_PATH); !wfs.empty())
    File::SetUserPath(D_WFSROOT_IDX, wfs + '/');

  File::SetUserPath(F_WIISDCARDIMAGE_IDX, Config::Get(Config::MAIN_WII_SD_CARD_IMAGE_PATH));
  File::SetUserPath(D_WIISDCARDSYNCFOLDER_IDX,
                    Config::Get(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH));
  MakeFolder(D_WIISDCARDSYNCFOLDER_IDX);

#ifdef HAS_LIBMGBA
  File::SetUserPath(F_GBABIOS_IDX, Config::Get(Config::MAIN_GBA_BIOS_PATH));
  File::SetUserPath(D_GBASAVES_IDX, Config::Get(Config::MAIN_GBA_SAVES_PATH));
  MakeFolder(D_GBASAVES_IDX);
#endif
}

void ApplyAlertSettings()
{
  Common::SetEnableAlert(Config::Get(Config::MAIN_USE_PANIC_HANDLERS));
  Common::SetAbortOnPanicAlert(Config::Get(Config::MAIN_ABORT_ON_PANIC_ALERT));
}

// Emulated input layers, started in this order and stopped in the same order. (The GameCube
// adapter is started with them but only stopped in Shutdown.)
struct EmulatedInput
{
  void (*start)();
  void (*stop)();
};

constexpr EmulatedInput EMULATED_INPUTS[] = {
    {Pad::Initialize, Pad::Shutdown},
    {Pad::InitializeGBA, Pad::ShutdownGBA},
    {Keyboard::Initialize, Keyboard::Shutdown},
    {[] { Wiimote::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES); },
     Wiimote::Shutdown},
    {HotkeyManagerEmu::Initialize, HotkeyManagerEmu::Shutdown},
    {FreeLook::Initialize, FreeLook::Shutdown},
};

// Tries one locale name; false if the C library does not know it.
bool TryLocale(const std::string& name)
{
#ifdef __linux__
  // glibc locale names carry the encoding.
  const std::string full_name = name.empty() ? name : name + ".UTF-8";
#else
  const std::string& full_name = name;
#endif
  // setlocale doubles as the validity check, since constructing a std::locale with an unknown
  // name would throw.
  if (std::setlocale(LC_ALL, full_name.c_str()) == nullptr)
    return false;
  std::locale::global(std::locale(full_name));
  return true;
}

#ifdef _WIN32
constexpr wchar_t REGISTRY_KEY[] = L"Software\\Dolphin Emulator";

std::optional<std::string> KnownFolder(REFKNOWNFOLDERID id)
{
  wil::unique_cotaskmem_string path;
  if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, path.put())))
    return std::nullopt;
  return TStrToUTF8(path.get());
}

// The Windows user folder, in order of preference:
//   1. <exe folder>\User when the registry value LocalUserConfig is non-zero or portable.txt
//      sits next to the executable;
//   2. the folder in the registry value UserConfigPath;
//   3. Documents\Dolphin Emulator, if it exists (the default of older versions);
//   4. AppData\Roaming\Dolphin Emulator, which is then also written to UserConfigPath so older
//      versions find it too;
//   5. <exe folder>\User.
std::string WindowsUserFolder()
{
  const std::optional<std::string> appdata = KnownFolder(FOLDERID_RoamingAppData);

  DWORD local_flag = 0;
  std::optional<std::wstring> registered_path;
  wil::unique_hkey key;
  if (RegOpenKeyEx(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_QUERY_VALUE, key.put()) == ERROR_SUCCESS)
  {
    DWORD size = sizeof(local_flag);
    if (RegQueryValueEx(key.get(), TEXT("LocalUserConfig"), nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(&local_flag), &size) != ERROR_SUCCESS)
    {
      local_flag = 0;
    }

    // Ask for the size first, then read. The extra zeroed element guarantees termination.
    size = 0;
    RegQueryValueEx(key.get(), TEXT("UserConfigPath"), nullptr, nullptr, nullptr, &size);
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueEx(key.get(), TEXT("UserConfigPath"), nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(buffer.data()), &size) == ERROR_SUCCESS)
    {
      registered_path = std::wstring(buffer.data());
    }
  }

  const std::string portable_folder = File::GetExeDirectory() + DIR_SEP PORTABLE_USER_DIR DIR_SEP;
  const bool portable =
      local_flag != 0 || File::Exists(File::GetExeDirectory() + DIR_SEP "portable.txt");

  std::optional<std::string> legacy_folder;
  if (const std::optional<std::string> documents = KnownFolder(FOLDERID_Documents))
    legacy_folder = *documents + DIR_SEP NORMAL_USER_DIR DIR_SEP;

  if (portable)
    return portable_folder;
  if (registered_path)
    return WStringToUTF8(*registered_path);
  if (legacy_folder && File::Exists(*legacy_folder))
    return *legacy_folder;
  if (!appdata)
    return portable_folder;

  std::string roaming_folder = *appdata + DIR_SEP NORMAL_USER_DIR DIR_SEP;
  const std::wstring wide = UTF8ToWString(roaming_folder);
  RegSetKeyValueW(HKEY_CURRENT_USER, REGISTRY_KEY, L"UserConfigPath", REG_SZ, wide.c_str(),
                  static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
  return roaming_folder;
}
#else
std::string FromEnvironment(const char* name, const std::string& fallback)
{
  // Only absolute paths count, as the XDG specification requires.
  const char* value = std::getenv(name);
  return value != nullptr && value[0] == '/' ? std::string(value) : fallback;
}

// PixelU: the folder portable.txt asks for. It is "User" next to the executable on every system,
// as RetroPixels expects (PixelU/wii/User, internal/dolphin's userDirFor), not Dolphin's
// lower-case Unix "user".
constexpr char PIXELU_PORTABLE_FOLDER[] = "User";

// Elsewhere: portable.txt next to the executable (PixelU's packages have it), an embedded user
// folder, $DOLPHIN_EMU_USERPATH, ~/.dolphin-emu (outside Flatpak), and otherwise the XDG data,
// config and cache folders. Returns nullopt when it set the XDG folders itself.
std::optional<std::string> PosixUserFolder()
{
  const std::string exe_folder = File::GetExeDirectory();
  if (File::Exists(exe_folder + DIR_SEP "portable.txt"))
    return exe_folder + DIR_SEP + PIXELU_PORTABLE_FOLDER + DIR_SEP;

  if (File::IsDirectory(ROOT_DIR DIR_SEP EMBEDDED_USER_DIR))
    return ROOT_DIR DIR_SEP EMBEDDED_USER_DIR DIR_SEP;

  const char* custom = std::getenv("DOLPHIN_EMU_USERPATH");
  const char* home = std::getenv("HOME");
  if (home == nullptr)
    home = std::getenv("PWD");
  const std::string home_folder = std::string(home != nullptr ? home : "") + DIR_SEP;

  if (custom != nullptr)
    return std::string(custom);

  std::string dot_folder = home_folder + "." NORMAL_USER_DIR DIR_SEP;
  if (!File::Exists("/.flatpak-info") && File::Exists(dot_folder))
    return dot_folder;

  const std::string suffix = DIR_SEP NORMAL_USER_DIR DIR_SEP;
  File::SetUserPath(D_USER_IDX,
                    FromEnvironment("XDG_DATA_HOME", home_folder + ".local" DIR_SEP "share") +
                        suffix);
  File::SetUserPath(D_CONFIG_IDX,
                    FromEnvironment("XDG_CONFIG_HOME", home_folder + ".config") + suffix);
  File::SetUserPath(D_CACHE_IDX,
                    FromEnvironment("XDG_CACHE_HOME", home_folder + ".cache") + suffix);
  return std::nullopt;
}
#endif
}  // namespace

void Init()
{
  Core::RestoreWiiSettings(Core::RestoreReason::CrashRecovery);

  Config::Init();
  s_config_callback = Config::AddConfigChangedCallback([] {
    ApplyFolderSettings();
    ApplyAlertSettings();
  });
  Config::AddLayer(ConfigLoaders::GenerateBaseConfigLoader());
  SConfig::Init();
  g_Config.Init();
  Discord::Init();
  Common::Log::LogManager::Init();
  VideoBackendBase::ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));
  Statistics::Init();

  ApplyAlertSettings();
}

void Shutdown()
{
  Config::RemoveConfigChangedCallback(s_config_callback);

  // Roughly the reverse of Init, with the controller back ends that outlive a game in between.
  // The configuration goes last since the others may still read it.
  for (const auto stop : {Statistics::Shutdown, GCAdapter::Shutdown, WiimoteReal::Shutdown,
                          Common::Log::LogManager::Shutdown, Discord::Shutdown})
  {
    stop();
  }
  g_Config.Shutdown();
  SConfig::Shutdown();
  Config::Shutdown();
}

Common::EventHook AddFlushUnsavedDataCallback(std::function<void()> callback)
{
  return s_flush_requested.Register(std::move(callback));
}

void FlushUnsavedData()
{
  INFO_LOG_FMT(CORE, "Flushing unsaved data...");
  s_flush_requested.Trigger();
}

void InitControllers(const WindowSystemInfo& wsi)
{
  if (g_controller_interface.IsInit())
    return;

  // Host input devices come first; the emulated controllers map onto them.
  g_controller_interface.Initialize(wsi);
  if (!g_controller_interface.HasDefaultDevice())
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "No default device has been added in time. Premade control "
                                      "mappings intended for the default device may not work.");
  }

  GCAdapter::Init();
  for (const EmulatedInput& input : EMULATED_INPUTS)
    input.start();
}

void ShutdownControllers()
{
  for (const EmulatedInput& input : EMULATED_INPUTS)
    input.stop();
  g_controller_interface.Shutdown();
}

void SetLocale(std::string locale_name)
{
#ifdef _WIN32
  constexpr char SEPARATOR = '-';
  constexpr char FOREIGN_SEPARATOR = '_';
#else
  constexpr char SEPARATOR = '_';
  constexpr char FOREIGN_SEPARATOR = '-';
#endif

  // The language setting only offers plain "en"; use British conventions for it, since anyone
  // wanting US formats most likely runs an en_US system and keeps the system-language default.
  if (locale_name == "en")
    locale_name = "en_GB";
  std::ranges::replace(locale_name, FOREIGN_SEPARATOR, SEPARATOR);

  // Try the full name, then drop one subtag at a time; the last attempt, with an empty name,
  // selects the system locale. If even that fails, the current locale stays.
  if (TryLocale(locale_name))
    return;
  while (!locale_name.empty())
  {
    const std::size_t cut = locale_name.rfind(SEPARATOR);
    locale_name.resize(cut == std::string::npos ? 0 : cut);
    if (TryLocale(locale_name))
      return;
  }
}

void CreateDirectories()
{
  struct Folder
  {
    unsigned int index;
    const char* subfolder;
  };
  static constexpr Folder FOLDERS[] = {
      {D_RESOURCEPACK_IDX, ""},
      {D_USER_IDX, ""},
      {D_CACHE_IDX, ""},
      {D_COVERCACHE_IDX, ""},
      {D_CONFIG_IDX, ""},
      {D_CONFIG_IDX, GRAPHICSMOD_CONFIG_DIR DIR_SEP},
      {D_DUMPDSP_IDX, ""},
      {D_DUMPSSL_IDX, ""},
      {D_DUMPTEXTURES_IDX, ""},
      {D_DUMPDEBUG_IDX, ""},
      {D_DUMPDEBUG_BRANCHWATCH_IDX, ""},
      {D_DUMPDEBUG_JITBLOCKS_IDX, ""},
      {D_GAMESETTINGS_IDX, ""},
      {D_GCUSER_IDX, ""},
      {D_GCUSER_IDX, USA_DIR DIR_SEP},
      {D_GCUSER_IDX, EUR_DIR DIR_SEP},
      {D_GCUSER_IDX, JAP_DIR DIR_SEP},
      {D_TRIUSER_IDX, ""},
      {D_HIRESTEXTURES_IDX, ""},
      {D_GRAPHICSMOD_IDX, ""},
      {D_LOGS_IDX, ""},
      {D_MAPS_IDX, ""},
      {D_SCREENSHOTS_IDX, ""},
      {D_SHADERS_IDX, ""},
      {D_SHADERS_IDX, ANAGLYPH_DIR DIR_SEP},
      {D_RETROACHIEVEMENTSCACHE_IDX, ""},
      {D_STATESAVES_IDX, ""},
      {D_ASM_ROOT_IDX, ""},
      {D_WFSROOT_IDX, ""},
      {D_THEMES_IDX, ""},
      {D_STYLES_IDX, ""},
  };

  for (const Folder& folder : FOLDERS)
    MakeFolder(folder.index, folder.subfolder);
}

void SetUserDirectory(std::string custom_path)
{
  if (!custom_path.empty())
  {
    File::CreateFullPath(custom_path + DIR_SEP);
    File::SetUserPath(D_USER_IDX, std::move(custom_path));
    return;
  }

#ifdef _WIN32
  File::SetUserPath(D_USER_IDX, WindowsUserFolder());
#else
  if (std::optional<std::string> folder = PosixUserFolder())
    File::SetUserPath(D_USER_IDX, std::move(*folder));
#endif
}

bool TriggerSTMPowerEvent()
{
  const auto ios = Core::System::GetInstance().GetIOS();
  if (!ios)
    return false;

  // Only titles that registered an STM event hook react to the power button.
  const auto device = ios->GetDeviceByName("/dev/stm/eventhook");
  if (!device)
    return false;
  const auto hook = std::static_pointer_cast<IOS::HLE::STMEventHookDevice>(device);
  if (!hook->HasHookInstalled())
    return false;

  Core::DisplayMessage("Shutting down", 30000);
  ios->GetSystem().GetProcessorInterface().PowerButton_Tap();
  return true;
}

void InhibitScreenSaver(bool inhibit)
{
#ifdef _WIN32
  // Keeps the display on and the system awake (no idle sleep) while set.
  EXECUTION_STATE state = ES_CONTINUOUS;
  if (inhibit)
    state |= ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED;
  SetThreadExecutionState(state);
#else
  (void)inhibit;
#endif
}

std::string FormatSize(u64 bytes, int decimals)
{
  // i18n: The symbol for the unit "bytes"
  static constexpr const char* UNITS[] = {_trans("B"),   _trans("KiB"), _trans("MiB"),
                                          _trans("GiB"), _trans("TiB"), _trans("PiB"),
                                          _trans("EiB")};

  // Each unit is 2^10 of the previous one; a u64 needs at most EiB (log2 63 / 10 = 6).
  const int unit = MathUtil::IntLog2(std::max<u64>(bytes, 1)) / 10;
  const double scale = std::pow(2, unit * 10);
  const double value = static_cast<double>(bytes) / scale;
  return fmt::format("{:.{}Lf} {}", value, decimals, Common::GetStringT(UNITS[unit]));
}

}  // namespace UICommon
