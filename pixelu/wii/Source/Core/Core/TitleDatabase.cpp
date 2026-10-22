// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/TitleDatabase.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "DiscIO/Enums.h"

namespace Core
{
namespace
{
const std::string NO_NAME;

// GameTDB IDs are at least four characters (channels) and usually six (discs).
constexpr std::size_t SHORTEST_ID = 4;

std::unordered_map<std::string, std::string> ReadNameFile(const std::string& path)
{
  std::unordered_map<std::string, std::string> names;

  std::ifstream stream;
  File::OpenFStream(stream, path, std::ios::in);

  for (std::string line; std::getline(stream, line);)
  {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos)
      continue;

    const std::string_view text{line};
    const std::string_view id = StripWhitespace(text.substr(0, separator));
    if (id.size() < SHORTEST_ID)
      continue;

    // emplace keeps the first entry when an ID is listed twice.
    names.emplace(id, StripWhitespace(text.substr(separator + 1)));
  }
  return names;
}

// Finds `id` in `names`, returning nullptr when it is absent.
const std::string* Find(const std::unordered_map<std::string, std::string>& names,
                        const std::string& id)
{
  const auto it = names.find(id);
  return it == names.end() ? nullptr : &it->second;
}
}  // namespace

TitleDatabase::TitleDatabase()
{
  const std::string& load_dir = File::GetUserPath(D_LOAD_IDX);
  m_user_names = ReadNameFile(load_dir + "wiitdb.txt");
  if (m_user_names.empty())
    m_user_names = ReadNameFile(load_dir + "titles.txt");

  RegisterLanguage(DiscIO::Language::Japanese, "ja");
  RegisterLanguage(DiscIO::Language::English, "en");
  RegisterLanguage(DiscIO::Language::German, "de");
  RegisterLanguage(DiscIO::Language::French, "fr");
  RegisterLanguage(DiscIO::Language::Spanish, "es");
  RegisterLanguage(DiscIO::Language::Italian, "it");
  RegisterLanguage(DiscIO::Language::Dutch, "nl");
  RegisterLanguage(DiscIO::Language::SimplifiedChinese, "zh_CN");
  RegisterLanguage(DiscIO::Language::TraditionalChinese, "zh_TW");
  RegisterLanguage(DiscIO::Language::Korean, "ko");
  // An unknown language has no bundled names of its own (it still falls back to English).
  m_bundled[DiscIO::Language::Unknown].wii = [] { return NameMap(); };

  // Software that GameTDB does not list but users will often see.
  // i18n: "Wii Menu" (or System Menu) refers to the Wii's main menu,
  // which is (usually) the first thing users see when a Wii console starts.
  m_fallback_names.emplace("0000000100000002", Common::GetStringT("Wii Menu"));
  m_fallback_names.emplace("HAXX", "The Homebrew Channel");
  m_fallback_names.emplace("00010001af1bf516", "The Homebrew Channel");
}

TitleDatabase::~TitleDatabase() = default;

void TitleDatabase::RegisterLanguage(DiscIO::Language language, const std::string& suffix)
{
  LanguageTables& tables = m_bundled[language];
  tables.wii = [suffix] {
    return ReadNameFile(File::GetSysDirectory() + "wiitdb-" + suffix + ".txt");
  };
  tables.triforce = [suffix] {
    return ReadNameFile(File::GetSysDirectory() + "triforcetdb-" + suffix + ".txt");
  };
}

const TitleDatabase::NameMap& TitleDatabase::BundledNames(DiscIO::Language language) const
{
  // at() throws for a language that was never registered, as callers expect.
  return *m_bundled.at(language).wii;
}

const std::string& TitleDatabase::GetTitleName(const std::string& gametdb_id,
                                               DiscIO::Language language) const
{
  if (const std::string* user_name = Find(m_user_names, gametdb_id))
    return *user_name;

  if (!Config::Get(Config::MAIN_USE_BUILT_IN_TITLE_DATABASE))
    return NO_NAME;

  if (const std::string* name = Find(BundledNames(language), gametdb_id))
    return *name;

  if (language != DiscIO::Language::English)
  {
    if (const std::string* english_name =
            Find(BundledNames(DiscIO::Language::English), gametdb_id))
    {
      return *english_name;
    }
  }

  if (const std::string* fallback = Find(m_fallback_names, gametdb_id))
    return *fallback;

  return NO_NAME;
}

const std::string& TitleDatabase::GetChannelName(u64 title_id, DiscIO::Language language) const
{
  // The low word of a channel's title ID is its four-character code.
  std::string code(4, '\0');
  for (std::size_t i = 0; i < code.size(); ++i)
    code[i] = static_cast<char>((title_id >> (24 - 8 * i)) & 0xff);
  return GetTitleName(code, language);
}

std::string TitleDatabase::Describe(const std::string& gametdb_id, DiscIO::Language language) const
{
  const std::string& name = GetTitleName(gametdb_id, language);
  return name.empty() ? gametdb_id : fmt::format("{} ({})", name, gametdb_id);
}
}  // namespace Core
