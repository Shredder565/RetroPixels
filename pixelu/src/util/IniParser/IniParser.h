// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <vector>
#include <span>
#include <string>
#include <optional>

// Reader for the INI dialect of graphic pack rules files:
//  - [name] starts a section; '#' or ';' outside double quotes start a comment
//  - name = value pairs; a value in double quotes has the quotes removed
//  - option names are matched case-insensitively (ASCII)
//  - a UTF-8 byte order mark is skipped; malformed lines are ignored
// Line numbers count non-empty lines only.
// Iterate with NextSection() and query the current section.
class IniParser
{
public:
	IniParser(std::span<char> iniContents, std::string_view name = {});
	IniParser(std::span<unsigned char> iniContents, std::string_view name = {}) : IniParser(std::span<char>((char*)iniContents.data(), iniContents.size()), name) {}

	IniParser(const IniParser&) = delete;
	IniParser& operator=(const IniParser&) = delete;

	// advances to the next (on the first call: the first) section; false when there are no more
	bool NextSection();
	std::string_view GetCurrentSectionName();
	size_t GetCurrentSectionLineNumber();
	// value of the first option with this name in the current section
	std::optional<std::string_view> FindOption(std::string_view optionName);
	std::span<std::pair<std::string_view, std::string_view>> GetAllOptions();

private:
	struct Section
	{
		std::string_view name;
		size_t lineNumber;
		std::vector<std::pair<std::string_view, std::string_view>> options;
	};

	void parseAll();
	void parseLine(std::string_view line, size_t lineNumber);
	Section* current();

	std::vector<char> m_text; // views point into this
	std::string m_name;
	std::vector<Section> m_sections;
	size_t m_cursor{ 0 }; // 0 = before the first section, else index + 1
};
