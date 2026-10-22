// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"

#include <QSignalBlocker>

ConfigChoice::ConfigChoice(const QStringList& options, const Config::Info<int>& setting,
                           Config::Layer* layer)
    : ConfigControl(setting.GetLocation(), layer), m_setting(setting)
{
  addItems(options);
  OnConfigChanged();
  connect(this, &QComboBox::currentIndexChanged, this, &ConfigChoice::Store);
}

void ConfigChoice::Store(int index)
{
  SaveValue(m_setting, index);
}

void ConfigChoice::OnConfigChanged()
{
  setCurrentIndex(ReadValue(m_setting));
}

ConfigChoiceU32::ConfigChoiceU32(const QStringList& options, const Config::Info<u32>& setting,
                                 Config::Layer* layer)
    : ConfigControl(setting.GetLocation(), layer), m_setting(setting)
{
  addItems(options);
  OnConfigChanged();
  connect(this, &QComboBox::currentIndexChanged, this, &ConfigChoiceU32::Store);
}

void ConfigChoiceU32::Store(int index)
{
  SaveValue(m_setting, static_cast<u32>(index));
}

void ConfigChoiceU32::OnConfigChanged()
{
  setCurrentIndex(static_cast<int>(ReadValue(m_setting)));
}

ConfigStringChoice::ConfigStringChoice(std::span<const std::string> options,
                                       const Config::Info<std::string>& setting,
                                       Config::Layer* layer)
    : ConfigControl(setting.GetLocation(), layer), m_setting(setting), m_value_is_label(true)
{
  for (const std::string& option : options)
    addItem(QString::fromStdString(option));
  Load();
  connect(this, &QComboBox::currentIndexChanged, this, &ConfigStringChoice::Store);
}

ConfigStringChoice::ConfigStringChoice(std::span<const std::pair<QString, QString>> options,
                                       const Config::Info<std::string>& setting,
                                       Config::Layer* layer)
    : ConfigControl(setting.GetLocation(), layer), m_setting(setting), m_value_is_label(false)
{
  for (const auto& option : options)
    addItem(option.first, option.second);
  Load();
  connect(this, &QComboBox::currentIndexChanged, this, &ConfigStringChoice::Store);
}

void ConfigStringChoice::Store(int index)
{
  const QString value = m_value_is_label ? itemText(index) : itemData(index).toString();
  SaveValue(m_setting, value.toStdString());
}

// Public so owners can reselect after changing the item list.
void ConfigStringChoice::Load()
{
  const QString stored = QString::fromStdString(ReadValue(m_setting));
  setCurrentIndex(m_value_is_label ? findText(stored) : findData(stored));
}

void ConfigStringChoice::OnConfigChanged()
{
  Load();
}

ConfigComplexChoice::ConfigComplexChoice(const InfoVariant& setting1, const InfoVariant& setting2,
                                         Config::Layer* layer)
    : m_layer(layer), m_first(setting1), m_second(setting2)
{
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &ConfigComplexChoice::Refresh);
  connect(this, &QComboBox::currentIndexChanged, this, &ConfigComplexChoice::SaveValue);
}

const std::pair<Config::Location, Config::Location> ConfigComplexChoice::GetLocation() const
{
  const auto location_of = [](const auto& info) { return info.GetLocation(); };
  return {std::visit(location_of, m_first), std::visit(location_of, m_second)};
}

void ConfigComplexChoice::Refresh()
{
  const auto [first, second] = GetLocation();
  bool own_value;
  if (m_layer == nullptr)
  {
    own_value = Config::GetActiveLayerForConfig(first) != Config::LayerType::Base ||
                Config::GetActiveLayerForConfig(second) != Config::LayerType::Base;
  }
  else
  {
    own_value = m_layer->Exists(first) || m_layer->Exists(second);
  }

  QFont face = font();
  face.setBold(own_value);
  setFont(face);
  UpdateComboIndex();
}

void ConfigComplexChoice::Add(const QString& name, const OptionVariant option1,
                              const OptionVariant option2)
{
  const QSignalBlocker quiet(this);
  addItem(name);
  m_choices.emplace_back(option1, option2);
}

void ConfigComplexChoice::Reset()
{
  clear();
  m_choices.clear();
}

void ConfigComplexChoice::SetDefault(int index)
{
  m_fallback_index = index;
}

void ConfigComplexChoice::SaveValue(int choice)
{
  // Writes one option into one setting. A DefaultState option removes the
  // key; other values may differ in type from the setting and are stored as
  // they are.
  const auto write = [this](const auto& info, const auto& value) {
    if (m_layer == nullptr)
    {
      Config::SetBaseOrCurrent(info, value);
      return;
    }
    m_layer->Set(info.GetLocation(), value);
    Config::OnConfigChanged();
  };

  const auto& picked = m_choices[static_cast<size_t>(choice)];
  std::visit(write, m_first, picked.first);
  std::visit(write, m_second, picked.second);
}

// Whether a setting currently holds an option's value (or, for a
// DefaultState option, its default). Values only match with the same
// alternative type, so a u32 option never matches an int setting.
bool ConfigComplexChoice::Matches(const InfoVariant& setting, const OptionVariant& option) const
{
  const OptionVariant current = std::visit(
      [this](const auto& info) -> OptionVariant {
        if (m_layer == nullptr)
          return Config::Get(info);
        return m_layer->Get(info);
      },
      setting);

  if (!std::holds_alternative<Config::DefaultState>(option))
    return current == option;

  const OptionVariant fallback =
      std::visit([](const auto& info) -> OptionVariant { return info.GetDefaultValue(); }, setting);
  return current == fallback;
}

void ConfigComplexChoice::UpdateComboIndex()
{
  int index = m_fallback_index;
  for (size_t i = 0; i < m_choices.size(); ++i)
  {
    if (Matches(m_first, m_choices[i].first) && Matches(m_second, m_choices[i].second))
    {
      index = static_cast<int>(i);
      break;
    }
  }

  // Selecting must not write back through SaveValue.
  const QSignalBlocker quiet(this);
  setCurrentIndex(index);
}

void ConfigComplexChoice::mousePressEvent(QMouseEvent* event)
{
  if (m_layer == nullptr || event->button() != Qt::RightButton)
  {
    QComboBox::mousePressEvent(event);
    return;
  }

  const auto [first, second] = GetLocation();
  m_layer->DeleteKey(first);
  m_layer->DeleteKey(second);
  Config::OnConfigChanged();
}
