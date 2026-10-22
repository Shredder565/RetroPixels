// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QSignalBlocker>

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"

#include "Common/Config/ConfigInfo.h"

// A combo box whose item index is the value of an int setting.
class ConfigChoice final : public ConfigControl<ToolTipComboBox>
{
  Q_OBJECT
public:
  ConfigChoice(const QStringList& options, const Config::Info<int>& setting,
               Config::Layer* layer = nullptr);

protected:
  void OnConfigChanged() override;

private:
  void Store(int index);

  Config::Info<int> m_setting;
};

// The same for a u32 setting.
class ConfigChoiceU32 final : public ConfigControl<ToolTipComboBox>
{
  Q_OBJECT
public:
  ConfigChoiceU32(const QStringList& options, const Config::Info<u32>& setting,
                  Config::Layer* layer = nullptr);

protected:
  void OnConfigChanged() override;

private:
  void Store(int index);

  Config::Info<u32> m_setting;
};

// A combo box listing (label, value) pairs for a setting of any type. A
// stored value that is not in the list leaves the box empty.
template <typename T>
class ConfigChoiceMap final : public ConfigControl<ToolTipComboBox>
{
public:
  ConfigChoiceMap(const std::vector<std::pair<QString, T>>& options, const Config::Info<T>& setting,
                  Config::Layer* layer = nullptr)
      : ConfigControl<ToolTipComboBox>(setting.GetLocation(), layer), m_setting(setting),
        m_entries(options)
  {
    for (const auto& entry : m_entries)
      addItem(entry.first);
    OnConfigChanged();
    connect(this, &QComboBox::currentIndexChanged, this, &ConfigChoiceMap::Store);
  }

protected:
  void OnConfigChanged() override
  {
    const T current = ReadValue(m_setting);
    int row = -1;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
      if (m_entries[i].second == current)
      {
        row = static_cast<int>(i);
        break;
      }
    }
    const QSignalBlocker quiet(this);
    setCurrentIndex(row);
  }

private:
  void Store(int index) { SaveValue(m_setting, m_entries[static_cast<size_t>(index)].second); }

  const Config::Info<T> m_setting;
  const std::vector<std::pair<QString, T>> m_entries;
};

// A combo box for a string setting. The stored string is either the item
// text itself, or (for the pair constructor) the item data next to a
// separate display label.
class ConfigStringChoice final : public ConfigControl<ToolTipComboBox>
{
  Q_OBJECT
public:
  ConfigStringChoice(std::span<const std::string> options, const Config::Info<std::string>& setting,
                     Config::Layer* layer = nullptr);
  ConfigStringChoice(std::span<const std::pair<QString, QString>> options,
                     const Config::Info<std::string>& setting, Config::Layer* layer = nullptr);
  void Load();

protected:
  void OnConfigChanged() override;

private:
  void Store(int index);

  const Config::Info<std::string> m_setting;
  const bool m_value_is_label;
};

// A combo box whose options each set two settings at once (for example a
// mode and a sub-mode). Options may name DefaultState to mean "the
// setting's default". When no option matches, the default index is shown.
class ConfigComplexChoice final : public ToolTipComboBox
{
  Q_OBJECT

  using InfoVariant = std::variant<Config::Info<u32>, Config::Info<int>, Config::Info<bool>>;
  using OptionVariant = std::variant<Config::DefaultState, u32, int, bool>;

public:
  ConfigComplexChoice(const InfoVariant& setting1, const InfoVariant& setting2,
                      Config::Layer* layer = nullptr);

  void Add(const QString& name, const OptionVariant option1, const OptionVariant option2);
  void Refresh();
  void Reset();
  void SetDefault(int index);
  const std::pair<Config::Location, Config::Location> GetLocation() const;

private:
  void SaveValue(int choice);
  void UpdateComboIndex();
  bool Matches(const InfoVariant& setting, const OptionVariant& option) const;
  void mousePressEvent(QMouseEvent* event) override;

  Config::Layer* m_layer;
  const InfoVariant m_first;
  const InfoVariant m_second;
  std::vector<std::pair<OptionVariant, OptionVariant>> m_choices;
  int m_fallback_index = -1;
};
