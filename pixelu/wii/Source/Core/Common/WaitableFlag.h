// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "Common/Flag.h"

namespace Common
{
// A thread-safe bool that other threads can wait on until it has a particular value.
class WaitableFlag final
{
public:
  void Set(bool value = true)
  {
    if (!m_value.TestAndSet(value))
      return;  // no change

    // See Event::Set for why the mutex is taken here.
    {
      std::lock_guard guard(m_mutex);
    }
    m_changed.notify_all();
  }

  void Reset() { Set(false); }

  void Wait(bool expected_value)
  {
    if (m_value.IsSet() == expected_value)
      return;
    std::unique_lock lock(m_mutex);
    m_changed.wait(lock, [&] { return m_value.IsSet() == expected_value; });
  }

  // Returns false if the time ran out first.
  template <class Rep, class Period>
  bool WaitFor(bool expected_value, const std::chrono::duration<Rep, Period>& rel_time)
  {
    if (m_value.IsSet() == expected_value)
      return true;
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, rel_time,
                              [&] { return m_value.IsSet() == expected_value; });
  }

private:
  Flag m_value;
  std::condition_variable m_changed;
  std::mutex m_mutex;
};
}  // namespace Common
