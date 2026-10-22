// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <type_traits>

#include <fmt/format.h>

#include "Common/EnumMap.h"

// Base for fmt::formatter specialisations of enums. A specialisation passes one name per value
// (nullptr for gaps) to the constructor:
//
//   template <>
//   struct fmt::formatter<MyEnum> : EnumFormatter<MyEnum::LastValue>
//   {
//     static constexpr array_type names = {"First", nullptr, "Third"};
//     constexpr formatter() : EnumFormatter(names) {}
//   };
//
// Format specs: {} or {:u} gives "Name (3)", {:n} gives "Name", and {:s} gives "0x3u /* Name */"
// for generated shader code. Values without a name print as "Invalid".
template <Common::Enum auto last_member>
class EnumFormatter
{
  using EnumType = decltype(last_member);

public:
  constexpr auto parse(fmt::format_parse_context& ctx)
  {
    auto it = ctx.begin();
    if (it != ctx.end() && (*it == 'u' || *it == 'n' || *it == 's'))
    {
      format_type = *it;
      ++it;
    }
    return it;
  }

  template <typename FormatContext>
  auto format(const EnumType& value, FormatContext& ctx) const
  {
    using Signed = std::underlying_type_t<EnumType>;
    using Unsigned = std::make_unsigned_t<EnumType>;  // also valid for bool-based enums
    const Signed number = static_cast<Signed>(value);
    const char* name = m_names.InBounds(value) ? m_names[value] : nullptr;

    if (format_type == 's')
    {
      if (name == nullptr)
        return fmt::format_to(ctx.out(), "{:#x}u /* Invalid */", static_cast<Unsigned>(number));
      return fmt::format_to(ctx.out(), "{:#x}u /* {} */", static_cast<Unsigned>(number), name);
    }
    if (name == nullptr)
      return fmt::format_to(ctx.out(), "Invalid ({})", number);
    if (format_type == 'n')
      return fmt::format_to(ctx.out(), "{}", name);
    return fmt::format_to(ctx.out(), "{} ({})", name, number);
  }

protected:
  // An EnumMap rather than a std::array, so that a braced list containing nullptr works.
  using array_type = Common::EnumMap<const char*, last_member>;

  constexpr explicit EnumFormatter(const array_type names) : m_names(names) {}

  const array_type m_names;
  char format_type = 'u';
};
