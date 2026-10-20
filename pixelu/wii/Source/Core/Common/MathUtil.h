// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"

namespace MathUtil
{
constexpr double TAU = 6.2831853071795865;
constexpr double PI = TAU / 2;
// Standard gravity in m/s^2.
constexpr double GRAVITY_ACCELERATION = 9.80665;

// -1, 0 or 1.
template <typename T>
constexpr auto Sign(const T& val) -> decltype((T{} < val) - (val < T{}))
{
  return (T{} < val) - (val < T{});
}

// Linear interpolation from x (a = 0) to y (a = 1).
template <typename T, typename F>
constexpr auto Lerp(const T& x, const T& y, const F& a) -> decltype(x + (y - x) * a)
{
  return x + (y - x) * a;
}

// Converts to an integer type, clamping to its range. NaN gives an unspecified result.
template <std::integral Dest, typename T>
constexpr Dest SaturatingCast(T value)
{
  constexpr Dest smallest = std::numeric_limits<Dest>::min();
  constexpr Dest largest = std::numeric_limits<Dest>::max();
  if constexpr (std::is_integral_v<T>)
  {
    if (std::cmp_less(value, smallest))
      return smallest;
    if (std::cmp_greater(value, largest))
      return largest;
  }
  else
  {
    // `largest` may not be exactly representable in T, so anything at or above its rounded
    // value saturates.
    if (value < static_cast<T>(smallest))
      return smallest;
    if (value >= static_cast<T>(largest))
      return largest;
  }
  return static_cast<Dest>(value);
}

template <typename T>
constexpr bool IsPow2(T imm)
{
  return imm > 0 && (imm & (imm - 1)) == 0;
}

// Smallest power of two >= value (0 for 0 and for values above 2^31).
constexpr u32 NextPowerOf2(u32 value)
{
  if (value <= 1)
    return value;
  if (value > 0x80000000u)
    return 0;
  return std::bit_ceil(value);
}

template <class T>
struct Rectangle
{
  T left{};
  T top{};
  T right{};
  T bottom{};

  constexpr Rectangle() = default;

  constexpr Rectangle(T l, T t, T r, T b) : left(l), top(t), right(r), bottom(b) {}

  // Member-wise, in declaration order.
  constexpr bool operator==(const Rectangle&) const = default;

  constexpr T GetWidth() const { return Distance(left, right); }
  constexpr T GetHeight() const { return Distance(top, bottom); }

  // Clamp for coordinate systems with the origin at the lower left (y1 is the top).
  void ClampLL(T x1, T y1, T x2, T y2)
  {
    left = std::clamp(left, x1, x2);
    right = std::clamp(right, x1, x2);
    top = std::clamp(top, y2, y1);
    bottom = std::clamp(bottom, y2, y1);
  }

  // Clamp for coordinate systems with the origin at the upper left (y1 is the top).
  void ClampUL(T x1, T y1, T x2, T y2)
  {
    left = std::clamp(left, x1, x2);
    right = std::clamp(right, x1, x2);
    top = std::clamp(top, y1, y2);
    bottom = std::clamp(bottom, y1, y2);
  }

private:
  static constexpr T Distance(T a, T b)
  {
    if constexpr (std::is_unsigned_v<T>)
      return a < b ? b - a : a - b;
    else
      return std::abs(b - a);
  }
};

// Mean of a stream of samples, updated incrementally.
template <typename T>
class RunningMean
{
public:
  constexpr void Clear() { *this = {}; }

  constexpr void Push(T x)
  {
    ++m_count;
    m_mean = m_mean + (x - m_mean) / m_count;
  }

  constexpr size_t Count() const { return m_count; }
  constexpr T Mean() const { return m_mean; }

private:
  size_t m_count = 0;
  T m_mean{};
};

// Mean and variance of a stream of samples (Welford's method).
template <typename T>
class RunningVariance
{
public:
  constexpr void Clear() { *this = {}; }

  constexpr void Push(T x)
  {
    const T previous_mean = m_mean.Mean();
    m_mean.Push(x);
    m_sum_of_squares += (x - previous_mean) * (x - m_mean.Mean());
  }

  constexpr size_t Count() const { return m_mean.Count(); }
  constexpr T Mean() const { return m_mean.Mean(); }

  // Sample variance (divides by n - 1).
  constexpr T Variance() const { return m_sum_of_squares / (Count() - 1); }
  T StandardDeviation() const { return std::sqrt(Variance()); }

  // Population variance (divides by n).
  constexpr T PopulationVariance() const { return m_sum_of_squares / Count(); }
  T PopulationStandardDeviation() const { return std::sqrt(PopulationVariance()); }

private:
  RunningMean<T> m_mean;
  T m_sum_of_squares{};
};

// floor(log2(val)); val must not be 0.
constexpr int IntLog2(u64 val)
{
  return std::bit_width(val) - 1;
}
}  // namespace MathUtil
