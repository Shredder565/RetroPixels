// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QVBoxLayout>
#include <QWidget>

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"

class ConfigBool;
class QComboBox;
class QGroupBox;

// Settings for the emulated Logitech USB microphones: which ones are plugged in, and for each one
// mute, a volume offset and the host input device.
class LogitechMicWindow final : public QWidget
{
  Q_OBJECT
public:
  explicit LogitechMicWindow(QWidget* parent = nullptr);

private:
  void BuildLayout();
  QGroupBox* BuildPresenceBox();
  QGroupBox* BuildMicrophonesBox();
  QGroupBox* BuildMicrophoneBox(std::size_t mic);

  void OnEmulationStateChanged(Core::State state);
  void OnInputDeviceChange(std::size_t index);

  std::array<ConfigBool*, Config::EMULATED_LOGITECH_MIC_COUNT> m_plugged_in{};
  std::array<QComboBox*, Config::EMULATED_LOGITECH_MIC_COUNT> m_input_device{};
};
