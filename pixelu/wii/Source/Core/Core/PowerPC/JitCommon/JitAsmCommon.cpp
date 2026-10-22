// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/JitCommon/JitAsmCommon.h"

#include <cstddef>

namespace
{
// 2^exponent for -32 <= exponent <= 31, exactly representable as a float.
constexpr float PowerOfTwo(int exponent)
{
  float value = 1.0f;
  for (int i = 0; i < exponent; ++i)
    value *= 2.0f;
  for (int i = 0; i > exponent; --i)
    value *= 0.5f;
  return value;
}

// The GQR scale field is a 6-bit two's-complement exponent.
constexpr int ScaleExponent(std::size_t field)
{
  return field < 32 ? static_cast<int>(field) : static_cast<int>(field) - 64;
}

constexpr std::array<float, 128> ScaleTable(int sign)
{
  std::array<float, 128> table{};
  for (std::size_t field = 0; field < 64; ++field)
  {
    const float factor = PowerOfTwo(sign * ScaleExponent(field));
    table[2 * field] = factor;
    table[2 * field + 1] = factor;
  }
  return table;
}

constexpr std::array<u8, 16> SwapLanes(std::size_t lanes)
{
  std::array<u8, 16> mask{};
  for (std::size_t i = 0; i < 16; ++i)
  {
    const std::size_t lane = i / 4;
    mask[i] = static_cast<u8>(lane < lanes ? lane * 4 + 3 - i % 4 : i);
  }
  return mask;
}
}  // namespace

alignas(16) const std::array<u8, 16> pbswapShuffle1x4 = SwapLanes(1);
alignas(16) const std::array<u8, 16> pbswapShuffle2x4 = SwapLanes(2);

alignas(16) const std::array<float, 4> m_one = {1.0f, 0.0f, 0.0f, 0.0f};

alignas(16) const std::array<float, 128> m_quantizeTableS = ScaleTable(1);
alignas(16) const std::array<float, 128> m_dequantizeTableS = ScaleTable(-1);
