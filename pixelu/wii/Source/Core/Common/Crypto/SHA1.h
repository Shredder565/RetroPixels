// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace Common::SHA1
{
using Digest = std::array<u8, 20>;
static constexpr size_t DIGEST_LEN = sizeof(Digest);

// Incremental SHA-1. Finish() may only be called once.
class Context
{
public:
  virtual ~Context() = default;

  virtual void Update(const u8* msg, size_t len) = 0;
  void Update(std::span<const u8> msg) { return Update(msg.data(), msg.size()); }
  void Update(std::string_view msg)
  {
    return Update(reinterpret_cast<const u8*>(msg.data()), msg.size());
  }

  virtual Digest Finish() = 0;

  // True when the CPU's SHA instructions are used.
  virtual bool HwAccelerated() const = 0;
};

std::unique_ptr<Context> CreateContext();

Digest CalculateDigest(const u8* msg, size_t len);

template <typename T>
inline Digest CalculateDigest(const std::vector<T>& msg)
{
  static_assert(std::is_trivially_copyable_v<T>);
  ASSERT(msg.size() <= std::numeric_limits<size_t>::max() / sizeof(T));
  return CalculateDigest(reinterpret_cast<const u8*>(msg.data()), msg.size() * sizeof(T));
}

inline Digest CalculateDigest(const std::string_view& msg)
{
  return CalculateDigest(reinterpret_cast<const u8*>(msg.data()), msg.size());
}

template <typename T, size_t Size>
inline Digest CalculateDigest(const std::array<T, Size>& msg)
{
  static_assert(std::is_trivially_copyable_v<T>);
  return CalculateDigest(reinterpret_cast<const u8*>(msg.data()), sizeof(msg));
}

// Upper-case hex, 40 characters.
std::string DigestToString(const Digest& digest);

// Parses 40 hex digits (either case). Anything else fails an assertion.
constexpr Digest StringToDigest(std::string_view str)
{
  ASSERT(str.size() == DIGEST_LEN * 2);

  const auto nibble = [](char c) -> u8 {
    if (c >= '0' && c <= '9')
      return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
      return static_cast<u8>(c - 'A' + 10);
    ASSERT(false);
    return 0;
  };

  Digest result{};
  for (size_t i = 0; i + 1 < str.size() && i / 2 < DIGEST_LEN; i += 2)
    result[i / 2] = static_cast<u8>((nibble(str[i]) << 4) | nibble(str[i + 1]));
  return result;
}
}  // namespace Common::SHA1
