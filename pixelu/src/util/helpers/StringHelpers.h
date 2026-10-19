// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "boost/nowide/convert.hpp"
#include <charconv>

namespace StringHelpers
{
	// Wii U wide strings are UTF-16 big endian. The conversions go through the
	// host wchar_t, like the rest of the code base (boost::nowide).

	// big-endian UTF-16 to UTF-8, stops at a null character or after maxLength characters
	static std::string ToUtf8(const uint16be* ptr, size_t maxLength)
	{
		std::wstring wide;
		for (size_t i = 0; i < maxLength; i++)
		{
			const uint16 c = ptr[i];
			if (c == 0)
				break;
			wide.push_back((wchar_t)c);
		}
		return boost::nowide::narrow(wide);
	}

	static std::string ToUtf8(std::span<uint16be> input)
	{
		return ToUtf8(input.data(), input.size());
	}

	// host-order UTF-16 to UTF-8
	static std::string ToUtf8FromLE(const uint16* ptr, size_t maxLength)
	{
		std::wstring wide;
		for (size_t i = 0; i < maxLength; i++)
		{
			if (ptr[i] == 0)
				break;
			wide.push_back((wchar_t)ptr[i]);
		}
		return boost::nowide::narrow(wide);
	}

	// UTF-8 to big-endian UTF-16 (no terminator is added)
	static std::vector<uint16be> FromUtf8(std::string_view str)
	{
		const std::wstring wide = boost::nowide::widen(str.data(), str.size());
		std::vector<uint16be> result;
		result.reserve(wide.size());
		for (wchar_t c : wide)
			result.emplace_back((uint16)c);
		return result;
	}

	namespace detail
	{
		// "0x"/"0X" selects hex, otherwise decimal. Parsing stops at the first
		// character that does not belong to the number; an unparsable or
		// out-of-range number gives defaultValue.
		template<typename T>
		T ParseIntegerPrefixed(std::string_view input, T defaultValue)
		{
			int base = 10;
			if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
			{
				base = 16;
				input.remove_prefix(2);
			}
			T value = defaultValue;
			const auto res = std::from_chars(input.data(), input.data() + input.size(), value, base);
			if (res.ec == std::errc::invalid_argument || res.ec == std::errc::result_out_of_range)
				return defaultValue;
			return value;
		}
	}

	static sint32 ToInt(const std::string_view& input, sint32 defaultValue = 0)
	{
		return detail::ParseIntegerPrefixed<sint32>(input, defaultValue);
	}

	static sint64 ToInt64(const std::string_view& input, sint64 defaultValue = 0)
	{
		return detail::ParseIntegerPrefixed<sint64>(input, defaultValue);
	}

	// Parses pairs of hex digits into bytes until a non-hex character, the end
	// of the input or maxOutputLength bytes. Returns the number of bytes written.
	static size_t ParseHexString(std::string_view input, uint8* output, size_t maxOutputLength)
	{
		auto nibble = [](char c) -> int {
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			return -1;
		};
		size_t written = 0;
		for (size_t i = 0; i + 1 < input.size() && written < maxOutputLength; i += 2)
		{
			const int hi = nibble(input[i]);
			if (hi < 0)
				break;
			const int lo = nibble(input[i + 1]);
			if (lo < 0)
				break;
			output[written++] = (uint8)((hi << 4) | lo);
		}
		return written;
	}

	// Iterates the lines of a text. Lines end at '\n'; trailing '\r' characters
	// are removed. A final '\n' does not start another (empty) line.
	class StringLineIterator
	{
	public:
		class Iterator
		{
		public:
			using iterator_category = std::input_iterator_tag;
			using value_type = std::string_view;
			using difference_type = std::ptrdiff_t;
			using pointer = const std::string_view*;
			using reference = const std::string_view&;

			Iterator(std::string_view text, sint32 pos) : m_text(text), m_pos(pos)
			{
				loadLine();
			}

			reference operator*() const { return m_line; }
			pointer operator->() const { return &m_line; }

			Iterator& operator++()
			{
				m_pos = m_next;
				loadLine();
				return *this;
			}

			friend bool operator==(const Iterator& a, const Iterator& b)
			{
				return a.m_text.data() == b.m_text.data() && a.m_pos == b.m_pos;
			}

			friend bool operator!=(const Iterator& a, const Iterator& b)
			{
				return !(a == b);
			}

		private:
			void loadLine()
			{
				if (m_pos < 0 || (size_t)m_pos >= m_text.size())
				{
					m_pos = -1; // end
					m_next = -1;
					m_line = {};
					return;
				}
				const size_t newline = m_text.find('\n', (size_t)m_pos);
				if (newline == std::string_view::npos)
				{
					m_line = m_text.substr((size_t)m_pos);
					m_next = -1;
				}
				else
				{
					m_line = m_text.substr((size_t)m_pos, newline - (size_t)m_pos);
					m_next = (sint32)(newline + 1);
				}
				while (!m_line.empty() && m_line.back() == '\r')
					m_line.remove_suffix(1);
			}

			std::string_view m_text;
			sint32 m_pos;
			sint32 m_next{ -1 };
			std::string_view m_line;
		};

		StringLineIterator(std::string_view text) : m_text(text) {}
		StringLineIterator(std::span<uint8> text) : m_text((const char*)text.data(), text.size()) {}

		Iterator begin() const { return Iterator{ m_text, 0 }; }
		Iterator end() const { return Iterator{ m_text, -1 }; }

	private:
		std::string_view m_text;
	};
};
