// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/DynamicInputTextureManager.h"

namespace Common
{
class IniFile;
}

namespace ControllerEmu
{
class EmulatedController;
}

// The emulated controllers of one kind (GameCube pads, Wii Remotes, hotkeys, ...) and the ini
// file their mappings live in (<User>/Config/<ini_name>.ini, one section per controller).
class InputConfig
{
public:
  InputConfig(std::string ini_name, std::string gui_name, std::string profile_directory_name,
              std::string profile_key);

  ~InputConfig();

  // Loads the mappings, using a game's profiles where its game ini names any. Returns false (and
  // loads defaults) when the ini file is missing or empty.
  bool LoadConfig();
  void SaveConfig();

  template <typename T, typename... Args>
  void CreateController(Args&&... args)
  {
    m_controllers.push_back(std::make_unique<T>(std::forward<Args>(args)...));
  }

  ControllerEmu::EmulatedController* GetController(int index) const;
  void ClearControllers();
  bool ControllersNeedToBeCreated() const;
  bool IsControllerControlledByGamepadDevice(int index) const;

  std::string GetGUIName() const { return m_gui_name; }
  std::string GetProfileKey() const { return m_profile_key; }
  std::string GetProfileDirectoryName() const { return m_profile_directory_name; }
  std::string GetUserProfileDirectoryPath() const;
  std::string GetSysProfileDirectoryPath() const;
  int GetControllerCount() const;

  // Call after creating all controllers and before clearing them, respectively.
  void RegisterHotplugCallback();
  void UnregisterHotplugCallback();

  void GenerateControllerTextures(const Common::IniFile& file);
  void GenerateControllerTextures();

private:
  Common::EventHook m_hotplug_event_hook;
  std::vector<std::unique_ptr<ControllerEmu::EmulatedController>> m_controllers;
  const std::string m_ini_name;
  const std::string m_gui_name;
  const std::string m_profile_directory_name;
  const std::string m_profile_key;
  InputCommon::DynamicInputTextureManager m_dynamic_input_tex_config_manager;
};
