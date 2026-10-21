// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class ConfigBool;
class ConfigChoice;
class QCheckBox;
class QPushButton;
class ToolTipCheckBox;

// Free Look options: on/off, camera 1's control scheme and its mapping
// window, and background input.
class FreeLookWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit FreeLookWidget(QWidget* parent);

private:
  void OpenControllerMapping();
  void RefreshEnabledState();

  ConfigBool* m_enabled;
  ConfigChoice* m_control_type;
  QPushButton* m_configure_button;
  ConfigBool* m_background_input;
};
