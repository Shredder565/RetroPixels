// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
// Gyroscope data from a host device, with automatic zero-rate calibration: once the reading has
// stayed within the dead zone of its running mean for the calibration period, that mean becomes
// the new zero.
class IMUGyroscope : public ControlGroup
{
public:
  using StateData = Common::Vec3;

  IMUGyroscope(std::string name, std::string ui_name);

  StateData GetRawState() const;
  // Calibrated and dead-zoned rates; nothing when no axis is mapped. `update` also advances the
  // calibration.
  std::optional<StateData> GetState(bool update = true);

  // In rad/s.
  ControlState GetDeadzone() const;

  bool IsCalibrating() const;

private:
  using Clock = std::chrono::steady_clock;

  bool AreInputsBound() const;
  bool CanCalibrate() const;
  void RestartCalibration();
  void UpdateCalibration(const StateData&);

  SettingValue<double> m_deadzone_setting;
  SettingValue<double> m_calibration_period_setting;

  StateData m_zero_rate = {};
  MathUtil::RunningMean<StateData> m_stable_mean;
  Clock::time_point m_stable_since = Clock::now();
};
}  // namespace ControllerEmu
