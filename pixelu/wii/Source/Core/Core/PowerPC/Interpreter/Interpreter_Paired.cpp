// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Paired-single arithmetic (primary opcode 4). Both halves are computed with the scalar rules
// and rounded to single precision; FPRF describes the first half (ps_sum1: the second).

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <cmath>

#include "Common/CommonTypes.h"
#include "Common/FloatUtils.h"
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

// Stores two single precision results and classifies `classified` in FPRF.
void WritePair(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, float ps0, float ps1,
               float classified)
{
  ppc_state.ps[inst.FD].SetBoth(static_cast<double>(ps0), static_cast<double>(ps1));
  ppc_state.UpdateFPRFSingle(classified);
  FinishRc(ppc_state, inst);
}

float Single(const PowerPC::PowerPCState& ppc_state, double value)
{
  return RoundToSingle(ppc_state.fpscr, value);
}

float NegateUnlessNaN(float value)
{
  return std::isnan(value) ? value : -value;
}

// Element-wise frA op frB.
template <typename Op>
void Elementwise(Interpreter& interpreter, PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst,
                 Op op)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const float ps0 = Single(ppc_state, op(ppc_state, a.PS0AsDouble(), b.PS0AsDouble()).value);
  const float ps1 = Single(ppc_state, op(ppc_state, a.PS1AsDouble(), b.PS1AsDouble()).value);
  WritePair(ppc_state, inst, ps0, ps1, ps0);
}

// Products: frA * frC, where frC0/frC1 select which half of frC multiplies each half of frA.
void Multiply(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, bool c0_from_ps1,
              bool c1_from_ps1)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& c = ppc_state.ps[inst.FC];
  const double c0 = RoundMultiplierInput(c0_from_ps1 ? c.PS1AsDouble() : c.PS0AsDouble());
  const double c1 = RoundMultiplierInput(c1_from_ps1 ? c.PS1AsDouble() : c.PS0AsDouble());
  const float ps0 = Single(ppc_state, Mul(ppc_state, a.PS0AsDouble(), c0).value);
  const float ps1 = Single(ppc_state, Mul(ppc_state, a.PS1AsDouble(), c1).value);
  WritePair(ppc_state, inst, ps0, ps1, ps0);
}

// Fused frA * frC +/- frB, optionally negated, with the same choice of frC halves.
void MultiplyAdd(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, bool subtract,
                 bool negate, bool c0_from_ps1, bool c1_from_ps1)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];
  const double c0 = c0_from_ps1 ? c.PS1AsDouble() : c.PS0AsDouble();
  const double c1 = c1_from_ps1 ? c.PS1AsDouble() : c.PS0AsDouble();

  float ps0 = Single(ppc_state,
                     FusedMulAdd(ppc_state, a.PS0AsDouble(), c0, b.PS0AsDouble(), subtract, true)
                         .value);
  float ps1 = Single(ppc_state,
                     FusedMulAdd(ppc_state, a.PS1AsDouble(), c1, b.PS1AsDouble(), subtract, true)
                         .value);
  if (negate)
  {
    ps0 = NegateUnlessNaN(ps0);
    ps1 = NegateUnlessNaN(ps1);
  }
  WritePair(ppc_state, inst, ps0, ps1, ps0);
}

void Compare(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, bool second_half,
             bool ordered)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  if (second_half)
    CompareFP(ppc_state, inst.CRFD, a.PS1AsDouble(), b.PS1AsDouble(), ordered);
  else
    CompareFP(ppc_state, inst.CRFD, a.PS0AsDouble(), b.PS0AsDouble(), ordered);
}

// Bitwise moves of two halves (no FPSCR change).
void MovePair(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, u64 ps0, u64 ps1)
{
  ppc_state.ps[inst.FD].SetBoth(ps0, ps1);
  FinishRc(ppc_state, inst);
}
}  // namespace

// ---- moves, merges, sign manipulation, select

void Interpreter::ps_mr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.ps[inst.FD] = ppc_state.ps[inst.FB];
  FinishRc(ppc_state, inst);
}

void Interpreter::ps_neg(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const auto& b = ppc_state.ps[inst.FB];
  MovePair(ppc_state, inst, b.PS0AsU64() ^ SIGN_BIT, b.PS1AsU64() ^ SIGN_BIT);
}

void Interpreter::ps_nabs(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const auto& b = ppc_state.ps[inst.FB];
  MovePair(ppc_state, inst, b.PS0AsU64() | SIGN_BIT, b.PS1AsU64() | SIGN_BIT);
}

void Interpreter::ps_abs(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const auto& b = ppc_state.ps[inst.FB];
  MovePair(ppc_state, inst, b.PS0AsU64() & ~SIGN_BIT, b.PS1AsU64() & ~SIGN_BIT);
}

void Interpreter::ps_merge00(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  MovePair(ppc_state, inst, ppc_state.ps[inst.FA].PS0AsU64(), ppc_state.ps[inst.FB].PS0AsU64());
}

void Interpreter::ps_merge01(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  MovePair(ppc_state, inst, ppc_state.ps[inst.FA].PS0AsU64(), ppc_state.ps[inst.FB].PS1AsU64());
}

void Interpreter::ps_merge10(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  MovePair(ppc_state, inst, ppc_state.ps[inst.FA].PS1AsU64(), ppc_state.ps[inst.FB].PS0AsU64());
}

void Interpreter::ps_merge11(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  MovePair(ppc_state, inst, ppc_state.ps[inst.FA].PS1AsU64(), ppc_state.ps[inst.FB].PS1AsU64());
}

// Per half: frC if frA >= 0 (including -0), else frB.
void Interpreter::ps_sel(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];
  MovePair(ppc_state, inst, a.PS0AsDouble() >= -0.0 ? c.PS0AsU64() : b.PS0AsU64(),
           a.PS1AsDouble() >= -0.0 ? c.PS1AsU64() : b.PS1AsU64());
}

// ---- arithmetic

void Interpreter::ps_add(Interpreter& interpreter, UGeckoInstruction inst)
{
  Elementwise(interpreter, interpreter.m_ppc_state, inst, Add);
}

void Interpreter::ps_sub(Interpreter& interpreter, UGeckoInstruction inst)
{
  Elementwise(interpreter, interpreter.m_ppc_state, inst, Sub);
}

void Interpreter::ps_div(Interpreter& interpreter, UGeckoInstruction inst)
{
  Elementwise(interpreter, interpreter.m_ppc_state, inst, Div);
}

void Interpreter::ps_mul(Interpreter& interpreter, UGeckoInstruction inst)
{
  Multiply(interpreter.m_ppc_state, inst, false, true);
}

void Interpreter::ps_muls0(Interpreter& interpreter, UGeckoInstruction inst)
{
  Multiply(interpreter.m_ppc_state, inst, false, false);
}

void Interpreter::ps_muls1(Interpreter& interpreter, UGeckoInstruction inst)
{
  Multiply(interpreter.m_ppc_state, inst, true, true);
}

void Interpreter::ps_madd(Interpreter& interpreter, UGeckoInstruction inst)
{
  MultiplyAdd(interpreter.m_ppc_state, inst, false, false, false, true);
}

void Interpreter::ps_msub(Interpreter& interpreter, UGeckoInstruction inst)
{
  MultiplyAdd(interpreter.m_ppc_state, inst, true, false, false, true);
}

void Interpreter::ps_nmadd(Interpreter& interpreter, UGeckoInstruction inst)
{
  MultiplyAdd(interpreter.m_ppc_state, inst, false, true, false, true);
}

void Interpreter::ps_nmsub(Interpreter& interpreter, UGeckoInstruction inst)
{
  MultiplyAdd(interpreter.m_ppc_state, inst, true, true, false, true);
}

void Interpreter::ps_madds0(Interpreter& interpreter, UGeckoInstruction inst)
{
  MultiplyAdd(interpreter.m_ppc_state, inst, false, false, false, false);
}

void Interpreter::ps_madds1(Interpreter& interpreter, UGeckoInstruction inst)
{
  MultiplyAdd(interpreter.m_ppc_state, inst, false, false, true, true);
}

// ps_sum0: frD = {frA.ps0 + frB.ps1, frC.ps1}; ps_sum1: frD = {frC.ps0, frA.ps0 + frB.ps1}.
void Interpreter::ps_sum0(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];
  const float sum = Single(ppc_state, Add(ppc_state, a.PS0AsDouble(), b.PS1AsDouble()).value);
  const float other = Single(ppc_state, c.PS1AsDouble());
  WritePair(ppc_state, inst, sum, other, sum);
}

void Interpreter::ps_sum1(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];
  const float other = Single(ppc_state, c.PS0AsDouble());
  const float sum = Single(ppc_state, Add(ppc_state, a.PS0AsDouble(), b.PS1AsDouble()).value);
  WritePair(ppc_state, inst, other, sum, sum);
}

// ---- estimates. The flags of both halves are combined; the results are always written.

void Interpreter::ps_res(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double b0 = ppc_state.ps[inst.FB].PS0AsDouble();
  const double b1 = ppc_state.ps[inst.FB].PS1AsDouble();

  if (b0 == 0.0 || b1 == 0.0)
  {
    RaiseFP(ppc_state, FPSCR_ZX);
    ppc_state.fpscr.ClearFIFR();
  }
  if (!std::isfinite(b0) || !std::isfinite(b1))
    ppc_state.fpscr.ClearFIFR();
  if (Common::IsSNAN(b0) || Common::IsSNAN(b1))
    RaiseFP(ppc_state, FPSCR_VXSNAN);

  const double ps0 = Common::ApproximateReciprocal(b0);
  const double ps1 = Common::ApproximateReciprocal(b1);
  ppc_state.ps[inst.FD].SetBoth(ps0, ps1);
  ppc_state.UpdateFPRFSingle(static_cast<float>(ps0));
  FinishRc(ppc_state, inst);
}

void Interpreter::ps_rsqrte(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double b0 = ppc_state.ps[inst.FB].PS0AsDouble();
  const double b1 = ppc_state.ps[inst.FB].PS1AsDouble();

  if (b0 == 0.0 || b1 == 0.0)
  {
    RaiseFP(ppc_state, FPSCR_ZX);
    ppc_state.fpscr.ClearFIFR();
  }
  if (b0 < 0.0 || b1 < 0.0)
  {
    RaiseFP(ppc_state, FPSCR_VXSQRT);
    ppc_state.fpscr.ClearFIFR();
  }
  if (!std::isfinite(b0) || !std::isfinite(b1))
    ppc_state.fpscr.ClearFIFR();
  if (Common::IsSNAN(b0) || Common::IsSNAN(b1))
    RaiseFP(ppc_state, FPSCR_VXSNAN);

  const float ps0 = Single(ppc_state, Common::ApproximateReciprocalSquareRoot(b0));
  const float ps1 = Single(ppc_state, Common::ApproximateReciprocalSquareRoot(b1));
  WritePair(ppc_state, inst, ps0, ps1, ps0);
}

// ---- compares

void Interpreter::ps_cmpu0(Interpreter& interpreter, UGeckoInstruction inst)
{
  Compare(interpreter.m_ppc_state, inst, false, false);
}

void Interpreter::ps_cmpo0(Interpreter& interpreter, UGeckoInstruction inst)
{
  Compare(interpreter.m_ppc_state, inst, false, true);
}

void Interpreter::ps_cmpu1(Interpreter& interpreter, UGeckoInstruction inst)
{
  Compare(interpreter.m_ppc_state, inst, true, false);
}

void Interpreter::ps_cmpo1(Interpreter& interpreter, UGeckoInstruction inst)
{
  Compare(interpreter.m_ppc_state, inst, true, true);
}
