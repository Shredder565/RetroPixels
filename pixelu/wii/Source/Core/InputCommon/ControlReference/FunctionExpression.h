// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/ControlReference/ExpressionParser.h"

namespace ciface::ExpressionParser
{
// Values above this count as "pressed" for conditions, toggles, hotkey modifiers and the like.
constexpr ControlState CONDITION_THRESHOLD = 0.5;

// Base of the built-in functions (toggle, if, sin, smooth, ...).
class FunctionExpression : public Expression
{
public:
  struct ArgumentsAreValid
  {
  };

  // The expected argument list, shown to the user as "name(text)".
  struct ExpectedArguments
  {
    std::string text;
  };

  using ArgumentValidation = std::variant<ArgumentsAreValid, ExpectedArguments>;

  int CountNumControls() const override;
  void UpdateReferences(ControlEnvironment& env) override;

  void SetArguments(std::vector<std::unique_ptr<Expression>>&& args);
  virtual ArgumentValidation ValidateArguments() = 0;

  // Functions ignore writes unless they say otherwise.
  void SetValue(ControlState value) override;

protected:
  Expression& GetArg(u32 number);
  u32 GetArgCount() const;

private:
  std::vector<std::unique_ptr<Expression>> m_args;
};

// Returns nullptr for names that are not functions.
std::unique_ptr<FunctionExpression> MakeFunctionExpression(std::string_view name);

}  // namespace ciface::ExpressionParser
