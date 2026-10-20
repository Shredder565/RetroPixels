// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <memory>
#include <semaphore>

#include "Common/Functional.h"

namespace Common
{
// Signals one waiting thread exactly once. Unlike Event, the waiter may destroy the object as
// soon as it wakes up, because Set() does not touch the object after releasing the waiter.
class OneShotEvent
{
public:
  // Call once, from one thread.
  void Set() { m_released.release(); }

  // Call Wait() once, or WaitFor() until it returns true, from one thread.
  void Wait() { m_released.acquire(); }

  template <typename Rep, typename Period>
  bool WaitFor(const std::chrono::duration<Rep, Period>& rel_time)
  {
    return m_released.try_acquire_for(rel_time);
  }

private:
  std::binary_semaphore m_released{0};
};

// Calls Set() on the referenced object when destroyed, unless dismissed. Move-only.
template <typename EventType>
class ScopedSetter
{
public:
  ScopedSetter() = default;
  explicit ScopedSetter(EventType* ptr) : m_target{ptr} {}

  // Forget the object without calling Set().
  void Dismiss() { static_cast<void>(m_target.release()); }

private:
  // A unique_ptr whose "deleter" calls Set() gives move-only semantics for free.
  std::unique_ptr<EventType, InvokerOf<&EventType::Set>> m_target;
};
}  // namespace Common
