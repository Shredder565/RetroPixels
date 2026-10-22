// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <span>
#include <string>

#include "Common/Matrix.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class EmulatedController;
}  // namespace ControllerEmu

// Helpers for turning detected inputs into mapping expressions, used by the mapping UI.
namespace ciface::MappingCommon
{
enum class Quote
{
  On,
  Off
};

// The expression for one control: "Device:Control" when the control's device is not the default
// device, plain "Control" otherwise; with Quote::On it is put in backticks unless it is all
// letters.
std::string GetExpressionForControl(const std::string& control_name,
                                    const Core::DeviceQualifier& control_device,
                                    const Core::DeviceQualifier& default_device,
                                    Quote quote = Quote::On);

// Builds an expression from a sequence of detections: inputs held together become "a&b" (or a
// "@(a+b)" hotkey when pressed one after another), separate presses become alternatives "a|b".
std::string BuildExpression(const Core::InputDetector::Results&,
                            const Core::DeviceQualifier& default_device, Quote quote);

// Drops digital detections made alongside an analog one (e.g. the click at the end of a trigger).
void RemoveSpuriousTriggerCombinations(Core::InputDetector::Results*);
// Drops detections still held at, or released at or after, `after`.
void RemoveDetectionsAfterTimePoint(Core::InputDetector::Results*, Clock::time_point after);
// True if any detection has been released.
bool ContainsCompleteDetection(const Core::InputDetector::Results&);

// Maps the four directions of a stick (or similar) by detecting them one after another,
// starting with up and going clockwise.
class ReshapableInputMapper
{
public:
  static constexpr std::size_t REQUIRED_INPUT_COUNT = 4;

  // The caller holds the controller state lock.
  ReshapableInputMapper(const Core::DeviceContainer& container,
                        std::span<const std::string> device_strings);

  // Polls the inputs; returns true if a new direction was detected by this call.
  // The caller holds the controller state lock.
  bool Update();

  // Angle of the direction wanted next, in radians counter-clockwise from east (for the UI).
  float GetCurrentAngle() const;

  // True once all four directions are detected or the time ran out.
  bool IsComplete() const;

  // True if any of the four detections was analog, so the stick should be calibrated.
  // Must be asked before ApplyResults().
  bool IsCalibrationNeeded() const;

  // Writes the four mappings once IsComplete(). The caller holds the controller state lock.
  bool ApplyResults(ControllerEmu::EmulatedController*, ControllerEmu::ReshapableInput* stick);

private:
  Core::InputDetector m_detector;
};

// Records the shape of a stick's range of motion while the user turns it around.
class CalibrationBuilder
{
public:
  // With nullopt the first point seen becomes the centre.
  explicit CalibrationBuilder(std::optional<Common::DVec2> center = Common::DVec2{});

  // Adds the movement from the previous point to `point`.
  void Update(Common::DVec2 point);

  // True once the data looks like a full turn of a stick (for the UI's "Finish" hint).
  bool IsCalibrationDataSensible() const;

  // True once the data is sensible and the stick is back near the centre.
  bool IsComplete() const;

  // The recorded radius at `angle`, for drawing the calibration in progress.
  ControlState GetCalibrationRadiusAtAngle(double angle) const;

  // Stores the centre and calibration in `stick`. The caller holds the controller state lock.
  void ApplyResults(ControllerEmu::ReshapableInput* stick);

  Common::DVec2 GetCenter() const;

private:
  ControllerEmu::ReshapableInput::CalibrationData m_samples;
  std::optional<Common::DVec2> m_origin = std::nullopt;
  Common::DVec2 m_last_offset{};
};

}  // namespace ciface::MappingCommon
