// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Cursor over a (not necessarily null-terminated) string for simple
// hand-written parsers. Whitespace means space and tab. Functions that fail
// leave the cursor where it was.
class StringTokenParser
{
public:
	StringTokenParser() : m_str(nullptr), m_len(0) {}
	StringTokenParser(const char* input, sint32 inputLen) : m_str(input), m_len(inputLen) {}
	StringTokenParser(std::string_view str) : m_str(str.data()), m_len((sint32)str.size()) {}

	void skipWhitespaces()
	{
		sint32 n = 0;
		m_str = afterWhitespace(m_str, m_len, n);
		m_len = n;
	}

	// drops whitespace at the end of the remaining text
	void trimWhitespaces()
	{
		while (m_len > 0 && isBlank(m_str[m_len - 1]))
			m_len--;
	}

	bool isEndOfString()
	{
		return m_len <= 0;
	}

	// Moves the cursor onto the next occurrence of c and returns how many
	// characters were skipped, or returns -1 (cursor unchanged) if there is none.
	sint32 skipToCharacter(const char c)
	{
		for (sint32 i = 0; i < m_len; i++)
		{
			if (m_str[i] == c)
			{
				m_str += i;
				m_len -= i;
				return i;
			}
		}
		return -1;
	}

	// Case-insensitive match of word after optional whitespace. Only checks
	// that the text starts with word; there is no word boundary check.
	bool matchWordI(const char* word)
	{
		sint32 len = 0;
		const char* p = afterWhitespace(m_str, m_len, len);
		sint32 i = 0;
		for (; word[i] != '\0'; i++)
		{
			if (i >= len || upper(p[i]) != upper(word[i]))
				return false;
		}
		m_str = p + i;
		m_len = len - i;
		return true;
	}

	bool compareCharacter(sint32 relativeIndex, const char c)
	{
		return relativeIndex < m_len && m_str[relativeIndex] == c;
	}

	bool compareCharacterI(sint32 relativeIndex, const char c)
	{
		return relativeIndex < m_len && upper(m_str[relativeIndex]) == upper(c);
	}

	void skipCharacters(sint32 count)
	{
		count = std::min(count, m_len);
		m_str += count;
		m_len -= count;
	}

	// Unsigned number after optional whitespace. "0x..." is hex; a number with
	// a leading zero is also read as hex, and a lone "0" is not accepted (both
	// are long-standing behaviour that patch files rely on). No overflow check.
	bool parseU32(uint32& val)
	{
		sint32 len = 0;
		const char* p = afterWhitespace(m_str, m_len, len);
		if (len <= 0)
			return false;
		sint32 pos = 0;
		uint32 radix = 10;
		if (len >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		{
			radix = 16;
			pos = 2;
		}
		else if (p[0] == '0')
		{
			radix = 16;
			pos = 1;
		}
		if (pos >= len)
			return false;
		const sint32 firstDigit = pos;
		uint32 result = 0;
		for (; pos < len; pos++)
		{
			const sint32 digit = digitValue(p[pos], radix);
			if (digit < 0)
				break;
			result = result * radix + (uint32)digit;
		}
		if (pos == firstDigit)
			return false;
		m_str = p + pos;
		m_len = len - pos;
		val = result;
		return true;
	}

	// identifier after optional whitespace: [A-Za-z_][A-Za-z0-9_.]*
	bool parseSymbolName(const char*& symbolStr, sint32& symbolLength)
	{
		sint32 len = 0;
		const char* p = afterWhitespace(m_str, m_len, len);
		if (len <= 0 || !(isLetter(p[0]) || p[0] == '_'))
			return false;
		sint32 n = 1;
		while (n < len && (isLetter(p[n]) || (p[n] >= '0' && p[n] <= '9') || p[n] == '_' || p[n] == '.'))
			n++;
		symbolStr = p;
		symbolLength = n;
		m_str = p + n;
		m_len = len - n;
		return true;
	}

	const char* getCurrentPtr() { return m_str; }
	sint32 getCurrentLen() { return m_len; }

	// the parser state is just the remaining text, so a copy saves and restores it
	void storeParserState(StringTokenParser* bak) { *bak = *this; }
	void restoreParserState(const StringTokenParser* bak) { *this = *bak; }

private:
	static bool isBlank(char c)
	{
		return c == ' ' || c == '\t';
	}

	static bool isLetter(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	static char upper(char c)
	{
		return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
	}

	static sint32 digitValue(char c, uint32 radix)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (radix == 16)
		{
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
		}
		return -1;
	}

	// returns the first non-blank position, remaining length in lenOut
	static const char* afterWhitespace(const char* p, sint32 len, sint32& lenOut)
	{
		while (len > 0 && isBlank(*p))
		{
			p++;
			len--;
		}
		lenOut = len;
		return p;
	}

	const char* m_str;
	sint32 m_len;
};
