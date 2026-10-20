// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <memory>
#include <utility>

// UniqueBuffer<T>: a heap array with a size, like a std::vector<T> that never grows and whose
// elements start out uninitialised (no cost for zeroing large buffers that are about to be
// overwritten anyway).

namespace Common
{
namespace detail
{
template <auto MakeFunc>
class BufferBase final
{
public:
  using PtrType = decltype(MakeFunc(1));
  using value_type = typename PtrType::element_type;
  using size_type = std::size_t;

  BufferBase() {}
  BufferBase(PtrType ptr, size_type new_size) : m_ptr{std::move(ptr)}, m_size{new_size} {}
  explicit BufferBase(size_type new_size) : m_ptr{MakeFunc(new_size)}, m_size{new_size} {}

  BufferBase(const BufferBase&) = default;
  BufferBase& operator=(const BufferBase&) = default;

  BufferBase(BufferBase&& other) noexcept
      : m_ptr{std::move(other.m_ptr)}, m_size{std::exchange(other.m_size, 0)}
  {
  }
  BufferBase& operator=(BufferBase&& other) noexcept
  {
    if (this != &other)
    {
      m_ptr = std::move(other.m_ptr);
      m_size = std::exchange(other.m_size, 0);
    }
    return *this;
  }

  // Replaces the contents with the given allocation.
  void assign(PtrType ptr, size_type new_size)
  {
    m_ptr = std::move(ptr);
    m_size = new_size;
  }
  // Replaces the contents with a fresh, uninitialised allocation of new_size elements.
  void reset(size_type new_size = 0)
  {
    m_ptr = MakeFunc(new_size);
    m_size = new_size;
  }
  void clear() { reset(); }

  // Not provided on purpose: keeping old contents is rarely wanted and costs a copy.
  void resize(std::size_t) = delete;

  // Hands over the allocation and leaves the buffer empty.
  std::pair<PtrType, size_type> extract()
  {
    std::pair<PtrType, size_type> taken{std::move(m_ptr), m_size};
    reset();
    return taken;
  }

  void swap(BufferBase& other)
  {
    std::swap(m_ptr, other.m_ptr);
    std::swap(m_size, other.m_size);
  }

  size_type size() const { return m_size; }
  bool empty() const { return m_size == 0; }

  value_type* get() { return m_ptr.get(); }
  const value_type* get() const { return m_ptr.get(); }
  value_type* data() { return m_ptr.get(); }
  const value_type* data() const { return m_ptr.get(); }

  value_type* begin() { return m_ptr.get(); }
  value_type* end() { return m_ptr.get() + m_size; }
  const value_type* begin() const { return m_ptr.get(); }
  const value_type* end() const { return m_ptr.get() + m_size; }

  value_type& operator[](size_type index) { return m_ptr[index]; }
  const value_type& operator[](size_type index) const { return m_ptr[index]; }

private:
  PtrType m_ptr;
  size_type m_size = 0;
};
}  // namespace detail

template <typename T>
using UniqueBuffer = detail::BufferBase<std::make_unique_for_overwrite<T[]>>;
}  // namespace Common
