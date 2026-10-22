// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

namespace PowerPC
{
// Bits of one 4-bit CR field as the architecture numbers them (LT is the most
// significant bit of the field), plus the bit positions the emulated form
// below uses for SO and LT.
enum CRBits
{
  CR_SO = 1,
  CR_EQ = 2,
  CR_GT = 4,
  CR_LT = 8,

  CR_SO_BIT = 0,
  CR_EQ_BIT = 1,
  CR_GT_BIT = 2,
  CR_LT_BIT = 3,

  CR_EMU_SO_BIT = 59,
  CR_EMU_LT_BIT = 62,
};

// The eight CR fields are kept as one 64-bit word each, laid out so that a
// compare result can be produced cheaply by the JITs:
//
//   EQ is set  <=>  the low 32 bits are all zero
//   GT is set  <=>  the word, read as signed, is greater than zero
//   LT is set  <=>  bit 62 is one
//   SO is set  <=>  bit 59 is one
//
// A 32-bit result sign-extended to 64 bits is therefore directly a valid
// field for a signed compare against zero (LT comes from the sign copies in
// bit 62, EQ and GT follow from the value). The JITs rely on this encoding
// and on the bit numbers above, and savestates store the words as they are.
struct ConditionRegister
{
  // Emulated word for each of the 16 possible 4-bit field values.
  static const std::array<u64, 16> s_crTable;

  u64 fields[8];

  // Builds the emulated word for a 4-bit field value. Bit 32 is always set so
  // the word is never zero, and bit 0 carries "not EQ" (so the low half is
  // zero exactly when EQ is wanted). When GT is clear the sign bit is set so
  // the word reads as negative; when GT is set the word is positive because
  // bit 32 is set.
  static constexpr u64 PPCToInternal(u8 value)
  {
    const bool so = (value & CR_SO) != 0;
    const bool eq = (value & CR_EQ) != 0;
    const bool gt = (value & CR_GT) != 0;
    const bool lt = (value & CR_LT) != 0;

    u64 word = u64{1} << 32;
    if (!eq)
      word |= 1;
    if (!gt)
      word |= u64{1} << 63;
    if (lt)
      word |= u64{1} << CR_EMU_LT_BIT;
    if (so)
      word |= u64{1} << CR_EMU_SO_BIT;
    return word;
  }

  // Field access goes through the conversion, so it is slower than working
  // on the words directly.
  void SetField(u32 cr_field, u32 value) { fields[cr_field] = s_crTable[value]; }

  u32 GetField(u32 cr_field) const
  {
    const u64 word = fields[cr_field];
    u32 result = 0;
    if (word & (u64{1} << CR_EMU_SO_BIT))
      result |= CR_SO;
    if (static_cast<u32>(word) == 0)
      result |= CR_EQ;
    if (static_cast<s64>(word) > 0)
      result |= CR_GT;
    if (word & (u64{1} << CR_EMU_LT_BIT))
      result |= CR_LT;
    return result;
  }

  // CR bit numbers count from the most significant bit of CR0 (bit 0 = CR0
  // LT), as in the crxxx instructions.
  u32 GetBit(u32 bit) const
  {
    const u32 mask = 8u >> (bit & 3);
    return (GetField(bit >> 2) & mask) != 0 ? 1 : 0;
  }

  void SetBit(u32 bit, u32 value)
  {
    const u32 field = bit >> 2;
    const u32 mask = 8u >> (bit & 3);
    const u32 old_value = GetField(field);
    SetField(field, (value & 1) != 0 ? (old_value | mask) : (old_value & ~mask));
  }

  // Whole-register conversion (CR0 in the top nibble). Slow; avoid in hot
  // paths.
  void Set(u32 new_cr);
  u32 Get() const;
};

}  // namespace PowerPC
