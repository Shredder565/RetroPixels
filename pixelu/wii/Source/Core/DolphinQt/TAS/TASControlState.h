// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>

#include "Common/CommonTypes.h"

// One value of a TAS input control, shared between the UI thread (the widget the user edits) and
// the CPU thread (which polls it and may push the real controller's value into it).
//
// Each side keeps its own snapshot. Only the CPU side ever bumps the generation counter; the UI
// side picks up a newer generation by copying the CPU snapshot over its own. A difference in
// generations therefore means "the controller changed and the widget has not caught up yet".
class TASControlState
{
public:
  // The value in effect right now. Safe from either thread.
  int GetValue() const;
  // CPU thread: the controller now reports new_value. Returns true when the widget needs to be
  // refreshed with ApplyControllerValueChange (queued to the UI thread).
  bool OnControllerValueChanged(int new_value);
  // UI thread: the user set new_value.
  void OnUIValueChanged(int new_value);
  // UI thread: adopt the controller's value if it is newer; returns the value to show.
  int ApplyControllerValueChange();

private:
  struct Snapshot
  {
    int generation = 0;
    int value = 0;
  };

  std::atomic<Snapshot> m_widget_side = Snapshot{};
  std::atomic<Snapshot> m_controller_side = Snapshot{};
};
