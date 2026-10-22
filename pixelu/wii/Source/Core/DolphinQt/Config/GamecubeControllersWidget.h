// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include <QWidget>

class QComboBox;
class QGridLayout;
class QGroupBox;
class QPushButton;

namespace Core
{
enum class State;
}

// The "GameCube Controllers" box of the controller settings: a device
// choice and a Configure button for each of the four SI ports.
class GamecubeControllersWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit GamecubeControllersWidget(QWidget* parent);

private:
  static constexpr size_t PORTS = 4;

  void ShowConfig(Core::State state);
  void StoreConfig();
  void RefreshConfigureButton(size_t port);
  void ConfigurePort(size_t port);

  QGroupBox* m_group;
  QGridLayout* m_grid;
  std::array<QComboBox*, PORTS> m_devices;
  std::array<QPushButton*, PORTS> m_configure;
};
