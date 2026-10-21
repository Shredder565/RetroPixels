// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

#include <string>
#include <utility>

namespace ControllerEmu
{
Buttons::Buttons(const std::string& name_) : Buttons(name_, name_)
{
}

Buttons::Buttons(std::string ini_name, std::string group_name)
    : ControlGroup(std::move(ini_name), std::move(group_name), GroupType::Buttons)
{
}
}  // namespace ControllerEmu
