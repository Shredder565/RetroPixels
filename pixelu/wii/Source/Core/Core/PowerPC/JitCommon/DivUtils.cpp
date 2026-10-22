// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/JitCommon/DivUtils.h"

#include <algorithm>
#include <bit>

namespace JitCommon
{
// Signed case (the classic "magic number" construction): look for the
// smallest exponent p >= 32 for which 2^p / limit, rounded down, exceeds the
// distance from 2^p to the next multiple of |divisor|. `limit` is the largest
// dividend magnitude whose remainder is |divisor| - 1. The multiplier is then
// floor(2^p / |divisor|) + 1. Every value involved stays below 2^64 because p
// never passes 63 for a valid divisor.
SignedMagic SignedDivisionConstants(s32 divisor)
{
  const bool negative = divisor < 0;
  const u64 magnitude = negative ? u64{0} - static_cast<u64>(static_cast<s64>(divisor)) :
                                   static_cast<u64>(divisor);
  const u64 range = (u64{1} << 31) - (negative ? 1 : 0);
  const u64 limit = range - 1 - range % magnitude;

  u32 p = 32;
  for (;; ++p)
  {
    const u64 power = u64{1} << p;
    const u64 limit_quotient = power / limit;
    const u64 limit_remainder = power % limit;
    const u64 gap = magnitude - power % magnitude;
    const bool too_small =
        limit_quotient < gap || (limit_quotient == gap && limit_remainder == 0);
    if (!too_small)
      break;
  }

  const u32 base = static_cast<u32>((u64{1} << p) / magnitude) + 1;
  SignedMagic result;
  result.multiplier = static_cast<s32>(negative ? u32{0} - base : base);
  result.shift = static_cast<u8>(p - 32);
  return result;
}

// Unsigned case: take m = floor(2^(32+s) / divisor) with s = floor(log2
// divisor). Rounding m up gives an exact quotient for every dividend unless
// the error reaches the largest quotient; that is checked by evaluating the
// rounded-up product at the dividend just below max_quotient * divisor.
// When it holds, the multiplier is m + 1 with common factors of two removed
// from it and the shift; when it fails, m itself is used together with an
// incremented dividend.
UnsignedMagic UnsignedDivisionConstants(u32 divisor)
{
  u32 shift = static_cast<u32>(std::bit_width(divisor)) - 1;
  u32 multiplier = static_cast<u32>((u64{1} << (32 + shift)) / divisor);
  const u32 max_quotient = multiplier >> shift;

  const u64 probe = static_cast<u64>(static_cast<u32>(multiplier + 1)) *
                    static_cast<u64>(static_cast<u32>(max_quotient * divisor - 1));
  const bool exact_round_up = static_cast<u32>(probe >> (shift + 32)) == max_quotient - 1;

  if (exact_round_up)
  {
    ++multiplier;
    const u32 drop = std::min(shift, static_cast<u32>(std::countr_zero(multiplier)));
    multiplier >>= drop;
    shift -= drop;
  }

  UnsignedMagic result;
  result.multiplier = multiplier;
  result.shift = static_cast<u8>(shift);
  result.fast = exact_round_up;
  return result;
}

}  // namespace JitCommon
