// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

#include <string>

#include <fmt/format.h>

#include "Common/StringUtil.h"

namespace ControllerEmu
{
NumericSettingBase::NumericSettingBase(const NumericSettingDetails& details) : m_details(details)
{
}

template <typename T>
void NumericSetting<T>::SimplifyIfPossible()
{
  const std::string trimmed(StripWhitespace(m_value.m_reference.GetExpression()));
  ValueType parsed{};
  if (TryParse(trimmed, &parsed))
    m_value.SetValue(parsed);
}

template <typename T>
void NumericSetting<T>::SetExpressionFromValue()
{
  // The explicit sign keeps a number from being read as a key name: "1" is a key on keyboard
  // devices, "+1" is not.
  const ControlState value = ControlState(GetValue());
  m_value.m_reference.SetExpression(fmt::format("{:+g}", value));
}

// The member functions defined here exist for these three types only.
template class NumericSetting<int>;
template class NumericSetting<double>;
template class NumericSetting<bool>;

}  // namespace ControllerEmu
