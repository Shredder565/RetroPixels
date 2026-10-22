// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

#include "Common/CommonTypes.h"

namespace Common
{
// std::span::subspan with the out-of-range cases clamped to the span instead of undefined.
template <class T>
[[nodiscard]] constexpr std::span<T> SafeSubspan(std::span<T> span, size_t offset,
                                                 size_t count = std::dynamic_extent)
{
  const size_t start = std::min(offset, span.size());
  const size_t available = span.size() - start;
  const size_t length = count == std::dynamic_extent ? available : std::min(count, available);
  return span.subspan(start, length);
}

// Reads a T from `offset` in the span; bytes past the end of the span are left
// value-initialised.
template <class T>
[[nodiscard]] T SafeSpanRead(std::span<const u8> span, size_t offset)
{
  static_assert(std::is_trivially_copyable_v<T>, "SafeSpanRead needs a trivially copyable type");
  const std::span<const u8> source = SafeSubspan(span, offset);
  T value{};
  std::memcpy(&value, source.data(), std::min(source.size(), sizeof(T)));
  return value;
}
}  // namespace Common
