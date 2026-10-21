// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/IMUGyroscope.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
namespace
{
enum GyroControl : std::size_t
{
  PITCH_UP,
  PITCH_DOWN,
  ROLL_LEFT,
  ROLL_RIGHT,
  YAW_LEFT,
  YAW_RIGHT,
};

using Seconds = std::chrono::duration<double>;

// A stable period is restarted after this long (keeps the clock arithmetic bounded).
constexpr auto LONGEST_STABLE_PERIOD = std::chrono::hours(1);

// Readings must arrive at least this often (per second) to count as one stable period, so a
// pause between sessions does not get averaged in. Somewhat below the UI's 30 Hz refresh.
constexpr auto MINIMUM_SAMPLE_RATE = 25;

// Input names, in GyroControl order.
constexpr std::array<const char*, 6> GYRO_INPUT_NAMES = {
    _trans("Pitch Up"), _trans("Pitch Down"), _trans("Roll Left"),
    _trans("Roll Right"), _trans("Yaw Left"), _trans("Yaw Right"),
};
}  // namespace

IMUGyroscope::IMUGyroscope(std::string name_, std::string ui_name_)
    : ControlGroup(std::move(name_), std::move(ui_name_), GroupType::IMUGyroscope)
{
  for (const char* input_name : GYRO_INPUT_NAMES)
    AddInput(Translatability::Translate, input_name);

  AddSetting(&m_deadzone_setting,
             {_trans("Dead Zone"),
              // i18n: Unit suffix, degrees per second.
              _trans("°/s"),
              // i18n: Tooltip of the gyroscope dead zone setting.
              _trans("Angular velocity to ignore and remap.")},
             2, 0, 180);

  AddSetting(&m_calibration_period_setting,
             {_trans("Calibration Period"),
              // i18n: Unit suffix, seconds.
              _trans("s"),
              // i18n: Tooltip of the gyroscope calibration setting.
              _trans("Time period of stable input to trigger calibration. (zero to disable)")},
             3, 0, 30);
}

void IMUGyroscope::RestartCalibration()
{
  m_stable_since = Clock::now();
  m_stable_mean.Clear();
}

void IMUGyroscope::UpdateCalibration(const StateData& state)
{
  const Clock::time_point now = Clock::now();
  const double period = m_calibration_period_setting.GetValue();

  // A zero period turns calibration off.
  if (!period)
  {
    m_zero_rate = {};
    RestartCalibration();
    return;
  }

  if (m_stable_mean.Count() == 0)
  {
    // Calibration just started (a gyro was mapped or calibration enabled): the first reading is
    // a better zero than nothing.
    m_zero_rate = state;
  }
  else
  {
    const double sample_rate =
        static_cast<double>(m_stable_mean.Count()) /
        std::chrono::duration_cast<Seconds>(now - m_stable_since).count();
    const StateData drift = state - m_stable_mean.Mean();
    const double deadzone = GetDeadzone();

    const bool too_slow = sample_rate < MINIMUM_SAMPLE_RATE;
    if (too_slow || std::ranges::any_of(drift.data, [&](float rate) {
          return std::abs(rate) > deadzone;
        }))
    {
      RestartCalibration();
    }
  }

  m_stable_mean.Push(state);

  // Stable for long enough: the mean becomes the zero rate.
  const Clock::duration stable_for = now - m_stable_since;
  if (stable_for >= Seconds(period))
  {
    m_zero_rate = m_stable_mean.Mean();
    if (stable_for >= LONGEST_STABLE_PERIOD)
    {
      RestartCalibration();
      m_stable_mean.Push(m_zero_rate);
    }
  }
}

auto IMUGyroscope::GetRawState() const -> StateData
{
  return StateData(
      static_cast<float>(controls[PITCH_DOWN]->GetState() - controls[PITCH_UP]->GetState()),
      static_cast<float>(controls[ROLL_LEFT]->GetState() - controls[ROLL_RIGHT]->GetState()),
      static_cast<float>(controls[YAW_LEFT]->GetState() - controls[YAW_RIGHT]->GetState()));
}

bool IMUGyroscope::AreInputsBound() const
{
  return std::ranges::any_of(controls, [](const std::unique_ptr<Control>& control) {
    return control->control_ref->BoundCount() > 0;
  });
}

bool IMUGyroscope::CanCalibrate() const
{
  // With the input gate closed every reading is 0, which would calibrate to a wrong zero.
  return ControlReference::GetInputGate();
}

std::optional<IMUGyroscope::StateData> IMUGyroscope::GetState(bool update)
{
  if (!AreInputsBound())
  {
    if (update)
    {
      m_zero_rate = {};
      RestartCalibration();
    }
    return std::nullopt;
  }

  StateData state = GetRawState();

  // Calibration reads the gated state too, so background input behaves like everything else.
  if (update && CanCalibrate())
    UpdateCalibration(state);

  state -= m_zero_rate;

  // Rates inside the dead zone become 0 (the dead zone is read for each axis).
  for (float& rate : state.data)
    rate *= static_cast<float>(std::abs(rate) > GetDeadzone());

  return state;
}

ControlState IMUGyroscope::GetDeadzone() const
{
  return m_deadzone_setting.GetValue() / 360 * MathUtil::TAU;
}

bool IMUGyroscope::IsCalibrating() const
{
  const double period = m_calibration_period_setting.GetValue();
  if (!period)
    return false;
  return (Clock::now() - m_stable_since) >= Seconds(period);
}

}  // namespace ControllerEmu
