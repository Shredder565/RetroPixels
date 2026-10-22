// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Predicates behind the carry, overflow and "over 32 bits" status flags.

#pragma once

#include "Common/CommonTypes.h"

namespace DSP::Interpreter
{
// An addition carried out of the top bit when the (unsigned) result came out
// below the first operand.
constexpr bool CarriedOnAdd(u64 first, u64 result)
{
  return result < first;
}

// A subtraction "carries" (does not borrow) unless the result went above the
// minuend; an unchanged value counts as carry too.
constexpr bool CarriedOnSubtract(u64 minuend, u64 result)
{
  return result <= minuend;
}

// Two's complement overflow of result = a + b: both inputs have the same sign
// and the result has the other one.
constexpr bool SignedOverflow(s64 a, s64 b, s64 result)
{
  return ((result ^ a) & (result ^ b)) < 0;
}

// The value needs more than 32 bits (as a signed number).
constexpr bool ExceedsS32(s64 value)
{
  return static_cast<s64>(static_cast<s32>(value)) != value;
}
}  // namespace DSP::Interpreter
