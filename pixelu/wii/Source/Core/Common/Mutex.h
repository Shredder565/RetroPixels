// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>

namespace Common
{
namespace detail
{
// A one-flag lock meeting the Lockable requirements. With `Sleep`, a waiting thread blocks on
// the flag (std::atomic::wait) instead of spinning. Unlike std::mutex, unlock() may be called
// from a thread other than the one that locked.
template <bool Sleep>
class FlagLock
{
public:
  void lock()
  {
    for (;;)
    {
      if (!m_held.exchange(true, std::memory_order_acquire))
        return;
      if constexpr (Sleep)
        m_held.wait(true, std::memory_order_relaxed);
    }
  }

  bool try_lock()
  {
    bool was_held = false;
    return m_held.compare_exchange_weak(was_held, true, std::memory_order_acquire,
                                        std::memory_order_relaxed);
  }

  void unlock()
  {
    m_held.store(false, std::memory_order_release);
    if constexpr (Sleep)
      m_held.notify_one();
  }

private:
  std::atomic<bool> m_held{false};
};
}  // namespace detail

// Cheap lock that sleeps while contended.
using AtomicMutex = detail::FlagLock<true>;

// Busy-waiting lock for very short, rarely contended critical sections.
using SpinMutex = detail::FlagLock<false>;
}  // namespace Common
