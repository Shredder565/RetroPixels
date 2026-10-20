// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/GeckoCodeConfig.h"

#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Common/HttpRequest.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/CheatCodes.h"

namespace Gecko
{
namespace
{
// A code title is "Name [Creator]"; the creator part is optional. The name is trimmed, the creator
// is kept as written (up to the closing bracket, or the end of the line).
void ParseTitle(std::string_view title, GeckoCode* code)
{
  const std::size_t open = title.find('[');
  code->name = StripWhitespace(title.substr(0, open));
  if (open == std::string_view::npos)
  {
    code->creator.clear();
    return;
  }
  const std::string_view after = title.substr(open + 1);
  code->creator = after.substr(0, after.find(']'));
}

std::string Title(const GeckoCode& code)
{
  std::string title = "$" + code.name;
  if (!code.creator.empty())
    title += " [" + code.creator + "]";
  return title;
}

// A line of a downloaded list is a code line when its first two words are 8 characters each.
// (Notes can start with hex digits too, so the length is what tells them apart.) The values are
// read like a C++ stream reads hex numbers.
std::optional<GeckoCode::Code> ParseDownloadedCodeLine(const std::string& line)
{
  std::istringstream words(line);
  // Splitting on spaces must not depend on the user's locale.
  words.imbue(std::locale::classic());
  std::string first, second;
  words >> first >> second;
  if (first.size() != 8 || second.size() != 8)
    return std::nullopt;

  std::istringstream numbers(line);
  numbers.imbue(std::locale::classic());
  GeckoCode::Code code;
  code.original_line = line;
  numbers >> std::hex >> code.address >> code.data;
  return code;
}

// The downloaded list: three header lines, then codes separated by blank lines. Each code is a
// title line, code lines, then note lines (once a note starts, the rest of the block is notes).
std::vector<GeckoCode> ParseDownloadedList(const std::string& text)
{
  enum class Expect
  {
    Title,
    CodeLines,
    Notes,
  };

  std::vector<GeckoCode> result;
  std::istringstream input(text);
  std::string raw;
  for (int header = 0; header < 3; ++header)
    std::getline(input, raw);

  GeckoCode current;
  Expect expect = Expect::Title;
  auto finish_code = [&] {
    if (!current.codes.empty())
      result.push_back(current);
    current = GeckoCode();
    expect = Expect::Title;
  };

  // Only newline-terminated lines are read: a last line without a newline is ignored.
  while (std::getline(input, raw) && !input.eof())
  {
    // Also drops the '\r' of Windows line endings.
    const std::string line(StripWhitespace(raw));
    if (line.empty())
    {
      finish_code();
      continue;
    }

    switch (expect)
    {
    case Expect::Title:
      ParseTitle(line, &current);
      current.user_defined = true;
      expect = Expect::CodeLines;
      break;
    case Expect::CodeLines:
      if (std::optional<GeckoCode::Code> code = ParseDownloadedCodeLine(line))
      {
        current.codes.push_back(std::move(*code));
        break;
      }
      expect = Expect::Notes;
      current.notes.push_back(line);
      break;
    case Expect::Notes:
      current.notes.push_back(line);
      break;
    }
  }

  if (!current.codes.empty())
    result.push_back(current);
  return result;
}

// Reads one ini's [Gecko] section and appends its codes. Lines:
//   $Name [Creator]   starts a code (disabled)
//   +$Name [Creator]  starts a code (enabled; the two leading characters are skipped)
//   *text             a note
//   anything else     a code line (kept as text if it doesn't parse)
// Codes without a name are dropped.
void ReadSection(const Common::IniFile& ini, bool user_defined, std::vector<GeckoCode>* codes)
{
  std::vector<std::string> lines;
  ini.GetLines("Gecko", &lines, false);

  std::optional<GeckoCode> current;
  auto finish_code = [&] {
    if (current && !current->name.empty())
      codes->push_back(std::move(*current));
    current.reset();
  };

  for (const std::string& line : lines)
  {
    if (line.empty() || line[0] == '#')
      continue;

    if (line[0] == '$' || line[0] == '+')
    {
      finish_code();
      current.emplace();
      current->enabled = line[0] == '+';
      current->user_defined = user_defined;
      const std::size_t skip = current->enabled ? 2 : 1;
      if (skip <= line.size())
        ParseTitle(std::string_view(line).substr(skip), &*current);
      continue;
    }

    // Lines before the first title belong to a nameless code, which is dropped.
    if (!current)
      current.emplace();

    if (line[0] == '*')
    {
      current->notes.push_back(line.substr(1));
      continue;
    }

    if (std::optional<GeckoCode::Code> code = DeserializeLine(line))
    {
      current->codes.push_back(std::move(*code));
    }
    else
    {
      // TODO: Gecko option lines aren't supported; they're kept so they are saved back.
      GeckoCode::Code unparsed;
      unparsed.original_line = line;
      current->codes.push_back(std::move(unparsed));
    }
  }
  finish_code();
}
}  // namespace

std::expected<std::vector<GeckoCode>, int> DownloadCodes(std::string_view gametdb_id)
{
  // A mirror of the old geckocodes.org database.
  const std::string url = "https://codes.rc24.xyz/txt.php?txt=" + std::string(gametdb_id);

  Common::HttpRequest request;
  // The server answers with one redirect to the same address.
  request.FollowRedirects(1);

  const Common::HttpRequest::Response response = request.Get(url);
  if (!response)
    return std::unexpected{request.GetLastResponseCode()};

  return ParseDownloadedList(std::string(response->begin(), response->end()));
}

std::vector<GeckoCode> LoadCodes(const Common::IniFile& globalIni, const Common::IniFile& localIni)
{
  std::vector<GeckoCode> codes;

  ReadSection(globalIni, false, &codes);
  ReadEnabledAndDisabled(globalIni, "Gecko", &codes);
  // What the global ini says is the default the local ini's lists are saved against.
  for (GeckoCode& code : codes)
    code.default_enabled = code.enabled;

  ReadSection(localIni, true, &codes);
  ReadEnabledAndDisabled(localIni, "Gecko", &codes);

  return codes;
}

void SaveCodes(Common::IniFile& inifile, std::span<const GeckoCode> gcodes)
{
  std::vector<std::string> code_lines;
  std::vector<std::string> turned_on;
  std::vector<std::string> turned_off;

  for (const GeckoCode& code : gcodes)
  {
    // Only changes from the default are stored.
    if (code.enabled != code.default_enabled)
      (code.enabled ? turned_on : turned_off).push_back("$" + code.name);

    // Codes from the global ini are not copied into the user's ini.
    if (!code.user_defined)
      continue;

    code_lines.push_back(Title(code));
    for (const GeckoCode::Code& line : code.codes)
      code_lines.push_back(line.original_line);
    for (const std::string& note : code.notes)
      code_lines.push_back("*" + note);
  }

  inifile.SetLines("Gecko", std::move(code_lines));
  inifile.SetLines("Gecko_Enabled", std::move(turned_on));
  inifile.SetLines("Gecko_Disabled", std::move(turned_off));
}

std::optional<GeckoCode::Code> DeserializeLine(const std::string& line)
{
  const std::vector<std::string> words = SplitString(line, ' ');
  if (words.size() < 2)
    return std::nullopt;

  GeckoCode::Code code;
  code.original_line = line;
  if (!TryParse(words[0], &code.address, 16) || !TryParse(words[1], &code.data, 16))
    return std::nullopt;
  return code;
}

}  // namespace Gecko
