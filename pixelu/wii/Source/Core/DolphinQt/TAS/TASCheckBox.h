// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QCheckBox>

#include "DolphinQt/TAS/TASControlState.h"

class QMouseEvent;
class TASInputWindow;

// A TAS button. Left click toggles it; right click starts (or stops) turbo, shown as the
// partially-checked state, which presses and releases the button on a frame cycle taken from the
// owning window's turbo settings.
class TASCheckBox : public QCheckBox
{
  Q_OBJECT
public:
  explicit TASCheckBox(const QString& text, TASInputWindow* parent);

  // Any thread.
  bool GetValue() const;
  // CPU thread only.
  void OnControllerValueChanged(bool new_value);

protected:
  void mousePressEvent(QMouseEvent* event) override;

private slots:
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  void OnUIValueChanged(Qt::CheckState new_value);
#else
  void OnUIValueChanged(int new_value);
#endif
  void ApplyControllerValueChange();

private:
  const TASInputWindow* m_window;
  TASControlState m_state;
  // Turbo cycle: the frame it started on, frames held down, and the whole cycle length.
  int m_turbo_origin_frame = 0;
  int m_turbo_down_frames = 0;
  int m_turbo_cycle_frames = 0;
};
