// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/SmallVector.h"
#include "Common/x64Emitter.h"
#include "Core/Config/SessionSettings.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/PPCAnalyst.h"

using namespace Gen;

namespace
{
// Lane masks for the sign bit (low lane only, or both lanes).
alignas(16) constexpr std::array<u64, 2> SIGN_LOW{0x8000000000000000ULL, 0};
alignas(16) constexpr std::array<u64, 2> SIGN_BOTH{0x8000000000000000ULL, 0x8000000000000000ULL};
// The complements, for clearing the sign; the high lane of ABS_LOW is kept.
alignas(16) constexpr std::array<u64, 2> ABS_LOW{0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
alignas(16) constexpr std::array<u64, 2> ABS_BOTH{0x7FFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};
// The quiet bit plus exponent: ORing it makes a NaN quiet, and on its own
// it is the PowerPC default NaN (positive, unlike x86's).
alignas(16) constexpr std::array<u64, 2> QUIET_NAN{0x7FF8000000000000ULL, 0x7FF8000000000000ULL};
// fctiw clamps to the s32 range; MINSD against the low lane does the upper
// clamp (and turns NaN into the bound, as MINSD returns the second operand
// for NaN). The high lane, once converted, supplies the 0xFFF80000 upper
// word of the result.
alignas(16) constexpr std::array<double, 2> FCTIW_BOUNDS{0x7FFFFFFF, -0x80000};

double (*const SOFTWARE_FMA)(double, double, double) = &std::fma;
}  // namespace

// ---------------------------------------------------------------------------
// Result helpers

void Jit64::SetFPRFIfNeeded(const OpArg& input, bool single)
{
  // FPRF is only computed when the option is on and a later instruction
  // can observe it before it is overwritten.
  if (!m_fprf || !js.op->wantsFPRF)
    return;

  X64Reg value = XMM0;
  if (input.IsSimpleReg())
    value = input.GetSimpleReg();
  else
    MOVSD(value, input);
  SetFPRF(value, single);
}

void Jit64::FinalizeSingleResult(X64Reg output, const OpArg& input, bool packed, bool duplicate)
{
  if (!jo.accurateSinglePrecision)
  {
    // Results are left in double precision.
    if (!input.IsSimpleReg(output))
    {
      if (duplicate)
        MOVDDUP(output, input);
      else
        MOVAPD(output, input);
    }
    SetFPRFIfNeeded(input, false);
    return;
  }

  // Round to single and back. FPRF is taken from the single value.
  if (packed)
  {
    CVTPD2PS(output, input);
    SetFPRFIfNeeded(R(output), true);
    CVTPS2PD(output, R(output));
    return;
  }
  CVTSD2SS(output, input);
  SetFPRFIfNeeded(R(output), true);
  CVTSS2SD(output, R(output));
  if (duplicate)
    MOVDDUP(output, R(output));
}

void Jit64::FinalizeDoubleResult(X64Reg output, const OpArg& input)
{
  if (!input.IsSimpleReg(output))
    MOVSD(output, input);
  SetFPRFIfNeeded(input, false);
}

// x86 and PowerPC differ for NaN results: when several inputs are NaN, x86
// picks by operand position (first, then second) while PowerPC prefers
// a, then b, then c; and a NaN made from non-NaN inputs is negative on x86
// but positive on PowerPC. When `xmm` holds a NaN, this far-code fix-up
// recomputes it the PowerPC way. The returned branch must be bound where
// the near code continues.
FixupBranch Jit64::HandleNaNs(UGeckoInstruction inst, X64Reg xmm, X64Reg clobber,
                              std::optional<OpArg> Ra, std::optional<OpArg> Rb,
                              std::optional<OpArg> Rc)
{
  const bool paired = inst.OPCD == 4;

  if (!paired)
  {
    UCOMISD(xmm, R(xmm));
    const FixupBranch is_nan = J_CC(CC_P, Jump::Near);
    SwitchToFarCode();
    SetJumpTarget(is_nan);

    // Take the first NaN input, in PowerPC order.
    Common::SmallVector<FixupBranch, 3> found;
    const auto try_input = [&](const OpArg& input) {
      MOVDDUP(xmm, input);
      UCOMISD(xmm, R(xmm));
      found.push_back(J_CC(CC_P));
    };
    if (Ra)
      try_input(*Ra);
    if (Rb && Rb != Ra)
      try_input(*Rb);
    if (Rc && Rc != Ra && Rc != Rb)
      try_input(*Rc);

    // None: the default NaN, completed by the OR below.
    XORPD(xmm, R(xmm));
    for (const FixupBranch& branch : found)
      SetJumpTarget(branch);
    // Quiets a signalling NaN input.
    ORPD(xmm, MConst(QUIET_NAN));

    const FixupBranch back = J(Jump::Near);
    SwitchToNearCode();
    return back;
  }

  // Paired: each lane is fixed up on its own. Later inputs are blended in
  // first so that earlier ones win.
  ASSERT(xmm != clobber);

  if (cpu_info.bSSE4_1)
  {
    avx_op(&XEmitter::VCMPPD, &XEmitter::CMPPD, clobber, R(xmm), R(xmm), CMP_UNORD);
    PTEST(clobber, R(clobber));
    const FixupBranch any_nan = J_CC(CC_NZ, Jump::Near);
    SwitchToFarCode();
    SetJumpTarget(any_nan);

    ASSERT_MSG(DYNA_REC, clobber == XMM0, "BLENDVPD implicitly uses XMM0");
    BLENDVPD(xmm, MConst(QUIET_NAN));
    const auto blend_input = [&](const OpArg& input) {
      avx_op(&XEmitter::VCMPPD, &XEmitter::CMPPD, clobber, input, input, CMP_UNORD);
      BLENDVPD(xmm, input);
    };
    if (Rc)
      blend_input(*Rc);
    if (Rb && Rb != Rc)
      blend_input(*Rb);
    if (Ra && Ra != Rb && Ra != Rc)
      blend_input(*Ra);
  }
  else
  {
    // Without BLENDVPD the selects are done with AND/ANDN/OR.
    RCX64Reg mask = fpr.Scratch();
    RegCache::Realize(mask);
    MOVAPD(clobber, R(xmm));
    CMPPD(clobber, R(clobber), CMP_UNORD);
    MOVMSKPD(RSCRATCH, R(clobber));
    TEST(32, R(RSCRATCH), R(RSCRATCH));
    const FixupBranch any_nan = J_CC(CC_NZ, Jump::Near);
    SwitchToFarCode();
    SetJumpTarget(any_nan);

    // xmm = nan_lanes ? default NaN : xmm
    MOVAPD(mask, R(clobber));
    ANDNPD(clobber, R(xmm));
    ANDPD(mask, MConst(QUIET_NAN));
    ORPD(mask, R(clobber));
    MOVAPD(xmm, mask);

    // xmm = input_is_nan ? input : xmm
    const auto select_input = [&](const OpArg& input) {
      MOVAPD(clobber, input);
      CMPPD(clobber, R(clobber), CMP_ORD);
      MOVAPD(mask, R(clobber));
      ANDNPD(clobber, input);
      ANDPD(xmm, mask);
      ORPD(xmm, R(clobber));
    };
    if (Rc)
      select_input(*Rc);
    if (Rb && Rb != Rc)
      select_input(*Rb);
    if (Ra && Ra != Rb && Ra != Rc)
      select_input(*Ra);
  }

  // Quiet whichever lanes are NaN now.
  avx_op(&XEmitter::VCMPPD, &XEmitter::CMPPD, clobber, R(xmm), R(xmm), CMP_UNORD);
  ANDPD(clobber, MConst(QUIET_NAN));
  ORPD(xmm, R(clobber));

  const FixupBranch back = J(Jump::Near);
  SwitchToNearCode();
  return back;
}

// ---------------------------------------------------------------------------
// Arithmetic

void Jit64::fp_arith(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || (jo.div_by_zero_exceptions && inst.SUBOP5 == 18));

  enum : u32
  {
    OP_DIV = 18,
    OP_SUB = 20,
    OP_ADD = 21,
    OP_MUL = 25,
  };
  const u32 op = inst.SUBOP5;

  const u32 a = inst.FA;
  const u32 d = inst.FD;
  // fmul takes its second operand from frC, the others from frB.
  const u32 rhs = op == OP_MUL ? u32{inst.FC} : u32{inst.FB};

  const bool single = inst.OPCD == 4 || inst.OPCD == 59;
  // Single-precision scalars whose inputs are duplicated across both lanes
  // can use packed math and skip the final duplication. Packed division is
  // slow everywhere, and Atom is slow at packed math in general.
  bool packed = inst.OPCD == 4;
  if (inst.OPCD == 59 && op != OP_DIV && !cpu_info.bAtom)
    packed = js.op->fprIsDuplicated[a] && js.op->fprIsDuplicated[rhs];

  using AvxOp = void (XEmitter::*)(X64Reg, X64Reg, const OpArg&);
  using SseOp = void (XEmitter::*)(X64Reg, const OpArg&);
  AvxOp avx = nullptr;
  SseOp sse = nullptr;
  // The operands may be swapped (not for add with accurate NaNs, which
  // would change which NaN wins).
  bool commutative = false;
  // The Gekko rounds a double multiplier to 25 bits in single arithmetic.
  bool round_rhs = false;
  // The NaN fix-up reads the inputs after the operation.
  bool keep_inputs = false;

  if (op == OP_DIV)
  {
    keep_inputs = m_accurate_nans;
    avx = packed ? &XEmitter::VDIVPD : &XEmitter::VDIVSD;
    sse = packed ? &XEmitter::DIVPD : &XEmitter::DIVSD;
  }
  else if (op == OP_SUB)
  {
    avx = packed ? &XEmitter::VSUBPD : &XEmitter::VSUBSD;
    sse = packed ? &XEmitter::SUBPD : &XEmitter::SUBSD;
  }
  else if (op == OP_ADD)
  {
    commutative = !m_accurate_nans;
    avx = packed ? &XEmitter::VADDPD : &XEmitter::VADDSD;
    sse = packed ? &XEmitter::ADDPD : &XEmitter::ADDSD;
  }
  else if (op == OP_MUL)
  {
    commutative = true;
    round_rhs = single && !js.op->fprIsSingle[rhs];
    keep_inputs = m_accurate_nans;
    avx = packed ? &XEmitter::VMULPD : &XEmitter::VMULSD;
    sse = packed ? &XEmitter::MULPD : &XEmitter::MULSD;
  }
  else
  {
    ASSERT_MSG(DYNA_REC, 0, "fp_arith WTF!!!");
  }

  // Double results replace only ps0.
  RCX64Reg Rd = fpr.Bind(d, single ? RCMode::Write : RCMode::ReadWrite);
  RCOpArg Ra = fpr.Use(a, RCMode::Read);
  RCOpArg Rrhs = fpr.Use(rhs, RCMode::Read);
  RegCache::Realize(Rd, Ra, Rrhs);

  X64Reg out = X64Reg(Rd);
  if (keep_inputs && (a == d || rhs == d))
    out = XMM1;

  if (round_rhs)
  {
    if (a == d && !keep_inputs)
    {
      Force25BitPrecision(XMM0, Rrhs, XMM1);
      (this->*sse)(Rd, R(XMM0));
    }
    else
    {
      // a * round(c) == round(c) * a
      Force25BitPrecision(out, Rrhs, XMM0);
      (this->*sse)(out, Ra);
    }
  }
  else if (Ra.IsSimpleReg(out))
  {
    (this->*sse)(out, Rrhs);
  }
  else if (commutative && Rrhs.IsSimpleReg(out))
  {
    (this->*sse)(out, Ra);
  }
  else if (cpu_info.bAVX && Ra.IsSimpleReg())
  {
    (this->*avx)(out, Ra.GetSimpleReg(), Rrhs);
  }
  else if (cpu_info.bAVX && commutative && Rrhs.IsSimpleReg())
  {
    (this->*avx)(out, Rrhs.GetSimpleReg(), Ra);
  }
  else
  {
    // Copy a into the output first, unless that would overwrite the rhs.
    if (Rrhs.IsSimpleReg(out))
      out = XMM1;
    if (packed)
      MOVAPD(out, Ra);
    else
      MOVSD(out, Ra);
    (this->*sse)(out, a == rhs ? R(out) : Rrhs);
  }

  // Additions and subtractions already give the PowerPC NaN.
  if (m_accurate_nans && (op == OP_DIV || op == OP_MUL))
  {
    const FixupBranch done = op == OP_DIV ?
                                 HandleNaNs(inst, out, XMM0, Ra, Rrhs, std::nullopt) :
                                 HandleNaNs(inst, out, XMM0, Ra, std::nullopt, Rrhs);
    SetJumpTarget(done);
  }

  if (single)
    FinalizeSingleResult(Rd, R(out), packed, true);
  else
    FinalizeDoubleResult(Rd, R(out));
}

// fmadd, fmsub, fnmadd, fnmsub in single, double and paired forms, plus
// ps_madds0/ps_madds1.
//
// Three ways of computing a*c + b exist:
//  * hardware FMA (use_fma and the host has it),
//  * std::fma called per lane (use_fma without host FMA: needed so that
//    results match a recording made on a host with FMA),
//  * a separate multiply and add (use_fma off).
// With accurate single-precision fmadds, the single-precision result is
// further corrected with an error-free transformation: the exact error of
// the double result is computed and the shared routine rounds the sum of
// result and error to single in one step, avoiding double rounding.
void Jit64::fmaddXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  const bool use_fma = Config::Get(Config::SESSION_USE_FMA);
  const bool software_fma = use_fma && !cpu_info.bFMA;

  const u32 a = inst.FA;
  const u32 b = inst.FB;
  const u32 c = inst.FC;
  const u32 d = inst.FD;
  const u32 op = inst.SUBOP5;

  const bool subtract = op == 28 || op == 30;  // msub, nmsub
  const bool negate = op == 30 || op == 31;    // nmsub, nmadd
  const bool madds0 = op == 14;
  const bool madds1 = op == 15;
  const bool broadcast_c = madds0 || madds1;
  const bool single = inst.OPCD == 4 || inst.OPCD == 59;
  const bool round_c = single && !js.op->fprIsSingle[c];

  const bool eft = single && m_accurate_fmadds;
  const bool packed =
      inst.OPCD == 4 || (single && !cpu_info.bAtom && !software_fma && !eft &&
                         js.op->fprIsDuplicated[a] && js.op->fprIsDuplicated[b] &&
                         js.op->fprIsDuplicated[c]);

  // c_rounded (XMM3): c (broadcast if madds) rounded to 25 bits, kept for
  // the EFT or for the second software-FMA lane.
  const bool keep_c_rounded = (eft || (software_fma && packed)) && round_c;
  // c_dup: the broadcast c, kept for the EFT or for the NaN fix-up.
  const bool eft_keeps_c_dup = eft && !keep_c_rounded && broadcast_c;
  const bool nans_keep_c_dup = m_accurate_nans && broadcast_c;
  const bool keep_c_dup = eft_keeps_c_dup || nans_keep_c_dup;

  // Whether the result must not be built in frD because an input still to
  // be read lives there.
  const bool d_is_a_or_b = (m_accurate_nans || eft) && (a == d || b == d);
  const bool d_is_c = c == d && ((m_accurate_nans && (!keep_c_dup || software_fma)) ||
                                 (eft && !keep_c_rounded));
  const bool keep_d = d_is_a_or_b || d_is_c;

  const X64Reg tmp_xmm = XMM0;
  X64Reg result = XMM1;
  X64Reg c_dup = XMM2;
  const X64Reg c_rounded = XMM3;

  // XMM registers this instruction uses as scratch; std::fma calls save
  // only the ones asked for.
  BitSet32 own_scratch{XMM0 + 16, XMM1 + 16};

  RCX64Reg xmm2_guard;
  RCX64Reg xmm3_guard;
  if (eft)
  {
    xmm2_guard = fpr.Scratch(XMM2);
    xmm3_guard = fpr.Scratch(XMM3);
    RegCache::Realize(xmm2_guard, xmm3_guard);
    own_scratch[XMM2 + 16] = true;
    own_scratch[XMM3 + 16] = true;
  }
  else if (software_fma)
  {
    xmm2_guard = fpr.Scratch(XMM2);
    RegCache::Realize(xmm2_guard);
    own_scratch[XMM2 + 16] = true;
  }

  RCOpArg Ra;
  RCOpArg Rb;
  RCOpArg Rc;
  RCX64Reg Rd;
  RCX64Reg result_guard;
  RCX64Reg c_dup_guard;
  const bool c_bound_for_eft = eft && !keep_c_rounded && !keep_c_dup;
  if (software_fma)
  {
    // The high lanes are read with MOVHLPS, which needs registers.
    const bool bind_ab = packed || eft;
    Ra = bind_ab ? fpr.Bind(a, RCMode::Read) : fpr.Use(a, RCMode::Read);
    Rb = bind_ab ? fpr.Bind(b, RCMode::Read) : fpr.Use(b, RCMode::Read);
    Rc = (packed || c_bound_for_eft) ? fpr.Bind(c, RCMode::Read) : fpr.Use(c, RCMode::Read);
    Rd = fpr.Bind(d, single ? RCMode::Write : RCMode::ReadWrite);
    if (keep_d && packed)
    {
      result_guard = fpr.Scratch();
      RegCache::Realize(Ra, Rb, Rc, Rd, result_guard);
      result = X64Reg(result_guard);
      own_scratch[result + 16] = true;
    }
    else
    {
      RegCache::Realize(Ra, Rb, Rc, Rd);
      result = packed ? X64Reg(Rd) : XMM0;
    }
  }
  else
  {
    // The FMA forms need b in a register; b is the operand most likely to
    // be cached already.
    Ra = eft ? fpr.Bind(a, RCMode::Read) : fpr.Use(a, RCMode::Read);
    Rb = (use_fma || eft) ? fpr.Bind(b, RCMode::Read) : fpr.Use(b, RCMode::Read);
    Rc = c_bound_for_eft ? fpr.Bind(c, RCMode::Read) : fpr.Use(c, RCMode::Read);
    Rd = fpr.Bind(d, single ? RCMode::Write : RCMode::ReadWrite);
    RegCache::Realize(Ra, Rb, Rc, Rd);
  }

  if (eft_keeps_c_dup ||
      (nans_keep_c_dup && ((!software_fma && !eft) || (eft && packed))))
  {
    c_dup_guard = fpr.Scratch();
    RegCache::Realize(c_dup_guard);
    c_dup = c_dup_guard;
    own_scratch[c_dup + 16] = true;
  }

  // The registers to save around a std::fma call: everything live that is
  // caller-saved, except this instruction's scratch registers other than
  // `keep`.
  const auto saved_around_call = [&](BitSet32 keep) {
    const BitSet32 discard = own_scratch & ~keep;
    return CallerSavedRegistersInUse(keep) & ~discard;
  };
  const auto call_fma = [&](BitSet32 saved) {
    ABI_PushRegistersAndAdjustStack(saved, 0);
    ABI_CallFunction(SOFTWARE_FMA);
    ABI_PopRegistersAndAdjustStack(saved, 0);
  };

  if (software_fma)
  {
    if (keep_c_rounded)
    {
      if (eft && madds0)
      {
        MOVDDUP(c_rounded, Rc);
        Force25BitPrecision(c_rounded, R(c_rounded), XMM2);
      }
      else if (eft && madds1)
      {
        avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, c_rounded, Rc, Rc, 3);
        Force25BitPrecision(c_rounded, R(c_rounded), XMM2);
      }
      else
      {
        Force25BitPrecision(c_rounded, Rc, XMM2);
      }
    }

    // std::fma(XMM0 = a, XMM1 = c, XMM2 = b) per lane; the high lane (lane
    // 1) first when packed, so lane 0's result ends up in XMM0.
    const int top_lane = packed ? 1 : 0;
    for (int lane = top_lane; lane >= 0; --lane)
    {
      const bool c_from_low =
          madds0 || (lane == 0 && !madds1) || (keep_c_rounded && eft && madds1);
      if (c_from_low)
      {
        if (keep_c_rounded)
          MOVAPD(XMM1, R(c_rounded));
        else if (round_c)
          Force25BitPrecision(XMM1, Rc, XMM2);
        else if (Rc.IsSimpleReg())
          MOVAPD(XMM1, Rc);
        else
          MOVSD(XMM1, Rc);
      }
      else
      {
        MOVHLPS(XMM1, keep_c_rounded ? c_rounded : Rc.GetSimpleReg());
        if (round_c && !keep_c_rounded)
          Force25BitPrecision(XMM1, R(XMM1), XMM2);
      }

      // Lane 1's result moves to the high lane of `result` only now, after
      // c was read, since `result` may be frD and frD may be frC.
      if (packed && lane == 0)
        MOVLHPS(result, XMM0);

      if (lane != 0)
      {
        MOVHLPS(XMM0, Ra.GetSimpleReg());
        MOVHLPS(XMM2, Rb.GetSimpleReg());
      }
      else
      {
        if (Ra.IsSimpleReg())
          MOVAPD(XMM0, Ra);
        else
          MOVSD(XMM0, Ra);
        if (Rb.IsSimpleReg())
          MOVAPD(XMM2, Rb);
        else
          MOVSD(XMM2, Rb);
      }

      if (subtract)
        XORPS(XMM2, MConst(SIGN_LOW));

      BitSet32 keep{};
      if (packed && lane == 0)
        keep[result + 16] = true;
      if (keep_c_rounded && (eft || lane == 1))
        keep[c_rounded + 16] = true;
      call_fma(saved_around_call(keep));
    }

    if (packed)
      MOVSD(R(result), XMM0);  // lane 1 is already in the high half
    else
      DEBUG_ASSERT(result == XMM0);

    if (keep_c_dup)
    {
      if (madds0)
        MOVDDUP(c_dup, Rc);
      else if (madds1)
        avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, c_dup, Rc, Rc, 3);
      else
        DEBUG_ASSERT(false);
    }
  }
  else
  {
    // result = c (broadcast for madds, rounded if needed)
    if (broadcast_c)
    {
      if (madds0)
        MOVDDUP(result, Rc);
      else
        avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, result, Rc, Rc, 3);
      if (keep_c_dup)
        MOVAPD(R(c_dup), result);
      if (round_c)
        Force25BitPrecision(result, R(result), tmp_xmm);
    }
    else
    {
      DEBUG_ASSERT(!keep_c_dup);
      if (round_c)
        Force25BitPrecision(result, Rc, tmp_xmm);
      else
        MOVAPD(result, Rc);
    }
    if (keep_c_rounded)
      MOVAPD(R(c_rounded), result);

    if (use_fma)
    {
      // result = result * a +/- b
      const X64Reg addend = Rb.GetSimpleReg();
      if (packed && subtract)
        VFMSUB132PD(result, addend, Ra);
      else if (packed)
        VFMADD132PD(result, addend, Ra);
      else if (subtract)
        VFMSUB132SD(result, addend, Ra);
      else
        VFMADD132SD(result, addend, Ra);
    }
    else if (packed)
    {
      MULPD(result, Ra);
      if (subtract)
        SUBPD(result, Rb);
      else
        ADDPD(result, Rb);
    }
    else
    {
      MULSD(result, Ra);
      if (subtract)
        SUBSD(result, Rb);
      else
        ADDSD(result, Rb);
    }
  }

  // HandleNaNs uses XMM0 itself.
  if (m_accurate_nans && result == XMM0)
  {
    result = eft ? XMM1 : X64Reg(Rd);
    MOVAPD(result, R(XMM0));
    DEBUG_ASSERT(!keep_d);
  }

  const OpArg c_for_nans = keep_c_dup ? R(c_dup) : OpArg(Rc);

  // For scalars a NaN result skips the negation and the EFT entirely.
  std::optional<FixupBranch> nan_done;
  if (m_accurate_nans && !packed)
    nan_done = HandleNaNs(inst, result, XMM0, Ra, Rb, c_for_nans);

  if (eft)
  {
    // With s = result = p + q, p = a * c' and q = +/-b (see the interpreter's
    // NI_madd_msub): p' = s - q, q' = s - p', dp = p - p' (exactly, by FMA),
    // and -dq = q' - q. The routine then rounds s + dp + dq to single.
    if (result != XMM1)
    {
      MOVAPD(XMM1, R(result));
      result = XMM1;
    }

    X64Reg c_used = Rc.GetSimpleReg();
    BitSet32 keep{XMM1 + 16, XMM2 + 16};
    if (keep_c_rounded)
    {
      // c_rounded is XMM3, which the packed path below overwrites with
      // lane 0's error between a push and its pop, so it is not saved
      // around these calls.
      c_used = c_rounded;
    }
    else if (keep_c_dup)
    {
      c_used = c_dup;
      keep[c_dup + 16] = true;
    }

    if (packed)
    {
      // p' = s - q
      if (subtract)
        avx_op(&XEmitter::VADDPD, &XEmitter::ADDPD, XMM0, R(XMM1), Rb);
      else
        avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM0, R(XMM1), Rb);
      // q' = s - p'
      avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM2, R(XMM1), R(XMM0));

      // dp = a * c' - p'
      if (software_fma)
      {
        // Lane 0 into XMM3 (keeping XMM3's high lane if it is c_used), then
        // lane 1.
        keep[XMM0 + 16] = true;
        const BitSet32 first_saved = saved_around_call(keep);
        ABI_PushRegistersAndAdjustStack(first_saved, 0);
        avx_op(&XEmitter::VXORPS, &XEmitter::XORPS, XMM2, R(XMM0), MConst(SIGN_LOW));
        MOVAPD(XMM0, R(c_used));
        MOVAPD(XMM1, Ra);
        ABI_CallFunction(SOFTWARE_FMA);
        if (c_used == XMM3)
          MOVSD(XMM3, R(XMM0));
        else
          MOVAPD(XMM3, R(XMM0));
        ABI_PopRegistersAndAdjustStack(first_saved, 0);

        keep[XMM0 + 16] = false;
        keep[XMM3 + 16] = true;
        const BitSet32 second_saved = saved_around_call(keep);
        ABI_PushRegistersAndAdjustStack(second_saved, 0);
        MOVHLPS(XMM2, XMM0);
        XORPS(XMM2, MConst(SIGN_LOW));
        MOVHLPS(XMM0, c_used);
        MOVHLPS(XMM1, Ra.GetSimpleReg());
        ABI_CallFunction(SOFTWARE_FMA);
        ABI_PopRegistersAndAdjustStack(second_saved, 0);

        UNPCKLPD(XMM0, R(XMM3));
      }
      else if (use_fma)
      {
        VFMSUB231PD(XMM0, c_used, Ra);
      }
      else
      {
        avx_op(&XEmitter::VMULPD, &XEmitter::MULPD, XMM3, R(c_used), Ra);
        avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM0, R(XMM3), R(XMM0), true, false, XMM3);
      }

      // -dq = q' - q
      if (subtract)
        avx_op(&XEmitter::VADDPD, &XEmitter::ADDPD, XMM2, R(XMM2), Rb);
      else
        avx_op(&XEmitter::VSUBPD, &XEmitter::SUBPD, XMM2, R(XMM2), Rb);

      CALL(GetAsmRoutines()->ps_madd_eft);
    }
    else
    {
      if (subtract)
        avx_op(&XEmitter::VADDSD, &XEmitter::ADDSD, XMM0, R(XMM1), Rb, false);
      else
        avx_op(&XEmitter::VSUBSD, &XEmitter::SUBSD, XMM0, R(XMM1), Rb, false);
      avx_op(&XEmitter::VSUBSD, &XEmitter::SUBSD, XMM2, R(XMM1), R(XMM0), false);

      if (software_fma)
      {
        const BitSet32 saved = saved_around_call(keep);
        ABI_PushRegistersAndAdjustStack(saved, 0);
        avx_op(&XEmitter::VXORPS, &XEmitter::XORPS, XMM2, R(XMM0), MConst(SIGN_LOW));
        MOVAPD(XMM0, R(c_used));
        MOVAPD(XMM1, Ra);
        ABI_CallFunction(SOFTWARE_FMA);
        ABI_PopRegistersAndAdjustStack(saved, 0);
      }
      else if (use_fma)
      {
        VFMSUB231SD(XMM0, c_used, Ra);
      }
      else
      {
        avx_op(&XEmitter::VMULSD, &XEmitter::MULSD, XMM3, R(c_used), Ra, false);
        avx_op(&XEmitter::VSUBSD, &XEmitter::SUBSD, XMM0, R(XMM3), R(XMM0), false, false, XMM3);
      }

      if (subtract)
        ADDSD(XMM2, Rb);
      else
        SUBSD(XMM2, Rb);

      CALL(GetAsmRoutines()->fmadds_eft);
    }
  }

  // PowerPC rounds before negating, and x86's fnmadd would also flip the
  // sign of a zero addend, so the negation is a separate XOR.
  if (negate)
    XORPD(result, MConst(packed ? SIGN_BOTH : SIGN_LOW));

  // A packed NaN may be in one lane only, so nothing can be skipped.
  if (m_accurate_nans && packed)
    nan_done = HandleNaNs(inst, result, XMM0, Ra, Rb, c_for_nans);

  if (nan_done)
    SetJumpTarget(*nan_done);

  if (single)
    FinalizeSingleResult(Rd, R(result), packed, true);
  else
    FinalizeDoubleResult(Rd, R(result));
}

// ---------------------------------------------------------------------------
// Moves, sign operations and selection

void Jit64::fsign(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);

  const bool packed = inst.OPCD == 4;

  RCOpArg src = fpr.Use(inst.FB, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(src, Rd);

  const auto& sign = packed ? SIGN_BOTH : SIGN_LOW;
  switch (inst.SUBOP10)
  {
  case 40:  // fneg / ps_neg
    avx_op(&XEmitter::VXORPD, &XEmitter::XORPD, Rd, src, MConst(sign), packed);
    break;
  case 136:  // fnabs / ps_nabs
    avx_op(&XEmitter::VORPD, &XEmitter::ORPD, Rd, src, MConst(sign), packed);
    break;
  case 264:  // fabs / ps_abs
    avx_op(&XEmitter::VANDPD, &XEmitter::ANDPD, Rd, src, MConst(packed ? ABS_BOTH : ABS_LOW),
           packed);
    break;
  default:
    PanicAlertFmt("fsign bleh");
    break;
  }
}

void Jit64::fselx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);

  const u32 d = inst.FD;
  const u32 c = inst.FC;
  const bool packed = inst.OPCD == 4;  // ps_sel

  RCOpArg Ra = fpr.Use(inst.FA, RCMode::Read);
  RCOpArg Rb = fpr.Use(inst.FB, RCMode::Read);
  RCOpArg Rc = fpr.Use(c, RCMode::Read);
  // fsel keeps ps1 of frD.
  RCX64Reg Rd = fpr.Bind(d, packed ? RCMode::Write : RCMode::ReadWrite);
  RegCache::Realize(Ra, Rb, Rc, Rd);

  // d = a >= 0 ? c : b, with -0 counting as >= 0 and NaN choosing b. That is
  // d = (0 > a or unordered) ? b : c, so the mask is NLE(0, a).
  XORPD(XMM0, R(XMM0));
  if (packed)
    CMPPD(XMM0, Ra, CMP_NLE);
  else
    CMPSD(XMM0, Ra, CMP_NLE);

  if ((cpu_info.bAVX || cpu_info.bSSE4_1) && d == c)
  {
    // The select can happen in place. BLENDVPD is preferred to VBLENDVPD,
    // which costs extra uops on recent Intel cores.
    BLENDVPD(Rd, Rb);
    return;
  }

  if (cpu_info.bAVX)
  {
    X64Reg chosen_c = XMM1;
    if (Rc.IsSimpleReg())
      chosen_c = Rc.GetSimpleReg();
    else
      MOVAPD(XMM1, Rc);

    if (packed)
    {
      VBLENDVPD(Rd, chosen_c, Rb, XMM0);
      return;
    }
    VBLENDVPD(XMM1, chosen_c, Rb, XMM0);
  }
  else if (cpu_info.bSSE4_1)
  {
    MOVAPD(XMM1, Rc);
    BLENDVPD(XMM1, Rb);
  }
  else
  {
    // XMM1 = (mask & b) | (~mask & c)
    MOVAPD(XMM1, R(XMM0));
    ANDPD(XMM0, Rb);
    ANDNPD(XMM1, Rc);
    ORPD(XMM1, R(XMM0));
  }

  if (packed)
    MOVAPD(Rd, R(XMM1));
  else
    MOVSD(Rd, R(XMM1));
}

void Jit64::fmrx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);

  const u32 d = inst.FD;
  const u32 b = inst.FB;
  if (d == b)
    return;

  // Only ps0 is copied; ps1 of frD stays.
  RCOpArg Rd = fpr.Use(d, RCMode::Write);
  RegCache::Realize(Rd);
  if (!Rd.IsSimpleReg())
  {
    // frD lives in memory: store b's low lane over it.
    RCOpArg Rb = fpr.Bind(b, RCMode::Read);
    RegCache::Realize(Rb);
    MOVSD(Rd, Rb.GetSimpleReg());
    return;
  }

  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RegCache::Realize(Rb);
  // A load with MOVSD would zero the high lane; MOVLPD does not.
  if (Rb.IsSimpleReg())
    MOVSD(Rd, Rb.GetSimpleReg());
  else
    MOVLPD(Rd.GetSimpleReg(), Rb);
}

// ---------------------------------------------------------------------------
// Compares and conversions

void Jit64::FloatCompare(UGeckoInstruction inst, bool upper)
{
  const bool fprf = m_fprf && js.op->wantsFPRF;
  const u32 a = inst.FA;
  const u32 b = inst.FB;
  const u32 crf = inst.CRFD;

  // The PowerPC CR bits stored for each outcome, indexed by the CR bit
  // helpers' numbering (SO, EQ, GT, LT).
  std::array<u32, 4> outcome{PowerPC::CR_SO, PowerPC::CR_EQ, PowerPC::CR_GT, PowerPC::CR_LT};

  // fcmp followed by "cror x, x, y" within the same field (commonly to get
  // <= or >=) folds the OR into the outcome table.
  if (analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CROR_MERGE) &&
      CanMergeNextInstructions(1))
  {
    const UGeckoInstruction next = js.op[1].inst;
    const auto in_field = [crf](u32 crbit) { return (crbit >> 2) == crf; };
    if (next.OPCD == 19 && next.SUBOP10 == 449 && in_field(next.CRBA) && in_field(next.CRBB) &&
        in_field(next.CRBD))
    {
      js.skipInstructions = 1;
      js.downcountAmount++;
      const u32 dst = 3 - (next.CRBD & 3);
      outcome[dst] &= ~(1u << dst);
      outcome[3 - (next.CRBA & 3)] |= 1u << dst;
      outcome[3 - (next.CRBB & 3)] |= 1u << dst;
    }
  }

  RCOpArg Ra = upper ? fpr.Bind(a, RCMode::Read) : fpr.Use(a, RCMode::Read);
  RCX64Reg Rb = fpr.Bind(b, RCMode::Read);
  RegCache::Realize(Ra, Rb);

  if (fprf)
    AND(32, PPCSTATE(fpscr), Imm32(~u32{FPCC_MASK}));

  if (upper)
  {
    MOVHLPS(XMM0, Ra.GetSimpleReg());
    MOVHLPS(XMM1, Rb);
    UCOMISD(XMM1, R(XMM0));
  }
  else
  {
    UCOMISD(Rb, Ra);
  }

  // Loads the CR field value for one outcome into RSCRATCH and sets FPCC.
  const auto emit_outcome = [&](int helper_bit, u32 fpcc) {
    const u8 bits = static_cast<u8>(outcome[static_cast<size_t>(helper_bit)]);
    MOV(64, R(RSCRATCH), Imm64(PowerPC::ConditionRegister::PPCToInternal(bits)));
    if (fprf)
      OR(32, PPCSTATE(fpscr), Imm32(fpcc << FPRF_SHIFT));
  };

  // Comparing a register with itself can only be equal or unordered.
  const bool distinct = a != b;

  // UCOMISD(b, a): "above" is b > a, i.e. a < b. The parity check comes
  // before "below" because JB would also be taken for unordered.
  FixupBranch less, unordered, greater;
  if (distinct)
    less = J_CC(CC_A);
  unordered = J_CC(CC_P);
  if (distinct)
    greater = J_CC(CC_B);

  emit_outcome(PowerPC::CR_EQ_BIT, PowerPC::CR_EQ);
  const FixupBranch done_eq = J();

  SetJumpTarget(unordered);
  emit_outcome(PowerPC::CR_SO_BIT, PowerPC::CR_SO);

  FixupBranch done_unordered, done_greater;
  if (distinct)
  {
    done_unordered = J();
    SetJumpTarget(greater);
    emit_outcome(PowerPC::CR_GT_BIT, PowerPC::CR_GT);
    done_greater = J();
    SetJumpTarget(less);
    emit_outcome(PowerPC::CR_LT_BIT, PowerPC::CR_LT);
  }

  SetJumpTarget(done_eq);
  if (distinct)
  {
    SetJumpTarget(done_unordered);
    SetJumpTarget(done_greater);
  }

  MOV(64, PPCSTATE_CR(crf), R(RSCRATCH));
}

void Jit64::fcmpX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(jo.fp_exceptions);

  FloatCompare(inst);
}

void Jit64::fctiwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  RCOpArg Rb = fpr.Use(inst.RB, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.RD, RCMode::Write);
  RegCache::Realize(Rb, Rd);

  // CVT(T)PD2DQ returns 0x80000000 for anything out of range or NaN, which
  // is already right for large negative inputs and NaN. Large positive
  // inputs must give 0x7FFFFFFF, hence the MINSD with 2^31 - 1 first.
  // (fctiw of -0.0 should set the upper word to 0xFFF80001; not done.)
  MOVAPD(XMM0, MConst(FCTIW_BOUNDS));
  MINSD(XMM0, Rb);
  if (inst.SUBOP10 == 14)  // fctiw: current rounding mode
    CVTPD2DQ(XMM0, R(XMM0));
  else if (inst.SUBOP10 == 15)  // fctiwz: toward zero
    CVTTPD2DQ(XMM0, R(XMM0));
  // ps1 of frD is kept.
  MOVSD(Rd, XMM0);
}

void Jit64::frspx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  const u32 b = inst.FB;
  const bool packed = js.op->fprIsDuplicated[b] && !cpu_info.bAtom;

  RCOpArg Rb = fpr.Bind(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(Rb, Rd);

  FinalizeSingleResult(Rd, Rb, packed, true);
}

void Jit64::frsqrtex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);

  // The routine works on XMM0 and uses RSCRATCH_EXTRA.
  RCX64Reg extra = gpr.Scratch(RSCRATCH_EXTRA);
  RCOpArg Rb = fpr.Use(inst.FB, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(extra, Rb, Rd);

  MOVAPD(XMM0, Rb);
  CALL(asm_routines.frsqrte);
  FinalizeDoubleResult(Rd, R(XMM0));
}

void Jit64::fresx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);

  RCX64Reg extra = gpr.Scratch(RSCRATCH_EXTRA);
  RCOpArg Rb = fpr.Use(inst.FB, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(extra, Rb, Rd);

  // fres is a single-precision result written to both lanes.
  MOVAPD(XMM0, Rb);
  CALL(asm_routines.fres);
  MOVDDUP(Rd, R(XMM0));
  SetFPRFIfNeeded(R(XMM0), true);
}
