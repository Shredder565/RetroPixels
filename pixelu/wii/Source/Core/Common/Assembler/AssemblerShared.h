// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Common::GekkoAssembler
{
// A half-open range [begin, begin + len) of columns or indices.
struct Interval
{
  size_t begin;
  size_t len;
  constexpr size_t End() const { return begin + len; }
};

// A diagnostic produced while assembling. `line` and `col` are zero-based; `error_line` is the
// full text of the offending source line (possibly including its trailing newline).
struct AssemblerError
{
  std::string message;
  std::string_view error_line;
  size_t line;
  size_t col;
  size_t len;

  // Multi-line, human readable form: location, the source line, a caret marker, the message.
  std::string FormatError() const;
};

// A value annotated with some extra information, such as where it came from.
template <typename Tag, typename T>
using Tagged = std::pair<Tag, T>;

template <typename Tag, typename T>
constexpr const Tag& TagOf(const Tagged<Tag, T>& val)
{
  return val.first;
}
template <typename Tag, typename T>
constexpr Tag& TagOf(Tagged<Tag, T>& val)
{
  return val.first;
}
template <typename Tag, typename T>
constexpr const T& ValueOf(const Tagged<Tag, T>& val)
{
  return val.second;
}
template <typename Tag, typename T>
constexpr T& ValueOf(Tagged<Tag, T>& val)
{
  return val.second;
}

// Either an error or a result.
template <typename T>
using FailureOr = std::variant<AssemblerError, T>;

template <typename T>
constexpr bool IsFailure(const FailureOr<T>& var)
{
  return std::holds_alternative<AssemblerError>(var);
}
template <typename T>
constexpr AssemblerError& GetFailure(FailureOr<T>& var)
{
  return std::get<AssemblerError>(var);
}
template <typename T>
constexpr const AssemblerError& GetFailure(const FailureOr<T>& var)
{
  return std::get<AssemblerError>(var);
}
template <typename T>
constexpr const T& GetT(const FailureOr<T>& var)
{
  return std::get<T>(var);
}
template <typename T>
constexpr T& GetT(FailureOr<T>& var)
{
  return std::get<T>(var);
}

// Assembler directives (".byte", ".align", ...).
enum class GekkoDirective
{
  Byte,
  _2byte,
  _4byte,
  _8byte,
  Float,
  Double,
  Locate,
  PadAlign,
  Align,
  Zeros,
  Skip,
  DefVar,
  Ascii,
  Asciz
};
}  // namespace Common::GekkoAssembler
