// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/Control/Control.h"

#include <utility>

#include "InputCommon/ControlReference/ControlReference.h"

namespace ControllerEmu
{
Control::Control(std::unique_ptr<ControlReference> ref, Translatability translate_,
                 std::string name_, std::string ui_name_)
    : control_ref(std::move(ref)), translate(translate_), name(std::move(name_)),
      ui_name(std::move(ui_name_))
{
}

Control::Control(std::unique_ptr<ControlReference> ref, Translatability translate_,
                 std::string name_)
    : Control(std::move(ref), translate_, name_, name_)
{
}

Control::~Control() = default;
}  // namespace ControllerEmu
