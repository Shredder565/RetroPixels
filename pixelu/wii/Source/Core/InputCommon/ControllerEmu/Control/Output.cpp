// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/Control/Output.h"

#include <memory>
#include <string>
#include <utility>

#include "InputCommon/ControlReference/ControlReference.h"

namespace ControllerEmu
{
Output::Output(Translatability translate_, std::string name_)
    : Control(std::make_unique<OutputReference>(), translate_, std::move(name_))
{
}
}  // namespace ControllerEmu
