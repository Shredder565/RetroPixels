// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/IMUAccelerometer.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "Common/Common.h"
#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
namespace
{
enum AccelControl : std::size_t
{
  ACCEL_UP,
  ACCEL_DOWN,
  ACCEL_LEFT,
  ACCEL_RIGHT,
  ACCEL_FORWARD,
  ACCEL_BACKWARD,
};
}  // namespace

IMUAccelerometer::IMUAccelerometer(std::string name_, std::string ui_name_)
    : ControlGroup(std::move(name_), std::move(ui_name_), GroupType::IMUAccelerometer)
{
  AddInput(Translatability::Translate, _trans("Up"));
  AddInput(Translatability::Translate, _trans("Down"));
  AddInput(Translatability::Translate, _trans("Left"));
  AddInput(Translatability::Translate, _trans("Right"));
  AddInput(Translatability::Translate, _trans("Forward"));
  AddInput(Translatability::Translate, _trans("Backward"));
}

bool IMUAccelerometer::AreInputsBound() const
{
  for (const auto& control : controls)
  {
    if (control->control_ref->BoundCount() > 0)
      return true;
  }
  return false;
}

std::optional<IMUAccelerometer::StateData> IMUAccelerometer::GetState() const
{
  if (!AreInputsBound())
    return std::nullopt;

  // x: left minus right, y: backward minus forward, z: up minus down.
  StateData state;
  state.x = static_cast<float>(controls[ACCEL_LEFT]->GetState() -
                               controls[ACCEL_RIGHT]->GetState());
  state.y = static_cast<float>(controls[ACCEL_BACKWARD]->GetState() -
                               controls[ACCEL_FORWARD]->GetState());
  state.z = static_cast<float>(controls[ACCEL_UP]->GetState() - controls[ACCEL_DOWN]->GetState());
  return state;
}

}  // namespace ControllerEmu
