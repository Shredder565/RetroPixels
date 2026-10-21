// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QFont>
#include <QMouseEvent>

#include "Common/Config/Enums.h"
#include "Common/Config/Layer.h"
#include "DolphinQt/Settings.h"

namespace Config
{
template <typename T>
class Info;
struct Location;
}  // namespace Config

// Base for widgets bound to one config setting. Without a layer the widget
// edits the effective configuration; with one (a game's INI being edited)
// it edits that layer only, falls back to the global value for display,
// shows layer-local values in bold and drops the key on a right click.
template <class Derived>
class ConfigControl : public Derived
{
public:
  ConfigControl(const Config::Location& location, Config::Layer* layer)
      : m_location(location), m_layer(layer)
  {
    WatchConfig();
  }
  ConfigControl(const QString& label, const Config::Location& location, Config::Layer* layer)
      : Derived(label), m_location(location), m_layer(layer)
  {
    WatchConfig();
  }
  ConfigControl(const Qt::Orientation& orient, const Config::Location& location,
                Config::Layer* layer)
      : Derived(orient), m_location(location), m_layer(layer)
  {
    WatchConfig();
  }

  const Config::Location GetLocation() const { return m_location; }

protected:
  template <typename T>
  void SaveValue(const Config::Info<T>& setting, const T& value)
  {
    // Refreshing from the config must not write the same value back.
    if (m_refreshing)
      return;

    if (m_layer == nullptr)
    {
      Config::SetBaseOrCurrent(setting, value);
      return;
    }
    m_layer->Set(m_location, value);
    Config::OnConfigChanged();
  }

  template <typename T>
  const T ReadValue(const Config::Info<T>& setting) const
  {
    if (m_layer == nullptr)
      return Config::Get(setting);
    // The game being edited is unknown here, so a missing game value shows
    // the global (base) one.
    return m_layer->Exists(m_location) ? m_layer->Get(setting) : Config::GetBase(setting);
  }

  virtual void OnConfigChanged() {}

private:
  void WatchConfig()
  {
    Derived::connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this] {
      QFont face = Derived::font();
      face.setBold(HasOwnValue());
      Derived::setFont(face);

      // Not signal blocked: dependants may need the change notifications.
      m_refreshing = true;
      OnConfigChanged();
      m_refreshing = false;
    });
  }

  bool HasOwnValue() const
  {
    if (m_layer == nullptr)
      return Config::GetActiveLayerForConfig(m_location) != Config::LayerType::Base;
    return m_layer->Exists(m_location);
  }

  void mousePressEvent(QMouseEvent* event) override
  {
    const bool reset_request = m_layer != nullptr && event->button() == Qt::RightButton;
    if (!reset_request)
    {
      Derived::mousePressEvent(event);
      return;
    }
    m_layer->DeleteKey(m_location);
    Config::OnConfigChanged();
  }

  bool m_refreshing = false;
  const Config::Location m_location;
  Config::Layer* m_layer;
};
