// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/StickGate.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include <fmt/format.h>

#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
namespace
{
// Ini keys and scales (both values are stored in percent).
constexpr const char* KEY_CALIBRATION = "Calibration";
constexpr const char* KEY_CENTER = "Center";
constexpr double SAVED_PERCENT = 100;
// A calibration sample that does not parse.
constexpr double UNPARSED_SAMPLE = 1.0;
// The modifier's range defaults to half travel.
constexpr double MODIFIER_RANGE_DEFAULT = 50.0;

constexpr double EPSILON = 0.00001;
constexpr double SEGMENT_END = 1.00001;

Common::DVec2 PolarToCartesian(double angle, double length)
{
  return Common::DVec2{std::cos(angle), std::sin(angle)} * length;
}

// Where the ray from the origin in direction `ray` crosses the segment a-b, as a distance along
// the ray (in units of the ray's length). Nothing when a and b coincide (as seen along the ray)
// or the ray misses the segment.
std::optional<double> RayHitsSegment(Common::DVec2 ray, Common::DVec2 a, Common::DVec2 b)
{
  const Common::DVec2 edge = b - a;
  const double denominator = edge.Dot({-ray.y, ray.x});
  if (std::abs(denominator) < EPSILON)
    return std::nullopt;

  const double along_edge = a.Dot({ray.y, -ray.x}) / denominator;
  if (along_edge < -EPSILON || along_edge > SEGMENT_END)
    return std::nullopt;

  return edge.Cross(-a) / denominator;
}

// Snaps an angle to the nearest of the eight stick axes when within half the notch width.
double SnapToNotch(double angle, double notch_width)
{
  constexpr double axis_spacing = MathUtil::TAU / 8;
  const double nearest_axis = std::round(angle / axis_spacing) * axis_spacing;
  const double offset =
      std::fmod(angle - nearest_axis + MathUtil::PI, MathUtil::TAU) - MathUtil::PI;
  if (std::abs(offset) < notch_width / 2)
    return nearest_axis;
  return angle;
}
}  // namespace

// --- Gates --------------------------------------------------------------------------------------

std::optional<u32> StickGate::GetIdealCalibrationSampleCount() const
{
  return std::nullopt;
}

OctagonStickGate::OctagonStickGate(ControlState radius) : m_radius(radius)
{
}

ControlState OctagonStickGate::GetRadiusAtAngle(double angle) const
{
  // Within one of the eight sectors, the triangle centre-corner-point has the angle `angle` at
  // the centre and half an interior angle (67.5 degrees) at the corner, so by the law of sines
  // the distance to the edge is radius * sin(corner) / sin(pi - angle - corner).
  constexpr int corners = 8;
  constexpr double interior_sum = (corners - 2) * MathUtil::PI;
  constexpr double corner_angle = interior_sum / corners / 2;

  angle = std::fmod(angle, MathUtil::TAU / corners);
  return m_radius / std::sin(MathUtil::PI - angle - corner_angle) * std::sin(corner_angle);
}

std::optional<u32> OctagonStickGate::GetIdealCalibrationSampleCount() const
{
  return 8;
}

RoundStickGate::RoundStickGate(ControlState radius) : m_radius(radius)
{
}

ControlState RoundStickGate::GetRadiusAtAngle(double) const
{
  return m_radius;
}

std::optional<u32> RoundStickGate::GetIdealCalibrationSampleCount() const
{
  // Same radius everywhere.
  return 1;
}

SquareStickGate::SquareStickGate(ControlState half_width) : m_half_width(half_width)
{
}

ControlState SquareStickGate::GetRadiusAtAngle(double angle) const
{
  // Measure the angle from the nearest side's normal (sides face 0, 90, 180 and 270 degrees).
  constexpr double quarter = MathUtil::TAU / 4;
  const double from_normal = std::fmod(angle + quarter / 2, quarter) - quarter / 2;
  return m_half_width / std::cos(from_normal);
}

std::optional<u32> SquareStickGate::GetIdealCalibrationSampleCount() const
{
  // Samples start at angle 0 (a side's middle), so the corners need samples of their own.
  return 8;
}

// --- ReshapableInput ----------------------------------------------------------------------------

ReshapableInput::ReshapableInput(std::string name_, std::string ui_name_, GroupType type_)
    : ControlGroup(std::move(name_), std::move(ui_name_), type_)
{
  // Users needing more than 50% can still use an expression.
  AddDeadzoneSetting(&m_deadzone, 50);
}

ControlState ReshapableInput::GetDeadzonePercentage() const
{
  return m_deadzone.GetValue() / 100;
}

ControlState ReshapableInput::GetInputRadiusAtAngle(double angle) const
{
  if (m_input_shape.empty())
    return GetDefaultInputRadiusAtAngle(angle);
  return GetCalibrationDataRadiusAtAngle(m_input_shape, angle);
}

ControlState ReshapableInput::GetDeadzoneRadiusAtAngle(double angle) const
{
  // The dead zone follows the calibrated input shape.
  return GetInputRadiusAtAngle(angle) * GetDeadzonePercentage();
}

ControlState ReshapableInput::GetDefaultInputRadiusAtAngle(double angle) const
{
  // Uncalibrated inputs are assumed to have the gate's shape.
  return GetGateRadiusAtAngle(angle);
}

ControlState ReshapableInput::GetCalibrationDataRadiusAtAngle(const CalibrationData& data,
                                                              double angle)
{
  // The outline between two neighbouring samples is a straight line.
  const auto count = data.size();
  const double samples = static_cast<double>(count);
  const auto position = angle / MathUtil::TAU * samples;

  const u32 before = static_cast<u32>(u32(position) % count);
  const u32 after = static_cast<u32>((before + 1) % count);

  const double before_angle = before * MathUtil::TAU / samples;
  const double after_angle = after * MathUtil::TAU / samples;

  const std::optional<double> hit = RayHitsSegment(PolarToCartesian(angle, 1.0),
                                                   PolarToCartesian(before_angle, data[before]),
                                                   PolarToCartesian(after_angle, data[after]));
  return hit.value_or(data[before]);
}

void ReshapableInput::SetCalibrationToDefault()
{
  m_input_shape.clear();
}

void ReshapableInput::SetCalibrationFromGate(const StickGate& gate)
{
  const u32 samples = gate.GetIdealCalibrationSampleCount().value_or(CALIBRATION_SAMPLE_COUNT);
  m_input_shape.resize(samples);

  for (u32 i = 0; i != m_input_shape.size(); ++i)
  {
    const double angle = MathUtil::TAU * i / static_cast<double>(m_input_shape.size());
    m_input_shape[i] = gate.GetRadiusAtAngle(angle);
  }
}

void ReshapableInput::UpdateCalibrationData(CalibrationData& data, Common::DVec2 point1,
                                            Common::DVec2 point2)
{
  const auto count = data.size();
  for (u32 i = 0; i != count; ++i)
  {
    const double angle = i * MathUtil::TAU / static_cast<double>(count);
    const std::optional<double> hit = RayHitsSegment(PolarToCartesian(angle, 1.0), point1, point2);
    data[i] = std::max(data[i], hit.value_or(data[i]));
  }
}

const ReshapableInput::CalibrationData& ReshapableInput::GetCalibrationData() const
{
  return m_input_shape;
}

void ReshapableInput::SetCalibrationData(CalibrationData data)
{
  m_input_shape = std::move(data);
}

const ReshapableInput::ReshapeData& ReshapableInput::GetCenter() const
{
  return m_center;
}

void ReshapableInput::SetCenter(ReshapableInput::ReshapeData center)
{
  m_center = center;
}

Control* ReshapableInput::GetModifierInput() const
{
  return nullptr;
}

void ReshapableInput::LoadConfig(Common::IniFile::Section* section, const std::string& base_name)
{
  ControlGroup::LoadConfig(section, base_name);

  const std::string prefix = base_name + name + '/';

  // Unlike other controls, a modifier's range defaults to 50%.
  if (const Control* const modifier = GetModifierInput())
  {
    ControlState& range = modifier->control_ref->range;
    section->Get(prefix + modifier->name + "/Range", &range, MODIFIER_RANGE_DEFAULT);
    range /= 100;
  }

  std::string text;
  section->Get(prefix + KEY_CALIBRATION, &text, "");
  const std::vector<std::string> samples = SplitString(text, ' ');

  m_input_shape.assign(samples.size(), UNPARSED_SAMPLE);
  for (std::size_t i = 0; i != samples.size(); ++i)
  {
    if (TryParse(samples[i], &m_input_shape[i]))
      m_input_shape[i] /= SAVED_PERCENT;
  }

  section->Get(prefix + KEY_CENTER, &text, "");
  const std::vector<std::string> center = SplitString(text, ' ');

  m_center = Common::DVec2();
  if (center.size() != 2)
    return;
  if (TryParse(center[0], &m_center.x))
    m_center.x /= SAVED_PERCENT;
  if (TryParse(center[1], &m_center.y))
    m_center.y /= SAVED_PERCENT;
}

void ReshapableInput::SaveConfig(Common::IniFile::Section* section, const std::string& base_name)
{
  ControlGroup::SaveConfig(section, base_name);

  const std::string prefix = base_name + name + '/';

  if (const Control* const modifier = GetModifierInput())
  {
    section->Set(prefix + modifier->name + "/Range", modifier->control_ref->range * 100,
                 MODIFIER_RANGE_DEFAULT);
  }

  // Space-separated percentages with two decimals.
  std::string samples;
  for (const ControlState radius : m_input_shape)
  {
    if (!samples.empty())
      samples += ' ';
    fmt::format_to(std::back_inserter(samples), "{:.2f}", radius * SAVED_PERCENT);
  }
  section->Set(prefix + KEY_CALIBRATION, samples, "");

  const auto format_center = [](double x, double y) { return fmt::format("{:.2f} {:.2f}", x, y); };
  section->Set(prefix + KEY_CENTER,
               format_center(m_center.x * SAVED_PERCENT, m_center.y * SAVED_PERCENT),
               format_center(0.0, 0.0));
}

ReshapableInput::ReshapeData ReshapableInput::Reshape(ControlState x, ControlState y,
                                                      ControlState modifier,
                                                      ControlState clamp) const
{
  x -= m_center.x;
  y -= m_center.y;

  // atan2 is fine at the origin too (the distance is 0 then). The radius functions expect
  // non-negative angles, hence the extra turn.
  double angle = std::atan2(y, x) + MathUtil::TAU;

  const ControlState input_radius = GetInputRadiusAtAngle(angle);
  ControlState gate_radius = GetGateRadiusAtAngle(angle);

  // A zero calibrated radius would divide by zero; use the gate instead.
  const ControlState input_scale = input_radius != 0.0 ? input_radius : gate_radius;
  ControlState distance = Common::DVec2{x, y}.Length() / input_scale;

  const double notch = GetVirtualNotchSize();
  if (notch > 0.0 && distance >= MINIMUM_NOTCH_DISTANCE)
  {
    angle = SnapToNotch(angle, notch);
    gate_radius = GetGateRadiusAtAngle(angle);
  }

  // A held modifier scales the distance by its value (its range, 50% by default).
  if (modifier != 0.0)
    distance *= modifier;

  distance = ApplyDeadzone(distance, GetDeadzonePercentage());
  distance *= gate_radius;

  return {std::clamp(std::cos(angle) * distance, -clamp, clamp),
          std::clamp(std::sin(angle) * distance, -clamp, clamp)};
}

}  // namespace ControllerEmu
