// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/IniParser/IniParser.h"

namespace
{
	bool isBlank(char c)
	{
		return c == ' ' || c == '\t';
	}

	std::string_view stripBlanks(std::string_view s)
	{
		while (!s.empty() && isBlank(s.front()))
			s.remove_prefix(1);
		while (!s.empty() && isBlank(s.back()))
			s.remove_suffix(1);
		return s;
	}

	// cuts the line at the first '#' or ';' that is not inside double quotes
	std::string_view stripComment(std::string_view s)
	{
		bool quoted = false;
		for (size_t i = 0; i < s.size(); i++)
		{
			if (s[i] == '"')
				quoted = !quoted;
			else if (!quoted && (s[i] == '#' || s[i] == ';'))
				return s.substr(0, i);
		}
		return s;
	}

	// printable ASCII (code 128 is tolerated), no space and no brackets
	bool isValidOptionName(std::string_view s)
	{
		for (char ch : s)
		{
			const uint8 c = (uint8)ch;
			if (c == '[' || c == ']' || c == ' ' || c < 32 || c > 128)
				return false;
		}
		return true;
	}

	char asciiLower(char c)
	{
		return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
	}

	bool equalsIgnoreCase(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); i++)
		{
			if (asciiLower(a[i]) != asciiLower(b[i]))
				return false;
		}
		return true;
	}
}

IniParser::IniParser(std::span<char> iniContents, std::string_view name) : m_name(name)
{
	static constexpr uint8 kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };
	if (iniContents.size() >= 3 && memcmp(iniContents.data(), kUtf8Bom, 3) == 0)
		iniContents = iniContents.subspan(3);
	m_text.assign(iniContents.begin(), iniContents.end());
	parseAll();
}

void IniParser::parseAll()
{
	const char* const begin = m_text.data();
	const size_t size = m_text.size();
	size_t pos = 0;
	size_t lineNumber = 0;
	while (pos < size)
	{
		if (m_text[pos] == '\r' || m_text[pos] == '\n')
		{
			pos++;
			continue;
		}
		const size_t start = pos;
		while (pos < size && m_text[pos] != '\r' && m_text[pos] != '\n')
			pos++;
		lineNumber++;
		parseLine(std::string_view(begin + start, pos - start), lineNumber);
	}
}

void IniParser::parseLine(std::string_view line, size_t lineNumber)
{
	while (!line.empty() && isBlank(line.front()))
		line.remove_prefix(1);
	line = stripComment(line);
	if (line.empty())
		return;

	if (line.front() == '[')
	{
		// without a closing bracket the whole text, '[' included, is the name
		const size_t close = line.find(']', 1);
		const std::string_view sectionName = (close == std::string_view::npos) ? line : line.substr(1, close - 1);
		m_sections.push_back(Section{ sectionName, lineNumber, {} });
		return;
	}

	const size_t eq = line.find('=');
	if (eq == std::string_view::npos)
		return; // neither a section nor an option
	const std::string_view optName = stripBlanks(line.substr(0, eq));
	std::string_view optValue = stripBlanks(line.substr(eq + 1));
	if (optName.empty() || !isValidOptionName(optName))
		return;
	if (!optValue.empty() && optValue.front() == '"')
	{
		// needs a closing quote that is not the opening one and not directly after it
		if (optValue.size() < 3 || optValue.back() != '"')
			return;
		optValue = optValue.substr(1, optValue.size() - 2);
	}
	if (m_sections.empty())
		return; // options before the first section are ignored
	m_sections.back().options.emplace_back(optName, optValue);
}

IniParser::Section* IniParser::current()
{
	if (m_cursor == 0 || m_cursor > m_sections.size())
		return nullptr;
	return &m_sections[m_cursor - 1];
}

bool IniParser::NextSection()
{
	if (m_cursor != 0 && m_cursor > m_sections.size())
		return false;
	m_cursor++;
	return m_cursor <= m_sections.size();
}

std::string_view IniParser::GetCurrentSectionName()
{
	Section* s = current();
	return s ? s->name : std::string_view("");
}

size_t IniParser::GetCurrentSectionLineNumber()
{
	Section* s = current();
	return s ? s->lineNumber : 0;
}

std::optional<std::string_view> IniParser::FindOption(std::string_view optionName)
{
	Section* s = current();
	if (!s)
		return std::nullopt;
	for (auto& [key, value] : s->options)
	{
		if (equalsIgnoreCase(key, optionName))
			return value;
	}
	return std::nullopt;
}

std::span<std::pair<std::string_view, std::string_view>> IniParser::GetAllOptions()
{
	Section* s = current();
	if (!s)
		return {};
	return s->options;
}
