// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string_view>

// Compile-time inspection of fmt format strings, used by the logging and alert macros to check
// argument counts and translatable strings.
namespace Common
{
// Number of replacement fields ("{...}") in a format string. "{{" is a literal brace.
constexpr std::size_t CountFmtReplacementFields(std::string_view s)
{
  std::size_t fields = 0;
  std::size_t pos = 0;
  while ((pos = s.find('{', pos)) != std::string_view::npos)
  {
    if (pos + 1 < s.size() && s[pos + 1] == '{')
    {
      pos += 2;  // escaped brace
      continue;
    }
    ++fields;
    ++pos;
  }
  return fields;
}

// True if any replacement field has no argument index ("{}" or "{:...}"). Translated strings must
// use indices so that translators can reorder the arguments.
constexpr bool ContainsNonPositionalArguments(std::string_view s)
{
  std::size_t pos = 0;
  while ((pos = s.find('{', pos)) != std::string_view::npos)
  {
    if (pos + 1 >= s.size())
      return false;
    const char after = s[pos + 1];
    if (after == '{')
    {
      pos += 2;
      continue;
    }
    if (after == '}' || after == ':')
      return true;
    ++pos;
  }
  return false;
}

static_assert(CountFmtReplacementFields("") == 0);
static_assert(CountFmtReplacementFields("{} and {:08x}") == 2);
static_assert(CountFmtReplacementFields("{{}} {} {{{}}}") == 2);
static_assert(!ContainsNonPositionalArguments("no fields"));
static_assert(ContainsNonPositionalArguments("{}"));
static_assert(ContainsNonPositionalArguments("{:d}"));
static_assert(!ContainsNonPositionalArguments("{0} {1:x} {{}}"));
static_assert(!ContainsNonPositionalArguments("{{{0}}}"));
}  // namespace Common
