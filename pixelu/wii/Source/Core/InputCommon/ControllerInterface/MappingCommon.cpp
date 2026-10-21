// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/MappingCommon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ranges>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/MathUtil.h"
#include "Common/StringUtil.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/InputConfig.h"

namespace ciface::MappingCommon
{
namespace
{
using Detection = Core::InputDetector::Detection;

// Two inputs pressed further apart than this form a hotkey ("@(a+b)") rather than a
// simultaneous combination ("a&b").
constexpr auto HOTKEY_GAP = std::chrono::milliseconds(50);

// A digital detection this close to an analog one is taken to come from the same physical
// control (e.g. a DualShock 4 trigger reports both).
constexpr auto TRIGGER_PAIR_GAP = std::chrono::milliseconds(150);

// Time the user gets for each direction when mapping a stick.
constexpr auto DIRECTION_WAIT = std::chrono::seconds{4};

// Collects the alternatives of an expression while walking through the detections in order.
class AlternativesBuilder
{
public:
  AlternativesBuilder(const Core::DeviceQualifier& default_device, Quote quote)
      : m_default_device(default_device), m_quote(quote)
  {
  }

  void Press(const Detection& detection)
  {
    // Inputs let go before this press end the current combination.
    auto it = m_held.begin();
    while (it != m_held.end())
    {
      const Detection& held = **it;
      if (held.release_time && held.release_time <= detection.press_time)
      {
        Flush();
        it = m_held.erase(it);
      }
      else
      {
        ++it;
      }
    }

    m_held.push_back(&detection);
    m_has_new_press = true;
  }

  // Turns the held inputs into an alternative, if something was pressed since the last one.
  void Flush()
  {
    if (!m_has_new_press)
      return;
    m_has_new_press = false;

    std::vector<std::string> parts;
    for (const Detection* held : m_held)
      parts.push_back(ControlExpression(*held));

    const bool pressed_in_turn =
        m_held.size() >= 2 && (m_held[1]->press_time - m_held[0]->press_time) > HOTKEY_GAP;
    if (pressed_in_turn)
    {
      m_alternatives.push_back(fmt::format("@({})", fmt::join(parts, "+")));
      return;
    }

    std::ranges::sort(parts);
    m_alternatives.push_back(fmt::to_string(fmt::join(parts, "&")));
  }

  // The finished expression: distinct alternatives, sorted, separated by '|'.
  std::string Finish()
  {
    Flush();

    std::ranges::sort(m_alternatives);
    const auto duplicates = std::ranges::unique(m_alternatives);
    m_alternatives.erase(duplicates.begin(), duplicates.end());

    return fmt::to_string(fmt::join(m_alternatives, "|"));
  }

private:
  std::string ControlExpression(const Detection& detection) const
  {
    // With quoting on, a modifier is written by its combined name ("Ctrl" for "L_Ctrl") so
    // hotkeys read naturally; the specific side can still be mapped by hand.
    const Core::Device::Input* const input =
        m_quote == Quote::On ? detection.device->GetParentMostInput(detection.input) :
                               detection.input;

    Core::DeviceQualifier qualifier;
    qualifier.FromDevice(detection.device.get());
    return GetExpressionForControl(input->GetName(), qualifier, m_default_device, m_quote);
  }

  const Core::DeviceQualifier& m_default_device;
  const Quote m_quote;

  std::vector<const Detection*> m_held;
  std::vector<std::string> m_alternatives;
  bool m_has_new_press = false;
};
}  // namespace

std::string GetExpressionForControl(const std::string& control_name,
                                    const ciface::Core::DeviceQualifier& control_device,
                                    const ciface::Core::DeviceQualifier& default_device,
                                    Quote quote)
{
  std::string expression;
  if (!(control_device == default_device))
    expression = control_device.ToString() + ':';
  expression += control_name;

  // Anything but plain letters must be quoted to be parsed back as one name.
  if (quote == Quote::On && !std::ranges::all_of(expression, Common::IsAlpha))
    return fmt::format("`{}`", expression);

  return expression;
}

std::string BuildExpression(const Core::InputDetector::Results& detections,
                            const ciface::Core::DeviceQualifier& default_device, Quote quote)
{
  AlternativesBuilder builder(default_device, quote);
  for (const Detection& detection : detections)
    builder.Press(detection);
  return builder.Finish();
}

void RemoveSpuriousTriggerCombinations(Core::InputDetector::Results* detections)
{
  Core::InputDetector::Results& all = *detections;

  // A digital detection is dropped when an analog one happened close to it. The check looks at
  // the list as the removal rearranges it.
  std::erase_if(all, [&all](const Detection& candidate) {
    if (candidate.IsAnalogPress())
      return false;
    return std::ranges::any_of(all, [&candidate](const Detection& other) {
      return &other != &candidate && other.IsAnalogPress() &&
             std::chrono::abs(other.press_time - candidate.press_time) < TRIGGER_PAIR_GAP;
    });
  });
}

void RemoveDetectionsAfterTimePoint(Core::InputDetector::Results* results, Clock::time_point after)
{
  // Still-held detections count as released at `after`.
  std::erase_if(*results, [after](const Detection& detection) {
    return detection.release_time.value_or(after) >= after;
  });
}

bool ContainsCompleteDetection(const Core::InputDetector::Results& results)
{
  return std::ranges::any_of(
      results, [](const Detection& detection) { return detection.release_time.has_value(); });
}

ReshapableInputMapper::ReshapableInputMapper(const Core::DeviceContainer& container,
                                             std::span<const std::string> device_strings)
{
  m_detector.Start(container, device_strings);
}

bool ReshapableInputMapper::Update()
{
  const size_t count_before = m_detector.GetResults().size();

  // Each direction gets the same wait, and the whole mapping the sum of them.
  m_detector.Update(DIRECTION_WAIT, DIRECTION_WAIT, DIRECTION_WAIT * REQUIRED_INPUT_COUNT);

  return m_detector.GetResults().size() != count_before;
}

float ReshapableInputMapper::GetCurrentAngle() const
{
  // Up first, then clockwise: a quarter turn less for each detected direction.
  constexpr float quarter_turn = float(MathUtil::TAU) * 0.25f;
  const float done = float(m_detector.GetResults().size());
  return quarter_turn - done * quarter_turn;
}

bool ReshapableInputMapper::IsComplete() const
{
  return m_detector.IsComplete() || m_detector.GetResults().size() >= REQUIRED_INPUT_COUNT;
}

bool ReshapableInputMapper::IsCalibrationNeeded() const
{
  const auto first_four = m_detector.GetResults() | std::views::take(REQUIRED_INPUT_COUNT);
  return std::ranges::any_of(first_four, &Detection::IsAnalogPress);
}

bool ReshapableInputMapper::ApplyResults(ControllerEmu::EmulatedController* controller,
                                         ControllerEmu::ReshapableInput* stick)
{
  const Core::InputDetector::Results detected = m_detector.TakeResults();
  if (detected.size() < REQUIRED_INPUT_COUNT)
    return false;

  // Detected as up, right, down, left; the stick's controls are up, down, left, right.
  static constexpr std::array<size_t, REQUIRED_INPUT_COUNT> detection_for_control{0, 2, 3, 1};

  const auto default_device = controller->GetDefaultDevice();

  for (size_t control = 0; control != REQUIRED_INPUT_COUNT; ++control)
  {
    const Detection& source = detected[detection_for_control[control]];

    Core::DeviceQualifier qualifier;
    qualifier.FromDevice(source.device.get());

    ControlReference* const reference = stick->controls[control]->control_ref.get();
    reference->SetExpression(
        GetExpressionForControl(source.input->GetName(), qualifier, default_device, Quote::On));
    controller->UpdateSingleControlReference(g_controller_interface, reference);
  }

  controller->GetConfig()->GenerateControllerTextures();

  return true;
}

CalibrationBuilder::CalibrationBuilder(std::optional<Common::DVec2> center)
    : m_samples(ControllerEmu::ReshapableInput::CALIBRATION_SAMPLE_COUNT, 0.0), m_origin{center}
{
}

void CalibrationBuilder::Update(Common::DVec2 point)
{
  if (!m_origin)
    m_origin = point;

  const Common::DVec2 offset = point - *m_origin;
  ControllerEmu::ReshapableInput::UpdateCalibrationData(m_samples, m_last_offset, offset);
  m_last_offset = offset;
}

bool CalibrationBuilder::IsCalibrationDataSensible() const
{
  // Average radius the user must reach; even the GameCube stick's short throw passes.
  constexpr double MIN_MEAN_RADIUS = 0.6;
  // Spread of the radius around the circle; about what a square gate gives. More than this
  // means only part of the range was covered.
  constexpr double MAX_RADIUS_DEVIATION = 0.14;

  MathUtil::RunningVariance<ControlState> radius;
  for (const ControlState sample : m_samples)
    radius.Push(sample);

  if (radius.Mean() < MIN_MEAN_RADIUS)
    return false;
  return radius.StandardDeviation() < MAX_RADIUS_DEVIATION;
}

bool CalibrationBuilder::IsComplete() const
{
  if (!IsCalibrationDataSensible())
    return false;

  // Back within half the calibrated radius of the centre.
  const double angle = std::atan2(m_last_offset.y, m_last_offset.x) + MathUtil::TAU;
  const double half_radius = 0.5 * GetCalibrationRadiusAtAngle(angle);
  return m_last_offset.LengthSquared() < half_radius * half_radius;
}

ControlState CalibrationBuilder::GetCalibrationRadiusAtAngle(double angle) const
{
  return ControllerEmu::ReshapableInput::GetCalibrationDataRadiusAtAngle(m_samples, angle);
}

void CalibrationBuilder::ApplyResults(ControllerEmu::ReshapableInput* stick)
{
  stick->SetCenter(GetCenter());
  stick->SetCalibrationData(std::move(m_samples));
}

Common::DVec2 CalibrationBuilder::GetCenter() const
{
  return m_origin.value_or(Common::DVec2{});
}

}  // namespace ciface::MappingCommon
