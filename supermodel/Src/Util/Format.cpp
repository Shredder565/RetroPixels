// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Util/Format.h"

namespace Util
{
  namespace
  {
    bool IsAsciiSpace(char c)
    {
      return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    }

    char AsciiLower(char c)
    {
      return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    }
  }

  std::ostream &operator<<(std::ostream &os, const Format &format)
  {
    format.Write(os);
    return os;
  }

  std::string ToLower(const std::string &str)
  {
    std::string out(str);
    for (char &c : out)
      c = AsciiLower(c);
    return out;
  }

  std::string TrimWhiteSpace(const std::string &str)
  {
    size_t begin = 0;
    size_t end = str.size();
    while (begin < end && IsAsciiSpace(str[begin]))
      ++begin;
    while (end > begin && IsAsciiSpace(str[end - 1]))
      --end;
    return str.substr(begin, end - begin);
  }

  std::string Hex(uint64_t n, size_t num_digits)
  {
    std::string out(2 + num_digits, '0');
    out[1] = 'x';
    for (size_t i = 0; i < num_digits; i++)
    {
      unsigned shift = unsigned(4 * (num_digits - 1 - i));
      unsigned digit = shift < 64 ? unsigned((n >> shift) & 0xf) : 0;
      out[2 + i] = "0123456789abcdef"[digit];
    }
    return out;
  }

  std::string Hex(uint64_t n) { return Hex(n, 16); }
  std::string Hex(uint32_t n) { return Hex(uint64_t(n), 8); }
  std::string Hex(uint16_t n) { return Hex(uint64_t(n), 4); }
  std::string Hex(uint8_t n)  { return Hex(uint64_t(n), 2); }

  int Stricmp(const char *s1, const char *s2)
  {
    for (;; ++s1, ++s2)
    {
      int a = (unsigned char)AsciiLower(*s1);
      int b = (unsigned char)AsciiLower(*s2);
      if (a != b || a == 0)
        return a - b;
    }
  }
} // Util
