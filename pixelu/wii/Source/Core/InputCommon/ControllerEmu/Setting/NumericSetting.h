// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
enum class SettingType
{
  Int,
  Double,
  Bool,
};

enum class SettingVisibility
{
  Normal,
  // Shown only in the advanced view of the mapping UI.
  Advanced,
};

// Names and texts of a numeric setting. All strings are static, untranslated literals.
struct NumericSettingDetails
{
  NumericSettingDetails(const char* const _ini_name, const char* const _ui_suffix = nullptr,
                        const char* const _ui_description = nullptr,
                        const char* const _ui_name = nullptr,
                        SettingVisibility _visibility = SettingVisibility::Normal)
      : ini_name(_ini_name), ui_suffix(_ui_suffix), ui_description(_ui_description),
        ui_name(_ui_name != nullptr ? _ui_name : _ini_name), visibility(_visibility)
  {
  }

  // Key in the ini file.
  const char* const ini_name;

  // Unit shown after the number in the UI.
  const char* const ui_suffix;

  // Tooltip text.
  const char* const ui_description;

  // Label in the UI (the ini name unless given).
  const char* const ui_name;

  const SettingVisibility visibility;
};

class NumericSettingBase
{
public:
  NumericSettingBase(const NumericSettingDetails& details);

  virtual ~NumericSettingBase() = default;

  virtual void LoadFromIni(const Common::IniFile::Section& section,
                           const std::string& group_name) = 0;
  virtual void SaveToIni(Common::IniFile::Section& section,
                         const std::string& group_name) const = 0;

  virtual InputReference& GetInputReference() = 0;
  virtual const InputReference& GetInputReference() const = 0;

  // True when the setting holds a plain number rather than an expression.
  virtual bool IsSimpleValue() const = 0;

  // Turns an expression that is just a number (e.g. "7.0") back into a plain value.
  virtual void SimplifyIfPossible() = 0;

  // Turns the plain value into an equivalent expression (before the user edits it as one).
  virtual void SetExpressionFromValue() = 0;

  virtual SettingType GetType() const = 0;

  virtual void SetToDefault() = 0;

  // The details given at construction.
  const char* GetININame() const { return m_details.ini_name; }
  const char* GetUIName() const { return m_details.ui_name; }
  const char* GetUISuffix() const { return m_details.ui_suffix; }
  const char* GetUIDescription() const { return m_details.ui_description; }
  SettingVisibility GetVisibility() const { return m_details.visibility; }

protected:
  NumericSettingDetails m_details;
};

template <typename T>
class SettingValue;

// A setting of type int, double or bool, stored in a SettingValue owned by the group.
template <typename T>
class NumericSetting final : public NumericSettingBase
{
public:
  using ValueType = T;

  static_assert(std::is_same<ValueType, int>() || std::is_same<ValueType, double>() ||
                    std::is_same<ValueType, bool>(),
                "NumericSetting supports int, double and bool.");

  NumericSetting(SettingValue<ValueType>* value, const NumericSettingDetails& details,
                 ValueType default_value, ValueType min_value, ValueType max_value)
      : NumericSettingBase(details), m_value(*value), m_default_value(default_value),
        m_min_value(min_value), m_max_value(max_value)
  {
    SetToDefault();
  }

  void SetToDefault() override { m_value.SetValue(m_default_value); }

  // A present key is read as an expression (and simplified to a number when it is one); an
  // absent key restores the default.
  void LoadFromIni(const Common::IniFile::Section& section, const std::string& group_name) override
  {
    std::string text;
    if (!section.Get(group_name + m_details.ini_name, &text))
    {
      SetValue(m_default_value);
      return;
    }
    m_value.m_reference.SetExpression(std::move(text));
    SimplifyIfPossible();
  }

  void SaveToIni(Common::IniFile::Section& section, const std::string& group_name) const override
  {
    const std::string key = group_name + m_details.ini_name;
    if (!IsSimpleValue())
    {
      // Ini values are single lines.
      std::string expression = m_value.m_reference.GetExpression();
      ReplaceBreaksWithSpaces(expression);
      section.Set(key, expression, "");
      return;
    }
    section.Set(key, GetValue(), m_default_value);
  }

  bool IsSimpleValue() const override { return m_value.IsSimpleValue(); }
  void SimplifyIfPossible() override;
  void SetExpressionFromValue() override;
  InputReference& GetInputReference() override { return m_value.m_reference; }
  const InputReference& GetInputReference() const override { return m_value.m_reference; }

  ValueType GetValue() const { return m_value.GetValue(); }
  void SetValue(ValueType value) { m_value.SetValue(value); }

  ValueType GetDefaultValue() const { return m_default_value; }
  ValueType GetMinValue() const { return m_min_value; }
  ValueType GetMaxValue() const { return m_max_value; }

  SettingType GetType() const override
  {
    if constexpr (std::is_same<ValueType, int>())
      return SettingType::Int;
    else if constexpr (std::is_same<ValueType, double>())
      return SettingType::Double;
    else
      return SettingType::Bool;
  }

private:
  SettingValue<ValueType>& m_value;

  const ValueType m_default_value;
  const ValueType m_min_value;
  const ValueType m_max_value;
};

// The value behind a setting: either a plain number or the result of an input expression.
// Read by the CPU thread and written by the UI thread.
template <typename T>
class SettingValue
{
  using ValueType = T;

  friend class NumericSetting<T>;

public:
  virtual ~SettingValue() = default;

  virtual ValueType GetValue() const
  {
    // An expression is re-evaluated only while the input gate is open, so losing window focus
    // does not reset settings such as the battery level or the attached extension to 0.
    if (!IsSimpleValue() && ControlReference::GetInputGate())
      m_cached = m_reference.GetState<ValueType>();
    return m_cached;
  }

  ValueType GetCachedValue() const { return m_cached; }

  bool IsSimpleValue() const { return m_reference.GetExpression().empty(); }

  // Stores a plain value (and drops any expression).
  virtual void SetValue(const ValueType value)
  {
    m_cached = value;
    m_reference.SetExpression("");
  }

private:
  mutable std::atomic<ValueType> m_cached = {};

  // Reading a reference's state is not const.
  mutable InputReference m_reference;
};

// A SettingValue that calls back when its value changes (used to follow the attached extension
// and Motion Plus).
template <typename ValueType>
class SubscribableSettingValue final : public SettingValue<ValueType>
{
public:
  using Base = SettingValue<ValueType>;
  using SettingChangedCallback = std::function<void(ValueType)>;

  ValueType GetValue() const override
  {
    const ValueType before = GetCachedValue();
    if (IsSimpleValue())
      return before;

    const ValueType now = Base::GetValue();
    if (now != before)
      NotifyAll();
    return now;
  }

  void SetValue(const ValueType value) override
  {
    if (value == GetCachedValue())
    {
      // Same value: no notification, but an expression still gives way to the plain value.
      if (!IsSimpleValue())
        Base::SetValue(value);
      return;
    }
    Base::SetValue(value);
    NotifyAll();
  }

  ValueType GetCachedValue() const { return Base::GetCachedValue(); }
  bool IsSimpleValue() const { return Base::IsSimpleValue(); }

  int AddCallback(const SettingChangedCallback& callback)
  {
    std::lock_guard guard(m_callbacks_mutex);
    const int id = m_next_id++;
    m_callbacks.emplace_back(id, callback);
    return id;
  }

  void RemoveCallback(const int id)
  {
    std::lock_guard guard(m_callbacks_mutex);
    const auto match = std::ranges::find_if(
        m_callbacks, [id](const Subscription& subscription) { return subscription.first == id; });
    if (match != m_callbacks.end())
      m_callbacks.erase(match);
  }

private:
  using Subscription = std::pair<int, SettingChangedCallback>;

  void NotifyAll() const
  {
    std::lock_guard guard(m_callbacks_mutex);
    // The value is read again (re-evaluating an expression) for the callbacks.
    const ValueType value = Base::GetValue();
    for (const Subscription& subscription : m_callbacks)
      subscription.second(value);
  }

  std::vector<Subscription> m_callbacks;
  int m_next_id = 0;

  mutable std::mutex m_callbacks_mutex;
};

}  // namespace ControllerEmu
