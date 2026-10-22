// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"

namespace Common
{
// Width of T in bits.
template <typename T>
constexpr size_t BitSize() noexcept
{
  return sizeof(T) * CHAR_BIT;
}

// Bit `bit` of src, as 0 or 1.
template <typename T>
constexpr T ExtractBit(const T src, const size_t bit) noexcept
{
  return static_cast<T>((src >> bit) & static_cast<T>(1));
}

template <size_t bit, typename T>
constexpr T ExtractBit(const T src) noexcept
{
  static_assert(bit < BitSize<T>(), "Bit index outside the type");
  return ExtractBit(src, bit);
}

// Bits begin..end (both inclusive) of src, shifted down to bit 0. Signed inputs are treated as
// their unsigned counterpart.
template <typename T, typename Result = std::make_unsigned_t<T>>
constexpr Result ExtractBits(const T src, const size_t begin, const size_t end) noexcept
{
  using U = std::make_unsigned_t<T>;
  const size_t count = end - begin + 1;
  const U all_ones = static_cast<U>(~U{0});
  const U mask = count >= BitSize<T>() ? all_ones : static_cast<U>(all_ones >> (BitSize<T>() - count));
  return static_cast<Result>(static_cast<U>(static_cast<U>(src) >> begin) & mask);
}

template <size_t begin, size_t end, typename T, typename Result = std::make_unsigned_t<T>>
constexpr Result ExtractBits(const T src) noexcept
{
  static_assert(begin < end, "The first bit must come before the last");
  static_assert(begin < BitSize<T>(), "First bit outside the type");
  static_assert(end < BitSize<T>(), "Last bit outside the type");
  return ExtractBits<T, Result>(src, begin, end);
}

// True for masks of the form 0...01...1, including all zeros and all ones.
template <typename T>
constexpr bool IsValidLowMask(const T mask) noexcept
{
  static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>, "Masks must be unsigned");
  return (mask & static_cast<T>(mask + 1)) == 0;
}

// Proxy that reads or writes a T at a possibly unaligned address through memcpy, avoiding
// strict-aliasing problems. Indexing steps through an array of T.
template <typename T, typename PtrType>
class BitCastPtrType
{
  static_assert(std::is_trivially_copyable_v<PtrType>, "Source type must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<T>, "Target type must be trivially copyable");
  static_assert(!std::is_pointer_v<T>, "Target type must not be a pointer");

public:
  explicit BitCastPtrType(PtrType* ptr) : m_address(ptr) {}

  auto& operator=(const T& source) requires(!std::is_const_v<PtrType>)
  {
    std::memcpy(m_address, &source, sizeof(T));
    return *this;
  }

  inline operator T() const
  {
    T value;
    std::memcpy(&value, m_address, sizeof(T));
    return value;
  }

  inline auto operator[](std::size_t index) const
  {
    using Byte = std::conditional_t<std::is_const_v<PtrType>, const std::byte, std::byte>;
    Byte* const element = reinterpret_cast<Byte*>(m_address) + index * sizeof(T);
    return BitCastPtrType<T, Byte>{element};
  }

private:
  PtrType* m_address;
};

// T value = BitCastPtr<T>(ptr);  BitCastPtr<T>(ptr) = value;  BitCastPtr<T>(ptr)[i] = value;
template <typename T, typename PtrType>
inline auto BitCastPtr(PtrType* ptr) noexcept -> BitCastPtrType<T, PtrType>
{
  return BitCastPtrType<T, PtrType>{ptr};
}

// The object's bytes reinterpreted as an array of ValueType.
template <typename ValueType, typename From>
[[nodiscard]] constexpr auto BitCastToArray(const From& obj) noexcept
{
  return std::bit_cast<std::array<ValueType, sizeof(From) / sizeof(ValueType)>>(obj);
}

template <typename T>
constexpr void SetBit(T& value, size_t bit_number, bool bit_value)
{
  static_assert(std::is_unsigned_v<T>, "SetBit needs an unsigned type");
  const T bit = static_cast<T>(T{1} << bit_number);
  if (bit_value)
    value = static_cast<T>(value | bit);
  else
    value = static_cast<T>(value & static_cast<T>(~bit));
}

template <size_t bit_number, typename T>
constexpr void SetBit(T& value, bool bit_value)
{
  SetBit(value, bit_number, bit_value);
}

// One flag inside an integer, usable as a bool.
template <typename T>
class FlagBit
{
public:
  FlagBit(T* bits, std::type_identity_t<T> bit) : m_word(*bits), m_flag(bit) {}

  explicit operator bool() const { return (m_word & m_flag) != 0; }

  FlagBit& operator=(const bool rhs) requires(!std::is_const_v<T>)
  {
    m_word = rhs ? static_cast<T>(m_word | m_flag) : static_cast<T>(m_word & ~m_flag);
    return *this;
  }

private:
  T& m_word;
  const T m_flag;
};

// A set of enum flags (each enumerator a bit mask) stored in the enum's underlying type.
template <typename T>
class Flags
{
public:
  constexpr Flags() = default;
  constexpr Flags(std::initializer_list<T> bits)
  {
    for (const T bit : bits)
      m_hex |= std::to_underlying(bit);
  }

  auto operator[](T bit) { return FlagBit(&m_hex, std::to_underlying(bit)); }
  auto operator[](T bit) const { return FlagBit(&m_hex, std::to_underlying(bit)); }

  std::underlying_type_t<T> m_hex = 0;
};

// Shifts left and fills the new low bits with copies of the old lowest bit, widening an N-bit
// value to N + left_shift_amount bits (0b101 by 2 -> 0b10111).
template <typename T>
T ExpandValue(T value, size_t left_shift_amount)
{
  static_assert(std::is_unsigned_v<T>, "ExpandValue needs an unsigned type");
  const T fill = (value & 1) ? static_cast<T>(~T{0}) : T{0};
  return static_cast<T>((value << left_shift_amount) | (fill >> (BitSize<T>() - left_shift_amount)));
}

// Byte views of contiguous ranges and of single trivially copyable objects.
template <std::ranges::contiguous_range T>
requires(std::is_trivially_copyable_v<std::ranges::range_value_t<T>>)
constexpr auto AsU8Span(const T& range)
{
  return std::span{reinterpret_cast<const u8*>(std::ranges::data(range)),
                   std::ranges::size(range) * sizeof(std::ranges::range_value_t<T>)};
}

template <std::ranges::contiguous_range T>
requires(std::is_trivially_copyable_v<std::ranges::range_value_t<T>>)
constexpr auto AsWritableU8Span(T& range)
{
  return std::span{reinterpret_cast<u8*>(std::ranges::data(range)),
                   std::ranges::size(range) * sizeof(std::ranges::range_value_t<T>)};
}

template <typename T>
requires(!std::ranges::contiguous_range<T> && std::is_trivially_copyable_v<T>)
constexpr auto AsU8Span(const T& obj)
{
  return std::span{reinterpret_cast<const u8*>(std::addressof(obj)), sizeof(T)};
}

template <typename T>
requires(!std::ranges::contiguous_range<T> && std::is_trivially_copyable_v<T>)
constexpr auto AsWritableU8Span(T& obj)
{
  return std::span{reinterpret_cast<u8*>(std::addressof(obj)), sizeof(T)};
}
}  // namespace Common
