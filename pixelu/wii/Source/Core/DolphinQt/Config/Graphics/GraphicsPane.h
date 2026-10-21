// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class MainWindow;

namespace Config
{
class Layer;
}  // namespace Config

// The graphics settings: General, Enhancements, Hacks and Advanced tabs.
// With a config layer (a game's INI) the tabs edit that layer; main_window
// is null in that case.
class GraphicsPane final : public QWidget
{
  Q_OBJECT
public:
  explicit GraphicsPane(MainWindow* main_window, Config::Layer* config_layer);

  Config::Layer* GetConfigLayer();

signals:
  void BackendChanged(const QString& backend);
  void UseFastTextureSamplingChanged();
  void UpdateGPUTextureDecoding();

private:
  void OnBackendChanged(const QString& backend);

  MainWindow* const m_main_window;
  Config::Layer* const m_config_layer;
};
