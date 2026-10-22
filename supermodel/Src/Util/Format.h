// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Small string helpers. Util::Format is a string builder that can be used in
// an expression:
//
//   std::string s = Util::Format() << "slot " << n;
//   Util::Format(", ").Join(names)     -> the names with ", " between them
//   Util::Format("a/b/c").Split('/')   -> { "a", "b", "c" }

#ifndef INCLUDED_FORMAT_H
#define INCLUDED_FORMAT_H

#include <cstdint>
#include <iomanip>  // (other files use std::setw etc. through this header)
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace Util
{
  class Format
  {
  public:
    Format() = default;

    // Starts with the given text; further << appends to it.
    Format(const std::string &text)
    {
      m_out << text;
    }

    // Copying a builder copies the text built so far.
    Format(const Format &other)
    {
      m_out << other.str();
    }

    Format &operator=(const Format &other)
    {
      if (this != &other)
      {
        std::string text = other.str();
        m_out.str(std::string());
        m_out.clear();
        m_out << text;
      }
      return *this;
    }

    template <typename T>
    Format &operator<<(const T &value)
    {
      m_out << value;
      return *this;
    }

    std::string str() const
    {
      return m_out.str();
    }

    operator std::string() const
    {
      return str();
    }

    void Write(std::ostream &os) const
    {
      os << str();
    }

    // Uses the current text as the separator and replaces it with the items
    // of 'items' (anything iterable whose elements can be streamed).
    template <typename Collection>
    Format &Join(const Collection &items)
    {
      const std::string separator = str();
      m_out.str(std::string());
      m_out.clear();
      bool first = true;
      for (const auto &item : items)
      {
        if (!first)
          m_out << separator;
        m_out << item;
        first = false;
      }
      return *this;
    }

    // Cuts the current text at every 'separator'. Empty pieces are kept, so
    // "" gives one empty piece and "a," gives "a" and "".
    std::vector<std::string> Split(char separator) const
    {
      // Only the text up to a NUL counts (the text is treated as a C string).
      const std::string all = str();
      const std::string text(all.c_str());
      std::vector<std::string> pieces;
      size_t from = 0;
      for (;;)
      {
        size_t at = text.find(separator, from);
        if (at == std::string::npos)
        {
          pieces.push_back(text.substr(from));
          return pieces;
        }
        pieces.push_back(text.substr(from, at - from));
        from = at + 1;
      }
    }

  private:
    std::ostringstream m_out;
  };

  std::ostream &operator<<(std::ostream &os, const Format &format);

  // ASCII lower case (other bytes unchanged).
  std::string ToLower(const std::string &str);

  // Removes leading and trailing ASCII white space.
  std::string TrimWhiteSpace(const std::string &str);

  // "0x" followed by exactly num_digits lower-case hex digits of n (higher
  // digits are dropped). The single-argument forms use the type's width.
  std::string Hex(uint64_t n, size_t num_digits);
  std::string Hex(uint64_t n);
  std::string Hex(uint32_t n);
  std::string Hex(uint16_t n);
  std::string Hex(uint8_t n);

  // Case-insensitive (ASCII) comparison of two C strings: 0 when equal, else
  // the difference of the first differing characters after lower-casing.
  int Stricmp(const char *s1, const char *s2);
} // Util

#endif  // INCLUDED_FORMAT_H
