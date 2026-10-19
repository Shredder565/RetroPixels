// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <type_traits>
#include <utility>
#include <climits>
#include <cstring>
#include <bit>

namespace Latte
{
	class LATTEREG;
};

// Byte order reversal for any unsigned integer type, usable in constant
// expressions: byte N of the input moves to byte (size - 1 - N).
template<class T, std::size_t... N>
constexpr T bswap_impl(T i, std::index_sequence<N...>)
{
	constexpr T byteMask = (T)(uint8_t)0xFF;
	return (T)((((i >> (N * CHAR_BIT)) & byteMask) << ((sizeof(T) - 1 - N) * CHAR_BIT)) | ...);
}

template<class T, class U = std::make_unsigned_t<T>>
constexpr T bswap(T i)
{
	return (T)bswap_impl<U>((U)i, std::make_index_sequence<sizeof(T)>{});
}

// Reverses the byte order of integers, floating point values, enums and
// Latte register structs (which are 32 bit).
template<typename T>
constexpr T SwapEndian(T value)
{
	if constexpr (std::is_integral_v<T>)
	{
#ifdef _MSC_VER
		if constexpr (sizeof(T) == sizeof(uint32_t))
			return (T)_byteswap_ulong(value);
#endif
		return (T)bswap((std::make_unsigned_t<T>)value);
	}
	else if constexpr (std::is_floating_point_v<T>)
	{
		static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t), "unsupported floating point size");
		using Bits = std::conditional_t<sizeof(T) == sizeof(uint32_t), uint32_t, uint64_t>;
		return std::bit_cast<T>(bswap<Bits>(std::bit_cast<Bits>(value)));
	}
	else if constexpr (std::is_enum_v<T>)
	{
		return (T)SwapEndian((std::underlying_type_t<T>)value);
	}
	else if constexpr (std::is_base_of_v<Latte::LATTEREG, T>)
	{
		static_assert(sizeof(T) == sizeof(uint32_t));
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		bits = bswap<uint32_t>(bits);
		T result;
		std::memcpy(&result, &bits, sizeof(bits));
		return result;
	}
	else
	{
		static_assert(std::is_integral_v<T>, "unsupported betype specialization!");
		return value;
	}
}

// host value <-> big-endian representation (the host is little-endian)
template<typename T>
constexpr T _BE(T value)
{
	return SwapEndian(value);
}

// host value <-> little-endian representation (no-op on little-endian hosts)
template<typename T>
constexpr T _LE(T value)
{
	return value;
}

// A T stored in big-endian byte order, e.g. a field of a guest structure.
// Reads and writes convert to and from the host order. Trivially copyable
// and default-constructible without initialization, so it can be used in
// structures that are mapped onto guest memory.
template<typename T>
class betype
{
public:
	constexpr betype() = default;
	constexpr betype(T value) : m_value(SwapEndian(value)) {}
	constexpr betype(const betype& value) = default;

	template<typename U>
	constexpr betype(const betype<U>& value) : betype((T)value.value()) {}

	// wraps a value that is already in big-endian order
	static betype from_bevalue(T value)
	{
		betype r;
		r.m_value = value;
		return r;
	}

	constexpr T value() const { return SwapEndian<T>(m_value); } // host order
	constexpr T bevalue() const { return m_value; }              // stored order
	constexpr operator T() const { return value(); }

	betype<T>& operator+=(const betype<T>& v) { return set(T(value() + v.value())); }
	betype<T>& operator+=(const T& v) requires std::integral<T> { return set(T(value() + v)); }
	betype<T>& operator-=(const betype<T>& v) { return set(T(value() - v.value())); }
	betype<T>& operator*=(const betype<T>& v) { return set(T(value() * v.value())); }
	betype<T>& operator/=(const betype<T>& v) { return set(T(value() / v.value())); }

	// bitwise operations work on the stored bytes directly
	betype<T>& operator&=(const betype<T>& v) requires (requires (T& x, const T& y) { x &= y; })
	{
		m_value &= v.m_value;
		return *this;
	}

	betype<T>& operator|=(const betype<T>& v) requires (requires (T& x, const T& y) { x |= y; })
	{
		m_value |= v.m_value;
		return *this;
	}

	betype<T> operator|(const betype<T>& v) const requires (requires (T& x, const T& y) { x | y; })
	{
		return from_bevalue(T(m_value | v.m_value));
	}

	betype<T> operator|(const T& v) const requires (requires (T& x, const T& y) { x | y; })
	{
		return from_bevalue(T(m_value | SwapEndian(v)));
	}

	betype<T>& operator^=(const betype<T>& v) requires std::integral<T>
	{
		m_value ^= v.m_value;
		return *this;
	}

	betype<T>& operator>>=(std::size_t idx) requires std::integral<T> { return set(T(value() >> idx)); }
	betype<T>& operator<<=(std::size_t idx) requires std::integral<T> { return set(T(value() << idx)); }

	betype<T> operator~() const requires std::integral<T>
	{
		return from_bevalue(T(~m_value));
	}

	betype<T>& operator++() requires std::integral<T> { return set(T(value() + 1)); }
	betype<T>& operator--() requires std::integral<T> { return set(T(value() - 1)); }

	betype<T> operator++(int) requires std::integral<T>
	{
		const betype<T> before(*this);
		set(T(value() + 1));
		return before;
	}

	betype<T> operator--(int) requires std::integral<T>
	{
		const betype<T> before(*this);
		set(T(value() - 1));
		return before;
	}

private:
	betype<T>& set(T hostValue)
	{
		m_value = SwapEndian(hostValue);
		return *this;
	}

	T m_value; // deliberately uninitialized by the default constructor
};

using uint64be = betype<uint64>;
using uint32be = betype<uint32>;
using uint16be = betype<uint16>;
using uint8be = betype<uint8>;

using sint64be = betype<sint64>;
using sint32be = betype<sint32>;
using sint16be = betype<sint16>;
using sint8be = betype<sint8>;

using float32be = betype<float>;
using float64be = betype<double>;

static_assert(sizeof(betype<uint64>) == sizeof(uint64));
static_assert(sizeof(betype<uint32>) == sizeof(uint32));
static_assert(sizeof(betype<uint16>) == sizeof(uint16));
static_assert(sizeof(betype<uint8>) == sizeof(uint8));
static_assert(sizeof(betype<float>) == sizeof(float));
static_assert(sizeof(betype<double>) == sizeof(double));

static_assert(std::is_trivially_copyable_v<uint32be>);
static_assert(std::is_trivially_constructible_v<uint32be>);
static_assert(std::is_copy_constructible_v<uint32be>);
static_assert(std::is_move_constructible_v<uint32be>);
static_assert(std::is_copy_assignable_v<uint32be>);
static_assert(std::is_move_assignable_v<uint32be>);
