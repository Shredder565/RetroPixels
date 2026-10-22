// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace Common
{
// A reader/writer lock with the std::shared_mutex interface whose locks may be released by a
// different thread than the one that took them.
//
// The whole state is one counter: 0 = free, 1..WRITER-1 = that many readers, WRITER = held
// exclusively. Waiters sleep on the counter with std::atomic::wait.
class TransferableSharedMutex
{
public:
  void lock()
  {
    for (;;)
    {
      State seen = FREE;
      if (m_state.compare_exchange_strong(seen, WRITER, std::memory_order_acquire,
                                          std::memory_order_relaxed))
      {
        return;
      }
      m_state.wait(seen, std::memory_order_relaxed);
    }
  }

  bool try_lock()
  {
    State expected = FREE;
    return m_state.compare_exchange_weak(expected, WRITER, std::memory_order_acquire,
                                         std::memory_order_relaxed);
  }

  void unlock()
  {
    m_state.store(FREE, std::memory_order_release);
    m_state.notify_all();  // both readers and writers may be waiting
  }

  void lock_shared()
  {
    for (;;)
    {
      State seen = m_state.load(std::memory_order_relaxed);
      while (seen < MAX_READERS)
      {
        if (m_state.compare_exchange_weak(seen, seen + 1, std::memory_order_acquire,
                                          std::memory_order_relaxed))
        {
          return;
        }
      }
      assert(seen == WRITER);  // otherwise the reader count overflowed
      m_state.wait(seen, std::memory_order_relaxed);
    }
  }

  bool try_lock_shared()
  {
    State seen = m_state.load(std::memory_order_relaxed);
    if (seen >= MAX_READERS)
      return false;
    return m_state.compare_exchange_weak(seen, seen + 1, std::memory_order_acquire,
                                         std::memory_order_relaxed);
  }

  void unlock_shared()
  {
    // The last reader out wakes a writer.
    if (m_state.fetch_sub(1, std::memory_order_release) == 1)
      m_state.notify_one();
  }

private:
  using State = std::uintptr_t;

  static constexpr State FREE = 0;
  static constexpr State WRITER = ~State{0};
  static constexpr State MAX_READERS = WRITER - 1;

  std::atomic<State> m_state{FREE};
};
}  // namespace Common
