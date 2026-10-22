// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/TASControlState.h"

#include <atomic>

namespace
{
constexpr auto RELAXED = std::memory_order_relaxed;
}

int TASControlState::GetValue() const
{
  const Snapshot shown = m_widget_side.load(RELAXED);
  const Snapshot polled = m_controller_side.load(RELAXED);

  // While the widget lags behind the controller, the controller's value wins.
  if (shown.generation == polled.generation)
    return shown.value;
  return polled.value;
}

bool TASControlState::OnControllerValueChanged(int new_value)
{
  const Snapshot polled = m_controller_side.load(RELAXED);
  if (new_value == polled.value)
    return false;

  Snapshot next;
  next.generation = polled.generation + 1;
  next.value = new_value;
  m_controller_side.store(next, RELAXED);
  return true;
}

void TASControlState::OnUIValueChanged(int new_value)
{
  Snapshot shown = m_widget_side.load(RELAXED);
  shown.value = new_value;
  m_widget_side.store(shown, RELAXED);
}

int TASControlState::ApplyControllerValueChange()
{
  const Snapshot polled = m_controller_side.load(RELAXED);
  const Snapshot shown = m_widget_side.load(RELAXED);

  if (shown.generation == polled.generation)
    return shown.value;

  m_widget_side.store(polled, RELAXED);
  return polled.value;
}
