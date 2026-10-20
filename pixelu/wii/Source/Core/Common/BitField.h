// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// BitField<position, bits, T> names a group of bits inside an integer, for describing hardware
// registers and packed structures independently of the compiler's own bit-field layout:
//
//   union Control
//   {
//     u32 hex;
//     BitField<0, 1, bool, u32> enable;
//     BitField<1, 3, MyEnum> mode;      // the storage type is the enum's underlying type
//     BitField<4, 12, s32> offset;      // signed: read back sign-extended
//   };
//
// Reading converts to T; assigning changes only the field's bits. Plain copy assignment is
// deleted (it would copy the whole storage word) but copy construction stays, so that the
// containing unions remain trivially copyable for save states. BitFieldArray describes `size`
// equally sized fields packed one after another. fmt formats both like a T.

#pragma once

#include <cstddef>
#include <iterator>
#include <limits>
#include <type_traits>

#include <fmt/format.h>

#include "Common/Inline.h"

namespace Common::detail
{
template <typename T>
using BitFieldDefaultStorage =
    typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>,
                                std::type_identity<T>>::type;

// Shared arithmetic. Shifts use the usual integer promotions of the storage type.
template <typename StorageType>
struct BitFieldMath
{
  static constexpr std::size_t WIDTH = 8 * sizeof(StorageType);
  using Unsigned = std::make_unsigned_t<StorageType>;
  using Promoted = decltype(+StorageType{});

  // `bits` ones starting at bit `shift`.
  static constexpr StorageType Mask(std::size_t shift, std::size_t bits)
  {
    const Unsigned ones = std::numeric_limits<Unsigned>::max();
    return static_cast<StorageType>(static_cast<Unsigned>(ones >> (WIDTH - bits)) << shift);
  }

  template <typename T>
  static constexpr T Read(StorageType storage, std::size_t shift, std::size_t bits)
  {
    if constexpr (std::is_signed_v<T>)
    {
      // Move the field to the top, then shift it back down (arithmetically for signed storage).
      const Promoted at_top = static_cast<Promoted>(Promoted(storage) << (WIDTH - bits - shift));
      return static_cast<T>(at_top >> (WIDTH - bits));
    }
    else
    {
      return static_cast<T>((Promoted(storage) & Promoted(Mask(shift, bits))) >> shift);
    }
  }

  template <typename T>
  static constexpr StorageType Write(StorageType storage, std::size_t shift, std::size_t bits,
                                     T value)
  {
    const Unsigned mask = static_cast<Unsigned>(Mask(shift, bits));
    const Unsigned field = static_cast<Unsigned>(static_cast<Unsigned>(value) << shift);
    return static_cast<StorageType>((static_cast<Unsigned>(storage) & ~mask) | (field & mask));
  }
};
}  // namespace Common::detail

#pragma pack(1)
template <std::size_t position, std::size_t bits, typename T,
          typename StorageType = Common::detail::BitFieldDefaultStorage<T>>
struct BitField
{
private:
  using Math = Common::detail::BitFieldMath<StorageType>;

public:
  // Deleted: would it set the field or the whole storage? Assign instead.
  BitField(T val) = delete;

  constexpr BitField() = default;
  constexpr BitField(const BitField&) = default;
  BitField& operator=(const BitField&) = delete;

  DOLPHIN_FORCE_INLINE BitField& operator=(T val)
  {
    storage = Math::Write(storage, position, bits, val);
    return *this;
  }

  constexpr T Value() const { return Math::template Read<T>(storage, position, bits); }
  constexpr operator T() const { return Value(); }

  static constexpr bool IsSigned() { return std::is_signed_v<T>; }
  static constexpr std::size_t StartBit() { return position; }
  static constexpr std::size_t NumBits() { return bits; }

private:
  StorageType storage;

  static_assert(bits > 0, "A BitField needs at least one bit");
  static_assert(position < 8 * sizeof(StorageType), "BitField position outside its storage");
  static_assert(position + bits <= 8 * sizeof(StorageType), "BitField extends past its storage");
  static_assert(bits <= 8 * sizeof(T), "BitField is wider than its value type");
  static_assert(sizeof(T) <= sizeof(StorageType), "BitField value type larger than storage");
};
#pragma pack()

template <std::size_t position, std::size_t bits, typename T, typename S>
struct fmt::formatter<BitField<position, bits, T, S>>
{
  fmt::formatter<T> m_formatter;
  constexpr auto parse(format_parse_context& ctx) { return m_formatter.parse(ctx); }
  template <typename FormatContext>
  auto format(const BitField<position, bits, T, S>& field, FormatContext& ctx) const
  {
    return m_formatter.format(field.Value(), ctx);
  }
};

template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayConstRef;
template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayRef;
template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayConstIterator;
template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayIterator;

#pragma pack(1)
template <std::size_t position, std::size_t bits, std::size_t size, typename T,
          typename StorageType = Common::detail::BitFieldDefaultStorage<T>>
struct BitFieldArray
{
private:
  using Math = Common::detail::BitFieldMath<StorageType>;
  static constexpr std::size_t ShiftOf(std::size_t index) { return position + bits * index; }

public:
  using Ref = BitFieldArrayRef<position, bits, size, T, StorageType>;
  using ConstRef = BitFieldArrayConstRef<position, bits, size, T, StorageType>;
  using Iterator = BitFieldArrayIterator<position, bits, size, T, StorageType>;
  using ConstIterator = BitFieldArrayConstIterator<position, bits, size, T, StorageType>;

  BitFieldArray(T val) = delete;
  constexpr BitFieldArray() = default;
  BitFieldArray& operator=(const BitFieldArray&) = delete;

  constexpr bool IsSigned() const { return std::is_signed_v<T>; }
  constexpr std::size_t StartBit() const { return position; }
  constexpr std::size_t NumBits() const { return bits; }
  constexpr std::size_t Size() const { return size; }
  constexpr std::size_t TotalNumBits() const { return bits * size; }

  constexpr T Value(size_t index) const
  {
    return Math::template Read<T>(storage, ShiftOf(index), bits);
  }
  void SetValue(size_t index, T value)
  {
    storage = Math::Write(storage, ShiftOf(index), bits, value);
  }

  Ref operator[](size_t index) { return Ref(this, index); }
  constexpr const ConstRef operator[](size_t index) const { return ConstRef(this, index); }

  constexpr Iterator begin() { return Iterator(this, 0); }
  constexpr Iterator end() { return Iterator(this, size); }
  constexpr ConstIterator begin() const { return ConstIterator(this, 0); }
  constexpr ConstIterator end() const { return ConstIterator(this, size); }
  constexpr ConstIterator cbegin() const { return begin(); }
  constexpr ConstIterator cend() const { return end(); }

private:
  StorageType storage;

  static_assert(bits > 0, "A BitFieldArray element needs at least one bit");
  static_assert(size > 0 && size <= 8 * sizeof(StorageType), "Bad BitFieldArray size");
  static_assert(position < 8 * sizeof(StorageType), "BitFieldArray position outside storage");
  static_assert(position + bits * size <= 8 * sizeof(StorageType),
                "BitFieldArray extends past its storage");
  static_assert(bits <= 8 * sizeof(T), "BitFieldArray element wider than its value type");
  static_assert(sizeof(T) <= sizeof(StorageType), "BitFieldArray value type larger than storage");
};
#pragma pack()

// Read-only proxy for one element.
template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayConstRef
{
  using Array = BitFieldArray<position, bits, size, T, S>;
  friend struct BitFieldArray<position, bits, size, T, S>;
  friend class BitFieldArrayConstIterator<position, bits, size, T, S>;

public:
  constexpr T Value() const { return m_array->Value(m_index); }
  constexpr operator T() const { return Value(); }

private:
  constexpr BitFieldArrayConstRef(const Array* array, size_t index)
      : m_array(array), m_index(index)
  {
  }

  const Array* const m_array;
  const size_t m_index;
};

// Writable proxy for one element.
template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayRef
{
  using Array = BitFieldArray<position, bits, size, T, S>;
  friend struct BitFieldArray<position, bits, size, T, S>;
  friend class BitFieldArrayIterator<position, bits, size, T, S>;

public:
  constexpr T Value() const { return m_array->Value(m_index); }
  constexpr operator T() const { return Value(); }

  T operator=(const BitFieldArrayRef& other) const { return *this = other.Value(); }
  T operator=(T value) const
  {
    m_array->SetValue(m_index, value);
    return value;
  }

private:
  constexpr BitFieldArrayRef(Array* array, size_t index) : m_array(array), m_index(index) {}

  Array* const m_array;
  const size_t m_index;
};

// Output iterator over writable elements (proxies are not real references, so it cannot be a
// forward iterator). Enough for range-for loops and fmt::join.
template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayIterator
{
  using Array = BitFieldArray<position, bits, size, T, S>;
  friend struct BitFieldArray<position, bits, size, T, S>;

public:
  using iterator_category = std::output_iterator_tag;
  using value_type = T;
  using difference_type = ptrdiff_t;
  using pointer = void;
  using reference = BitFieldArrayRef<position, bits, size, T, S>;

  constexpr BitFieldArrayIterator() = default;
  constexpr BitFieldArrayIterator(const BitFieldArrayIterator&) = default;
  constexpr BitFieldArrayIterator(BitFieldArrayIterator&&) = default;
  BitFieldArrayIterator& operator=(const BitFieldArrayIterator&) = default;
  BitFieldArrayIterator& operator=(BitFieldArrayIterator&&) = default;

  BitFieldArrayIterator& operator++()
  {
    ++m_index;
    return *this;
  }
  BitFieldArrayIterator operator++(int)
  {
    BitFieldArrayIterator before = *this;
    ++m_index;
    return before;
  }

  constexpr reference operator*() const { return reference(m_array, m_index); }
  constexpr bool operator==(BitFieldArrayIterator other) const { return m_index == other.m_index; }
  constexpr bool operator!=(BitFieldArrayIterator other) const { return m_index != other.m_index; }

private:
  constexpr BitFieldArrayIterator(Array* array, size_t index) : m_array(array), m_index(index) {}

  Array* m_array = nullptr;
  size_t m_index = 0;
};

// Input iterator over read-only elements.
template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
class BitFieldArrayConstIterator
{
  using Array = BitFieldArray<position, bits, size, T, S>;
  friend struct BitFieldArray<position, bits, size, T, S>;

public:
  using iterator_category = std::input_iterator_tag;
  using value_type = T;
  using difference_type = ptrdiff_t;
  using pointer = void;
  using reference = BitFieldArrayConstRef<position, bits, size, T, S>;

  constexpr BitFieldArrayConstIterator() = default;
  constexpr BitFieldArrayConstIterator(const BitFieldArrayConstIterator&) = default;
  constexpr BitFieldArrayConstIterator(BitFieldArrayConstIterator&&) = default;
  BitFieldArrayConstIterator& operator=(const BitFieldArrayConstIterator&) = default;
  BitFieldArrayConstIterator& operator=(BitFieldArrayConstIterator&&) = default;

  BitFieldArrayConstIterator& operator++()
  {
    ++m_index;
    return *this;
  }
  BitFieldArrayConstIterator operator++(int)
  {
    BitFieldArrayConstIterator before = *this;
    ++m_index;
    return before;
  }

  constexpr reference operator*() const { return reference(m_array, m_index); }
  constexpr bool operator==(BitFieldArrayConstIterator other) const
  {
    return m_index == other.m_index;
  }
  constexpr bool operator!=(BitFieldArrayConstIterator other) const
  {
    return m_index != other.m_index;
  }

private:
  constexpr BitFieldArrayConstIterator(const Array* array, size_t index)
      : m_array(array), m_index(index)
  {
  }

  const Array* m_array = nullptr;
  size_t m_index = 0;
};

template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
struct fmt::formatter<BitFieldArrayRef<position, bits, size, T, S>>
{
  fmt::formatter<T> m_formatter;
  constexpr auto parse(format_parse_context& ctx) { return m_formatter.parse(ctx); }
  template <typename FormatContext>
  auto format(const BitFieldArrayRef<position, bits, size, T, S>& ref, FormatContext& ctx) const
  {
    return m_formatter.format(ref.Value(), ctx);
  }
};

template <std::size_t position, std::size_t bits, std::size_t size, typename T, typename S>
struct fmt::formatter<BitFieldArrayConstRef<position, bits, size, T, S>>
{
  fmt::formatter<T> m_formatter;
  constexpr auto parse(format_parse_context& ctx) { return m_formatter.parse(ctx); }
  template <typename FormatContext>
  auto format(const BitFieldArrayConstRef<position, bits, size, T, S>& ref,
              FormatContext& ctx) const
  {
    return m_formatter.format(ref.Value(), ctx);
  }
};
