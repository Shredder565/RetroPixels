// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>

namespace Common
{
// A boolean that several threads may read and change at once.
//
// TestAndSet(v) stores v and returns true only if the flag held the other value before, which
// lets exactly one thread "win" a transition.
class Flag final
{
public:
  // Explicit, so that a Flag is never mistaken for a plain bool in assignments.
  explicit Flag(bool initial_value = false) : m_state(initial_value) {}

  void Set(bool value = true) { m_state.store(value); }
  void Clear() { m_state.store(false); }
  bool IsSet() const { return m_state.load(); }

  bool TestAndSet(bool value = true)
  {
    bool previous = !value;
    return m_state.compare_exchange_strong(previous, value);
  }
  bool TestAndClear() { return TestAndSet(false); }

private:
  std::atomic<bool> m_state;
};
}  // namespace Common
