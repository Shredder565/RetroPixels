// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/FileSearch.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "Common/CommonPaths.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <strings.h>
#endif

namespace fs = std::filesystem;

namespace Common
{
namespace
{
using NativeString = fs::path::string_type;

// Case-insensitive "ends with", on native path strings.
bool EndsWithIgnoringCase(const NativeString& text, const NativeString& suffix)
{
  if (suffix.size() > text.size())
    return false;
  const auto* tail = text.data() + (text.size() - suffix.size());
#ifdef _WIN32
  return CompareStringOrdinal(tail, static_cast<int>(suffix.size()), suffix.data(),
                              static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
#else
  return strncasecmp(tail, suffix.data(), suffix.size()) == 0;
#endif
}
}  // namespace

std::vector<std::string> DoFileSearch(std::span<const std::string_view> directories,
                                      std::span<const std::string_view> exts, bool recursive)
{
  // Without extensions everything is returned, directories included; with extensions only files
  // whose path ends in one of them (ignoring case).
  std::vector<NativeString> suffixes;
  suffixes.reserve(exts.size());
  for (const std::string_view ext : exts)
    suffixes.push_back(StringToPath(ext).native());

  std::vector<std::string> found;
  const auto consider = [&](const fs::directory_entry& entry) {
    if (!suffixes.empty())
    {
      std::error_code ec;
      if (entry.is_directory(ec))
        return;
      const NativeString& native = entry.path().native();
      if (std::ranges::none_of(suffixes,
                               [&](const NativeString& s) { return EndsWithIgnoringCase(native, s); }))
      {
        return;
      }
    }
    found.push_back(PathToString(entry.path()));
  };

  for (const std::string_view directory : directories)
  {
    const fs::path root = StringToPath(directory);
    std::error_code ec;
    if (recursive)
    {
      for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        consider(*it);
    }
    else
    {
      for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        consider(*it);
    }
    if (ec)
      ERROR_LOG_FMT(COMMON, "DoFileSearch error on {}: {}", directory, ec.message());
  }

  // Overlapping or repeated input directories can list a path twice. (Only identical spellings
  // are merged; this is not a std::filesystem::equivalent check.)
  std::ranges::sort(found);
  found.erase(std::ranges::unique(found).begin(), found.end());

  // The rest of the code base expects '/' separators.
  if constexpr (fs::path::preferred_separator != DIR_SEP_CHR)
  {
    for (std::string& path : found)
      std::ranges::replace(path, '\\', DIR_SEP_CHR);
  }

  return found;
}
}  // namespace Common
