// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <utility>

// Projections for std::ranges algorithms over pairs (map entries and the like).
namespace Common::Projection
{
struct First
{
  template <class Pair>
  [[nodiscard]] constexpr auto&& operator()(Pair&& pair) const noexcept
  {
    return std::forward<Pair>(pair).first;
  }
};

struct Second
{
  template <class Pair>
  [[nodiscard]] constexpr auto&& operator()(Pair&& pair) const noexcept
  {
    return std::forward<Pair>(pair).second;
  }
};

using Key = First;
using Value = Second;
}  // namespace Common::Projection
