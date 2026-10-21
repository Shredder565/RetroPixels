// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/TAS/TASInputWindow.h"

class QGroupBox;
class QHideEvent;
class QShowEvent;
class QSpinBox;
class TASCheckBox;

// TAS input for one emulated Game Boy Advance: the D-pad and the six buttons.
class GBATASInputWindow final : public TASInputWindow
{
  Q_OBJECT
public:
  explicit GBATASInputWindow(QWidget* parent, int controller_id);

  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  int m_port;
  InputOverrider m_overrider;
};
