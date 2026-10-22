// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
// A control that drives a device output (rumble).
class Output : public Control
{
public:
  Output(Translatability translate, std::string name);
};
}  // namespace ControllerEmu
