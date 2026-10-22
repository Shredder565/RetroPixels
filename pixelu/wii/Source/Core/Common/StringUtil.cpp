// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/StringUtil.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#else
#include <cerrno>
#include <clocale>
#include <iconv.h>
#include <locale.h>
#endif

// ---------------------------------------------------------------------------------------------
// printf-style formatting

#ifndef _WIN32
namespace
{
// Switches the calling thread to the "C" locale for as long as the object lives.
class ScopedCLocale
{
public:
  ScopedCLocale()
  {
    static const locale_t c_locale = newlocale(LC_ALL_MASK, "C", locale_t{});
    m_previous = uselocale(c_locale);
  }
  ~ScopedCLocale() { uselocale(m_previous); }
  ScopedCLocale(const ScopedCLocale&) = delete;
  ScopedCLocale& operator=(const ScopedCLocale&) = delete;

private:
  locale_t m_previous;
};
}  // namespace
#endif

bool CharArrayFromFormatV(char* out, int outsize, const char* format, va_list args)
{
  // Always format with the "C" locale: our strings are UTF-8, and multibyte-aware printf
  // implementations would misread them under some system code pages.
#ifdef _WIN32
  static const _locale_t c_locale = _create_locale(LC_ALL, "C");
  const int written = _vsnprintf_l(out, outsize, format, c_locale, args);
#else
  int written;
  {
    ScopedCLocale use_c_locale;
    written = std::vsnprintf(out, outsize, format, args);
  }
#endif

  if (written > 0 && written < outsize)
  {
    out[written] = '\0';
    return true;
  }
  out[outsize - 1] = '\0';
  return false;
}

std::string StringFromFormatV(const char* format, va_list args)
{
  va_list measure_args;
  va_copy(measure_args, args);
#ifdef _WIN32
  const int length = _vscprintf(format, measure_args);
#else
  int length;
  {
    ScopedCLocale use_c_locale;
    length = std::vsnprintf(nullptr, 0, format, measure_args);
  }
#endif
  va_end(measure_args);

  if (length < 0)
  {
    ERROR_LOG_FMT(COMMON, "Unable to format string");
    return {};
  }

  std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
  CharArrayFromFormatV(buffer.data(), length + 1, format, args);
  return std::string(buffer.data());
}

std::string StringFromFormat(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  std::string result = StringFromFormatV(format, args);
  va_end(args);
  return result;
}

std::string ArrayToString(const u8* data, u32 size, int line_len, bool spaces)
{
  std::string out;
  out.reserve(size_t{size} * 3);
  int column = 0;
  for (u32 i = 0; i < size; ++i)
  {
    out += fmt::format("{:02x}", data[i]);
    ++column;
    if (column == line_len)
    {
      out += '\n';
      column = 0;
    }
    else if (spaces)
    {
      out += ' ';
    }
  }
  return out;
}

std::string HexDump(const u8* data, size_t size)
{
  constexpr size_t ROW = 16;
  std::string out;
  for (size_t offset = 0; offset < size; offset += ROW)
  {
    const size_t count = std::min(ROW, size - offset);
    out += fmt::format("{:06x}: ", offset);
    for (size_t i = 0; i < ROW; ++i)
      out += i < count ? fmt::format("{:02x} ", data[offset + i]) : std::string("   ");
    out += ' ';
    for (size_t i = 0; i < count; ++i)
    {
      const char c = static_cast<char>(data[offset + i]);
      out += Common::IsPrintableCharacter(c) ? c : '.';
    }
    out += '\n';
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Trimming and replacing

namespace
{
constexpr std::string_view WHITESPACE = " \t\r\n";

std::string_view TrimFront(std::string_view s, std::string_view set)
{
  const size_t first = s.find_first_not_of(set);
  return first == std::string_view::npos ? std::string_view{} : s.substr(first);
}

std::string_view TrimBack(std::string_view s, std::string_view set)
{
  const size_t last = s.find_last_not_of(set);
  return last == std::string_view::npos ? std::string_view{} : s.substr(0, last + 1);
}
}  // namespace

std::string_view StripWhitespace(std::string_view s)
{
  return TrimBack(TrimFront(s, WHITESPACE), WHITESPACE);
}

std::string_view StripLeadingWhitespace(std::string_view s)
{
  return TrimFront(s, WHITESPACE);
}

std::string_view StripTrailingWhitespace(std::string_view s)
{
  return TrimBack(s, WHITESPACE);
}

std::string_view StripSpaces(std::string_view s)
{
  return TrimBack(TrimFront(s, " "), " ");
}

std::string_view StripQuotes(std::string_view s)
{
  if (s.empty() || s.front() != '"' || s.back() != '"')
    return s;
  if (s.size() < 2)
    return {};
  return s.substr(1, s.size() - 2);
}

std::string ReplaceAll(std::string result, std::string_view src, std::string_view dest)
{
  if (src == dest)
    return result;

  size_t search_from = 0;
  for (;;)
  {
    const size_t hit = result.find(src, search_from);
    if (hit == std::string::npos)
      break;
    result.replace(hit, src.size(), dest);
    search_from = hit + dest.size();
  }
  return result;
}

void ReplaceBreaksWithSpaces(std::string& str)
{
  for (char& c : str)
  {
    if (c == '\r' || c == '\n')
      c = ' ';
  }
}

void TruncateToCString(std::string* s)
{
  const size_t nul = s->find('\0');
  if (nul != std::string::npos)
    s->erase(nul);
}

// ---------------------------------------------------------------------------------------------
// Values

bool TryParse(const std::string& str, bool* const output)
{
  float number;
  const bool numeric = TryParse(str, &number);
  if ((numeric && number == 1) || Common::CaseInsensitiveEquals(str.c_str(), "true"))
  {
    *output = true;
    return true;
  }
  if ((numeric && number == 0) || Common::CaseInsensitiveEquals(str.c_str(), "false"))
  {
    *output = false;
    return true;
  }
  return false;
}

namespace
{
// Unsigned values are written as zero-padded hex with every digit of their width; floating
// point values always keep their decimal point; signed values are plain decimal.
template <typename T>
std::string HexWithAllDigits(T value)
{
  return fmt::format("0x{:0{}x}", value, sizeof(T) * 2);
}

template <typename T>
std::string FloatWithPoint(T value)
{
  return fmt::format("{:#}", value);
}
}  // namespace

std::string ValueToString(bool value)
{
  return value ? "True" : "False";
}

std::string ValueToString(int value)
{
  return fmt::to_string(value);
}

std::string ValueToString(s64 value)
{
  return fmt::to_string(value);
}

std::string ValueToString(u16 value)
{
  return HexWithAllDigits(value);
}

std::string ValueToString(u32 value)
{
  return HexWithAllDigits(value);
}

std::string ValueToString(u64 value)
{
  return HexWithAllDigits(value);
}

std::string ValueToString(float value)
{
  return FloatWithPoint(value);
}

std::string ValueToString(double value)
{
  return FloatWithPoint(value);
}

// ---------------------------------------------------------------------------------------------
// Splitting and paths

std::vector<std::string> SplitString(const std::string& str, const char delim)
{
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < str.size())
  {
    size_t stop = str.find(delim, start);
    if (stop == std::string::npos)
      stop = str.size();
    parts.emplace_back(str, start, stop - start);
    start = stop + 1;
  }
  return parts;
}

bool SplitPath(std::string_view full_path, std::string* path, std::string* filename,
               std::string* extension)
{
  if (full_path.empty())
    return false;

#ifdef _WIN32
  // A drive letter ("C:") counts as a directory too.
  constexpr std::string_view separators = "/:";
#else
  constexpr std::string_view separators = "/";
#endif
  const size_t last_separator = full_path.find_last_of(separators);
  const size_t name_start = last_separator == std::string_view::npos ? 0 : last_separator + 1;

  size_t dot = full_path.rfind('.');
  if (dot == std::string_view::npos || dot < name_start)
    dot = full_path.size();

  if (path)
    *path = std::string(full_path.substr(0, name_start));
  if (filename)
    *filename = std::string(full_path.substr(name_start, dot - name_start));
  if (extension)
    *extension = std::string(full_path.substr(dot));
  return true;
}

void UnifyPathSeparators(std::string& path)
{
#ifdef _WIN32
  std::replace(path.begin(), path.end(), '\\', '/');
#else
  (void)path;
#endif
}

std::string WithUnifiedPathSeparators(std::string path)
{
  UnifyPathSeparators(path);
  return path;
}

std::string PathToFileName(std::string_view path)
{
  std::string name, extension;
  SplitPath(path, nullptr, &name, &extension);
  return name + extension;
}

void StringPopBackIf(std::string* s, char c)
{
  if (!s->empty() && s->back() == c)
    s->pop_back();
}

size_t StringUTF8CodePointCount(std::string_view str)
{
  size_t count = 0;
  for (const char c : str)
  {
    if ((static_cast<u8>(c) & 0xC0) != 0x80)
      ++count;
  }
  return count;
}

// ---------------------------------------------------------------------------------------------
// UTF-8 <-> UTF-16
//
// Each malformed sequence becomes one U+FFFD: a stray continuation byte or an invalid lead byte
// (one byte consumed); a sequence cut short by a non-continuation byte or the end of the input
// (the bytes read so far are consumed); an overlong form, an encoded surrogate or a value above
// U+10FFFF (the whole sequence is consumed). In UTF-16, unpaired surrogates become U+FFFD.

namespace
{
constexpr char32_t REPLACEMENT = 0xFFFD;
constexpr char32_t MAX_CODE_POINT = 0x10FFFF;

constexpr bool IsSurrogate(char32_t c)
{
  return c >= 0xD800 && c <= 0xDFFF;
}

char32_t NextFromUTF8(std::string_view in, size_t& pos)
{
  const u8 lead = static_cast<u8>(in[pos++]);
  if (lead < 0x80)
    return lead;

  int extra;
  char32_t minimum;
  char32_t value;
  if ((lead & 0xE0) == 0xC0)
  {
    extra = 1;
    minimum = 0x80;
    value = lead & 0x1F;
  }
  else if ((lead & 0xF0) == 0xE0)
  {
    extra = 2;
    minimum = 0x800;
    value = lead & 0x0F;
  }
  else if ((lead & 0xF8) == 0xF0)
  {
    extra = 3;
    minimum = 0x10000;
    value = lead & 0x07;
  }
  else
  {
    return REPLACEMENT;  // continuation byte or 0xF8..0xFF
  }

  for (int i = 0; i < extra; ++i)
  {
    if (pos >= in.size())
      return REPLACEMENT;
    const u8 next = static_cast<u8>(in[pos]);
    if ((next & 0xC0) != 0x80)
      return REPLACEMENT;
    ++pos;
    value = (value << 6) | (next & 0x3F);
  }

  if (value < minimum || IsSurrogate(value) || value > MAX_CODE_POINT)
    return REPLACEMENT;
  return value;
}

void AppendUTF8(std::string& out, char32_t c)
{
  if (c > MAX_CODE_POINT)
    c = REPLACEMENT;
  if (c < 0x80)
  {
    out += static_cast<char>(c);
  }
  else if (c < 0x800)
  {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
  else if (c < 0x10000)
  {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
  else
  {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

char32_t NextFromUTF16(std::u16string_view in, size_t& pos)
{
  const char16_t first = in[pos++];
  if (!IsSurrogate(first))
    return first;
  if (first >= 0xDC00 || pos >= in.size())
    return REPLACEMENT;  // lone low surrogate, or high surrogate at the end
  const char16_t second = in[pos];
  if (second < 0xDC00 || second > 0xDFFF)
    return REPLACEMENT;  // high surrogate not followed by a low one (which is kept)
  ++pos;
  return 0x10000 + ((char32_t(first) - 0xD800) << 10) + (char32_t(second) - 0xDC00);
}

void AppendUTF16(std::u16string& out, char32_t c)
{
  if (c > MAX_CODE_POINT)
    c = REPLACEMENT;
  if (c < 0x10000)
  {
    out += static_cast<char16_t>(c);
    return;
  }
  c -= 0x10000;
  out += static_cast<char16_t>(0xD800 + (c >> 10));
  out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
}
}  // namespace

std::string UTF16ToUTF8(std::u16string_view input)
{
  std::string out;
  out.reserve(input.size() * 3);
  size_t pos = 0;
  while (pos < input.size())
    AppendUTF8(out, NextFromUTF16(input, pos));
  return out;
}

std::u16string UTF8ToUTF16(std::string_view input)
{
  std::u16string out;
  out.reserve(input.size());
  size_t pos = 0;
  while (pos < input.size())
    AppendUTF16(out, NextFromUTF8(input, pos));
  return out;
}

// ---------------------------------------------------------------------------------------------
// Code page conversions

#ifdef _WIN32

namespace
{
constexpr UINT CODE_PAGE_SHIFT_JIS = 932;
constexpr UINT CODE_PAGE_WINDOWS_1252 = 1252;

std::wstring DecodeCodePage(UINT code_page, std::string_view input)
{
  const int input_length = static_cast<int>(input.size());
  const int needed = MultiByteToWideChar(code_page, 0, input.data(), input_length, nullptr, 0);
  if (needed <= 0)
    return {};

  std::wstring wide(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(code_page, 0, input.data(), input_length, wide.data(), needed) != needed)
    return {};
  return wide;
}

std::string EncodeCodePage(UINT code_page, std::wstring_view input)
{
  if (input.empty())
    return {};

  const int input_length = static_cast<int>(input.size());
  const int needed =
      WideCharToMultiByte(code_page, 0, input.data(), input_length, nullptr, 0, nullptr, nullptr);
  std::string narrow(static_cast<size_t>(std::max(needed, 0)), '\0');
  if (needed <= 0 || WideCharToMultiByte(code_page, 0, input.data(), input_length, narrow.data(),
                                         needed, nullptr, nullptr) != needed)
  {
    const DWORD error = GetLastError();
    ERROR_LOG_FMT(COMMON, "Converting to code page {} failed: error {}", code_page, error);
    return {};
  }
  return narrow;
}
}  // namespace

std::wstring UTF8ToWString(std::string_view input)
{
  return DecodeCodePage(CP_UTF8, input);
}

std::string WStringToUTF8(std::wstring_view input)
{
  return EncodeCodePage(CP_UTF8, input);
}

std::string SHIFTJISToUTF8(std::string_view input)
{
  return WStringToUTF8(DecodeCodePage(CODE_PAGE_SHIFT_JIS, input));
}

std::string UTF8ToSHIFTJIS(std::string_view input)
{
  return EncodeCodePage(CODE_PAGE_SHIFT_JIS, UTF8ToWString(input));
}

std::string CP1252ToUTF8(std::string_view input)
{
  return WStringToUTF8(DecodeCodePage(CODE_PAGE_WINDOWS_1252, input));
}

std::string UTF16BEToUTF8(const char16_t* str, size_t max_size)
{
  std::wstring native;
  for (size_t i = 0; i < max_size && str[i] != 0; ++i)
    native += static_cast<wchar_t>(Common::swap16(static_cast<u16>(str[i])));
  return WStringToUTF8(native);
}

#else

namespace
{
// Converts with iconv, skipping bytes it cannot convert.
std::string IconvConvert(const char* to, const char* from, const char* data, size_t size)
{
  const iconv_t cd = iconv_open(to, from);
  if (cd == reinterpret_cast<iconv_t>(-1))
  {
    ERROR_LOG_FMT(COMMON, "iconv cannot convert from {} to {}: {}", from, to,
                  Common::LastStrerrorString());
    return {};
  }

  std::string out(size * 4, '\0');
  char* in_ptr = const_cast<char*>(data);
  size_t in_left = size;
  char* out_ptr = out.data();
  size_t out_left = out.size();
  while (in_left != 0)
  {
    if (iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left) != static_cast<size_t>(-1))
      continue;
    if (errno != EILSEQ && errno != EINVAL)
    {
      ERROR_LOG_FMT(COMMON, "iconv failed converting from {}: {}", from,
                    Common::LastStrerrorString());
      break;
    }
    if (in_left == 0)
      break;
    ++in_ptr;
    --in_left;
  }
  out.resize(out.size() - out_left);
  iconv_close(cd);
  return out;
}
}  // namespace

std::string CP1252ToUTF8(std::string_view input)
{
  return IconvConvert("UTF-8", "CP1252", input.data(), input.size());
}

std::string SHIFTJISToUTF8(std::string_view input)
{
  return IconvConvert("UTF-8", "SJIS", input.data(), input.size());
}

std::string UTF8ToSHIFTJIS(std::string_view input)
{
  return IconvConvert("SJIS", "UTF-8", input.data(), input.size());
}

std::string WStringToUTF8(std::wstring_view input)
{
  return IconvConvert("UTF-8", sizeof(wchar_t) == 2 ? "UTF-16LE" : "UTF-32LE",
                      reinterpret_cast<const char*>(input.data()), input.size() * sizeof(wchar_t));
}

std::string UTF16BEToUTF8(const char16_t* str, size_t max_size)
{
  size_t length = 0;
  while (length < max_size && str[length] != 0)
    ++length;
  return IconvConvert("UTF-8", "UTF-16BE", reinterpret_cast<const char*>(str),
                      length * sizeof(char16_t));
}

#endif

std::filesystem::path StringToPath(std::string_view path)
{
#ifdef _WIN32
  return std::filesystem::path(UTF8ToWString(path));
#else
  return std::filesystem::path(std::string(path));
#endif
}

std::string PathToString(const std::filesystem::path& path)
{
#ifdef _WIN32
  return WStringToUTF8(path.native());
#else
  return path.native();
#endif
}

namespace Common
{
#ifdef _WIN32
std::vector<std::string> CommandLineToUtf8Argv(const wchar_t* command_line)
{
  int count = 0;
  wchar_t** args = CommandLineToArgvW(command_line, &count);
  if (args == nullptr)
    return {};

  std::vector<std::string> result;
  result.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i)
    result.push_back(WStringToUTF8(args[i]));
  LocalFree(args);
  return result;
}
#endif

std::string GetEscapedHtml(std::string html)
{
  std::string escaped;
  escaped.reserve(html.size());
  for (const char c : html)
  {
    switch (c)
    {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    case '\'':
      escaped += "&apos;";
      break;
    default:
      escaped += c;
      break;
    }
  }
  return escaped;
}

void ToLower(std::string* str)
{
  for (char& c : *str)
    c = ToLower(c);
}

void ToUpper(std::string* str)
{
  for (char& c : *str)
    c = ToUpper(c);
}

bool CaseInsensitiveEquals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (ToLower(a[i]) != ToLower(b[i]))
      return false;
  }
  return true;
}

bool CaseInsensitiveContains(std::string_view haystack, std::string_view needle)
{
  if (needle.size() > haystack.size())
    return false;
  for (size_t start = 0; start + needle.size() <= haystack.size(); ++start)
  {
    if (CaseInsensitiveEquals(haystack.substr(start, needle.size()), needle))
      return true;
  }
  return false;
}

bool CaseInsensitiveLess::operator()(std::string_view a, std::string_view b) const
{
  // Compares lowercased chars as (signed) char values, like std::string's default ordering on
  // the platforms we build for.
  const size_t common = std::min(a.size(), b.size());
  for (size_t i = 0; i < common; ++i)
  {
    const char ca = ToLower(a[i]);
    const char cb = ToLower(b[i]);
    if (ca != cb)
      return ca < cb;
  }
  return a.size() < b.size();
}

std::string BytesToHexString(std::span<const u8> bytes)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const u8 byte : bytes)
  {
    hex += DIGITS[byte >> 4];
    hex += DIGITS[byte & 0xF];
  }
  return hex;
}
}  // namespace Common
