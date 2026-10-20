// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/ConfigLoaders/IsSettingSaveable.h"

#include "Core/Config/WiimoteSettings.h"

namespace ConfigLoaders
{
bool IsSettingSaveable(const Config::Location& config_location)
{
  // Controller systems are saved by the input code itself. The only exception are the Wii Remote
  // sources, which live in the WiiPad file but are owned by the config system.
  switch (config_location.system)
  {
  case Config::System::GCPad:
  case Config::System::GCKeyboard:
    return false;
  case Config::System::WiiPad:
    for (int index = 0; index < 5; ++index)
    {
      if (Config::GetInfoForWiimoteSource(index).GetLocation() == config_location)
        return true;
    }
    return false;
  default:
    return true;
  }
}
}  // namespace ConfigLoaders
