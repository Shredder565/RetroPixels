// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
namespace
{
enum CursorControl : std::size_t
{
  CURSOR_UP,
  CURSOR_DOWN,
  CURSOR_LEFT,
  CURSOR_RIGHT,
  CURSOR_HIDE,
  CURSOR_RECENTER,
  CURSOR_RELATIVE_HOLD,
};
}  // namespace

Cursor::Cursor(std::string name_, std::string ui_name_)
    : ReshapableInput(std::move(name_), std::move(ui_name_), GroupType::Cursor),
      m_last_update(Clock::now())
{
  for (const char* direction : named_directions)
    AddInput(Translatability::Translate, direction);
  AddInput(Translatability::Translate, _trans("Hide"));
  AddInput(Translatability::Translate, _trans("Recenter"));
  AddInput(Translatability::Translate, _trans("Relative Input Hold"));

  // The defaults reach the screen edges in most games, the Wii Menu included.
  AddSetting(&m_vertical_offset_setting,
             // i18n: How far the emulated Wii Remote is moved up or down.
             {_trans("Vertical Offset"),
              // i18n: Unit suffix, centimeters.
              _trans("cm")},
             10, -100, 100);

  AddSetting(&m_yaw_setting,
             // i18n: Rotation about the vertical ("yaw") axis.
             {_trans("Total Yaw"),
              // i18n: Unit suffix, degrees.
              _trans("°"),
              // i18n: Tooltip of a Wii Remote pointing setting.
              _trans("Total rotation about the yaw axis.")},
             25, 0, 360);

  AddSetting(&m_pitch_setting,
             // i18n: Rotation about the side-to-side ("pitch") axis.
             {_trans("Total Pitch"),
              // i18n: Unit suffix, degrees.
              _trans("°"),
              // i18n: Tooltip of a Wii Remote pointing setting.
              _trans("Total rotation about the pitch axis.")},
             20, 0, 360);

  AddSetting(&m_relative_setting, {_trans("Relative Input")}, false);
  AddSetting(&m_autohide_setting, {_trans("Auto-Hide")}, false);
}

Cursor::ReshapeData Cursor::GetReshapableState(bool adjusted) const
{
  const ControlState y = controls[CURSOR_UP]->GetState() - controls[CURSOR_DOWN]->GetState();
  const ControlState x = controls[CURSOR_RIGHT]->GetState() - controls[CURSOR_LEFT]->GetState();

  if (!adjusted)
    return {x, y};
  return Reshape(x, y, 0.0);
}

ControlState Cursor::GetGateRadiusAtAngle(double ang) const
{
  return SquareStickGate(1.0).GetRadiusAtAngle(ang);
}

Cursor::StateData Cursor::GetState(const bool adjusted)
{
  const ReshapeData input = GetReshapableState(adjusted);
  if (!adjusted)
    return StateData{input.x, input.y};
  return UpdateState(input);
}

Cursor::StateData Cursor::GetState(const bool adjusted,
                                   const ControllerEmu::InputOverrideFunction& override_func)
{
  StateData state = GetState(adjusted);
  if (!override_func)
    return state;

  if (const std::optional<ControlState> x = override_func(name, X_INPUT_OVERRIDE, state.x))
    state.x = *x;
  if (const std::optional<ControlState> y = override_func(name, Y_INPUT_OVERRIDE, state.y))
    state.y = *y;
  return state;
}

Cursor::StateData Cursor::UpdateState(Cursor::ReshapeData input)
{
  // Wall-clock based for now; the pointer state really belongs to the emulated Wii Remote.
  const Clock::time_point now = Clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_update).count();
  m_last_update = now;

  const double step = STEP_PER_SEC / 1000.0 * static_cast<double>(elapsed_ms);

  // The hold control inverts the relative-input setting while held.
  const bool relative = m_relative_setting.GetValue() ^ controls[CURSOR_RELATIVE_HOLD]->GetState<bool>();
  if (!relative)
  {
    m_position.x = input.x;
    m_position.y = input.y;
  }
  else if (controls[CURSOR_RECENTER]->GetState<bool>())
  {
    m_position = {};
  }
  else
  {
    m_position.x = std::clamp(m_position.x + input.x * step, -1.0, 1.0);
    m_position.y = std::clamp(m_position.y + input.y * step, -1.0, 1.0);
  }

  StateData result = m_position;

  // Auto-hide: any movement (or auto-hide being off) restarts the countdown.
  const bool autohide = m_autohide_setting.GetValue();
  const bool moved = std::abs(m_last_result.x - result.x) > AUTO_HIDE_DEADZONE ||
                     std::abs(m_last_result.y - result.y) > AUTO_HIDE_DEADZONE;
  if (!autohide || moved)
    m_hide_countdown_ms = AUTO_HIDE_MS;
  else if (m_hide_countdown_ms != 0)
    m_hide_countdown_ms -= std::min(static_cast<int>(elapsed_ms), m_hide_countdown_ms);

  m_last_result = result;

  // Hidden once the countdown ran out, while "Hide" is held, or while the input gate is closed
  // (a closed gate makes "Hide" read as released, so it is checked on its own).
  if (m_hide_countdown_ms == 0 || !ControlReference::GetInputGate() ||
      controls[CURSOR_HIDE]->GetState<bool>())
  {
    result.x = std::numeric_limits<ControlState>::quiet_NaN();
    result.y = 0;
  }

  return result;
}

ControlState Cursor::GetTotalYaw() const
{
  return m_yaw_setting.GetValue() * MathUtil::TAU / 360;
}

ControlState Cursor::GetTotalPitch() const
{
  return m_pitch_setting.GetValue() * MathUtil::TAU / 360;
}

ControlState Cursor::GetVerticalOffset() const
{
  return m_vertical_offset_setting.GetValue() / 100;
}

void Cursor::SetRelativeInput(bool value)
{
  m_relative_setting.SetValue(value);
}

bool Cursor::StateData::IsVisible() const
{
  return !std::isnan(x);
}

}  // namespace ControllerEmu
