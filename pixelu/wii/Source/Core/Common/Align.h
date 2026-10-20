// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <type_traits>

namespace Common
{
// Rounds an unsigned value down to the nearest multiple of `size`.
template <typename T>
constexpr T AlignDown(T value, std::size_t size)
{
  static_assert(std::is_unsigned_v<T>, "AlignDown needs an unsigned type");
  const auto remainder = static_cast<T>(value % size);
  return static_cast<T>(value - remainder);
}

// Rounds an unsigned value up to the nearest multiple of `size` (wrapping like the type does).
template <typename T>
constexpr T AlignUp(T value, std::size_t size)
{
  static_assert(std::is_unsigned_v<T>, "AlignUp needs an unsigned type");
  const T bumped = static_cast<T>(value + static_cast<T>(size - 1));
  return AlignDown<T>(bumped, size);
}
}  // namespace Common
