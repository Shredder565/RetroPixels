// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "Common/Flag.h"

namespace Common
{
// An auto-reset event: Set() signals it and wakes one waiter; a successful Wait() consumes the
// signal. A Set() with nobody waiting is remembered for the next Wait().
class Event final
{
public:
  void Set()
  {
    if (!m_signaled.TestAndSet())
      return;  // already signalled

    // Taking the mutex once after changing the flag guarantees that a waiter is either before
    // its predicate check (and will see the flag) or already blocked (and will get the notify).
    {
      std::lock_guard guard(m_mutex);
    }
    m_wakeup.notify_one();
  }

  void Wait()
  {
    if (m_signaled.TestAndClear())
      return;
    std::unique_lock lock(m_mutex);
    m_wakeup.wait(lock, [this] { return m_signaled.TestAndClear(); });
  }

  // Returns false if the time ran out without a signal.
  template <class Rep, class Period>
  bool WaitFor(const std::chrono::duration<Rep, Period>& rel_time)
  {
    if (m_signaled.TestAndClear())
      return true;
    std::unique_lock lock(m_mutex);
    return m_wakeup.wait_for(lock, rel_time, [this] { return m_signaled.TestAndClear(); });
  }

  // Drops a pending signal. Rarely what you want: it races with waiters.
  void Reset() { m_signaled.Clear(); }

private:
  Flag m_signaled;
  std::condition_variable m_wakeup;
  std::mutex m_mutex;
};
}  // namespace Common
