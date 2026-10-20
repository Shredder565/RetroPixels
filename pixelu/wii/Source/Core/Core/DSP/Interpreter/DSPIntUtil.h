// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Arithmetic on the 40-bit accumulator format, shared by the interpreter files.

#pragma once

#include "Common/CommonTypes.h"

namespace DSP::Interpreter
{
// The bits an accumulator (or the product register) actually holds.
constexpr u64 ACC40_BITS = 0x0000'00ff'ffff'ffffULL;

// Wraps a 64-bit intermediate result to 40 bits and sign extends bit 39 again,
// which is how the hardware stores any result into $acX.
constexpr s64 WrapTo40Bits(s64 value)
{
  return static_cast<s64>(static_cast<u64>(value) << 24) >> 24;
}

// Rounds a value to a multiple of 0x10000 (to its middle word), with exact halves
// going to the even neighbour.
constexpr s64 RoundToMiddleWord(s64 value)
{
  const s64 half = (value & 0x10000) != 0 ? 0x8000 : 0x7fff;
  return (value + half) & ~s64{0xffff};
}
}  // namespace DSP::Interpreter
