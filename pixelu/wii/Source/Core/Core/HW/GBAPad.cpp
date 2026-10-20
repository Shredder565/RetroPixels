// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GBAPad.h"

#include "Core/HW/GBAPadEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"

namespace Pad
{
namespace
{
constexpr int GBA_COUNT = 4;

// Section "GBA" in the INI, profiles under "GBA", profile key "GBA".
InputConfig s_gba_config("GBA", _trans("Pad"), "GBA", "GBA");

GBAPad& GBAPadAt(int pad_num)
{
  return *static_cast<GBAPad*>(s_gba_config.GetController(pad_num));
}
}  // namespace

InputConfig* GetGBAConfig()
{
  return &s_gba_config;
}

void InitializeGBA()
{
  if (s_gba_config.ControllersNeedToBeCreated())
  {
    for (unsigned int index = 0; index < GBA_COUNT; ++index)
      s_gba_config.CreateController<GBAPad>(index);
  }
  s_gba_config.RegisterHotplugCallback();
  s_gba_config.LoadConfig();
}

void ShutdownGBA()
{
  s_gba_config.UnregisterHotplugCallback();
  s_gba_config.ClearControllers();
}

void LoadGBAConfig()
{
  s_gba_config.LoadConfig();
}

bool IsGBAInitialized()
{
  return !s_gba_config.ControllersNeedToBeCreated();
}

GCPadStatus GetGBAStatus(int pad_num)
{
  return GBAPadAt(pad_num).GetInput();
}

void SetGBAReset(int pad_num, bool reset)
{
  GBAPadAt(pad_num).SetReset(reset);
}

ControllerEmu::ControlGroup* GetGBAGroup(int pad_num, GBAPadGroup group)
{
  return GBAPadAt(pad_num).GetGroup(group);
}
}  // namespace Pad
