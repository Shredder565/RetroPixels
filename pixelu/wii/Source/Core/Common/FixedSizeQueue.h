// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace Common
{
// A ring buffer of at most N elements. Pushing onto a full queue drops the oldest element.
// No bounds checks: front()/pop() on an empty queue are not allowed.
template <class T, int N>
class FixedSizeQueue
{
public:
  void clear()
  {
    // Release resources held by non-trivial elements. (Resetting the whole array at once makes
    // some compilers generate enormous code for large N.)
    if constexpr (!std::is_trivial_v<T>)
    {
      while (m_count > 0)
        pop();
    }
    m_first = 0;
    m_next = 0;
    m_count = 0;
  }

  void push(T t) { Store(std::move(t)); }

  template <class... Args>
  void emplace(Args&&... args)
  {
    Store(T(std::forward<Args>(args)...));
  }

  void pop()
  {
    if constexpr (!std::is_trivial_v<T>)
      m_items[m_first] = {};
    m_first = Advance(m_first);
    --m_count;
  }

  T pop_front()
  {
    T value = std::move(front());
    pop();
    return value;
  }

  T& front() noexcept { return m_items[m_first]; }
  const T& front() const noexcept { return m_items[m_first]; }
  size_t size() const noexcept { return static_cast<size_t>(m_count); }
  bool empty() const noexcept { return m_count == 0; }

private:
  static constexpr int Advance(int index) { return index + 1 == N ? 0 : index + 1; }

  void Store(T&& value)
  {
    if (m_count == N)
      m_first = Advance(m_first);  // overwrite the oldest element
    else
      ++m_count;
    m_items[m_next] = std::move(value);
    m_next = Advance(m_next);
  }

  std::array<T, N> m_items{};
  int m_first = 0;  // index of the oldest element
  int m_next = 0;   // where the next element goes
  int m_count = 0;
};
}  // namespace Common
