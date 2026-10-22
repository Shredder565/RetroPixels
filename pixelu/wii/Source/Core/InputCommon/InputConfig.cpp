// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/InputConfig.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputProfile.h"

namespace
{
// Suffixes of the game ini keys naming per-game profiles ("PadProfile1", "WiimoteProfileBB"...).
// Only the first four are looked up.
constexpr std::array<std::string_view, MAX_BBMOTES> PROFILE_SLOT_NAMES = {"1", "2", "3", "4",
                                                                          "BB"};
constexpr std::size_t PROFILE_SLOTS_CHECKED = 4;

// How long the "Loading game specific input profile" message stays up.
constexpr int PROFILE_MESSAGE_MS = 6000;
}  // namespace

InputConfig::InputConfig(std::string ini_name, std::string gui_name,
                         std::string profile_directory_name, std::string profile_key)
    : m_ini_name(std::move(ini_name)), m_gui_name(std::move(gui_name)),
      m_profile_directory_name(std::move(profile_directory_name)),
      m_profile_key(std::move(profile_key))
{
}

InputConfig::~InputConfig() = default;

bool InputConfig::LoadConfig()
{
  // The profile file each controller slot should use instead of the main ini, if any.
  std::array<std::string, MAX_BBMOTES> game_profiles;
  std::array<bool, MAX_BBMOTES> use_game_profile{};

  if (SConfig::GetInstance().GetGameID() != "00000000")
  {
    const std::string profile_root = GetUserProfileDirectoryPath();
    Common::IniFile game_ini = SConfig::GetInstance().LoadGameIni();
    const Common::IniFile::Section* const controls = game_ini.GetOrCreateSection("Controls");

    for (std::size_t slot = 0; slot < PROFILE_SLOTS_CHECKED; ++slot)
    {
      const std::string key =
          fmt::format("{}Profile{}", GetProfileKey(), PROFILE_SLOT_NAMES[slot]);
      std::string setting;
      if (!controls->Exists(key) || !controls->Get(key, &setting))
        continue;

      const std::vector<std::string> found =
          InputProfile::GetProfilesFromSetting(setting, profile_root);
      if (found.empty())
      {
        PanicAlertFmtT("No profiles found for game setting '{0}'", setting);
        continue;
      }

      // The first match is the one used at start.
      game_profiles[slot] = found.front();
      use_game_profile[slot] = true;
    }
  }

  Common::IniFile ini;
  const bool loaded = ini.Load(File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini");
  if (!loaded || ini.GetSections().empty())
  {
    // Defaults go to the first controller only. The others are just cleared, so they do not all
    // end up mapped to the same default device.
    for (std::size_t index = 0; index != m_controllers.size(); ++index)
    {
      ControllerEmu::EmulatedController& controller = *m_controllers[index];
      if (index == 0)
        controller.LoadDefaults(g_controller_interface);
      else
        controller.EmulatedController::LoadDefaults(g_controller_interface);
      controller.UpdateReferences(g_controller_interface);
    }
    return false;
  }

  for (std::size_t slot = 0; slot != m_controllers.size(); ++slot)
  {
    ControllerEmu::EmulatedController& controller = *m_controllers[slot];

    // Note that loading a game profile replaces the ini's contents, so later controllers without
    // a profile read their section from that profile file.
    Common::IniFile::Section section;
    if (use_game_profile[slot])
    {
      std::string profile_name;
      SplitPath(game_profiles[slot], nullptr, &profile_name, nullptr);
      Core::DisplayMessage("Loading game specific input profile '" + profile_name +
                               "' for device '" + controller.GetName() + "'",
                           PROFILE_MESSAGE_MS);

      ini.Load(game_profiles[slot]);
      section = *ini.GetOrCreateSection("Profile");
    }
    else
    {
      section = *ini.GetOrCreateSection(controller.GetName());
    }

    controller.LoadConfig(&section);
    controller.UpdateReferences(g_controller_interface);
  }
  return true;
}

void InputConfig::SaveConfig()
{
  const std::string path = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  // Other sections of the file are kept.
  Common::IniFile ini;
  ini.Load(path);
  for (const auto& controller : m_controllers)
    controller->SaveConfig(ini.GetOrCreateSection(controller->GetName()));
  ini.Save(path);
}

ControllerEmu::EmulatedController* InputConfig::GetController(int index) const
{
  return m_controllers.at(static_cast<std::size_t>(index)).get();
}

void InputConfig::ClearControllers()
{
  m_controllers.clear();
}

bool InputConfig::ControllersNeedToBeCreated() const
{
  // Controllers are created once and kept until ClearControllers.
  return m_controllers.size() == 0;
}

// Profiles live in "Profiles/<directory name>/" under the user's config folder (the user's
// own) and under the Sys folder (the bundled ones).
std::string InputConfig::GetUserProfileDirectoryPath() const
{
  return File::GetUserPath(D_CONFIG_IDX) + "Profiles/" + GetProfileDirectoryName() + '/';
}

std::string InputConfig::GetSysProfileDirectoryPath() const
{
  return File::GetSysDirectory() + "Profiles/" + GetProfileDirectoryName() + '/';
}

int InputConfig::GetControllerCount() const
{
  const std::size_t count = m_controllers.size();
  return static_cast<int>(count);
}

void InputConfig::RegisterHotplugCallback()
{
  // Devices came or went: rebind every controller's references.
  m_hotplug_event_hook = g_controller_interface.RegisterDevicesChangedCallback([this] {
    for (const auto& controller : m_controllers)
      controller->UpdateReferences(g_controller_interface);
  });
}

void InputConfig::UnregisterHotplugCallback()
{
  m_hotplug_event_hook.reset();
}

bool InputConfig::IsControllerControlledByGamepadDevice(int index) const
{
  if (static_cast<std::size_t>(index) >= m_controllers.size())
    return false;

  const ciface::Core::DeviceQualifier& device = m_controllers[static_cast<std::size_t>(index)]
                                                    ->GetDefaultDevice();

  // No device selected.
  if (device.source.empty())
    return false;

  // Keyboard and mouse sources of the various platforms are not gamepads.
  const bool keyboard_mouse = device.source == "Quartz" || device.source == "XInput2" ||
                              (device.source == "Android" && device.cid <= 0) ||
                              (device.source == "DInput" && device.name == "Keyboard Mouse");
  return !keyboard_mouse;
}

void InputConfig::GenerateControllerTextures(const Common::IniFile& file)
{
  m_dynamic_input_tex_config_manager.Load();

  std::vector<std::string> names;
  names.reserve(m_controllers.size());
  for (const auto& controller : m_controllers)
    names.push_back(controller->GetName());

  m_dynamic_input_tex_config_manager.GenerateTextures(file, names);
}

void InputConfig::GenerateControllerTextures()
{
  // Textures follow the current mappings, which may not have been saved yet.
  Common::IniFile ini;
  ini.Load(File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini");
  for (const auto& controller : m_controllers)
    controller->SaveConfig(ini.GetOrCreateSection(controller->GetName()));

  GenerateControllerTextures(ini);
}
