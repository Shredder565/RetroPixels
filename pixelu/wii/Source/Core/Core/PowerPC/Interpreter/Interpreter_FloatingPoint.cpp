// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Scalar floating point instructions (primary opcodes 59 and 63).

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <cmath>

#include "Common/CommonTypes.h"
#include "Common/FloatUtils.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/PowerPC.h"

using namespace InterpreterFP;

namespace
{
constexpr u64 SIGN_BIT = 0x8000000000000000ULL;

void FinishRc(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst)
{
  if (inst.Rc)
    ppc_state.UpdateCR1();
}

// Whether an arithmetic result may be written: an invalid operation with VE set leaves frD and
// FPRF untouched.
bool MayWrite(const PowerPC::PowerPCState& ppc_state, const FPResult& r)
{
  return ppc_state.fpscr.VE == 0 || !r.IsInvalid();
}

// Writes a double precision result to frD (ps1 is kept) and classifies it in FPRF.
// The negated forms negate everything but NaNs.
void WriteDouble(PowerPC::PowerPCState& ppc_state, u32 fd, double value, bool negate = false)
{
  double result = RoundToDouble(ppc_state.fpscr, value);
  if (negate && !std::isnan(result))
    result = -result;
  ppc_state.ps[fd].SetPS0(result);
  ppc_state.UpdateFPRFDouble(result);
}

// Rounds to single precision and writes the result to both halves of frD.
float WriteSingle(PowerPC::PowerPCState& ppc_state, u32 fd, double value, bool negate = false)
{
  float result = RoundToSingle(ppc_state.fpscr, value);
  if (negate && !std::isnan(result))
    result = -result;
  ppc_state.ps[fd].Fill(static_cast<double>(result));
  ppc_state.UpdateFPRFSingle(result);
  return result;
}

enum class RoundTo
{
  Nearest = 0,
  Zero = 1,
  PlusInfinity = 2,
  MinusInfinity = 3,
};

double RoundToIntegral(double b, RoundTo mode)
{
  switch (mode)
  {
  case RoundTo::Zero:
    return std::trunc(b);
  case RoundTo::PlusInfinity:
    return std::ceil(b);
  case RoundTo::MinusInfinity:
    return std::floor(b);
  case RoundTo::Nearest:
  default:
  {
    // Ties to even, without depending on the host rounding mode.
    const double below = std::floor(b);
    const double fraction = b - below;
    if (fraction > 0.5 || (fraction == 0.5 && std::fmod(below, 2.0) != 0.0))
      return below + 1.0;
    return below;
  }
  }
}

// fctiw/fctiwz. The 32-bit result goes into the low word of frD; the high word reads
// 0xFFF80000, or 0xFFF80001 for a zero result from a negative operand (as measured on hardware).
// FPRF is not changed.
void ConvertToWord(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, RoundTo mode)
{
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();
  const double rounded = RoundToIntegral(b, mode);

  u32 word;
  bool invalid = true;
  if (std::isnan(b))
  {
    if (Common::IsSNAN(b))
      RaiseFP(ppc_state, FPSCR_VXSNAN);
    word = 0x80000000;
  }
  else if (rounded >= 2147483648.0)
  {
    word = 0x7FFFFFFF;
  }
  else if (rounded < -2147483648.0)
  {
    word = 0x80000000;
  }
  else
  {
    invalid = false;
    const s32 integer = static_cast<s32>(rounded);
    word = static_cast<u32>(integer);
    const double back = static_cast<double>(integer);
    if (back == b)
    {
      ppc_state.fpscr.ClearFIFR();
    }
    else
    {
      SetInexact(ppc_state, true);
      ppc_state.fpscr.FR = std::fabs(back) > std::fabs(b);
    }
  }

  if (invalid)
  {
    RaiseFP(ppc_state, FPSCR_VXCVI);
    ppc_state.fpscr.ClearFIFR();
  }

  if (!invalid || ppc_state.fpscr.VE == 0)
  {
    u64 result = 0xFFF8000000000000ULL | word;
    if (word == 0 && std::signbit(b))
      result |= 0x100000000ULL;
    ppc_state.ps[inst.FD].SetPS0(result);
  }

  FinishRc(ppc_state, inst);
}

// Shared body of the reciprocal estimates. `fault` is the FPSCR flag a zero (or, for frsqrte,
// a negative) operand raises; such operands still produce an estimate unless the matching
// enable bit is set.
template <typename Write>
void Estimate(PowerPC::PowerPCState& ppc_state, double b, bool negative_is_invalid, Write write)
{
  if (negative_is_invalid && b < 0.0)
  {
    RaiseFP(ppc_state, FPSCR_VXSQRT);
    ppc_state.fpscr.ClearFIFR();
    if (ppc_state.fpscr.VE == 0)
      write();
  }
  else if (b == 0.0)
  {
    RaiseFP(ppc_state, FPSCR_ZX);
    ppc_state.fpscr.ClearFIFR();
    if (ppc_state.fpscr.ZE == 0)
      write();
  }
  else if (Common::IsSNAN(b))
  {
    RaiseFP(ppc_state, FPSCR_VXSNAN);
    ppc_state.fpscr.ClearFIFR();
    if (ppc_state.fpscr.VE == 0)
      write();
  }
  else
  {
    if (std::isnan(b) || std::isinf(b))
      ppc_state.fpscr.ClearFIFR();
    write();
  }
}

// fmadd family: frA * frC +/- frB, optionally negated, in double or single precision.
void MulAddScalar(Interpreter& interpreter, PowerPC::PowerPCState& ppc_state,
                  UGeckoInstruction inst, bool subtract, bool negate, bool single)
{
  const double a = ppc_state.ps[inst.FA].PS0AsDouble();
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();
  const double c = ppc_state.ps[inst.FC].PS0AsDouble();
  const FPResult r = FusedMulAdd(ppc_state, a, c, b, subtract, single);

  if (MayWrite(ppc_state, r))
  {
    if (single)
    {
      const float result = WriteSingle(ppc_state, inst.FD, r.value, negate);
      // Only fmadds itself reports inexactness here.
      if (!subtract && !negate)
      {
        ppc_state.fpscr.FI = r.value != result;
        ppc_state.fpscr.FR = 0;
      }
    }
    else
    {
      WriteDouble(ppc_state, inst.FD, r.value, negate);
    }
  }
  FinishRc(ppc_state, inst);
}
}  // namespace

// ---- compares and conversions

void Interpreter::fcmpo(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  CompareFP(ppc_state, inst.CRFD, ppc_state.ps[inst.FA].PS0AsDouble(),
            ppc_state.ps[inst.FB].PS0AsDouble(), true);
}

void Interpreter::fcmpu(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  CompareFP(ppc_state, inst.CRFD, ppc_state.ps[inst.FA].PS0AsDouble(),
            ppc_state.ps[inst.FB].PS0AsDouble(), false);
}

void Interpreter::fctiwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ConvertToWord(ppc_state, inst, static_cast<RoundTo>(ppc_state.fpscr.RN.Value()));
}

void Interpreter::fctiwzx(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConvertToWord(interpreter.m_ppc_state, inst, RoundTo::Zero);
}

// frsp. Both halves of frD receive the rounded value.
void Interpreter::frspx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();
  const float rounded = RoundToSingle(ppc_state.fpscr, b);

  if (std::isnan(b))
  {
    const bool signalling = Common::IsSNAN(b);
    if (signalling)
      RaiseFP(ppc_state, FPSCR_VXSNAN);
    if (!signalling || ppc_state.fpscr.VE == 0)
    {
      ppc_state.ps[inst.FD].Fill(static_cast<double>(rounded));
      ppc_state.UpdateFPRFSingle(rounded);
    }
    ppc_state.fpscr.ClearFIFR();
  }
  else
  {
    SetInexact(ppc_state, b != rounded);
    ppc_state.fpscr.FR = std::fabs(rounded) > std::fabs(b);
    ppc_state.UpdateFPRFSingle(rounded);
    ppc_state.ps[inst.FD].Fill(static_cast<double>(rounded));
  }
  FinishRc(ppc_state, inst);
}

// ---- moves and sign manipulation (no FPSCR change)

void Interpreter::fmrx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.ps[inst.FD].SetPS0(ppc_state.ps[inst.FB].PS0AsU64());
  FinishRc(ppc_state, inst);
}

void Interpreter::fabsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.ps[inst.FD].SetPS0(ppc_state.ps[inst.FB].PS0AsU64() & ~SIGN_BIT);
  FinishRc(ppc_state, inst);
}

void Interpreter::fnabsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.ps[inst.FD].SetPS0(ppc_state.ps[inst.FB].PS0AsU64() | SIGN_BIT);
  FinishRc(ppc_state, inst);
}

void Interpreter::fnegx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.ps[inst.FD].SetPS0(ppc_state.ps[inst.FB].PS0AsU64() ^ SIGN_BIT);
  FinishRc(ppc_state, inst);
}

// fsel: frC if frA >= 0 (including -0), otherwise frB (also when frA is a NaN).
void Interpreter::fselx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const bool take_c = ppc_state.ps[inst.FA].PS0AsDouble() >= -0.0;
  ppc_state.ps[inst.FD].SetPS0(take_c ? ppc_state.ps[inst.FC].PS0AsU64() :
                                        ppc_state.ps[inst.FB].PS0AsU64());
  FinishRc(ppc_state, inst);
}

// ---- arithmetic

void Interpreter::faddx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const FPResult r = Add(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(),
                         ppc_state.ps[inst.FB].PS0AsDouble());
  if (MayWrite(ppc_state, r))
    WriteDouble(ppc_state, inst.FD, r.value);
  FinishRc(ppc_state, inst);
}

void Interpreter::faddsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const FPResult r = Add(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(),
                         ppc_state.ps[inst.FB].PS0AsDouble());
  if (MayWrite(ppc_state, r))
    WriteSingle(ppc_state, inst.FD, r.value);
  FinishRc(ppc_state, inst);
}

void Interpreter::fsubx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const FPResult r = Sub(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(),
                         ppc_state.ps[inst.FB].PS0AsDouble());
  if (MayWrite(ppc_state, r))
    WriteDouble(ppc_state, inst.FD, r.value);
  FinishRc(ppc_state, inst);
}

void Interpreter::fsubsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const FPResult r = Sub(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(),
                         ppc_state.ps[inst.FB].PS0AsDouble());
  if (MayWrite(ppc_state, r))
    WriteSingle(ppc_state, inst.FD, r.value);
  FinishRc(ppc_state, inst);
}

// The multiplies clear FI and FR when they write a result.
void Interpreter::fmulx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const FPResult r = Mul(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(),
                         ppc_state.ps[inst.FC].PS0AsDouble());
  if (MayWrite(ppc_state, r))
  {
    const double result = RoundToDouble(ppc_state.fpscr, r.value);
    ppc_state.ps[inst.FD].SetPS0(result);
    ppc_state.fpscr.ClearFIFR();
    ppc_state.UpdateFPRFDouble(result);
  }
  FinishRc(ppc_state, inst);
}

void Interpreter::fmulsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double c = RoundMultiplierInput(ppc_state.ps[inst.FC].PS0AsDouble());
  const FPResult r = Mul(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(), c);
  if (MayWrite(ppc_state, r))
  {
    const float result = RoundToSingle(ppc_state.fpscr, r.value);
    ppc_state.ps[inst.FD].Fill(static_cast<double>(result));
    ppc_state.fpscr.ClearFIFR();
    ppc_state.UpdateFPRFSingle(result);
  }
  FinishRc(ppc_state, inst);
}

// Division by zero with ZE set also leaves frD alone.
void Interpreter::fdivx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const FPResult r = Div(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(),
                         ppc_state.ps[inst.FB].PS0AsDouble());
  const bool zero_divide_trapped = ppc_state.fpscr.ZE != 0 && r.exception == FPSCR_ZX;
  if (!zero_divide_trapped && MayWrite(ppc_state, r))
    WriteDouble(ppc_state, inst.FD, r.value);
  FinishRc(ppc_state, inst);
}

void Interpreter::fdivsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const FPResult r = Div(ppc_state, ppc_state.ps[inst.FA].PS0AsDouble(),
                         ppc_state.ps[inst.FB].PS0AsDouble());
  const bool zero_divide_trapped = ppc_state.fpscr.ZE != 0 && r.exception == FPSCR_ZX;
  if (!zero_divide_trapped && MayWrite(ppc_state, r))
    WriteSingle(ppc_state, inst.FD, r.value);
  FinishRc(ppc_state, inst);
}

void Interpreter::fmaddx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, false, false, false);
}

void Interpreter::fmaddsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, false, false, true);
}

void Interpreter::fmsubx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, true, false, false);
}

void Interpreter::fmsubsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, true, false, true);
}

void Interpreter::fnmaddx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, false, true, false);
}

void Interpreter::fnmaddsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, false, true, true);
}

void Interpreter::fnmsubx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, true, true, false);
}

void Interpreter::fnmsubsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  MulAddScalar(interpreter, interpreter.m_ppc_state, inst, true, true, true);
}

// ---- estimates (hardware lookup tables live in Common/FloatUtils)

// fres: single precision; the estimate is written to both halves.
void Interpreter::fresx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();
  Estimate(ppc_state, b, false, [&] {
    const double estimate = Common::ApproximateReciprocal(b);
    ppc_state.ps[inst.FD].Fill(estimate);
    ppc_state.UpdateFPRFSingle(static_cast<float>(estimate));
  });
  FinishRc(ppc_state, inst);
}

// frsqrte: double precision; ps1 is kept.
void Interpreter::frsqrtex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();
  Estimate(ppc_state, b, true, [&] {
    const double estimate = Common::ApproximateReciprocalSquareRoot(b);
    ppc_state.ps[inst.FD].SetPS0(estimate);
    ppc_state.UpdateFPRFDouble(estimate);
  });
  FinishRc(ppc_state, inst);
}
