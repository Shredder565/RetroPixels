// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <type_traits>

// Opt-in bitwise operators for enum types:
//     enum class MyFlags : uint32 { A = 1, B = 2 };
//     ENABLE_BITMASK_OPERATORS(MyFlags);
template<typename TEnum>
struct EnableBitMaskOperators
{
	static const bool enable = false;
};

template<typename TEnum>
concept BitMaskEnum = EnableBitMaskOperators<TEnum>::enable;

namespace enumflags_detail
{
	template<typename TEnum>
	constexpr auto raw(TEnum v)
	{
		return static_cast<std::underlying_type_t<TEnum>>(v);
	}
}

template<BitMaskEnum TEnum>
TEnum operator&(TEnum a, TEnum b)
{
	return TEnum(enumflags_detail::raw(a) & enumflags_detail::raw(b));
}

template<BitMaskEnum TEnum>
TEnum operator|(TEnum a, TEnum b)
{
	return TEnum(enumflags_detail::raw(a) | enumflags_detail::raw(b));
}

template<BitMaskEnum TEnum>
TEnum operator^(TEnum a, TEnum b)
{
	return TEnum(enumflags_detail::raw(a) ^ enumflags_detail::raw(b));
}

template<BitMaskEnum TEnum>
TEnum operator~(TEnum a)
{
	return TEnum(~enumflags_detail::raw(a));
}

template<BitMaskEnum TEnum>
TEnum& operator&=(TEnum& a, TEnum b)
{
	return a = a & b;
}

template<BitMaskEnum TEnum>
TEnum& operator|=(TEnum& a, TEnum b)
{
	return a = a | b;
}

template<BitMaskEnum TEnum>
TEnum& operator^=(TEnum& a, TEnum b)
{
	return a = a ^ b;
}

// comparison against a raw value, e.g. (flags & X) == 0
template<BitMaskEnum TEnum>
constexpr bool operator==(TEnum a, std::underlying_type_t<TEnum> b)
{
	return enumflags_detail::raw(a) == b;
}

template<BitMaskEnum TEnum>
constexpr bool operator!=(TEnum a, std::underlying_type_t<TEnum> b)
{
	return enumflags_detail::raw(a) != b;
}

#define ENABLE_BITMASK_OPERATORS(x) template<> struct EnableBitMaskOperators<x> { static const bool enable = true; };

// Opt-in iteration over a contiguous range of enum values:
//     ENABLE_ENUM_ITERATORS(MyEnum, MyEnum::First, MyEnum::Last);
//     for (MyEnum v = begin(MyEnum{}); v != end(MyEnum{}); ++v) ...
// The trait also provides begin/rbegin (first/last value) and end/rend (one
// past either side) for code that queries the range, such as ConfigValue.
template<typename TEnum>
struct EnableEnumIterators
{
	static const bool enable = false;
};

template<typename TEnum>
concept IterableEnum = EnableEnumIterators<TEnum>::enable;

template<IterableEnum TEnum>
TEnum& operator++(TEnum& v)
{
	v = TEnum(enumflags_detail::raw(v) + 1);
	return v;
}

template<IterableEnum TEnum>
TEnum operator*(TEnum v)
{
	return v;
}

template<IterableEnum TEnum>
TEnum begin(TEnum)
{
	return EnableEnumIterators<TEnum>::begin;
}

template<IterableEnum TEnum>
TEnum rbegin(TEnum)
{
	return EnableEnumIterators<TEnum>::rbegin;
}

template<IterableEnum TEnum>
TEnum end(TEnum)
{
	return EnableEnumIterators<TEnum>::end;
}

template<IterableEnum TEnum>
TEnum rend(TEnum)
{
	return EnableEnumIterators<TEnum>::rend;
}

#define ENABLE_ENUM_ITERATORS(x, first_value, last_value)                                                     \
	template<> struct EnableEnumIterators<x>                                                                  \
	{                                                                                                         \
		static const bool enable = true;                                                                      \
		static const x begin = first_value;                                                                   \
		static const x rbegin = last_value;                                                                   \
		static const x end = static_cast<x>(static_cast<std::underlying_type_t<x>>(last_value) + 1);          \
		static const x rend = static_cast<x>(static_cast<std::underlying_type_t<x>>(first_value) - 1);        \
	};
