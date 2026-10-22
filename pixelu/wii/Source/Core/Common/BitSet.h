// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <bit>
#include <cstddef>
#include <initializer_list>
#include <type_traits>

#include "Common/CommonTypes.h"

namespace Common
{
// A set of small integers stored as the bits of an unsigned integer (bit 0 = element 0).
//
// It indexes like an array of bools (`set[3] = true`), combines like an integer
// (`a | b`, `a & ~b`), counts with Count(), and iterating it yields the indices of the set bits
// in increasing order:
//
//   for (int index : BitSet32{1, 5, 9}) ...
template <typename IntTy>
class BitSet
{
  static_assert(std::is_unsigned_v<IntTy>, "BitSet needs an unsigned integer type");

  static constexpr IntTy Bit(std::size_t index) { return static_cast<IntTy>(IntTy{1} << index); }

public:
  // Proxy for one bit, returned by the non-const operator[].
  class Ref
  {
  public:
    constexpr Ref(BitSet* owner, IntTy mask) : m_owner(owner), m_mask(mask) {}
    constexpr Ref(Ref&& other) : m_owner(other.m_owner), m_mask(other.m_mask) {}

    constexpr operator bool() const { return (m_owner->m_val & m_mask) != 0; }

    bool operator=(bool value)
    {
      if (value)
        m_owner->m_val |= m_mask;
      else
        m_owner->m_val &= static_cast<IntTy>(~m_mask);
      return value;
    }

  private:
    BitSet* m_owner;
    IntTy m_mask;
  };

  // Walks the set bits from lowest to highest. The end iterator has position -1.
  class Iterator
  {
  public:
    constexpr Iterator(IntTy remaining, int position) : m_remaining(remaining), m_position(position)
    {
    }
    constexpr Iterator(const Iterator& other) = default;
    constexpr Iterator& operator=(const Iterator& other) = default;

    constexpr Iterator& operator++()
    {
      if (m_remaining == 0)
      {
        m_position = -1;
        return *this;
      }
      m_position = std::countr_zero(m_remaining);
      m_remaining &= static_cast<IntTy>(m_remaining - 1);  // drop the lowest set bit
      return *this;
    }

    constexpr Iterator operator++(int)
    {
      Iterator before = *this;
      ++*this;
      return before;
    }

    constexpr int operator*() const { return m_position; }
    constexpr bool operator==(const Iterator& other) const { return m_position == other.m_position; }
    constexpr bool operator!=(const Iterator& other) const { return m_position != other.m_position; }

  private:
    IntTy m_remaining;
    int m_position;
  };

  constexpr BitSet() = default;
  constexpr explicit BitSet(IntTy bits) : m_val(bits) {}
  constexpr BitSet(std::initializer_list<int> indices)
  {
    for (const int index : indices)
      m_val |= Bit(static_cast<std::size_t>(index));
  }

  // The set {0, 1, ..., count - 1}.
  static constexpr BitSet AllTrue(std::size_t count)
  {
    if (count >= sizeof(IntTy) * 8)
      return BitSet(static_cast<IntTy>(~IntTy{0}));
    return BitSet(static_cast<IntTy>(Bit(count) - 1));
  }

  Ref operator[](std::size_t index) { return Ref(this, Bit(index)); }
  constexpr bool operator[](std::size_t index) const { return (m_val & Bit(index)) != 0; }

  constexpr bool operator==(BitSet other) const { return m_val == other.m_val; }
  constexpr bool operator!=(BitSet other) const { return m_val != other.m_val; }
  constexpr bool operator<(BitSet other) const { return m_val < other.m_val; }
  constexpr bool operator>(BitSet other) const { return m_val > other.m_val; }

  constexpr BitSet operator|(BitSet other) const { return BitSet(m_val | other.m_val); }
  constexpr BitSet operator&(BitSet other) const { return BitSet(m_val & other.m_val); }
  constexpr BitSet operator^(BitSet other) const { return BitSet(m_val ^ other.m_val); }
  constexpr BitSet operator~() const { return BitSet(static_cast<IntTy>(~m_val)); }
  constexpr BitSet operator<<(IntTy shift) const { return BitSet(static_cast<IntTy>(m_val << shift)); }
  constexpr BitSet operator>>(IntTy shift) const { return BitSet(static_cast<IntTy>(m_val >> shift)); }

  BitSet& operator|=(BitSet other) { return *this = *this | other; }
  BitSet& operator&=(BitSet other) { return *this = *this & other; }
  BitSet& operator^=(BitSet other) { return *this = *this ^ other; }
  BitSet& operator<<=(IntTy shift) { return *this = *this << shift; }
  BitSet& operator>>=(IntTy shift) { return *this = *this >> shift; }

  constexpr explicit operator bool() const { return m_val != 0; }

  // Number of elements in the set.
  constexpr unsigned int Count() const { return static_cast<unsigned int>(std::popcount(m_val)); }

  constexpr Iterator begin() const { return ++Iterator(m_val, 0); }
  constexpr Iterator end() const { return Iterator(m_val, -1); }

  IntTy m_val{};
};
}  // namespace Common

using BitSet8 = Common::BitSet<u8>;
using BitSet16 = Common::BitSet<u16>;
using BitSet32 = Common::BitSet<u32>;
using BitSet64 = Common::BitSet<u64>;
