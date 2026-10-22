// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

#include "Core/Core.h"

class QCheckBox;
class QComboBox;
class QGroupBox;

// Settings for the emulated Wii Speak: plugged in or not, mute, volume offset and host device.
class WiiSpeakWindow : public QWidget
{
  Q_OBJECT
public:
  explicit WiiSpeakWindow(QWidget* parent = nullptr);
  ~WiiSpeakWindow() override;

private:
  void BuildLayout();
  QGroupBox* BuildMicrophoneBox();
  void OnEmulationStateChanged(Core::State state);
  void EmulateWiiSpeak(bool emulate);
  void OnInputDeviceChange();

  QCheckBox* m_plugged_in;
  QComboBox* m_input_device;
};
