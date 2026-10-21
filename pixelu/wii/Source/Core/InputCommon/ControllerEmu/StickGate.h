// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/Matrix.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
// Virtual notches only snap a stick that is at least this far out.
constexpr ControlState MINIMUM_NOTCH_DISTANCE = 0.9;

// The outline that limits how far a stick can move, as a radius per direction.
class StickGate
{
public:
  // Angle in radians, not negative.
  virtual ControlState GetRadiusAtAngle(double ang) const = 0;

  // The smallest number of evenly spaced samples that describes the shape exactly, if there is
  // one (keeps saved calibration data short).
  virtual std::optional<u32> GetIdealCalibrationSampleCount() const;

  virtual ~StickGate() = default;
};

// The eight-sided gate of GameCube and Wii sticks. `radius` is the distance to the corners.
class OctagonStickGate : public StickGate
{
public:
  explicit OctagonStickGate(ControlState radius);
  ControlState GetRadiusAtAngle(double ang) const final;
  std::optional<u32> GetIdealCalibrationSampleCount() const final;

private:
  const ControlState m_radius;
};

// A circular gate.
class RoundStickGate : public StickGate
{
public:
  explicit RoundStickGate(ControlState radius);
  ControlState GetRadiusAtAngle(double ang) const final;
  std::optional<u32> GetIdealCalibrationSampleCount() const final;

private:
  const ControlState m_radius;
};

// A square gate (e.g. four keys driving a stick).
class SquareStickGate : public StickGate
{
public:
  explicit SquareStickGate(ControlState half_width);
  ControlState GetRadiusAtAngle(double ang) const final;
  std::optional<u32> GetIdealCalibrationSampleCount() const final;

private:
  const ControlState m_half_width;
};

// A two-axis group whose raw input is reshaped: recentred, scaled from the user's calibrated
// input shape to the emulated gate, given a dead zone and optionally snapped to notches.
class ReshapableInput : public ControlGroup
{
public:
  // Samples produced by the calibration UI (saved data may hold any number).
  static constexpr int CALIBRATION_SAMPLE_COUNT = 32;

  // Maximum input radius at evenly spaced angles, starting at angle 0.
  using CalibrationData = std::vector<ControlState>;

  ReshapableInput(std::string name, std::string ui_name, GroupType type);

  using ReshapeData = Common::DVec2;

  // Angles in radians, not negative.
  ControlState GetDeadzoneRadiusAtAngle(double angle) const;
  ControlState GetInputRadiusAtAngle(double angle) const;

  ControlState GetDeadzonePercentage() const;

  virtual ControlState GetVirtualNotchSize() const { return 0.0; }

  virtual ControlState GetGateRadiusAtAngle(double angle) const = 0;
  virtual ReshapeData GetReshapableState(bool adjusted) const = 0;
  virtual ControlState GetDefaultInputRadiusAtAngle(double ang) const;

  void SetCalibrationToDefault();
  void SetCalibrationFromGate(const StickGate& gate);

  // Grows the calibration so that it reaches the segment from point1 to point2.
  static void UpdateCalibrationData(CalibrationData& data, Common::DVec2 point1,
                                    Common::DVec2 point2);
  static ControlState GetCalibrationDataRadiusAtAngle(const CalibrationData& data, double angle);

  const CalibrationData& GetCalibrationData() const;
  void SetCalibrationData(CalibrationData data);

  const ReshapeData& GetCenter() const;
  void SetCenter(ReshapeData center);

  // Axis names passed to input override functions.
  static constexpr const char* X_INPUT_OVERRIDE = "X";
  static constexpr const char* Y_INPUT_OVERRIDE = "Y";
  static constexpr const char* Z_INPUT_OVERRIDE = "Z";

protected:
  ReshapeData Reshape(ControlState x, ControlState y, ControlState modifier = 0.0,
                      ControlState clamp = 1.0) const;

  // The "Modifier" control, for groups that have one.
  virtual Control* GetModifierInput() const;

private:
  void LoadConfig(Common::IniFile::Section*, const std::string& base_name) override;
  void SaveConfig(Common::IniFile::Section*, const std::string& base_name) override;

  CalibrationData m_input_shape;
  SettingValue<double> m_deadzone;
  ReshapeData m_center;
};

}  // namespace ControllerEmu
