// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Floating point helpers shared by the interpreter's scalar, paired-single and load/store code:
// FPSCR exception bookkeeping, the Gekko's rounding to single precision, its handling of NaNs
// and invalid operations, and the bit-level float <-> double conversions used by lfs/stfs.

#pragma once

#include <bit>
#include <cmath>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/FloatUtils.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/PowerPC.h"

namespace InterpreterFP
{
// The default NaN the Gekko produces for invalid operations (positive, quiet, no payload).
inline constexpr u64 DEFAULT_NAN_BITS = 0x7FF8000000000000ULL;

inline double DefaultNaN()
{
  return std::bit_cast<double>(DEFAULT_NAN_BITS);
}

inline u64 Bits(double d)
{
  return std::bit_cast<u64>(d);
}

// ---------------------------------------------------------------------------------------------
// FPSCR bookkeeping

// Recomputes the two summary bits (VX: any invalid-operation flag; FEX: any exception flag whose
// enable bit is set) and raises a program exception if FEX is set while MSR[FE0]/MSR[FE1] allow
// floating point exceptions to be taken.
inline void RefreshFPSummary(PowerPC::PowerPCState& ppc_state)
{
  UReg_FPSCR& fpscr = ppc_state.fpscr;
  fpscr.VX = (fpscr.Hex & FPSCR_VX_ANY) != 0;
  // The enable bits (VE..XE, bits 7..3) sit 22 places below their flags (VX..XX, bits 29..25).
  fpscr.FEX = ((fpscr.Hex >> 22) & fpscr.Hex & FPSCR_ANY_E) != 0;
  if (fpscr.FEX && (ppc_state.msr.FE0 || ppc_state.msr.FE1))
    GenerateProgramException(ppc_state, ProgramExceptionCause::FloatingPoint);
}

// Sets the given sticky exception flag(s). FX records that at least one of them was newly set.
inline void RaiseFP(PowerPC::PowerPCState& ppc_state, u32 flags)
{
  if ((ppc_state.fpscr.Hex & flags) != flags)
    ppc_state.fpscr.FX = 1;
  ppc_state.fpscr.Hex |= flags;
  RefreshFPSummary(ppc_state);
}

// Sets FPSCR[FI]; an inexact result also raises the sticky XX flag.
inline void SetInexact(PowerPC::PowerPCState& ppc_state, bool inexact)
{
  if (inexact)
    RaiseFP(ppc_state, FPSCR_XX);
  ppc_state.fpscr.FI = inexact;
}

// ---------------------------------------------------------------------------------------------
// Precision

// Rounds a double result to single precision the way the Gekko does. In non-IEEE mode (NI) a
// value whose magnitude is below the smallest normal single becomes a signed zero before
// rounding, so a value that would only reach the normal range by rounding up is still flushed.
inline float RoundToSingle(const UReg_FPSCR& fpscr, double value)
{
  if (fpscr.NI)
  {
    constexpr u64 SMALLEST_NORMAL_SINGLE_AS_DOUBLE = 0x3810000000000000ULL;  // 2^-126
    const u64 bits = Bits(value);
    if ((bits & ~Common::DOUBLE_SIGN) < SMALLEST_NORMAL_SINGLE_AS_DOUBLE)
      return std::bit_cast<float>(static_cast<u32>((bits & Common::DOUBLE_SIGN) >> 32));
  }

  float result = static_cast<float>(value);
  // Without host flush-to-zero support the flush has to be done by hand.
  if (fpscr.NI && !cpu_info.bFlushToZero)
    result = Common::FlushToZero(result);
  return result;
}

// Double precision results only need the non-IEEE flush when the host cannot do it.
inline double RoundToDouble(const UReg_FPSCR& fpscr, double value)
{
  if (fpscr.NI && !cpu_info.bFlushToZero)
    return Common::FlushToZero(value);
  return value;
}

// The single precision multiplier of the Gekko takes frC with only 24 fraction bits (25 bits of
// significand counting the implicit one): the low 28 fraction bits are rounded away, ties upward
// in magnitude, independent of the rounding mode. A carry may ripple into the exponent (and, for
// NaNs with every fraction bit set, beyond it, exactly as the integer addition does).
// Double denormals are first normalised, which moves the rounding position down by however many
// leading zero fraction bits they have.
inline double RoundMultiplierInput(double c)
{
  u64 bits = Bits(c);
  const u64 frac = bits & Common::DOUBLE_FRAC;
  int dropped = 28;
  if ((bits & Common::DOUBLE_EXP) == 0 && frac != 0)
    dropped -= std::countl_zero(frac) - (64 - Common::DOUBLE_FRAC_WIDTH - 1);
  if (dropped > 0)
  {
    const u64 half = u64{1} << (dropped - 1);
    bits = (bits + half) & ~((half << 1) - 1);
  }
  return std::bit_cast<double>(bits);
}

// ---------------------------------------------------------------------------------------------
// Arithmetic with the Gekko's NaN and invalid-operation rules

struct FPResult
{
  double value = 0.0;
  // The last FPSCR exception flag this operation raised (0 if none).
  u32 exception = 0;

  bool IsInvalid() const { return (exception & FPSCR_VX_ANY) != 0; }
};

// Records a raised flag in the result as well as in FPSCR.
inline void Raise(PowerPC::PowerPCState& ppc_state, FPResult& result, u32 flag)
{
  result.exception = flag;
  RaiseFP(ppc_state, flag);
}

// Common NaN handling of add/sub/mul/div: an SNaN operand raises VXSNAN, FI/FR are cleared, and
// the result is the first NaN operand made quiet. Returns false when neither operand is a NaN
// (the NaN came from an invalid operation, which the caller classifies).
inline bool PropagateNaN2(PowerPC::PowerPCState& ppc_state, FPResult& result, double a, double b)
{
  if (Common::IsSNAN(a) || Common::IsSNAN(b))
    Raise(ppc_state, result, FPSCR_VXSNAN);
  ppc_state.fpscr.ClearFIFR();

  if (std::isnan(a))
  {
    result.value = Common::MakeQuiet(a);
    return true;
  }
  if (std::isnan(b))
  {
    result.value = Common::MakeQuiet(b);
    return true;
  }
  result.value = DefaultNaN();
  return false;
}

inline FPResult Mul(PowerPC::PowerPCState& ppc_state, double a, double b)
{
  FPResult result{a * b};
  if (std::isnan(result.value) && !PropagateNaN2(ppc_state, result, a, b))
    Raise(ppc_state, result, FPSCR_VXIMZ);  // infinity * 0
  return result;
}

inline FPResult Div(PowerPC::PowerPCState& ppc_state, double a, double b)
{
  FPResult result{a / b};
  if (std::isnan(result.value))
  {
    if (!PropagateNaN2(ppc_state, result, a, b))
    {
      if (b == 0.0)
        Raise(ppc_state, result, FPSCR_VXZDZ);  // 0 / 0
      else if (std::isinf(a) && std::isinf(b))
        Raise(ppc_state, result, FPSCR_VXIDI);  // infinity / infinity
    }
  }
  else if (std::isinf(result.value) && b == 0.0)
  {
    Raise(ppc_state, result, FPSCR_ZX);
  }
  return result;
}

// a + b, or a - b when subtract is set.
inline FPResult AddSub(PowerPC::PowerPCState& ppc_state, double a, double b, bool subtract)
{
  FPResult result{subtract ? a - b : a + b};
  if (std::isnan(result.value))
  {
    if (!PropagateNaN2(ppc_state, result, a, b))
      Raise(ppc_state, result, FPSCR_VXISI);  // infinity - infinity
    return result;
  }
  if (std::isinf(a) || std::isinf(b))
    ppc_state.fpscr.ClearFIFR();
  return result;
}

inline FPResult Add(PowerPC::PowerPCState& ppc_state, double a, double b)
{
  return AddSub(ppc_state, a, b, false);
}

inline FPResult Sub(PowerPC::PowerPCState& ppc_state, double a, double b)
{
  return AddSub(ppc_state, a, b, true);
}

// Fused a * c + b (or a * c - b). The Gekko rounds only once, also in the single precision
// forms, whose operands may still carry double precision bits; frC is first cut to 24 fraction
// bits (RoundMultiplierInput).
//
// A double FMA followed by a conversion to float rounds twice. That can only go wrong when the
// double result lies exactly halfway between two floats (the 29 bits a float drops are 1000...0)
// and the double rounding moved it there: the exact result was slightly above or below the
// halfway point. The rounding error of the double FMA is recovered with Knuth's TwoSum, taking
// the product unrounded through FMA instructions, and the double result is nudged one ulp
// towards the exact value so that the later float conversion rounds the right way. (In the
// directed rounding modes the nudge cannot change the float that results.)
//
// NaNs are chosen in the order frA, frB, frC, even though the operation is frA * frC + frB.
inline FPResult FusedMulAdd(PowerPC::PowerPCState& ppc_state, double a, double c, double b,
                            bool negate_b, bool single)
{
  const double addend = negate_b ? -b : b;
  FPResult result;

  if (!single)
  {
    result.value = std::fma(a, c, addend);
  }
  else
  {
    const double c_rounded = RoundMultiplierInput(c);
    const double sum = std::fma(a, c_rounded, addend);
    result.value = sum;

    constexpr u64 FLOAT_DROPPED_BITS = 0x1FFFFFFF;
    constexpr u64 FLOAT_HALFWAY = 0x10000000;
    const u64 sum_bits = Bits(sum);
    if ((sum_bits & FLOAT_DROPPED_BITS) == FLOAT_HALFWAY)
    {
      // TwoSum(addend, product): s = addend + product is `sum`.
      const double product_virtual = addend - sum;           // -(s - addend)
      const double addend_virtual = sum + product_virtual;   // s - (s - addend)
      const double product_error = std::fma(a, c_rounded, product_virtual);
      const double addend_error = addend - addend_virtual;
      const double error = product_error + addend_error;  // exact result = sum + error

      if (error != 0.0)
      {
        const bool away_from_zero = (error > 0.0) == (sum > 0.0);
        result.value = std::bit_cast<double>(away_from_zero ? sum_bits + 1 : sum_bits - 1);
      }
    }
  }

  if (std::isnan(result.value))
  {
    if (Common::IsSNAN(a) || Common::IsSNAN(b) || Common::IsSNAN(c))
      Raise(ppc_state, result, FPSCR_VXSNAN);
    ppc_state.fpscr.ClearFIFR();

    for (const double operand : {a, b, c})
    {
      if (std::isnan(operand))
      {
        result.value = Common::MakeQuiet(operand);
        return result;
      }
    }

    // infinity * 0, or the product is an infinity that the addend cancels
    Raise(ppc_state, result, std::isnan(a * c) ? FPSCR_VXIMZ : FPSCR_VXISI);
    result.value = DefaultNaN();
    return result;
  }

  if (std::isinf(a) || std::isinf(b) || std::isinf(c))
    ppc_state.fpscr.ClearFIFR();
  return result;
}

// ---------------------------------------------------------------------------------------------
// Comparisons (fcmpu, fcmpo, ps_cmpu*, ps_cmpo*)

// The four condition bits as they appear in a CR field and in FPSCR[FPCC].
enum : u32
{
  FP_LESS = 8,
  FP_GREATER = 4,
  FP_EQUAL = 2,
  FP_UNORDERED = 1,
};

// Unordered compares only complain about SNaNs. Ordered compares also flag VXVC for any NaN
// operand, except that an SNaN with invalid-operation exceptions enabled gets VXSNAN alone.
inline void CompareFP(PowerPC::PowerPCState& ppc_state, u32 crfd, double a, double b, bool ordered)
{
  u32 flags;
  if (std::isnan(a) || std::isnan(b))
  {
    flags = FP_UNORDERED;
    const bool signalling = Common::IsSNAN(a) || Common::IsSNAN(b);
    if (signalling)
      RaiseFP(ppc_state, FPSCR_VXSNAN);
    if (ordered && (!signalling || ppc_state.fpscr.VE == 0))
      RaiseFP(ppc_state, FPSCR_VXVC);
  }
  else if (a < b)
  {
    flags = FP_LESS;
  }
  else if (a > b)
  {
    flags = FP_GREATER;
  }
  else
  {
    flags = FP_EQUAL;
  }

  // FPCC is the low four bits of FPRF; the class bit above it is kept.
  ppc_state.fpscr.FPRF = (ppc_state.fpscr.FPRF & 0x10) | flags;
  ppc_state.cr.SetField(crfd, flags);
}

// ---------------------------------------------------------------------------------------------
// Bit-level conversions

// Builds the single precision word from the sign, the top exponent bit and the low 7 exponent
// bits plus 23 fraction bits of a double. This is the plain truncating repack that lfs/stfs use
// for normal values, zeros, infinities and NaNs.
inline u32 RepackDoubleToSingle(u64 x)
{
  return static_cast<u32>(((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF));
}

// stfs: doubles in the single denormal range (unbiased exponents -150..-127) are denormalised;
// anything smaller is repacked like a normal number, as the hardware does.
inline u32 DoubleToSingleBits(u64 x)
{
  const u32 exponent = static_cast<u32>((x >> 52) & 0x7FF);
  if (exponent >= 874 && exponent <= 896)
  {
    const u32 significand = 0x80000000u | static_cast<u32>((x & Common::DOUBLE_FRAC) >> 21);
    return (significand >> (905 - exponent)) | static_cast<u32>((x >> 32) & 0x80000000);
  }
  return RepackDoubleToSingle(x);
}

// psq_st with a float GQR type: values below the single normal range become signed zeros.
inline u32 DoubleToSingleBitsFlushed(u64 x)
{
  const u32 exponent = static_cast<u32>((x >> 52) & 0x7FF);
  if (exponent > 896 || (x & ~Common::DOUBLE_SIGN) == 0)
    return RepackDoubleToSingle(x);
  return static_cast<u32>((x >> 32) & 0x80000000);
}

// lfs/psq_l: the exact widening of a single to a double, done on the bits so that SNaNs stay
// signalling. Denormal singles are normalised.
inline u64 SingleBitsToDouble(u32 value)
{
  const u64 x = value;
  const u32 exponent = (value >> 23) & 0xFF;
  const u32 fraction = value & 0x007FFFFF;

  if (exponent == 0 && fraction != 0)
  {
    // The leading one moves up to the implicit bit; each step costs one exponent step.
    const int shift = std::countl_zero(fraction) - 8;
    const u64 double_exponent = 897 - shift;
    const u64 double_fraction = static_cast<u64>((fraction << shift) & 0x007FFFFF) << 29;
    return ((x & 0x80000000) << 32) | (double_exponent << 52) | double_fraction;
  }

  // The double exponent is the single one with its top bit copied into the three new bits;
  // except for zero and infinity/NaN, whose all-zero or all-one pattern is simply extended.
  bool fill;
  if (exponent == 0 || exponent == 0xFF)
    fill = exponent == 0xFF;
  else
    fill = (exponent & 0x80) == 0;

  u64 result = ((x & 0xC0000000) << 32) | ((x & 0x3FFFFFFF) << 29);
  if (fill)
    result |= 0x3800000000000000ULL;
  return result;
}
}  // namespace InterpreterFP
