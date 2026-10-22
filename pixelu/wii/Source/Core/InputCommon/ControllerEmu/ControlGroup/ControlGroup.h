// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Common/IniFile.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class Control;

class NumericSettingBase;
struct NumericSettingDetails;

template <typename T>
class NumericSetting;

template <typename T>
class SettingValue;

// Lets scripting, TAS input and similar replace a control's value: called with the group name,
// the control (or axis) name and the computed state; a returned value replaces the state.
using InputOverrideFunction = std::function<std::optional<ControlState>(
    const std::string_view group_name, const std::string_view control_name, ControlState state)>;

// The kind of a group, used by the mapping UI to pick its widget.
enum class GroupType
{
  Other,
  Stick,
  MixedTriggers,
  Buttons,
  Force,
  Attachments,
  Tilt,
  Cursor,
  Triggers,
  Slider,
  Shake,
  IMUAccelerometer,
  IMUGyroscope,
  IMUCursor,
  IRPassthrough,
};

// A named set of controls and numeric settings, saved under "<base><name>/" in an ini section.
class ControlGroup
{
public:
  // Whether the group has an "Enabled" setting and what it defaults to.
  enum class DefaultValue
  {
    AlwaysEnabled,
    Enabled,
    Disabled,
  };

  explicit ControlGroup(const std::string& name, GroupType type = GroupType::Other,
                        DefaultValue default_value = DefaultValue::AlwaysEnabled);
  ControlGroup(std::string name, std::string ui_name, GroupType type = GroupType::Other,
               DefaultValue default_value = DefaultValue::AlwaysEnabled);
  virtual ~ControlGroup();

  virtual void LoadConfig(Common::IniFile::Section* sec, const std::string& base);
  virtual void SaveConfig(Common::IniFile::Section* sec, const std::string& base);

  virtual void UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env);

  void SetControlExpression(int index, const std::string& expression);

  void AddInput(Translatability translate, std::string name);
  void AddInput(Translatability translate, std::string name, std::string ui_name);
  void AddOutput(Translatability translate, std::string name);

  template <typename T>
  void AddSetting(SettingValue<T>* value, const NumericSettingDetails& details,
                  std::common_type_t<T> default_value_, std::common_type_t<T> min_value = {},
                  std::common_type_t<T> max_value = T(100))
  {
    auto setting =
        std::make_unique<NumericSetting<T>>(value, details, default_value_, min_value, max_value);
    numeric_settings.push_back(std::move(setting));
  }

  void AddVirtualNotchSetting(SettingValue<double>* value, double max_virtual_notch_deg);

  void AddDeadzoneSetting(SettingValue<double>* value, double maximum_deadzone);

  // Removes the first `deadzone` of the input's magnitude and stretches the rest back to 0..1,
  // keeping the sign.
  template <typename T>
  static T ApplyDeadzone(T input, std::common_type_t<T> deadzone)
  {
    const T magnitude = std::max(T{0}, std::abs(input) - deadzone);
    return std::copysign(magnitude / (T{1} - deadzone), input);
  }

  bool HasEnabledSetting() const;

  const std::string name;
  const std::string ui_name;
  const GroupType type;

  // What "Enabled" defaults to (AlwaysEnabled: there is no such setting).
  const DefaultValue default_value;
  SettingValue<bool> enabled;
  std::unique_ptr<NumericSetting<bool>> enabled_setting;

  std::vector<std::unique_ptr<Control>> controls;
  std::vector<std::unique_ptr<NumericSettingBase>> numeric_settings;
};
}  // namespace ControllerEmu
