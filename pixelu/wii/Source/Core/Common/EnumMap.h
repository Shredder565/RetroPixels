// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>

#include "Common/TypeUtils.h"

template <std::size_t position, std::size_t bits, typename T, typename StorageType>
struct BitField;

namespace Common
{
// A fixed array indexed by an enum whose members run from 0 up to `last_member`.
template <typename V, Common::Enum auto last_member>
class EnumMap final
{
  using Key = decltype(last_member);
  static constexpr std::size_t COUNT = static_cast<std::size_t>(last_member) + 1;
  using Storage = std::array<V, COUNT>;

public:
  constexpr EnumMap() = default;
  constexpr EnumMap(const EnumMap&) = default;
  constexpr EnumMap(EnumMap&&) = default;
  constexpr EnumMap& operator=(const EnumMap&) = default;
  constexpr EnumMap& operator=(EnumMap&&) = default;

  // Initialises every entry; exactly one value per enum member must be given.
  template <typename... Values>
  requires(Common::IsNOf<V, COUNT, Values...>::value)
  constexpr EnumMap(Values... values) : m_entries{static_cast<V>(values)...}
  {
  }

  constexpr V& operator[](Key key) { return m_entries[static_cast<std::size_t>(key)]; }
  constexpr const V& operator[](Key key) const { return m_entries[static_cast<std::size_t>(key)]; }

  // Indexing with a BitField (guest-controlled data) is only allowed when every value the field
  // can hold has an entry.
  template <std::size_t position, std::size_t bits, typename StorageType>
  constexpr V& operator[](BitField<position, bits, Key, StorageType> key)
  {
    static_assert((std::size_t{1} << bits) == COUNT,
                  "This BitField can hold values outside the EnumMap");
    return m_entries[static_cast<std::size_t>(key.Value())];
  }
  template <std::size_t position, std::size_t bits, typename StorageType>
  constexpr const V& operator[](BitField<position, bits, Key, StorageType> key) const
  {
    static_assert((std::size_t{1} << bits) == COUNT,
                  "This BitField can hold values outside the EnumMap");
    return m_entries[static_cast<std::size_t>(key.Value())];
  }

  constexpr bool InBounds(Key key) const { return static_cast<std::size_t>(key) < COUNT; }
  constexpr std::size_t size() const noexcept { return COUNT; }

  constexpr V* data() { return m_entries.data(); }
  constexpr const V* data() const { return m_entries.data(); }

  constexpr auto begin() { return m_entries.begin(); }
  constexpr auto end() { return m_entries.end(); }
  constexpr auto begin() const { return m_entries.begin(); }
  constexpr auto end() const { return m_entries.end(); }
  constexpr auto cbegin() const { return m_entries.cbegin(); }
  constexpr auto cend() const { return m_entries.cend(); }

  constexpr void fill(const V& value) { m_entries.fill(value); }

private:
  Storage m_entries{};
};
}  // namespace Common
