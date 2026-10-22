// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "betype.h"
#include "util/helpers/StringHelpers.h"

// Fixed-size, zero-terminated 8-bit string as stored in guest structures.
template<size_t N>
class CafeString
{
public:
	constexpr static size_t Size() { return N; }

	// true if sv plus its terminator fits
	bool CanHoldString(std::string_view sv) const { return sv.size() < N; }

	// copies sv; a string that does not fit is truncated and false is returned
	bool assign(std::string_view sv)
	{
		const bool fits = CanHoldString(sv);
		const size_t len = fits ? sv.size() : N - 1;
		if (len)
			memcpy(data, sv.data(), len);
		data[len] = '\0';
		return fits;
	}

	void Copy(CafeString<N>& other) { memcpy(data, other.data, N); }

	bool empty() const { return data[0] == '\0'; }
	const char* c_str() const { return (const char*)data; }
	void ClearAllBytes() { memset(data, 0, N); }

	// compares up to the terminator; bytes after it are ignored
	auto operator<=>(const CafeString<N>& other) const
	{
		const size_t i = firstDifference(other);
		if (i == N)
			return std::strong_ordering::equal;
		return data[i] <=> other.data[i];
	}

	bool operator==(const CafeString<N>& other) const { return firstDifference(other) == N; }

	uint8be data[N];

private:
	// index of the first differing byte, or N if equal up to the terminator
	size_t firstDifference(const CafeString<N>& other) const
	{
		for (size_t i = 0; i < N; i++)
		{
			const uint8 a = data[i], b = other.data[i];
			if (a != b)
				return i;
			if (a == 0)
				break;
		}
		return N;
	}
};

// Fixed-size, zero-terminated 16-bit big-endian string (guest wchar_t).
template<size_t N>
class CafeWideString
{
public:
	// copies a zero-terminated string; on overflow the copy is truncated to N-1 characters and false is returned
	bool assign(const uint16be* input)
	{
		size_t len = 0;
		for (; input[len] != 0; len++)
		{
			if (len == N - 1)
			{
				data[N - 1] = 0;
				return false;
			}
			data[len] = input[len];
		}
		data[len] = 0;
		return true;
	}

	bool assignFromUTF8(std::string_view sv)
	{
		const std::vector<uint16be> converted = StringHelpers::FromUtf8(sv);
		const bool fits = converted.size() <= N - 1;
		const size_t len = fits ? converted.size() : N - 1;
		if (len)
			memcpy(data, converted.data(), len * sizeof(uint16be));
		data[len] = 0;
		return fits;
	}

	uint16be data[N];
};

namespace CafeStringHelpers
{
	// number of characters before the terminator, at most maxLength
	static uint32 Length(const uint16be* input, uint32 maxLength)
	{
		uint32 len = 0;
		while (len < maxLength && input[len] != 0)
			len++;
		return len;
	}
};
