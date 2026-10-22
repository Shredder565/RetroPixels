// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

namespace JitCommon
{
// Replacement for a signed division by a constant: the quotient is the high
// word of dividend * multiplier, shifted right arithmetically by shift and
// corrected by one for negative results.
struct SignedMagic
{
  s32 multiplier;
  u8 shift;
};

// Constants for dividing a s32 by `divisor`. The divisor must not be -1, 0, 1
// or INT_MIN.
SignedMagic SignedDivisionConstants(s32 divisor);

// Replacement for an unsigned division by a constant, computed in 64 bits.
// With `fast` set the quotient is (dividend * multiplier) >> (32 + shift);
// otherwise ((dividend + 1) * multiplier) >> (32 + shift) is needed.
struct UnsignedMagic
{
  u32 multiplier;
  u8 shift;
  bool fast;
};

// Constants for dividing a u32 by `divisor`. The divisor must not be 0, 1 or a
// power of two.
UnsignedMagic UnsignedDivisionConstants(u32 divisor);

}  // namespace JitCommon
