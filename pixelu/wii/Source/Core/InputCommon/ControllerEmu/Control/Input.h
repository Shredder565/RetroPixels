// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
// A control read from the user's devices (button, axis direction, setting input).
class Input : public Control
{
public:
  Input(Translatability translate, std::string name, std::string ui_name);
  Input(Translatability translate, std::string name);
};
}  // namespace ControllerEmu
