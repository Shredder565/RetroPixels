// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <functional>
#include <iterator>
#include <ranges>
#include <utility>
#include <version>

// Common::Contains(range, value) and Common::ContainsSubrange(range, subrange): the C++23
// std::ranges algorithms, with a fallback for standard libraries that lack them.
namespace Common
{
#if defined(__cpp_lib_ranges_contains) && __cpp_lib_ranges_contains >= 202202L

inline constexpr auto& Contains = std::ranges::contains;
inline constexpr auto& ContainsSubrange = std::ranges::contains_subrange;

#else

struct ContainsFn
{
  template <std::input_iterator I, std::sentinel_for<I> S, class T, class Proj = std::identity>
  requires std::indirect_binary_predicate<std::ranges::equal_to, std::projected<I, Proj>, const T*>
  constexpr bool operator()(I first, S last, const T& value, Proj proj = {}) const
  {
    // Same constraints as ranges::find, so it can do the search.
    const auto found = std::ranges::find(std::move(first), last, value, std::move(proj));
    return found != last;
  }

  template <std::ranges::input_range R, class T, class Proj = std::identity>
  requires std::indirect_binary_predicate<
      std::ranges::equal_to, std::projected<std::ranges::iterator_t<R>, Proj>, const T*>
  constexpr bool operator()(R&& r, const T& value, Proj proj = {}) const
  {
    auto first = std::ranges::begin(r);
    return (*this)(std::move(first), std::ranges::end(r), value, std::move(proj));
  }
};

struct ContainsSubrangeFn
{
  template <std::forward_iterator I1, std::sentinel_for<I1> S1, std::forward_iterator I2,
            std::sentinel_for<I2> S2, class Pred = std::ranges::equal_to,
            class Proj1 = std::identity, class Proj2 = std::identity>
  requires std::indirectly_comparable<I1, I2, Pred, Proj1, Proj2>
  constexpr bool operator()(I1 first1, S1 last1, I2 first2, S2 last2, Pred pred = {},
                            Proj1 proj1 = {}, Proj2 proj2 = {}) const
  {
    // Like the standard version, an empty subrange is always contained.
    if (first2 == last2)
      return true;
    const auto match = std::ranges::search(std::move(first1), std::move(last1), std::move(first2),
                                           std::move(last2), std::move(pred), std::move(proj1),
                                           std::move(proj2));
    return !match.empty();
  }

  template <std::ranges::forward_range R1, std::ranges::forward_range R2,
            class Pred = std::ranges::equal_to, class Proj1 = std::identity,
            class Proj2 = std::identity>
  requires std::indirectly_comparable<std::ranges::iterator_t<R1>, std::ranges::iterator_t<R2>,
                                      Pred, Proj1, Proj2>
  constexpr bool operator()(R1&& r1, R2&& r2, Pred pred = {}, Proj1 proj1 = {},
                            Proj2 proj2 = {}) const
  {
    auto haystack = std::ranges::begin(r1);
    auto needle = std::ranges::begin(r2);
    return (*this)(std::move(haystack), std::ranges::end(r1), std::move(needle),
                   std::ranges::end(r2), std::move(pred), std::move(proj1), std::move(proj2));
  }
};

inline constexpr ContainsFn Contains{};
inline constexpr ContainsSubrangeFn ContainsSubrange{};

#endif
}  // namespace Common
