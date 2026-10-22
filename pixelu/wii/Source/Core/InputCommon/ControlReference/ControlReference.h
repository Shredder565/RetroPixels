// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "InputCommon/ControlReference/ExpressionParser.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// Converts a control state to the type a caller asks for (see the specialisations below).
template <typename T>
T ControlStateCast(ControlState value);
}  // namespace ControllerEmu

// A ControlReference owns one mapping expression (the text a user typed for a button, an axis or
// a rumble motor) together with its parsed form. After the text changes, UpdateReference() has to
// be called to resolve the device controls the expression names.
class ControlReference
{
public:
  // The input gate is a per-thread switch. While it is closed every input reads as 0 (used when
  // the render window does not have focus and background input is off).
  static void SetInputGate(bool enable);
  static bool GetInputGate();

  virtual ~ControlReference();

  // Inputs return their current value; outputs take the value to send and return 0.
  virtual ControlState State(const ControlState state = 0) = 0;
  virtual bool IsInput() const = 0;

  template <typename T>
  T GetState()
  {
    return ControllerEmu::ControlStateCast<T>(State());
  }

  int BoundCount() const;
  ciface::ExpressionParser::ParseStatus GetParseStatus() const;
  void UpdateReference(ciface::ExpressionParser::ControlEnvironment& env);
  std::string GetExpression() const;

  // Stores and parses a new expression. On a syntax error the text is still kept and a
  // human-readable description of the problem is returned.
  std::optional<std::string> SetExpression(std::string expr);

  // Scale applied to the expression's value (the "Range" a user can set per control).
  ControlState range = 1;

protected:
  ControlReference();

  std::string m_expression;
  std::unique_ptr<ciface::ExpressionParser::Expression> m_parsed_expression;
  ciface::ExpressionParser::ParseStatus m_parse_status =
      ciface::ExpressionParser::ParseStatus::EmptyExpression;
};

namespace ControllerEmu
{
// A digital reading is "on" once the value rounds to 1 or more.
template <>
inline bool ControlStateCast<bool>(ControlState value)
{
  return std::lround(value) >= 1;
}

template <>
inline int ControlStateCast<int>(ControlState value)
{
  return static_cast<int>(std::lround(value));
}

template <>
inline ControlState ControlStateCast<ControlState>(ControlState value)
{
  return value;
}
}  // namespace ControllerEmu

// Reference used for every mapped input (buttons, axes, settings).
class InputReference : public ControlReference
{
public:
  InputReference();
  bool IsInput() const override;
  ControlState State(const ControlState state) override;
};

// Reference used for outputs such as rumble motors.
class OutputReference : public ControlReference
{
public:
  OutputReference();
  bool IsInput() const override;
  ControlState State(const ControlState state) override;
};
