// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "Common/BitUtils.h"
#include "Common/Common.h"
#include "Common/IniFile.h"
#include "Common/MathUtil.h"
#include "InputCommon/ControlReference/ExpressionParser.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

class ControllerInterface;
class InputConfig;

// Direction names shared by sticks, cursors and similar groups (also their ini key names).
constexpr const char* DIRECTION_UP = _trans("Up");
constexpr const char* DIRECTION_DOWN = _trans("Down");
constexpr const char* DIRECTION_LEFT = _trans("Left");
constexpr const char* DIRECTION_RIGHT = _trans("Right");

constexpr const char* named_directions[] = {DIRECTION_UP, DIRECTION_DOWN, DIRECTION_LEFT,
                                            DIRECTION_RIGHT};

class ControlReference;

namespace ControllerEmu
{
class ControlGroup;

// Calibration given as a zero point and a full-scale point, e.g. Wii Remote accelerometer data.
// Bits is the precision the points are stored with.
template <typename T, size_t Bits>
struct TwoPointCalibration
{
  TwoPointCalibration() = default;
  TwoPointCalibration(const T& zero_, const T& max_) : zero{zero_}, max{max_} {}

  // Usable only when full scale differs from zero (in every component).
  constexpr bool IsSane() const
  {
    if constexpr (std::is_arithmetic_v<T>)
    {
      return zero != max;
    }
    else
    {
      for (size_t i = 0; i != std::size(zero.data); ++i)
      {
        if (zero.data[i] == max.data[i])
          return false;
      }
      return true;
    }
  }

  static constexpr size_t BITS_OF_PRECISION = Bits;

  T zero;
  T max;
};

// Calibration given as minimum, centre and maximum points, e.g. analog stick data.
template <typename T, size_t Bits>
struct ThreePointCalibration
{
  ThreePointCalibration() = default;
  ThreePointCalibration(const T& min_, const T& zero_, const T& max_)
      : min{min_}, zero{zero_}, max{max_}
  {
  }

  // Usable only when the centre lies strictly between minimum and maximum (in every component).
  constexpr bool IsSane() const
  {
    if constexpr (std::is_arithmetic_v<T>)
    {
      return MathUtil::Sign(zero - min) * MathUtil::Sign(zero - max) == -1;
    }
    else
    {
      for (size_t i = 0; i != std::size(zero.data); ++i)
      {
        const auto below = MathUtil::Sign(zero.data[i] - min.data[i]);
        const auto above = MathUtil::Sign(zero.data[i] - max.data[i]);
        if (below * above != -1)
          return false;
      }
      return true;
    }
  }

  static constexpr size_t BITS_OF_PRECISION = Bits;

  T min;
  T zero;
  T max;
};

// An uncalibrated reading (a scalar or a Common::TVecN of an unsigned type) with Bits of
// precision. GetNormalizedValue() maps it through a calibration, first widening whichever side
// has fewer bits so both have the same precision.
template <typename T, size_t Bits>
struct RawValue
{
  constexpr RawValue() = default;
  constexpr explicit RawValue(const T& value_) : value{value_} {}

  static constexpr size_t BITS_OF_PRECISION = Bits;

  T value;

  constexpr bool operator==(const RawValue& other) const = default;

  // (value - zero) / (max - zero)
  template <typename OtherT, size_t OtherBits>
  auto GetNormalizedValue(const TwoPointCalibration<OtherT, OtherBits>& calibration) const
  {
    const auto widen_value = WidthDifference(calibration.BITS_OF_PRECISION, BITS_OF_PRECISION);
    const auto widen_calibration =
        WidthDifference(BITS_OF_PRECISION, calibration.BITS_OF_PRECISION);

    // Multiplying by 1.f turns a scalar or a vector into floats.
    const auto zero_point = ExpandValue(calibration.zero, widen_calibration) * 1.f;
    const auto full_point = ExpandValue(calibration.max, widen_calibration) * 1.f;
    const auto reading = ExpandValue(value, widen_value) * 1.f;

    return (reading - zero_point) / (full_point - zero_point);
  }

  // (value - zero) divided by (max - zero) above the centre and by (zero - min) below it. The
  // side is chosen by comparing the unwidened values.
  template <typename OtherT, size_t OtherBits>
  auto GetNormalizedValue(const ThreePointCalibration<OtherT, OtherBits>& calibration) const
  {
    const auto widen_value = WidthDifference(calibration.BITS_OF_PRECISION, BITS_OF_PRECISION);
    const auto widen_calibration =
        WidthDifference(BITS_OF_PRECISION, calibration.BITS_OF_PRECISION);

    const auto low_point = ExpandValue(calibration.min, widen_calibration) * 1.f;
    const auto zero_point = ExpandValue(calibration.zero, widen_calibration) * 1.f;
    const auto high_point = ExpandValue(calibration.max, widen_calibration) * 1.f;
    const auto reading = ExpandValue(value, widen_value) * 1.f;

    const auto above = calibration.zero < value;
    const auto span =
        above * 1.f * (high_point - zero_point) + !above * 1.f * (zero_point - low_point);
    return (reading - zero_point) / span;
  }

  template <typename OtherT>
  static OtherT ExpandValue(OtherT value, size_t bits)
  {
    if constexpr (std::is_arithmetic_v<OtherT>)
    {
      return Common::ExpandValue(value, bits);
    }
    else
    {
      for (auto& component : value.data)
        component = Common::ExpandValue(component, bits);
      return value;
    }
  }

private:
  // How many bits `narrow` must be widened by to match `wide` (0 if it is not narrower).
  static constexpr int WidthDifference(size_t wide, size_t narrow)
  {
    return std::max(0, int(wide) - int(narrow));
  }
};

// Maps -1.0 .. +1.0 onto neg_1_value .. pos_1_value with 0.0 at zero_value, rounding to the
// nearest integer (so a tiny negative left over from deadzone maths still gives zero_value).
template <typename T, typename F>
T MapFloat(F input_value, T zero_value, T neg_1_value = std::numeric_limits<T>::min(),
           T pos_1_value = std::numeric_limits<T>::max())
{
  static_assert(std::is_integral<T>(), "MapFloat produces integers.");
  static_assert(std::is_floating_point<F>(), "MapFloat takes a floating point value.");
  static_assert(std::numeric_limits<long long>::min() <= std::numeric_limits<T>::min() &&
                    std::numeric_limits<long long>::max() >= std::numeric_limits<T>::max(),
                "T must fit in long long for std::llround.");

  const auto scaled = input_value > 0 ? (pos_1_value - zero_value) * input_value + zero_value :
                                        (zero_value - neg_1_value) * input_value + zero_value;
  return T(std::llround(scaled));
}

// Inverse of MapFloat: maps an integer in neg_1_value .. pos_1_value to -1.0 .. +1.0.
template <typename F, typename T>
F MapToFloat(T input_value, T zero_value, T neg_1_value = std::numeric_limits<T>::min(),
             T pos_1_value = std::numeric_limits<T>::max())
{
  static_assert(std::is_integral<T>(), "MapToFloat takes an integer.");
  static_assert(std::is_floating_point<F>(), "MapToFloat produces floating point values.");

  if (input_value < zero_value)
    return -F(zero_value - input_value) / F(zero_value - neg_1_value);
  return F(input_value - zero_value) / F(pos_1_value - zero_value);
}

// Something that owns control groups: an emulated controller or one of its attachments.
class ControlGroupContainer
{
public:
  virtual ~ControlGroupContainer();

  virtual void LoadGroupsConfig(Common::IniFile::Section* sec, const std::string& base);
  virtual void SaveGroupsConfig(Common::IniFile::Section* sec, const std::string& base);

  virtual std::string GetName() const = 0;
  virtual std::string GetDisplayName() const;

  void UpdateGroupsReferences(ciface::ExpressionParser::ControlEnvironment& env);

  void SetInputOverrideFunction(InputOverrideFunction override_func);
  void ClearInputOverrideFunction();

  std::vector<std::unique_ptr<ControlGroup>> groups;

protected:
  InputOverrideFunction m_input_override_function;
};

class EmulatedController : public ControlGroupContainer
{
public:
  ~EmulatedController() override;

  virtual InputConfig* GetConfig() const = 0;

  virtual void LoadDefaults(const ControllerInterface& ciface);

  void LoadConfig(Common::IniFile::Section* sec);
  void SaveConfig(Common::IniFile::Section* sec);

  bool IsDefaultDeviceConnected() const;
  const ciface::Core::DeviceQualifier& GetDefaultDevice() const;
  void SetDefaultDevice(const std::string& device);
  void SetDefaultDevice(ciface::Core::DeviceQualifier devq);

  void UpdateReferences(const ControllerInterface& devi);
  void UpdateSingleControlReference(const ControllerInterface& devi, ControlReference* ref);

  // Hold this lock while reading control references (State(), GetState() and so on): a device
  // hotplug rebinds the references under the same lock. The lock is recursive.
  [[nodiscard]] static std::unique_lock<std::recursive_mutex> GetStateLock();

  const ciface::ExpressionParser::ControlEnvironment::VariableContainer&
  GetExpressionVariables() const;

  // Sets every expression variable back to 0 (the variables themselves stay).
  void ResetExpressionVariables();

protected:
  ciface::ExpressionParser::ControlEnvironment::VariableContainer m_expression_vars;

private:
  ciface::Core::DeviceQualifier m_default_device;
  bool m_default_device_is_connected{false};
};
}  // namespace ControllerEmu
