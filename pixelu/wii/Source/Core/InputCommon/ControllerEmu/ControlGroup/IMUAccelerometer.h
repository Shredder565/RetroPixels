// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include "Common/Matrix.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

namespace ControllerEmu
{
// Accelerometer data from a host device with motion sensors, as six half-axes.
class IMUAccelerometer : public ControlGroup
{
public:
  using StateData = Common::Vec3;

  IMUAccelerometer(std::string name, std::string ui_name);

  // Nothing when no axis is mapped.
  std::optional<StateData> GetState() const;

  bool AreInputsBound() const;
};
}  // namespace ControllerEmu
