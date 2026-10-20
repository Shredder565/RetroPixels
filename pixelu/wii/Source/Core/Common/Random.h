// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <type_traits>

namespace Common::Random
{
// Fills `buffer` with `size` cryptographically secure random bytes. Thread-safe.
void Generate(void* buffer, std::size_t size);

// A random value of arithmetic type T (every bit pattern equally likely).
template <typename T>
T GenerateValue()
{
  static_assert(std::is_arithmetic_v<T>, "GenerateValue needs an arithmetic type");
  T value;
  Generate(&value, sizeof(value));
  return value;
}
}  // namespace Common::Random
