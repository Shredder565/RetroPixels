// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Byte-order helpers. The GameCube and Wii are big endian; the host is little endian.

#pragma once

#include <bit>
#include <cstring>
#include <type_traits>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

namespace Common
{
inline u8 swap8(u8 data)
{
  return data;
}

// Reads a 24-bit big-endian value.
inline u32 swap24(const u8* data)
{
  return (u32{data[0]} << 16) | (u32{data[1]} << 8) | u32{data[2]};
}

inline u16 swap16(u16 data)
{
  return std::byteswap(data);
}
inline u32 swap32(u32 data)
{
  return std::byteswap(data);
}
inline u64 swap64(u64 data)
{
  return std::byteswap(data);
}

// Read a big-endian value from unaligned memory.
inline u16 swap16(const u8* data)
{
  u16 raw;
  std::memcpy(&raw, data, sizeof(raw));
  return swap16(raw);
}
inline u32 swap32(const u8* data)
{
  u32 raw;
  std::memcpy(&raw, data, sizeof(raw));
  return swap32(raw);
}
inline u64 swap64(const u8* data)
{
  u64 raw;
  std::memcpy(&raw, data, sizeof(raw));
  return swap64(raw);
}

// Store a value as big endian to unaligned memory.
inline void WriteSwap16(u8* data, u16 value)
{
  const u16 swapped = swap16(value);
  std::memcpy(data, &swapped, sizeof(swapped));
}
inline void WriteSwap32(u8* data, u32 value)
{
  const u32 swapped = swap32(value);
  std::memcpy(data, &swapped, sizeof(swapped));
}
inline void WriteSwap64(u8* data, u64 value)
{
  const u64 swapped = swap64(value);
  std::memcpy(data, &swapped, sizeof(swapped));
}

// Reverses `count` bytes in place.
template <int count>
void swap(u8*);

template <>
inline void swap<1>(u8*)
{
}

template <>
inline void swap<2>(u8* data)
{
  const u16 reversed = swap16(static_cast<const u8*>(data));
  std::memcpy(data, &reversed, sizeof(reversed));
}

template <>
inline void swap<4>(u8* data)
{
  const u32 reversed = swap32(static_cast<const u8*>(data));
  std::memcpy(data, &reversed, sizeof(reversed));
}

template <>
inline void swap<8>(u8* data)
{
  const u64 reversed = swap64(static_cast<const u8*>(data));
  std::memcpy(data, &reversed, sizeof(reversed));
}

// Converts an arithmetic value between big endian and host order (the same operation both ways).
template <typename T>
inline T FromBigEndian(T data)
{
  static_assert(std::is_arithmetic_v<T>, "FromBigEndian only takes arithmetic types");
  swap<sizeof(data)>(reinterpret_cast<u8*>(&data));
  return data;
}

// An arithmetic value stored big endian in memory and used like a normal value.
template <typename value_type>
struct BigEndianValue
{
  static_assert(std::is_arithmetic_v<value_type>, "BigEndianValue only takes arithmetic types");

  BigEndianValue() = default;
  explicit BigEndianValue(value_type val) { *this = val; }

  operator value_type() const { return FromBigEndian(raw); }
  BigEndianValue& operator=(value_type v)
  {
    raw = FromBigEndian(v);
    return *this;
  }

private:
  value_type raw;
};
}  // namespace Common

template <typename value_type>
struct fmt::formatter<Common::BigEndianValue<value_type>>
{
  fmt::formatter<value_type> m_formatter;
  constexpr auto parse(format_parse_context& ctx) { return m_formatter.parse(ctx); }
  template <typename FormatContext>
  auto format(const Common::BigEndianValue<value_type>& value, FormatContext& ctx) const
  {
    return m_formatter.format(static_cast<value_type>(value), ctx);
  }
};
