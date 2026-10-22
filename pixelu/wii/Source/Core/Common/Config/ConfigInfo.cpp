// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Config/ConfigInfo.h"

#include <cstddef>
#include <string_view>

namespace Config
{
namespace
{
// Three-way ASCII case-insensitive comparison of bytes taken as unsigned, like strcasecmp.
int CompareNoCase(std::string_view a, std::string_view b)
{
  const std::size_t common = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < common; ++i)
  {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z')
      ca += 'a' - 'A';
    if (cb >= 'A' && cb <= 'Z')
      cb += 'a' - 'A';
    if (ca != cb)
      return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size())
    return 0;
  return a.size() < b.size() ? -1 : 1;
}

// Strings are compared up to their first NUL, as C strings.
std::string_view AsCString(const std::string& s)
{
  return std::string_view(s.c_str());
}
}  // namespace

bool Location::operator==(const Location& other) const
{
  return system == other.system && CompareNoCase(AsCString(section), AsCString(other.section)) == 0 &&
         CompareNoCase(AsCString(key), AsCString(other.key)) == 0;
}

bool Location::operator<(const Location& other) const
{
  if (system != other.system)
    return system < other.system;
  if (const int by_section = CompareNoCase(AsCString(section), AsCString(other.section)))
    return by_section < 0;
  return CompareNoCase(AsCString(key), AsCString(other.key)) < 0;
}
}  // namespace Config
