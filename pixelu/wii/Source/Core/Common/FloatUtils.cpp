// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/FloatUtils.h"

#include <bit>
#include <cmath>
#include <limits>

namespace Common
{
namespace
{
template <typename Bits>
u32 Classify(Bits bits, Bits sign_mask, Bits exp_mask, Bits frac_mask)
{
  const bool negative = (bits & sign_mask) != 0;
  const Bits exponent = bits & exp_mask;
  const Bits fraction = bits & frac_mask;

  if (exponent == exp_mask)
  {
    if (fraction != 0)
      return PPC_FPCLASS_QNAN;
    return negative ? PPC_FPCLASS_NINF : PPC_FPCLASS_PINF;
  }
  if (exponent != 0)
    return negative ? PPC_FPCLASS_NN : PPC_FPCLASS_PN;
  if (fraction != 0)
    return negative ? PPC_FPCLASS_ND : PPC_FPCLASS_PD;
  return negative ? PPC_FPCLASS_NZ : PPC_FPCLASS_PZ;
}

constexpr u64 EXP_FIELD_MAX = 0x7FF;
constexpr int FRAC_BITS = 52;
constexpr u64 FRAC_MASK = (u64{1} << FRAC_BITS) - 1;
}  // namespace

u32 ClassifyDouble(double dvalue)
{
  return Classify<u64>(std::bit_cast<u64>(dvalue), DOUBLE_SIGN, DOUBLE_EXP, DOUBLE_FRAC);
}

u32 ClassifyFloat(float fvalue)
{
  return Classify<u32>(std::bit_cast<u32>(fvalue), FLOAT_SIGN, FLOAT_EXP, FLOAT_FRAC);
}

// The estimate tables of the Broadway's frsqrte and fres units, as measured on hardware.
// frsqrte: 16 segments for even and 16 for odd exponents; fres: 32 segments of the mantissa.
const std::array<BaseAndDec, 32> frsqrte_expected = {{
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
}};

const std::array<BaseAndDec, 32> fres_expected = {{
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
    {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
    {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
    {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
    {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
}};

double ApproximateReciprocalSquareRoot(double val)
{
  const u64 bits = std::bit_cast<u64>(val);
  const u64 sign = bits & DOUBLE_SIGN;
  s64 exponent = static_cast<s64>((bits & DOUBLE_EXP) >> FRAC_BITS);
  u64 fraction = bits & FRAC_MASK;

  if (exponent == 0 && fraction == 0)
    return sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();

  if (exponent == static_cast<s64>(EXP_FIELD_MAX))
  {
    if (fraction != 0)
      return MakeQuiet(val);
    // 1/sqrt(+inf) = +0; 1/sqrt(-inf) is invalid.
    return sign ? std::numeric_limits<double>::quiet_NaN() : 0.0;
  }

  if (sign)
    return std::numeric_limits<double>::quiet_NaN();

  if (exponent == 0)
  {
    // Denormal: shift the fraction up until it has an implicit leading one, lowering the
    // exponent accordingly (it may go to zero or below).
    exponent = 1;
    while ((fraction & (u64{1} << FRAC_BITS)) == 0)
    {
      fraction <<= 1;
      --exponent;
    }
    fraction &= FRAC_MASK;
  }

  // Negate and halve the unbiased exponent: e' = 1023 - (e - 1022) / 2, rounded down and kept
  // to 11 bits, i.e. floor((2 * 1023 + 1022 - e) / 2).
  const u64 result_exponent =
      static_cast<u64>((2 * 0x3FF + 0x3FE - exponent) >> 1) & EXP_FIELD_MAX;

  // The table is indexed by the exponent's parity and the top 4 fraction bits; the next 11
  // fraction bits interpolate within the segment.
  const u32 index = static_cast<u32>(((static_cast<u64>(exponent) & 1) << 15) | (fraction >> 37));
  const BaseAndDec& segment = frsqrte_expected[index >> 11];
  const s64 estimate = segment.m_base + segment.m_dec * static_cast<int>(index & 0x7FF);

  const u64 result = sign | (result_exponent << FRAC_BITS) | (static_cast<u64>(estimate) << 26);
  return std::bit_cast<double>(result);
}

double ApproximateReciprocal(double val)
{
  const u64 bits = std::bit_cast<u64>(val);
  const u64 sign = bits & DOUBLE_SIGN;
  const u64 exponent = (bits & DOUBLE_EXP) >> FRAC_BITS;
  const u64 fraction = bits & FRAC_MASK;

  if (exponent == 0 && fraction == 0)
    return std::copysign(std::numeric_limits<double>::infinity(), val);

  if (exponent == EXP_FIELD_MAX)
  {
    if (fraction != 0)
      return MakeQuiet(val);
    return std::copysign(0.0, val);
  }

  // Results outside the single-precision range saturate (to FLT_MAX) or flush to zero.
  if (exponent < 895)
    return std::copysign(static_cast<double>(std::numeric_limits<float>::max()), val);
  if (exponent >= 1149)
    return std::copysign(0.0, val);

  const u64 result_exponent = 0x7FD - exponent;

  const u32 index = static_cast<u32>(fraction >> 37);  // top 15 fraction bits
  const BaseAndDec& segment = fres_expected[index >> 10];
  const int step = static_cast<int>(index & 0x3FF);
  const s64 estimate = segment.m_base - (segment.m_dec * step + 1) / 2;

  const u64 result = sign | (result_exponent << FRAC_BITS) | (static_cast<u64>(estimate) << 29);
  return std::bit_cast<double>(result);
}
}  // namespace Common
