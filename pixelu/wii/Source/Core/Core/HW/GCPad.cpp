// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCPad.h"

#include "Common/Common.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"

namespace Pad
{
namespace
{
constexpr int PAD_COUNT = 4;

// Section "GCPadNew" in the INI, profiles under "GCPad", profile key "Pad".
InputConfig s_pad_config("GCPadNew", _trans("Pad"), "GCPad", "Pad");

GCPad& PadAt(int pad_num)
{
  return *static_cast<GCPad*>(s_pad_config.GetController(pad_num));
}
}  // namespace

InputConfig* GetConfig()
{
  return &s_pad_config;
}

void Initialize()
{
  if (s_pad_config.ControllersNeedToBeCreated())
  {
    for (unsigned int index = 0; index < PAD_COUNT; ++index)
      s_pad_config.CreateController<GCPad>(index);
  }
  s_pad_config.RegisterHotplugCallback();
  s_pad_config.LoadConfig();
}

void Shutdown()
{
  s_pad_config.UnregisterHotplugCallback();
  s_pad_config.ClearControllers();
}

void LoadConfig()
{
  s_pad_config.LoadConfig();
}

void GenerateDynamicInputTextures()
{
  s_pad_config.GenerateControllerTextures();
}

bool IsInitialized()
{
  return !s_pad_config.ControllersNeedToBeCreated();
}

GCPadStatus GetStatus(int pad_num)
{
  return PadAt(pad_num).GetInput();
}

ControllerEmu::ControlGroup* GetGroup(int pad_num, PadGroup group)
{
  return PadAt(pad_num).GetGroup(group);
}

void Rumble(const int pad_num, const ControlState strength)
{
  PadAt(pad_num).SetOutput(strength);
}

void ResetRumble(const int pad_num)
{
  PadAt(pad_num).SetOutput(0.0);
}

bool GetMicButton(const int pad_num)
{
  return PadAt(pad_num).GetMicButton();
}
}  // namespace Pad
