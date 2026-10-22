// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/Lazy.h"

namespace DiscIO
{
enum class Language;
}

namespace Core
{
// Looks up user-facing game names from GameTDB-style text files
// ("ID = Name" per line): the user's own Load/wiitdb.txt (or titles.txt), then
// the bundled Sys/wiitdb-<lang>.txt for the requested language, then English,
// then a few built-in names.
class TitleDatabase final
{
public:
  TitleDatabase();
  ~TitleDatabase();

  // The name for a GameTDB ID, or an empty string if none is known.
  const std::string& GetTitleName(const std::string& gametdb_id, DiscIO::Language language) const;

  // The same lookup for a channel, keyed by the low four characters of its title ID.
  const std::string& GetChannelName(u64 title_id, DiscIO::Language language) const;

  // "Name (ID)" when a name is known, otherwise just the ID.
  std::string Describe(const std::string& gametdb_id, DiscIO::Language language) const;

private:
  using NameMap = std::unordered_map<std::string, std::string>;

  // The bundled databases of one language, each read from disk on first use.
  struct LanguageTables
  {
    Common::Lazy<NameMap> wii;
    Common::Lazy<NameMap> triforce;
  };

  void RegisterLanguage(DiscIO::Language language, const std::string& suffix);
  const NameMap& BundledNames(DiscIO::Language language) const;

  std::unordered_map<DiscIO::Language, LanguageTables> m_bundled;
  NameMap m_user_names;
  NameMap m_fallback_names;
};
}  // namespace Core
