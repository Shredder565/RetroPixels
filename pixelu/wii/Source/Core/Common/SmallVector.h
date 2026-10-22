// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <utility>

#include "Common/TypeUtils.h"

namespace Common
{
// A vector with fixed capacity stored inline (no heap allocation), similar to C++26's
// std::inplace_vector. Exceeding MaxSize is a programming error (asserted in debug builds).
template <typename T, size_t MaxSize>
class SmallVector final
{
public:
  using value_type = T;

  SmallVector() = default;
  explicit SmallVector(size_t new_size) { resize(new_size); }
  ~SmallVector() { clear(); }

  SmallVector(const SmallVector& other) { AppendCopies(other); }
  SmallVector(SmallVector&& other) { TakeFrom(other); }

  SmallVector& operator=(const SmallVector& rhs)
  {
    if (this != &rhs)
    {
      clear();
      AppendCopies(rhs);
    }
    return *this;
  }

  SmallVector& operator=(SmallVector&& rhs)
  {
    if (this != &rhs)
    {
      clear();
      TakeFrom(rhs);
    }
    return *this;
  }

  void push_back(const value_type& x) { emplace_back(x); }
  void push_back(value_type&& x) { emplace_back(std::move(x)); }

  template <typename... Args>
  value_type& emplace_back(Args&&... args)
  {
    assert(m_count < MaxSize);
    value_type& added = m_slots[m_count].Construct(std::forward<Args>(args)...);
    ++m_count;
    return added;
  }

  void pop_back()
  {
    assert(m_count != 0);
    --m_count;
    m_slots[m_count].Destroy();
  }

  value_type& operator[](size_t i)
  {
    assert(i < m_count);
    return data()[i];
  }
  const value_type& operator[](size_t i) const
  {
    assert(i < m_count);
    return data()[i];
  }

  // The slots have exactly the size of T, so they form a contiguous array of T.
  value_type* data() { return m_slots.data()->Ptr(); }
  const value_type* data() const { return m_slots.data()->Ptr(); }
  value_type* begin() { return data(); }
  value_type* end() { return data() + m_count; }
  const value_type* begin() const { return data(); }
  const value_type* end() const { return data() + m_count; }

  size_t capacity() const { return MaxSize; }
  size_t size() const { return m_count; }
  bool empty() const { return m_count == 0; }

  // New elements are value-initialised.
  void resize(size_t new_size)
  {
    assert(new_size <= MaxSize);
    while (m_count > new_size)
      pop_back();
    while (m_count < new_size)
      emplace_back();
  }

  void clear() { resize(0); }

private:
  void AppendCopies(const SmallVector& source)
  {
    for (const value_type& value : source)
      emplace_back(value);
  }

  void TakeFrom(SmallVector& source)
  {
    for (value_type& value : source)
      emplace_back(std::move(value));
    source.clear();
  }

  std::array<ManuallyConstructedValue<T>, MaxSize> m_slots;
  size_t m_count = 0;
};
}  // namespace Common
