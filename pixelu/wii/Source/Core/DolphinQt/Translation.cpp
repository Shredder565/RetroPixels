// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Translation.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <QApplication>
#include <QLocale>
#include <QTranslator>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"

#include "UICommon/UICommon.h"

namespace
{
// A GNU gettext message catalogue (.mo file), as described in the gettext manual. All numbers are
// 32-bit, in the byte order of the machine that wrote the file; only little-endian files (the
// magic number reads correctly on x86) are accepted.
//
//   0   magic 0x950412de
//   4   format revision
//   8   number of strings N
//   12  offset of the table of original strings
//   16  offset of the table of translations
//
// Each table has N entries of (length, offset); the strings are NUL-terminated and the originals
// are sorted, so a lookup is a binary search. A string with a context is stored as
// "context\x04text".
class MessageCatalogue
{
public:
  MessageCatalogue() = default;

  // Loads the file; on any problem the catalogue stays empty.
  explicit MessageCatalogue(const std::string& path)
  {
    File::IOFile file(path, "rb");
    std::vector<char> bytes(file.GetSize());
    file.ReadBytes(bytes.data(), bytes.size());
    if (!file)
    {
      WARN_LOG_FMT(COMMON, "Error reading MO file '{}'", path);
      return;
    }

    if (bytes.size() < HEADER_SIZE)
    {
      ERROR_LOG_FMT(COMMON, "MO file '{}' is too small to be a translation file", path);
      return;
    }

    const u32 magic = Word(bytes, 0);
    if (magic != MAGIC)
    {
      ERROR_LOG_FMT(COMMON, "MO file '{}' has bad magic number {:x}\n", path, magic);
      return;
    }

    // Only the low half of the revision word is looked at; revisions 0 and 1 are understood.
    u16 revision_low;
    std::memcpy(&revision_low, &bytes[4], sizeof(revision_low));
    if (revision_low > 1)
    {
      ERROR_LOG_FMT(COMMON, "MO file '{}' has unsupported version number {}", path,
                    revision_low);
      return;
    }

    const u32 count = Word(bytes, 8);
    const u32 originals = Word(bytes, 12);
    const u32 translations = Word(bytes, 16);
    if (!TableFits(bytes.size(), originals, count) || !TableFits(bytes.size(), translations, count))
    {
      ERROR_LOG_FMT(COMMON, "MO file '{}' has string tables outside the file", path);
      return;
    }

    m_bytes = std::move(bytes);
    m_count = count;
    m_originals_table = originals;
    m_translations_table = translations;
  }

  u32 Size() const { return m_count; }

  // The translation of `key`, or nullptr if the catalogue does not have it.
  const char* Lookup(const char* key) const
  {
    const auto entries = std::views::iota(u32{0}, m_count);
    const auto less_than_key = [this, key](u32 index) {
      const char* original = String(m_originals_table, index);
      return original != nullptr && std::strcmp(original, key) < 0;
    };
    const auto found = std::ranges::partition_point(entries, less_than_key);
    if (found == entries.end())
      return nullptr;

    const char* original = String(m_originals_table, *found);
    if (original == nullptr || std::strcmp(original, key) != 0)
      return nullptr;
    return String(m_translations_table, *found);
  }

private:
  static constexpr u32 MAGIC = 0x950412de;
  static constexpr std::size_t HEADER_SIZE = 20;
  static constexpr std::size_t ENTRY_SIZE = 8;

  static u32 Word(const std::vector<char>& bytes, std::size_t at)
  {
    u32 value;
    std::memcpy(&value, &bytes[at], sizeof(value));
    return value;
  }

  static bool TableFits(std::size_t file_size, u32 table_offset, u32 count)
  {
    return u64{table_offset} + u64{count} * ENTRY_SIZE <= file_size;
  }

  // String `index` of the table at `table_offset`, or nullptr if its entry points outside the
  // file or the string is not terminated where the entry says it ends.
  const char* String(u32 table_offset, u32 index) const
  {
    const std::size_t entry = table_offset + std::size_t{index} * ENTRY_SIZE;
    const u32 length = Word(m_bytes, entry);
    const u32 offset = Word(m_bytes, entry + 4);
    if (u64{offset} + length >= m_bytes.size() || m_bytes[std::size_t{offset} + length] != '\0')
      return nullptr;
    return &m_bytes[offset];
  }

  std::vector<char> m_bytes;
  u32 m_count = 0;
  u32 m_originals_table = 0;
  u32 m_translations_table = 0;
};

// Serves Qt's tr() calls from a message catalogue. The catalogue has no notion of Qt's class
// contexts, so the context is ignored; a disambiguation string is used as the gettext context.
class CatalogueTranslator final : public QTranslator
{
public:
  using QTranslator::QTranslator;

  bool Open(const std::string& path)
  {
    m_catalogue = MessageCatalogue(path);
    return !isEmpty();
  }

  bool isEmpty() const override { return m_catalogue.Size() == 0; }

  QString translate(const char*, const char* source_text, const char* disambiguation = nullptr,
                    int = -1) const override
  {
    const char* translated;
    if (disambiguation != nullptr)
    {
      const std::string key = std::string(disambiguation) + '\4' + source_text;
      translated = m_catalogue.Lookup(key.c_str());
    }
    else
    {
      translated = m_catalogue.Lookup(source_text);
    }
    return QString::fromUtf8(translated != nullptr ? translated : source_text);
  }

private:
  MessageCatalogue m_catalogue;
};

// The folder names to try for a language, most specific first: Qt's "fr-Latn-CA" becomes
// fr_Latn_CA, then fr_Latn, then fr (RFC 4647 lookup, as QTranslator does). Chinese scripts also
// get the region-based names used on some systems (zh_Hans: also zh_CN; zh_Hant: also zh_TW).
QStringList CandidateLanguageFolders(const QString& language)
{
  QString name = language;
  name.replace(QLatin1Char('-'), QLatin1Char('_'));

  QStringList folders{name};
  for (qsizetype cut = name.lastIndexOf(QLatin1Char('_')); cut != -1;
       cut = name.lastIndexOf(QLatin1Char('_')))
  {
    name.truncate(cut);
    folders.append(name);
  }

  const auto add_alias_after = [&folders](const QString& script_name, const QString& alias) {
    const qsizetype at = folders.indexOf(script_name);
    if (at != -1)
      folders.insert(at + 1, alias);
  };
  add_alias_after(QStringLiteral("zh_Hans"), QStringLiteral("zh_CN"));
  add_alias_after(QStringLiteral("zh_Hant"), QStringLiteral("zh_TW"));

  return folders;
}

std::string CataloguePath(const std::string& folder)
{
#if defined __APPLE__
  return fmt::format("{}/Contents/Resources/{}.lproj/dolphin-emu.mo", File::GetBundleDirectory(),
                     folder);
#else
  std::string path = fmt::format("{}/Languages/{}/dolphin-emu.mo", File::GetExeDirectory(), folder);
#ifndef _WIN32
  // Installed builds keep their catalogues in the system locale tree.
  if (!File::Exists(path))
    path = fmt::format("{}/../locale/{}/LC_MESSAGES/dolphin-emu.mo", DATA_DIR, folder);
#endif
  return path;
#endif
}

// Installs the first catalogue found for `language` and makes it the default locale.
bool InstallLanguage(const QString& language)
{
  for (const QString& folder : CandidateLanguageFolders(language))
  {
    auto* const translator = new CatalogueTranslator(QApplication::instance());
    if (!translator->Open(CataloguePath(folder.toStdString())))
    {
      translator->deleteLater();
      continue;
    }

    QApplication::instance()->installTranslator(translator);
    QLocale::setDefault(QLocale(language));
    UICommon::SetLocale(language.toStdString());
    return true;
  }

  ERROR_LOG_FMT(COMMON, "No suitable translation file found");
  return false;
}
}  // namespace

void Translation::Initialize()
{
  // Strings translated inside the core go through the same translators.
  Common::RegisterStringTranslator([](const char* text) { return QObject::tr(text).toStdString(); });

  const std::string chosen = Config::Get(Config::MAIN_INTERFACE_LANGUAGE);
  if (!chosen.empty())
  {
    if (InstallLanguage(QString::fromStdString(chosen)))
      return;

    ModalMessageBox::warning(
        nullptr, QObject::tr("Error"),
        QObject::tr("Error loading selected language. Falling back to system default."));
    Config::SetBase(Config::MAIN_INTERFACE_LANGUAGE, "");
  }

  for (const QString& language : QLocale::system().uiLanguages())
  {
    if (InstallLanguage(language))
      break;
  }
}
