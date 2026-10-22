// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QString>
#include <string_view>

namespace QtUtils
{
// QString from UTF-8 or UTF-16 text that need not be null-terminated.
inline QString FromStdString(std::string_view s)
{
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
inline QString FromStdString(std::u8string_view s)
{
  return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
inline QString FromStdString(std::u16string_view s)
{
  return QString::fromUtf16(s.data(), static_cast<qsizetype>(s.size()));
}
}  // namespace QtUtils
