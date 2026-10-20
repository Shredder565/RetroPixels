// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/JitCommon/ConstantPropagation.h"

#include <bit>

#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"

namespace JitCommon
{
namespace
{
using Result = ConstantPropagationResult;

// An instruction that writes `value` to `gpr` (and CR0 when `rc`).
Result Writes(u32 gpr, u32 value, bool rc = false)
{
  return Result(static_cast<s8>(gpr), value, rc);
}

bool AddCarries(u32 a, u32 b)
{
  return ((u64{a} + u64{b}) >> 32) != 0;
}

bool FitsS32(s64 value)
{
  return value >= -0x80000000LL && value <= 0x7FFFFFFFLL;
}

// Shifted-out bits of an algebraic right shift by n (0..63) of a negative value.
bool ArithmeticShiftCarry(u32 value, u32 n)
{
  if ((value & 0x80000000) == 0 || n == 0)
    return false;
  if (n >= 32)
    return true;
  return (value & ((u32{1} << n) - 1)) != 0;
}

u32 ArithmeticShift(u32 value, u32 n)
{
  const s32 v = static_cast<s32>(value);
  return static_cast<u32>(n >= 32 ? (v < 0 ? -1 : 0) : (v >> n));
}

// Extended opcodes (primary 31, 10-bit field including OE) of the a,b -> d group.
enum : u32
{
  XO_SUBFC = 8,
  XO_ADDC = 10,
  XO_MULHWU = 11,
  XO_SUBF = 40,
  XO_MULHW = 75,
  XO_NEG = 104,
  XO_MULLW = 235,
  XO_ADD = 266,
  XO_DIVWU = 459,
  XO_DIVW = 491,
  XO_SUBFCO = 520,
  XO_ADDCO = 522,
  XO_SUBFO = 552,
  XO_NEGO = 616,
  XO_MULLWO = 747,
  XO_ADDO = 778,
  XO_DIVWUO = 971,
  XO_DIVWO = 1003,
};

// The s,b -> a group.
enum : u32
{
  XO_SLW = 24,
  XO_CNTLZW = 26,
  XO_AND = 28,
  XO_ANDC = 60,
  XO_NOR = 124,
  XO_EQV = 284,
  XO_XOR = 316,
  XO_ORC = 412,
  XO_OR = 444,
  XO_NAND = 476,
  XO_SRW = 536,
  XO_SRAW = 792,
  XO_SRAWI = 824,
  XO_EXTSH = 922,
  XO_EXTSB = 954,
};

bool IsSubtract(u32 xo)
{
  return xo == XO_SUBFC || xo == XO_SUBF || xo == XO_SUBFCO || xo == XO_SUBFO;
}
bool IsMultiply(u32 xo)
{
  return xo == XO_MULHWU || xo == XO_MULHW || xo == XO_MULLW || xo == XO_MULLWO;
}
}  // namespace

ConstantPropagationResult ConstantPropagation::EvaluateInstruction(UGeckoInstruction inst,
                                                                   u64 flags) const
{
  if (inst.OPCD == 31)
    return EvaluateExtended(inst, flags);
  return EvaluatePrimary(inst);
}

ConstantPropagationResult ConstantPropagation::EvaluatePrimary(UGeckoInstruction inst) const
{
  const u32 simm = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
  const std::optional<u32> ra = Known(inst.RA);
  const std::optional<u32> rs = Known(inst.RS);

  switch (inst.OPCD)
  {
  case 7:  // mulli: a zero immediate gives zero whatever rA holds
    if (simm == 0)
      return Writes(inst.RD, 0);
    if (!ra)
      return {};
    return Writes(inst.RD, *ra * simm);

  case 8:  // subfic
  {
    if (!ra)
      return {};
    Result r = Writes(inst.RD, simm - *ra);
    r.carry = simm >= *ra;
    return r;
  }

  case 12:  // addic
  case 13:  // addic.
  {
    if (!ra)
      return {};
    Result r = Writes(inst.RD, *ra + simm, inst.OPCD == 13);
    r.carry = AddCarries(*ra, simm);
    return r;
  }

  case 14:  // addi
  case 15:  // addis
  {
    const u32 imm = inst.OPCD == 15 ? simm << 16 : simm;
    if (inst.RA == 0)
      return Writes(inst.RD, imm);
    if (!ra)
      return {};
    return Writes(inst.RD, *ra + imm);
  }

  case 20:  // rlwimi
  {
    if (!rs)
      return {};
    const u32 mask = MakeRotationMask(inst.MB, inst.ME);
    const u32 rotated = std::rotl(*rs, static_cast<int>(inst.SH));
    if (mask == 0xFFFFFFFF)
      return Writes(inst.RA, rotated, inst.Rc);
    if (!ra)
      return {};
    return Writes(inst.RA, (*ra & ~mask) | (rotated & mask), inst.Rc);
  }

  case 21:  // rlwinm
  case 23:  // rlwnm
  {
    u32 amount = inst.SH;
    if (inst.OPCD == 23)
    {
      const std::optional<u32> rb = Known(inst.RB);
      if (!rb)
        return {};
      amount = *rb & 0x1F;
    }
    if (!rs)
      return {};
    const u32 mask = MakeRotationMask(inst.MB, inst.ME);
    return Writes(inst.RA, std::rotl(*rs, static_cast<int>(amount)) & mask, inst.Rc);
  }

  case 24:  // ori
  case 25:  // oris
  case 26:  // xori
  case 27:  // xoris
  case 28:  // andi.
  case 29:  // andis.
  {
    const bool is_and = inst.OPCD >= 28;
    // "ori rX, rX, 0" style no-ops (but never andi., which still sets CR0).
    if (!is_and && inst.UIMM == 0 && inst.RA == inst.RS)
    {
      Result r;
      r.instruction_fully_executed = true;
      return r;
    }
    if (!rs)
      return {};
    const u32 imm = (inst.OPCD & 1) ? u32{inst.UIMM} << 16 : u32{inst.UIMM};
    u32 value;
    if (inst.OPCD <= 25)
      value = *rs | imm;
    else if (inst.OPCD <= 27)
      value = *rs ^ imm;
    else
      value = *rs & imm;
    return Writes(inst.RA, value, is_and);
  }

  default:
    return {};
  }
}

ConstantPropagationResult ConstantPropagation::EvaluateExtended(UGeckoInstruction inst,
                                                                u64 flags) const
{
  if (flags & FL_IN_B)
  {
    if (flags & FL_OUT_D)
      return EvaluateArithmetic(inst, flags);
    return EvaluateLogical(inst);
  }
  return EvaluateUnary(inst, flags);
}

// rD = f(rA, rB)
ConstantPropagationResult ConstantPropagation::EvaluateArithmetic(UGeckoInstruction inst,
                                                                  u64 flags) const
{
  const u32 xo = inst.SUBOP10;
  const std::optional<u32> ka = Known(inst.RA);
  const std::optional<u32> kb = Known(inst.RB);
  const bool sets_ca = (flags & FL_SET_CA) != 0;
  const bool sets_ov = (flags & FL_SET_OE) != 0;

  if (ka && kb)
  {
    const u32 a = *ka;
    const u32 b = *kb;
    u32 value;
    bool carry = false;
    bool overflow = false;
    switch (xo)
    {
    case XO_SUBFC:
    case XO_SUBF:
    case XO_SUBFCO:
    case XO_SUBFO:
      value = b - a;
      carry = AddCarries(~a, b) || (~a + b) == 0xFFFFFFFF;
      overflow = !FitsS32(s64{static_cast<s32>(b)} - s64{static_cast<s32>(a)});
      break;
    case XO_ADDC:
    case XO_ADDCO:
    case XO_ADD:
    case XO_ADDO:
      value = a + b;
      carry = AddCarries(a, b);
      overflow = !FitsS32(s64{static_cast<s32>(a)} + s64{static_cast<s32>(b)});
      break;
    case XO_MULHWU:
      value = static_cast<u32>((u64{a} * u64{b}) >> 32);
      break;
    case XO_MULHW:
      value = static_cast<u32>(
          static_cast<u64>(s64{static_cast<s32>(a)} * s64{static_cast<s32>(b)}) >> 32);
      break;
    case XO_MULLW:
    case XO_MULLWO:
    {
      const s64 product = s64{static_cast<s32>(a)} * s64{static_cast<s32>(b)};
      value = static_cast<u32>(product);
      carry = (static_cast<u64>(product) >> 32) != 0;
      overflow = !FitsS32(product);
      break;
    }
    case XO_DIVWU:
    case XO_DIVWUO:
      value = b == 0 ? 0 : a / b;
      carry = b == 0;
      overflow = b == 0;
      break;
    case XO_DIVW:
    case XO_DIVWO:
    {
      const bool undefined = b == 0 || (a == 0x80000000 && b == 0xFFFFFFFF);
      if (undefined)
      {
        value = (a & 0x80000000) ? 0xFFFFFFFF : 0;
        carry = (a & 0x80000000) == 0;
        overflow = true;
      }
      else
      {
        const s32 q = static_cast<s32>(a) / static_cast<s32>(b);
        value = static_cast<u32>(q);
        carry = q < 0;
      }
      break;
    }
    default:
      return {};
    }

    Result r = Writes(inst.RD, value, inst.Rc);
    if (sets_ca)
      r.carry = carry;
    if (sets_ov)
      r.overflow = overflow;
    return r;
  }

  if (ka || kb)
  {
    const u32 known = ka ? *ka : *kb;
    const bool known_is_b = !ka;

    // A zero factor makes any product zero.
    if (IsMultiply(xo) && known == 0)
    {
      Result r = Writes(inst.RD, 0, inst.Rc);
      if (sets_ov)
        r.overflow = false;
      return r;
    }
    // Unsigned division by zero gives zero and sets OV.
    if ((xo == XO_DIVWU || xo == XO_DIVWUO) && known_is_b && known == 0)
    {
      Result r = Writes(inst.RD, 0, inst.Rc);
      if (sets_ov)
        r.overflow = true;
      return r;
    }
    // Zero divided by anything is zero, unless OV must report a zero divisor.
    if ((xo == XO_DIVWU || xo == XO_DIVWUO || xo == XO_DIVW || xo == XO_DIVWO) && !known_is_b &&
        known == 0 && !sets_ov)
    {
      return Writes(inst.RD, 0, inst.Rc);
    }
    return {};
  }

  // x - x == 0 with the carry set and no overflow.
  if (inst.RA == inst.RB && IsSubtract(xo))
  {
    Result r = Writes(inst.RD, 0, inst.Rc);
    if (sets_ca)
      r.carry = true;
    if (sets_ov)
      r.overflow = false;
    return r;
  }
  return {};
}

// rA = f(rS, rB)
ConstantPropagationResult ConstantPropagation::EvaluateLogical(UGeckoInstruction inst) const
{
  const u32 xo = inst.SUBOP10;
  const std::optional<u32> ks = Known(inst.RS);
  const std::optional<u32> kb = Known(inst.RB);

  if (ks && kb)
  {
    const u32 s = *ks;
    const u32 b = *kb;
    const u32 n = b & 0x3F;
    switch (xo)
    {
    case XO_SLW:
      return Writes(inst.RA, n >= 32 ? 0 : s << n, inst.Rc);
    case XO_SRW:
      return Writes(inst.RA, n >= 32 ? 0 : s >> n, inst.Rc);
    case XO_AND:
      return Writes(inst.RA, s & b, inst.Rc);
    case XO_ANDC:
      return Writes(inst.RA, s & ~b, inst.Rc);
    case XO_NOR:
      return Writes(inst.RA, ~(s | b), inst.Rc);
    case XO_EQV:
      return Writes(inst.RA, ~(s ^ b), inst.Rc);
    case XO_XOR:
      return Writes(inst.RA, s ^ b, inst.Rc);
    case XO_ORC:
      return Writes(inst.RA, s | ~b, inst.Rc);
    case XO_OR:
      return Writes(inst.RA, s | b, inst.Rc);
    case XO_NAND:
      return Writes(inst.RA, ~(s & b), inst.Rc);
    case XO_SRAW:
    {
      Result r = Writes(inst.RA, ArithmeticShift(s, n), inst.Rc);
      r.carry = ArithmeticShiftCarry(s, n);
      return r;
    }
    default:
      return {};
    }
  }

  if (ks || kb)
  {
    const bool known_is_b = !ks;
    const u32 known = ks ? *ks : *kb;
    // For the complemented forms, look at the operand as it enters the AND/OR.
    const u32 effective = known_is_b && (xo == XO_ANDC || xo == XO_ORC) ? ~known : known;
    switch (xo)
    {
    case XO_SLW:
    case XO_SRW:
      // Shifting zero, or shifting by 32..63, gives zero.
      if ((!known_is_b && known == 0) || (known_is_b && (known & 0x20) != 0))
        return Writes(inst.RA, 0, inst.Rc);
      return {};
    case XO_AND:
    case XO_ANDC:
      if (effective == 0)
        return Writes(inst.RA, 0, inst.Rc);
      return {};
    case XO_NOR:
      if (known == 0xFFFFFFFF)
        return Writes(inst.RA, 0, inst.Rc);
      return {};
    case XO_OR:
    case XO_ORC:
      if (effective == 0xFFFFFFFF)
        return Writes(inst.RA, 0xFFFFFFFF, inst.Rc);
      return {};
    case XO_NAND:
      if (known == 0)
        return Writes(inst.RA, 0xFFFFFFFF, inst.Rc);
      return {};
    case XO_SRAW:
      if (!known_is_b && known == 0)
      {
        Result r = Writes(inst.RA, 0, inst.Rc);
        r.carry = false;
        return r;
      }
      return {};
    default:
      return {};
    }
  }

  // Both operands are the same unknown register.
  if (inst.RS == inst.RB)
  {
    switch (xo)
    {
    case XO_ANDC:
    case XO_XOR:
      return Writes(inst.RA, 0, inst.Rc);
    case XO_EQV:
    case XO_ORC:
      return Writes(inst.RA, 0xFFFFFFFF, inst.Rc);
    default:
      return {};
    }
  }
  return {};
}

// One register operand: neg (rA -> rD) or the rS -> rA group.
ConstantPropagationResult ConstantPropagation::EvaluateUnary(UGeckoInstruction inst,
                                                             u64 flags) const
{
  const u32 xo = inst.SUBOP10;
  if (xo == XO_NEG || xo == XO_NEGO)
  {
    const std::optional<u32> ka = Known(inst.RA);
    if (!ka)
      return {};
    Result r = Writes(inst.RD, u32{0} - *ka, inst.Rc);
    if (flags & FL_SET_OE)
      r.overflow = *ka == 0x80000000;
    return r;
  }

  const std::optional<u32> ks = Known(inst.RS);
  if (!ks)
    return {};
  const u32 s = *ks;
  switch (xo)
  {
  case XO_CNTLZW:
    return Writes(inst.RA, static_cast<u32>(std::countl_zero(s)), inst.Rc);
  case XO_SRAWI:
  {
    Result r = Writes(inst.RA, ArithmeticShift(s, inst.SH), inst.Rc);
    r.carry = ArithmeticShiftCarry(s, inst.SH);
    return r;
  }
  case XO_EXTSH:
    return Writes(inst.RA, static_cast<u32>(static_cast<s32>(static_cast<s16>(s))), inst.Rc);
  case XO_EXTSB:
    return Writes(inst.RA, static_cast<u32>(static_cast<s32>(static_cast<s8>(s))), inst.Rc);
  default:
    return {};
  }
}

void ConstantPropagation::Apply(ConstantPropagationResult result)
{
  if (result.gpr >= 0)
    SetGPR(static_cast<size_t>(result.gpr), result.gpr_value);
}

}  // namespace JitCommon
