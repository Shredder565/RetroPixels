// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/IRPassthrough.h"

#include <string>
#include <utility>

#include "Common/Common.h"

#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
namespace
{
// Each object has three controls: X, Y and size.
constexpr size_t CONTROLS_PER_OBJECT = 3;

constexpr const char* OBJECT_CONTROL_NAMES[] = {
    _trans("Object 1 X"), _trans("Object 1 Y"), _trans("Object 1 Size"),
    _trans("Object 2 X"), _trans("Object 2 Y"), _trans("Object 2 Size"),
    _trans("Object 3 X"), _trans("Object 3 Y"), _trans("Object 3 Size"),
    _trans("Object 4 X"), _trans("Object 4 Y"), _trans("Object 4 Size"),
};
}  // namespace

IRPassthrough::IRPassthrough(std::string name_, std::string ui_name_)
    : ControlGroup(std::move(name_), std::move(ui_name_), GroupType::IRPassthrough,
                   ControlGroup::DefaultValue::Disabled)
{
  for (const char* control_name : OBJECT_CONTROL_NAMES)
    AddInput(Translatability::Translate, control_name);
}

ControlState IRPassthrough::GetObjectPositionX(size_t object_index) const
{
  return controls[object_index * CONTROLS_PER_OBJECT]->GetState();
}

ControlState IRPassthrough::GetObjectPositionY(size_t object_index) const
{
  return controls[object_index * CONTROLS_PER_OBJECT + 1]->GetState();
}

ControlState IRPassthrough::GetObjectSize(size_t object_index) const
{
  return controls[object_index * CONTROLS_PER_OBJECT + 2]->GetState();
}

bool IRPassthrough::AreInputsBound() const
{
  for (const auto& control : controls)
  {
    if (control->control_ref->BoundCount() > 0)
      return true;
  }
  return false;
}

}  // namespace ControllerEmu
