// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <utility>
#include <variant>

namespace Common
{
// Holds either a value or a function producing it. The function runs the first time the value
// is accessed and its result replaces it. Not thread safe.
template <typename T>
class Lazy
{
public:
  using Source = std::variant<T, std::function<T()>>;

  Lazy() : m_slot(std::in_place_index<0>) {}
  Lazy(const Source& source) : m_slot(source) {}
  Lazy(Source&& source) : m_slot(std::move(source)) {}

  const Lazy<T>& operator=(const Source& source)
  {
    m_slot = source;
    return *this;
  }
  const Lazy<T>& operator=(Source&& source)
  {
    m_slot = std::move(source);
    return *this;
  }

  T& operator*() { return Resolve(); }
  const T& operator*() const { return Resolve(); }
  T* operator->() { return &Resolve(); }
  const T* operator->() const { return &Resolve(); }

private:
  T& Resolve() const
  {
    if (auto* producer = std::get_if<1>(&m_slot))
    {
      T produced = (*producer)();
      m_slot.template emplace<0>(std::move(produced));
    }
    return std::get<0>(m_slot);
  }

  mutable Source m_slot;
};
}  // namespace Common
