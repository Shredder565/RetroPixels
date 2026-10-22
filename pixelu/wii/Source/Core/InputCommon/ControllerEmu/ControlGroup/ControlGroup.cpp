// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

#include <utility>

#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Input.h"
#include "InputCommon/ControllerEmu/Control/Output.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
namespace
{
// Ranges are stored in percent.
constexpr double RANGE_SCALE = 100.0;
}  // namespace

ControlGroup::ControlGroup(const std::string& name_, const GroupType type_,
                           DefaultValue default_value_)
    : ControlGroup(name_, name_, type_, default_value_)
{
}

ControlGroup::ControlGroup(std::string name_, std::string ui_name_, const GroupType type_,
                           DefaultValue default_value_)
    : name(std::move(name_)), ui_name(std::move(ui_name_)), type(type_),
      default_value(default_value_)
{
  if (default_value == DefaultValue::AlwaysEnabled)
    return;

  const bool on_by_default = default_value == DefaultValue::Enabled;
  enabled_setting = std::make_unique<NumericSetting<bool>>(
      &enabled, NumericSettingDetails{_trans("Enabled")}, on_by_default, false, true);
}

ControlGroup::~ControlGroup() = default;

void ControlGroup::AddVirtualNotchSetting(SettingValue<double>* value, double max_virtual_notch_deg)
{
  const NumericSettingDetails details{
      _trans("Virtual Notches"),
      // i18n: Unit suffix, degrees.
      _trans("°"), _trans("Snap the thumbstick position to the nearest octagonal axis."), nullptr,
      SettingVisibility::Advanced};
  AddSetting(value, details, 0, 0, max_virtual_notch_deg);
}

void ControlGroup::AddDeadzoneSetting(SettingValue<double>* value, double maximum_deadzone)
{
  const NumericSettingDetails details{
      _trans("Dead Zone"),
      // i18n: Unit suffix, percent.
      _trans("%"),
      // i18n: Tooltip of the dead zone setting of an analog input.
      _trans("Input strength to ignore and remap.")};
  AddSetting(value, details, 0, 0, maximum_deadzone);
}

bool ControlGroup::HasEnabledSetting() const
{
  return enabled_setting != nullptr;
}

void ControlGroup::LoadConfig(Common::IniFile::Section* sec, const std::string& base)
{
  const std::string prefix = base + name + "/";

  if (enabled_setting)
    enabled_setting->LoadFromIni(*sec, prefix);

  for (const auto& setting : numeric_settings)
    setting->LoadFromIni(*sec, prefix);

  for (const auto& control : controls)
  {
    const std::string key = prefix + control->name;

    std::string expression;
    sec->Get(key, &expression, "");
    control->control_ref->SetExpression(std::move(expression));

    ControlState& range = control->control_ref->range;
    sec->Get(key + "/Range", &range, RANGE_SCALE);
    range /= 100;
  }
}

void ControlGroup::SaveConfig(Common::IniFile::Section* sec, const std::string& base)
{
  const std::string prefix = base + name + "/";

  if (enabled_setting)
    enabled_setting->SaveToIni(*sec, prefix);

  for (const auto& setting : numeric_settings)
    setting->SaveToIni(*sec, prefix);

  for (const auto& control : controls)
  {
    const std::string key = prefix + control->name;

    // Ini values are single lines.
    std::string expression = control->control_ref->GetExpression();
    ReplaceBreaksWithSpaces(expression);
    sec->Set(key, expression, "");

    sec->Set(key + "/Range", control->control_ref->range * RANGE_SCALE, RANGE_SCALE);
  }
}

void ControlGroup::UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env)
{
  if (enabled_setting)
    enabled_setting->GetInputReference().UpdateReference(env);

  for (const auto& control : controls)
    control->control_ref->UpdateReference(env);

  for (const auto& setting : numeric_settings)
    setting->GetInputReference().UpdateReference(env);
}

void ControlGroup::SetControlExpression(int index, const std::string& expression)
{
  controls.at(static_cast<std::size_t>(index))->control_ref->SetExpression(expression);
}

void ControlGroup::AddInput(Translatability translate, std::string name_)
{
  controls.push_back(std::make_unique<Input>(translate, std::move(name_)));
}

void ControlGroup::AddInput(Translatability translate, std::string name_, std::string ui_name_)
{
  controls.push_back(std::make_unique<Input>(translate, std::move(name_), std::move(ui_name_)));
}

void ControlGroup::AddOutput(Translatability translate, std::string name_)
{
  controls.push_back(std::make_unique<Output>(translate, std::move(name_)));
}

}  // namespace ControllerEmu
