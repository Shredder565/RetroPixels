// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCKeyboard.h"

#include "Common/Common.h"
#include "Core/HW/GCKeyboardEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/InputConfig.h"
#include "InputCommon/KeyboardStatus.h"

namespace Keyboard
{
namespace
{
constexpr int KEYBOARD_COUNT = 4;

// Section "GCKeyNew" in the INI, profiles under "GCKey", profile key "GCKey".
InputConfig s_keyboard_config("GCKeyNew", _trans("Keyboard"), "GCKey", "GCKey");

GCKeyboard& KeyboardAt(int port)
{
  return *static_cast<GCKeyboard*>(s_keyboard_config.GetController(port));
}
}  // namespace

InputConfig* GetConfig()
{
  return &s_keyboard_config;
}

void Initialize()
{
  if (s_keyboard_config.ControllersNeedToBeCreated())
  {
    for (unsigned int index = 0; index < KEYBOARD_COUNT; ++index)
      s_keyboard_config.CreateController<GCKeyboard>(index);
  }
  s_keyboard_config.RegisterHotplugCallback();
  s_keyboard_config.LoadConfig();
}

void Shutdown()
{
  s_keyboard_config.UnregisterHotplugCallback();
  s_keyboard_config.ClearControllers();
}

void LoadConfig()
{
  s_keyboard_config.LoadConfig();
}

void GenerateDynamicInputTextures()
{
  s_keyboard_config.GenerateControllerTextures();
}

ControllerEmu::ControlGroup* GetGroup(int port, KeyboardGroup group)
{
  return KeyboardAt(port).GetGroup(group);
}

KeyboardStatus GetStatus(int port)
{
  return KeyboardAt(port).GetInput();
}
}  // namespace Keyboard
