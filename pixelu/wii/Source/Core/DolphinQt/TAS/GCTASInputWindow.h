// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/TAS/TASInputWindow.h"

class QGroupBox;
class QHideEvent;
class QShowEvent;
class QSpinBox;
class TASCheckBox;

// TAS input for one GameCube controller port: both sticks, the analog triggers and every button.
class GCTASInputWindow : public TASInputWindow
{
  Q_OBJECT
public:
  explicit GCTASInputWindow(QWidget* parent, int controller_id);

  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  QGroupBox* BuildTriggersBox();
  QGroupBox* BuildButtonsBox();

  int m_port;
  InputOverrider m_overrider;

  QGroupBox* m_main_stick_box;
  QGroupBox* m_c_stick_box;
  QGroupBox* m_triggers_box;
  QGroupBox* m_buttons_box;
};
