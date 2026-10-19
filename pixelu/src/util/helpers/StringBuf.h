// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <cstdlib>
#include <cstring>

// Growable, null-terminated text buffer used to assemble shader source.
// The constructor argument is only the initial capacity; every add grows the
// buffer when needed.
class StringBuf
{
public:
	StringBuf(uint32 bufferSize)
	{
		m_capacity = bufferSize;
		m_data = (char*)malloc((size_t)m_capacity + kSlack);
		m_data[0] = '\0';
	}

	~StringBuf()
	{
		free(m_data);
	}

	StringBuf(const StringBuf&) = delete;
	StringBuf& operator=(const StringBuf&) = delete;

	// fmt-style formatting appended to the buffer (format is checked at runtime)
	template<typename TFmt, typename... TArgs>
	void addFmt(const TFmt& format, TArgs&&... args)
	{
		const std::string_view fmtText(format);
		const fmt::string_view fmtView(fmtText.data(), fmtText.size());
		const auto fmtArgs = fmt::make_format_args(args...);
		size_t room = m_capacity - m_length;
		auto result = fmt::vformat_to_n(m_data + m_length, room, fmtView, fmtArgs);
		if (result.size > room)
		{
			// did not fit, grow and format again
			ensureRoom(result.size);
			result = fmt::vformat_to_n(m_data + m_length, m_capacity - m_length, fmtView, fmtArgs);
		}
		m_length += (uint32)result.size;
		m_data[m_length] = '\0';
	}

	void add(const char* text)
	{
		add(std::string_view(text));
	}

	void add(std::string_view text)
	{
		ensureRoom(text.size());
		std::memcpy(m_data + m_length, text.data(), text.size());
		m_length += (uint32)text.size();
		m_data[m_length] = '\0';
	}

	void reset()
	{
		m_length = 0;
	}

	uint32 getLen() const
	{
		return m_length;
	}

	const char* c_str() const
	{
		m_data[m_length] = '\0';
		return m_data;
	}

	// releases unused capacity
	void shrink_to_fit()
	{
		m_capacity = m_length;
		m_data = (char*)realloc(m_data, (size_t)m_capacity + kSlack);
	}

private:
	// a few bytes past the capacity, so the terminator always fits
	static constexpr size_t kSlack = 4;

	void ensureRoom(size_t extra)
	{
		const size_t needed = (size_t)m_length + extra + 1;
		if (needed < m_capacity)
			return;
		size_t newCapacity = std::max<size_t>(needed + 64, (size_t)m_capacity + m_capacity / 2);
		cemu_assert(newCapacity < 0xFFFFFFF0ull);
		m_capacity = (uint32)newCapacity;
		m_data = (char*)realloc(m_data, (size_t)m_capacity + kSlack);
	}

	char* m_data;
	uint32 m_length{ 0 };
	uint32 m_capacity;
};
