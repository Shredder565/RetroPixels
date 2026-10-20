// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cerrno>
#include <charconv>
#include <concepts>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/TypeUtils.h"

// ---------------------------------------------------------------------------------------------
// printf-style formatting (always with the "C" locale)

std::string StringFromFormatV(const char* format, va_list args);

std::string StringFromFormat(const char* format, ...)
#if !defined _WIN32
    __attribute__((__format__(printf, 1, 2)))
#endif
    ;

// Formats into a fixed buffer. Returns false (with a truncated, terminated result) when the
// text did not fit or was empty.
bool CharArrayFromFormatV(char* out, int outsize, const char* format, va_list args);

template <size_t Count>
inline void CharArrayFromFormat(char (&out)[Count], const char* format, ...)
{
  va_list args;
  va_start(args, format);
  CharArrayFromFormatV(out, Count, format, args);
  va_end(args);
}

// Two hex digits per byte, `line_len` bytes per line, optionally separated by spaces.
std::string ArrayToString(const u8* data, u32 size, int line_len = 20, bool spaces = true);

// ---------------------------------------------------------------------------------------------
// Trimming and replacing

// Whitespace here means space, tab, CR and LF.
std::string_view StripWhitespace(std::string_view s);
std::string_view StripLeadingWhitespace(std::string_view s);
std::string_view StripTrailingWhitespace(std::string_view s);
// Only removes ' ' characters.
std::string_view StripSpaces(std::string_view s);
// Removes one pair of surrounding double quotes, if present.
std::string_view StripQuotes(std::string_view s);

std::string ReplaceAll(std::string result, std::string_view src, std::string_view dest);

// CR and LF become spaces.
void ReplaceBreaksWithSpaces(std::string& str);

// Cuts the string at its first NUL character.
void TruncateToCString(std::string* s);

// ---------------------------------------------------------------------------------------------
// Parsing text values (as used by the INI and config code)

// Accepts "true"/"false" (any case) and any number equal to 1 or 0.
bool TryParse(const std::string& str, bool* output);

// strtol rules: leading whitespace and a sign are allowed; base 0 also accepts 0x.. and 0..
// (octal). The whole string must be consumed (an empty string reads as 0) and the value must fit
// the target type. Enums are read through their underlying type.
template <typename T>
requires(std::is_integral_v<T> || (std::is_enum_v<T> && !Common::BooleanEnum<T>))
bool TryParse(const std::string& str, T* output, int base = 0)
{
  using Limits = typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>,
                                             std::type_identity<T>>::type;
  constexpr bool read_unsigned = std::is_unsigned_v<T>;  // false for every enum
  using Wide = std::conditional_t<read_unsigned, u64, s64>;

  const char* const begin = str.c_str();
  char* stop = nullptr;
  errno = 0;
  Wide parsed;
  if constexpr (read_unsigned)
    parsed = std::strtoull(begin, &stop, base);
  else
    parsed = std::strtoll(begin, &stop, base);

  if (stop == nullptr || *stop != '\0' || errno == ERANGE)
    return false;

  if (std::cmp_less(parsed, std::numeric_limits<Limits>::min()) ||
      std::cmp_greater(parsed, std::numeric_limits<Limits>::max()))
  {
    return false;
  }

  *output = static_cast<T>(parsed);
  return true;
}

template <Common::BooleanEnum T>
bool TryParse(const std::string& str, T* output)
{
  bool flag;
  if (!TryParse(str, &flag))
    return false;
  *output = static_cast<T>(flag);
  return true;
}

// Reads a decimal number with the "C" locale. A comma is accepted as the decimal separator.
// Leading whitespace is allowed, trailing characters are not.
template <std::floating_point T>
bool TryParse(std::string str, T* const output)
{
  for (char& c : str)
  {
    if (c == ',')
      c = '.';
  }

  std::istringstream stream(std::move(str));
  stream.imbue(std::locale::classic());
  T parsed;
  if (!(stream >> parsed) || !stream.eof())
    return false;

  *output = parsed;
  return true;
}

// Parses each `delimiter`-separated item (a trailing delimiter adds no empty item).
template <typename N>
bool TryParseVector(const std::string& str, std::vector<N>* output, const char delimiter = ',')
{
  output->clear();
  std::size_t start = 0;
  while (start < str.size())
  {
    std::size_t stop = str.find(delimiter, start);
    if (stop == std::string::npos)
      stop = str.size();

    N item = 0;
    if (!TryParse(str.substr(start, stop - start), &item))
      return false;
    output->push_back(item);
    start = stop + 1;
  }
  return true;
}

// Text forms used in config files: unsigned values as fixed-width hex, signed values in decimal,
// floats in their shortest exact form with a decimal point, bools as True/False.
std::string ValueToString(u16 value);
std::string ValueToString(u32 value);
std::string ValueToString(u64 value);
std::string ValueToString(float value);
std::string ValueToString(double value);
std::string ValueToString(int value);
std::string ValueToString(s64 value);
std::string ValueToString(bool value);
std::string ValueToString(Common::Enum auto value)
{
  return ValueToString(std::to_underlying(value));
}

// Classic hex dump: offset, 16 bytes in hex, then the printable ASCII characters.
std::string HexDump(const u8* data, size_t size);

inline auto HexDump(std::span<const u8> data)
{
  return HexDump(data.data(), data.size());
}

namespace Common
{
// std::from_chars over a string_view.
std::from_chars_result FromChars(std::string_view sv, std::integral auto& value, int base = 10)
{
  return std::from_chars(sv.data(), sv.data() + sv.size(), value, base);
}
std::from_chars_result FromChars(std::string_view sv, std::floating_point auto& value,
                                 std::chars_format fmt = std::chars_format::general)
{
  return std::from_chars(sv.data(), sv.data() + sv.size(), value, fmt);
}
}  // namespace Common

// ---------------------------------------------------------------------------------------------
// Splitting

// Splits at every `delim`; a trailing delimiter adds no empty item, and "" gives no items.
std::vector<std::string> SplitString(const std::string& str, char delim);

// Splits into exactly Count parts, or returns nullopt if `subject` does not contain exactly
// Count - 1 delimiters.
template <std::size_t Count>
requires(Count > 1)
std::optional<std::array<std::string_view, Count>> SplitStringIntoArray(std::string_view subject,
                                                                        char delim)
{
  std::array<std::string_view, Count> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i + 1 < Count; ++i)
  {
    const std::size_t stop = subject.find(delim, start);
    if (stop == std::string_view::npos)
      return std::nullopt;
    parts[i] = subject.substr(start, stop - start);
    start = stop + 1;
  }
  const std::string_view last = subject.substr(start);
  if (last.find(delim) != std::string_view::npos)
    return std::nullopt;
  parts[Count - 1] = last;
  return parts;
}

// ---------------------------------------------------------------------------------------------
// Paths (forward slashes are expected, also on Windows)

// "C:/dir/file.ext" -> "C:/dir/", "file", ".ext". Returns false for an empty path.
bool SplitPath(std::string_view full_path, std::string* path, std::string* filename,
               std::string* extension);

// On Windows, turns backslashes into forward slashes. Does nothing elsewhere.
void UnifyPathSeparators(std::string& path);
std::string WithUnifiedPathSeparators(std::string path);

// The last path component, extension included.
std::string PathToFileName(std::string_view path);

void StringPopBackIf(std::string* s, char c);
// Counts code points by counting bytes that are not UTF-8 continuation bytes.
size_t StringUTF8CodePointCount(std::string_view str);

// ---------------------------------------------------------------------------------------------
// Text encodings

std::string CP1252ToUTF8(std::string_view input);
std::string SHIFTJISToUTF8(std::string_view input);
std::string UTF8ToSHIFTJIS(std::string_view input);
std::string WStringToUTF8(std::wstring_view input);
// Reads big-endian UTF-16 up to the first NUL or max_size units.
std::string UTF16BEToUTF8(const char16_t* str, size_t max_size);
// Malformed input is replaced with U+FFFD.
std::string UTF16ToUTF8(std::u16string_view input);
std::u16string UTF8ToUTF16(std::string_view input);

#ifdef _WIN32

std::wstring UTF8ToWString(std::string_view str);

#ifdef _UNICODE
inline std::string TStrToUTF8(std::wstring_view str)
{
  return WStringToUTF8(str);
}

inline std::wstring UTF8ToTStr(std::string_view str)
{
  return UTF8ToWString(str);
}
#else
inline std::string TStrToUTF8(std::string_view str)
{
  return std::string(str);
}

inline std::string UTF8ToTStr(std::string_view str)
{
  return std::string(str);
}
#endif

#endif

// UTF-8 strings to and from std::filesystem paths (replacements for the deprecated u8path).
std::filesystem::path StringToPath(std::string_view path);
std::string PathToString(const std::filesystem::path& path);

namespace Common
{
// Locale-independent ASCII character classes and case mapping. Bytes >= 0x80 are never letters,
// digits or printable, and are left unchanged by ToLower/ToUpper.
inline bool IsPrintableCharacter(char c)
{
  return c >= 0x20 && c <= 0x7E;
}

inline bool IsUpper(char c)
{
  return c >= 'A' && c <= 'Z';
}

inline bool IsLower(char c)
{
  return c >= 'a' && c <= 'z';
}

inline bool IsAlpha(char c)
{
  return IsUpper(c) || IsLower(c);
}

inline bool IsAlnum(char c)
{
  return IsAlpha(c) || (c >= '0' && c <= '9');
}

inline bool IsXDigit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline char ToLower(char ch)
{
  return IsUpper(ch) ? static_cast<char>(ch - 'A' + 'a') : ch;
}

inline char ToUpper(char ch)
{
  return IsLower(ch) ? static_cast<char>(ch - 'a' + 'A') : ch;
}

// Right-aligns the number in a field of `spaces` characters, using the stream's (global) locale.
template <typename I>
std::string ThousandSeparate(I value, int spaces = 0)
{
#ifdef _WIN32
  std::wostringstream stream;
  stream << std::setw(spaces) << value;
  return WStringToUTF8(stream.str());
#else
  std::ostringstream stream;
  stream << std::setw(spaces) << value;
  return stream.str();
#endif
}

#ifdef _WIN32
std::vector<std::string> CommandLineToUtf8Argv(const wchar_t* command_line);
#endif

// Escapes & < > " and ' as HTML entities.
std::string GetEscapedHtml(std::string html);

void ToLower(std::string* str);
void ToUpper(std::string* str);
bool CaseInsensitiveEquals(std::string_view a, std::string_view b);
bool CaseInsensitiveContains(std::string_view haystack, std::string_view needle);

// Case-insensitive (ASCII) ordering for ordered containers; allows lookup by string_view.
struct CaseInsensitiveLess
{
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const;
};

// Lowercase hex digits, no separators.
std::string BytesToHexString(std::span<const u8> bytes);
}  // namespace Common
