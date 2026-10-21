// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/Control/Input.h"

#include <memory>
#include <string>
#include <utility>

#include "InputCommon/ControlReference/ControlReference.h"

namespace ControllerEmu
{
Input::Input(Translatability translate_, std::string name_, std::string ui_name_)
    : Control(std::make_unique<InputReference>(), translate_, std::move(name_), std::move(ui_name_))
{
}

Input::Input(Translatability translate_, std::string name_)
    : Control(std::make_unique<InputReference>(), translate_, std::move(name_))
{
}
}  // namespace ControllerEmu
