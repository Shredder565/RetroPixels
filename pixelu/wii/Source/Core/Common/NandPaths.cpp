// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/NandPaths.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

namespace Common
{
namespace
{
std::string RootPrefix(std::optional<FromWhichRoot> from)
{
  if (!from)
    return {};
  return RootUserPath(*from);
}

// "<root>/<dir>/%08x/%08x" for the two halves of a title ID.
std::string TitleIdPath(std::optional<FromWhichRoot> from, std::string_view dir, u64 title_id)
{
  return fmt::format("{}/{}/{:08x}/{:08x}", RootPrefix(from), dir,
                     static_cast<u32>(title_id >> 32), static_cast<u32>(title_id));
}

bool IsAllDots(std::string_view name)
{
  return std::ranges::all_of(name, [](char c) { return c == '.'; });
}

bool IsIllegalCharacter(char c)
{
  if (static_cast<u8>(c) < 0x20 || c == '\x7f')
    return true;
  switch (c)
  {
  case '"':
  case '*':
  case '/':
  case ':':
  case '<':
  case '>':
  case '?':
  case '\\':
  case '|':
    return true;
  default:
    return false;
  }
}

std::string EscapeSequence(char c)
{
  return fmt::format("__{:02x}__", static_cast<u8>(c));
}
}  // namespace

std::string RootUserPath(FromWhichRoot from)
{
  int index = D_WIIROOT_IDX;
  if (from == FromWhichRoot::Session)
    index = D_SESSION_WIIROOT_IDX;
  else if (from == FromWhichRoot::Banners)
    index = D_BANNERS_WIIROOT_IDX;

  std::string path = File::GetUserPath(index);
  if (!path.empty())
    path.pop_back();  // the user path ends with a separator
  return path;
}

std::string GetImportTitlePath(u64 title_id, std::optional<FromWhichRoot> from)
{
  return TitleIdPath(from, "import", title_id);
}

std::string GetTicketFileName(u64 title_id, std::optional<FromWhichRoot> from)
{
  return TitleIdPath(from, "ticket", title_id) + ".tik";
}

std::string GetV1TicketFileName(u64 title_id, std::optional<FromWhichRoot> from)
{
  return TitleIdPath(from, "ticket", title_id) + ".tv1";
}

std::string GetTitlePath(u64 title_id, std::optional<FromWhichRoot> from)
{
  return TitleIdPath(from, "title", title_id);
}

std::string GetTitleDataPath(u64 title_id, std::optional<FromWhichRoot> from)
{
  return GetTitlePath(title_id, from) + "/data";
}

std::string GetTitleContentPath(u64 title_id, std::optional<FromWhichRoot> from)
{
  return GetTitlePath(title_id, from) + "/content";
}

std::string GetTMDFileName(u64 title_id, std::optional<FromWhichRoot> from)
{
  return GetTitleContentPath(title_id, from) + "/title.tmd";
}

std::string GetMiiDatabasePath(std::optional<FromWhichRoot> from)
{
  return RootPrefix(from) + "/shared2/menu/FaceLib/RFL_DB.dat";
}

bool IsTitlePath(const std::string& path, std::optional<FromWhichRoot> from, u64* title_id)
{
  const std::string prefix = RootPrefix(from) + "/title/";
  if (!path.starts_with(prefix))
    return false;

  // The two components after the prefix must start with hex numbers that fit in 32 bits.
  const std::vector<std::string> parts = SplitString(path.substr(prefix.size()), '/');
  if (parts.size() < 2)
    return false;

  u32 high = 0;
  u32 low = 0;
  if (Common::FromChars(parts[0], high, 16).ec != std::errc{})
    return false;
  if (Common::FromChars(parts[1], low, 16).ec != std::errc{})
    return false;

  if (title_id)
    *title_id = (u64{high} << 32) | low;
  return true;
}

std::string EscapeFileName(const std::string& filename)
{
  // ".", "..", "..." and so on (and the empty name) would mean something else to the host.
  if (IsAllDots(filename))
  {
    std::string escaped;
    for (size_t i = 0; i < filename.size(); ++i)
      escaped += EscapeSequence('.');
    return escaped;
  }

  std::string escaped;
  escaped.reserve(filename.size());
  for (size_t i = 0; i < filename.size();)
  {
    if (filename.compare(i, 2, "__") == 0)
    {
      // A literal double underscore would look like the start of an escape sequence.
      escaped += "__5f____5f__";
      i += 2;
    }
    else
    {
      const char c = filename[i++];
      if (IsIllegalCharacter(c))
        escaped += EscapeSequence(c);
      else
        escaped += c;
    }
  }
  return escaped;
}

std::string EscapePath(const std::string& path)
{
  std::string escaped;
  bool first = true;
  for (const std::string& part : SplitString(path, '/'))
  {
    if (!first)
      escaped += '/';
    escaped += EscapeFileName(part);
    first = false;
  }
  return escaped;
}

std::string UnescapeFileName(const std::string& filename)
{
  std::string result;
  result.reserve(filename.size());
  const std::string_view view{filename};
  for (size_t i = 0; i < view.size();)
  {
    // "__", two characters starting with at least one hex digit, "__"
    if (i + 6 <= view.size() && view.compare(i, 2, "__") == 0 && view[i + 4] == '_' &&
        view[i + 5] == '_')
    {
      u32 value = 0;
      if (Common::FromChars(view.substr(i + 2, 2), value, 16).ec == std::errc{})
      {
        result += static_cast<char>(value);
        i += 6;
        continue;
      }
    }
    result += view[i++];
  }
  return result;
}

bool IsFileNameSafe(const std::string_view filename)
{
  if (filename.empty() || IsAllDots(filename))
    return false;
  return std::ranges::none_of(filename, IsIllegalCharacter);
}
}  // namespace Common
