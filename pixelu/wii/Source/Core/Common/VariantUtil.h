// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <utility>
#include <variant>

namespace detail
{
template <typename... From>
struct VariantCastProxy
{
  const std::variant<From...>& v;

  // Converts to any variant whose alternatives include the held alternative's type.
  template <typename... To>
  operator std::variant<To...>() const
  {
    return std::visit([](const auto& held) { return std::variant<To...>{held}; }, v);
  }
};
}  // namespace detail

// Converts a variant to another variant type that can hold each of its alternatives:
//   std::variant<A, B, C> wide = VariantCast(std::variant<A, C>{...});
template <typename... From>
auto VariantCast(const std::variant<From...>& v)
{
  return detail::VariantCastProxy<From...>{v};
}

// Builds a visitor from several lambdas: std::visit(overloaded{[](int) {}, [](float) {}}, v).
template <class... Ts>
struct overloaded : Ts...
{
  using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Calls func.template operator()<Alternative>() for the variant alternative with the given index;
// does nothing if the index is out of range.
template <typename Variant, std::size_t I = 0, typename Func>
void WithVariantAlternative(std::size_t index, Func&& func)
{
  if constexpr (I < std::variant_size_v<Variant>)
  {
    if (index == I)
      func.template operator()<std::variant_alternative_t<I, Variant>>();
    else
      WithVariantAlternative<Variant, I + 1>(index, std::forward<Func>(func));
  }
}
