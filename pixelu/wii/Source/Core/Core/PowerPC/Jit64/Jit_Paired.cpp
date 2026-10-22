// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include <optional>

#include "Common/MsgHandler.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"

using namespace Gen;

// Paired singles are held as two doubles in one XMM register: ps0 in the
// low lane, ps1 in the high lane.

void Jit64::ps_mr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);

  const u32 d = inst.FD;
  const u32 b = inst.FB;
  if (d == b)
    return;

  RCOpArg Rb = fpr.Use(b, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(d, RCMode::Write);
  RegCache::Realize(Rb, Rd);
  MOVAPD(Rd, Rb);
}

void Jit64::ps_mergeXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);

  RCOpArg Ra = fpr.Use(inst.FA, RCMode::Read);
  RCOpArg Rb = fpr.Use(inst.FB, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(Ra, Rb, Rd);

  // The two bits above the base opcode 528 pick the lanes: ps_mergeXY
  // makes {a.psX, b.psY}.
  const u32 lanes = (inst.SUBOP10 - 528) >> 5;
  if (inst.SUBOP10 < 528 || lanes > 3 || ((inst.SUBOP10 - 528) & 31) != 0)
  {
    ASSERT_MSG(DYNA_REC, 0, "ps_merge - invalid op");
    return;
  }

  if (lanes == 0)  // {a.ps0, b.ps0}
  {
    avx_op(&XEmitter::VUNPCKLPD, &XEmitter::UNPCKLPD, Rd, Ra, Rb);
  }
  else if (lanes == 3)  // {a.ps1, b.ps1}
  {
    avx_op(&XEmitter::VUNPCKHPD, &XEmitter::UNPCKHPD, Rd, Ra, Rb);
  }
  else if (lanes == 2)  // {a.ps1, b.ps0}
  {
    avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rd, Ra, Rb, 1);
  }
  else if (inst.FD != inst.FB)  // {a.ps0, b.ps1}
  {
    avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, Rd, Ra, Rb, 2);
  }
  else if (Ra.IsSimpleReg())
  {
    // d == b already holds b.ps1; only the low lane changes.
    MOVSD(Rd, Ra);
  }
  else
  {
    MOVLPD(Rd, Ra);
  }
}

void Jit64::ps_sum(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  RCOpArg Ra = fpr.Use(inst.FA, RCMode::Read);
  RCOpArg Rb = fpr.Use(inst.FB, RCMode::Read);
  RCOpArg Rc = fpr.Use(inst.FC, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(Ra, Rb, Rc, Rd);

  // sum = {a.ps0 + b.ps0, a.ps0 + b.ps1}; only the second lane is wanted.
  const X64Reg sum = XMM1;
  MOVDDUP(sum, Ra);
  ADDPD(sum, Rb);

  if (inst.SUBOP5 == 10)
  {
    // ps_sum0: {a.ps0 + b.ps1, c.ps1}
    UNPCKHPD(sum, Rc);
  }
  else if (inst.SUBOP5 == 11)
  {
    // ps_sum1: {c.ps0, a.ps0 + b.ps1}
    if (Rc.IsSimpleReg())
      MOVSD(sum, Rc);
    else
      MOVLPD(sum, Rc);
  }
  else
  {
    PanicAlertFmt("ps_sum WTF!!!");
  }

  // x86 and PowerPC produce the same NaN for additions, so no NaN fix-up.
  FinalizeSingleResult(Rd, R(sum));
}

void Jit64::ps_muls(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  const u32 c = inst.FC;
  // The Gekko rounds a double multiplier to 25 bits of mantissa.
  const bool round_c = !js.op->fprIsSingle[c];

  RCOpArg Ra = fpr.Use(inst.FA, RCMode::Read);
  RCOpArg Rc = fpr.Use(c, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  // The NaN fix-up needs the broadcast multiplier to survive, so it gets a
  // register of its own then.
  RCX64Reg c_lane = m_accurate_nans ? fpr.Scratch() : fpr.Scratch(XMM1);
  RegCache::Realize(Ra, Rc, Rd, c_lane);

  if (inst.SUBOP5 == 12)  // ps_muls0: broadcast c.ps0
    MOVDDUP(c_lane, Rc);
  else if (inst.SUBOP5 == 13)  // ps_muls1: broadcast c.ps1
    avx_op(&XEmitter::VSHUFPD, &XEmitter::SHUFPD, c_lane, Rc, Rc, 3);
  else
    PanicAlertFmt("ps_muls WTF!!!");

  if (round_c)
    Force25BitPrecision(XMM1, R(c_lane), XMM0);
  else if (XMM1 != c_lane)
    MOVAPD(XMM1, c_lane);
  MULPD(XMM1, Ra);

  if (m_accurate_nans)
  {
    const FixupBranch done = HandleNaNs(inst, XMM1, XMM0, Ra, std::nullopt, c_lane);
    SetJumpTarget(done);
  }

  FinalizeSingleResult(Rd, R(XMM1));
}

void Jit64::EmitPairedEstimate(UGeckoInstruction inst, const u8* scalar_routine)
{
  // The estimate routines work on the low lane of XMM0 and use
  // RSCRATCH_EXTRA; each lane goes through them in turn.
  RCX64Reg extra = gpr.Scratch(RSCRATCH_EXTRA);
  RCX64Reg Rb = fpr.Bind(inst.FB, RCMode::Read);
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(extra, Rb, Rd);

  MOVSD(XMM0, Rb);
  CALL(scalar_routine);
  MOVSD(Rd, XMM0);

  MOVHLPS(XMM0, Rb);
  CALL(scalar_routine);
  MOVLHPS(Rd, XMM0);

  FinalizeSingleResult(Rd, Rd);
}

void Jit64::ps_rsqrte(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);

  EmitPairedEstimate(inst, asm_routines.frsqrte);
}

void Jit64::ps_res(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || jo.div_by_zero_exceptions);

  EmitPairedEstimate(inst, asm_routines.fres);
}

void Jit64::ps_cmpXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(jo.fp_exceptions);

  // ps_cmpX1 (bit 6 of the extended opcode) compares the upper lanes.
  FloatCompare(inst, (inst.SUBOP10 & 64) != 0);
}
