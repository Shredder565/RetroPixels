// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QSpinBox>

#include "DolphinQt/TAS/TASControlState.h"

class TASInputWindow;

// The numeric half of a TAS slider/value pair. The value can be read from the CPU thread, and the
// real controller can push values into it from there.
class TASSpinBox : public QSpinBox
{
  Q_OBJECT
public:
  explicit TASSpinBox(QWidget* parent = nullptr);

  // Any thread.
  int GetValue() const;
  // CPU thread only.
  void OnControllerValueChanged(int new_value);
  QValidator::State validate(QString& input, int& pos) const override;
  void fixup(QString& input) const override;

private slots:
  void OnUIValueChanged(int new_value);
  void ApplyControllerValueChange();

private:
  TASControlState m_state;
};
