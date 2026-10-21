// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/StickGate.h"

namespace ControllerEmu
{
// The emulated Wii Remote pointer, mapped from four directions with hide, recenter and
// relative-mode controls. Absolute input places the pointer directly; relative input moves it.
class Cursor : public ReshapableInput
{
public:
  struct StateData
  {
    ControlState x{};
    ControlState y{};

    // A hidden pointer has x set to NaN.
    bool IsVisible() const;
  };

  Cursor(std::string name, std::string ui_name);

  ReshapeData GetReshapableState(bool adjusted) const final;
  ControlState GetGateRadiusAtAngle(double ang) const override;

  // Adjusted reads advance the pointer state (relative movement, auto-hide).
  StateData GetState(bool adjusted);
  StateData GetState(bool adjusted, const ControllerEmu::InputOverrideFunction& override_func);

  // Yaw movement in radians.
  ControlState GetTotalYaw() const;

  // Pitch movement in radians.
  ControlState GetTotalPitch() const;

  // Vertical offset in meters.
  ControlState GetVerticalOffset() const;

  void SetRelativeInput(bool enabled);

private:
  Cursor::StateData UpdateState(Cursor::ReshapeData input);

  // Relative movement speed at full deflection, in screen units (-1..1) per second.
  static constexpr double STEP_PER_SEC = 0.01 * 200;

  // The pointer hides after this long without movement (when auto-hide is on).
  static constexpr int AUTO_HIDE_MS = 2500;
  static constexpr double AUTO_HIDE_DEADZONE = 0.001;

  using Clock = std::chrono::steady_clock;

  // Position before hiding.
  StateData m_position;
  // The previous result, for auto-hide movement detection.
  StateData m_last_result;

  int m_hide_countdown_ms = AUTO_HIDE_MS;
  Clock::time_point m_last_update;

  SettingValue<double> m_yaw_setting;
  SettingValue<double> m_pitch_setting;
  SettingValue<double> m_vertical_offset_setting;

  SettingValue<bool> m_relative_setting;
  SettingValue<bool> m_autohide_setting;
};
}  // namespace ControllerEmu
