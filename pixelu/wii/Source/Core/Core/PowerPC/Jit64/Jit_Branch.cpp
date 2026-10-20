// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace Gen;

namespace
{
// rfi copies these MSR bits from SRR1 and keeps the rest.
constexpr u32 RFI_SRR1_BITS = 0x87C0FFFF;
// MSR[13] (POW) is always cleared by rfi.
constexpr u32 RFI_CLEARED_BITS = 0x00040000;

bool DecrementsCTR(UGeckoInstruction inst)
{
  return (inst.BO & BO_DONT_DECREMENT_FLAG) == 0;
}

bool TestsCondition(UGeckoInstruction inst)
{
  return (inst.BO & BO_DONT_CHECK_CONDITION) == 0;
}

// The CR bit a conditional branch tests, as (field, bit-within-field) in the
// emulated CR helpers' numbering, where bit 3 is LT and bit 0 is SO.
int ConditionField(UGeckoInstruction inst)
{
  return static_cast<int>(inst.BI >> 2);
}

int ConditionBit(UGeckoInstruction inst)
{
  return 3 - static_cast<int>(inst.BI & 3);
}

// The skip jumps when the condition does not hold.
bool SkipIfSet(UGeckoInstruction inst)
{
  return (inst.BO_2 & BO_BRANCH_IF_TRUE) == 0;
}
}  // namespace

void Jit64::sc(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  // The syscall exception returns to the next instruction.
  const u32 resume_at = js.compilerPC + 4;
  gpr.Flush();
  fpr.Flush();
  MOV(32, PPCSTATE(pc), Imm32(resume_at));
  LOCK();
  OR(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_SYSCALL));
  WriteExceptionExit();
}

void Jit64::rfi(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  constexpr u32 from_srr1 = RFI_SRR1_BITS & ~RFI_CLEARED_BITS;
  constexpr u32 kept = ~RFI_SRR1_BITS & ~RFI_CLEARED_BITS;
  gpr.Flush();
  fpr.Flush();

  AND(32, PPCSTATE(msr), Imm32(kept));
  MOV(32, R(RSCRATCH), PPCSTATE_SRR1);
  AND(32, R(RSCRATCH), Imm32(from_srr1));
  OR(32, PPCSTATE(msr), R(RSCRATCH));

  // The bits that matter for feature_flags all come from SRR1, so the
  // masked SRR1 value can stand in for the new MSR.
  MSRUpdated(R(RSCRATCH), RSCRATCH2);

  MOV(32, R(RSCRATCH), PPCSTATE_SRR0);
  WriteRfiExitDestInRSCRATCH();
}

template <bool condition>
void Jit64::WriteBranchWatch(u32 origin, u32 destination, UGeckoInstruction inst,
                             BitSet32 caller_save)
{
  if (!IsBranchWatchEnabled())
    return;

  using HitFn = void (*)(Core::BranchWatch*, u64, u32);
  HitFn hit;
  if (m_ppc_state.msr.IR)
    hit = condition ? &Core::BranchWatch::HitVirtualTrue_fk : &Core::BranchWatch::HitVirtualFalse_fk;
  else
    hit = condition ? &Core::BranchWatch::HitPhysicalTrue_fk :
                      &Core::BranchWatch::HitPhysicalFalse_fk;

  ABI_PushRegistersAndAdjustStack(caller_save, 0);
  MOV(64, R(ABI_PARAM1), ImmPtr(&m_branch_watch));
  MOV(64, R(ABI_PARAM2), Imm64(Core::FakeBranchWatchCollectionKey{origin, destination}));
  MOV(32, R(ABI_PARAM3), Imm32(inst.hex));
  ABI_CallFunction(hit);
  ABI_PopRegistersAndAdjustStack(caller_save, 0);
}

template void Jit64::WriteBranchWatch<true>(u32, u32, UGeckoInstruction, BitSet32);
template void Jit64::WriteBranchWatch<false>(u32, u32, UGeckoInstruction, BitSet32);

void Jit64::WriteBranchWatchDestInRSCRATCH(u32 origin, UGeckoInstruction inst, BitSet32 caller_save)
{
  if (!IsBranchWatchEnabled())
    return;

  // The destination is read from RSCRATCH after the first parameter is set.
  static_assert(ABI_PARAM1 != RSCRATCH);

  ABI_PushRegistersAndAdjustStack(caller_save, 0);
  MOV(64, R(ABI_PARAM1), ImmPtr(&m_branch_watch));
  MOV(32, R(ABI_PARAM3), R(RSCRATCH));
  MOV(32, R(ABI_PARAM2), Imm32(origin));
  MOV(32, R(ABI_PARAM4), Imm32(inst.hex));
  if (m_ppc_state.msr.IR)
    ABI_CallFunction(&Core::BranchWatch::HitVirtualTrue);
  else
    ABI_CallFunction(&Core::BranchWatch::HitPhysicalTrue);
  ABI_PopRegistersAndAdjustStack(caller_save, 0);
}

Jit64::BranchSkips Jit64::EmitBranchConditionTests(UGeckoInstruction inst)
{
  BranchSkips skips;
  if (DecrementsCTR(inst))
  {
    SUB(32, PPCSTATE_CTR, Imm8(1));
    // BO_BRANCH_IF_CTR_0 set: branch only when CTR reached zero.
    const CCFlags skip_cc = (inst.BO & BO_BRANCH_IF_CTR_0) ? CC_NZ : CC_Z;
    skips.ctr = J_CC(skip_cc, Jump::Near);
  }
  if (TestsCondition(inst))
    skips.condition = JumpIfCRFieldBit(ConditionField(inst), ConditionBit(inst), SkipIfSet(inst));
  return skips;
}

void Jit64::BindBranchSkips(const BranchSkips& skips)
{
  if (skips.condition)
    SetJumpTarget(*skips.condition);
  if (skips.ctr)
    SetJumpTarget(*skips.ctr);
}

void Jit64::EmitFlattenedBranch(UGeckoInstruction inst)
{
  // The analyzer merged the target into this block, so nothing is emitted
  // for the jump itself. A bl whose return is not in the block still needs
  // its return prediction.
  WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, CallerSavedRegistersInUse());
  if (inst.LK && !js.op->skipLRStack)
    FakeBLCall(js.compilerPC + 4);
}

void Jit64::ExitToBranchTarget(UGeckoInstruction inst)
{
  // Registers must already be flushed.
  WriteBranchWatch<true>(js.compilerPC, js.op->branchTo, inst, {});
  if (js.op->branchIsIdleLoop)
    WriteIdleExit(js.op->branchTo);
  else
    WriteExit(js.op->branchTo, inst.LK, js.compilerPC + 4);
}

void Jit64::EmitBranchNotTaken(UGeckoInstruction inst, bool watch_after_exit)
{
  const u32 next = js.compilerPC + 4;
  const bool continues = analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE);
  if (!continues)
  {
    gpr.Flush();
    fpr.Flush();
    WriteBranchWatch<false>(js.compilerPC, next, inst, {});
    WriteExit(next);
    if (!watch_after_exit)
      return;
  }
  WriteBranchWatch<false>(js.compilerPC, next, inst, CallerSavedRegistersInUse());
}

void Jit64::bx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  // LR is written even when the block continues at the target.
  if (inst.LK)
    MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));

  if (!js.isLastInstruction)
  {
    EmitFlattenedBranch(inst);
    return;
  }

  gpr.Flush();
  fpr.Flush();
  ExitToBranchTarget(inst);
}

void Jit64::bcx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  const BranchSkips skips = EmitBranchConditionTests(inst);

  if (inst.LK)
    MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));

  // An unconditional bc in the middle of a block was followed by the
  // analyzer, just like b.
  if (!js.isLastInstruction && !DecrementsCTR(inst) && !TestsCondition(inst))
  {
    EmitFlattenedBranch(inst);
    return;
  }

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();
    ExitToBranchTarget(inst);
  }

  BindBranchSkips(skips);
  EmitBranchNotTaken(inst, true);
}

void Jit64::bcctrx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  // Decrementing CTR while branching to it is an invalid form.
  DEBUG_ASSERT_MSG(POWERPC, inst.BO_2 & BO_DONT_DECREMENT_FLAG,
                   "bcctrx with decrement and test CTR option is invalid!");

  const u32 next = js.compilerPC + 4;

  if (inst.BO_2 & BO_DONT_CHECK_CONDITION)
  {
    gpr.Flush();
    fpr.Flush();

    MOV(32, R(RSCRATCH), PPCSTATE_CTR);
    if (inst.LK_3)
      MOV(32, PPCSTATE_LR, Imm32(next));
    AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
    WriteBranchWatchDestInRSCRATCH(js.compilerPC, inst, BitSet32{RSCRATCH});
    WriteExitDestInRSCRATCH(inst.LK_3, next);
    return;
  }

  // The conditional form is rare (some builds of the NES emulator use it).
  const FixupBranch skip =
      JumpIfCRFieldBit(ConditionField(inst), ConditionBit(inst), SkipIfSet(inst));
  MOV(32, R(RSCRATCH), PPCSTATE_CTR);
  AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
  if (inst.LK_3)
    MOV(32, PPCSTATE_LR, Imm32(next));

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();
    // WriteExitDestInRSCRATCH stores pc itself.
    WriteBranchWatchDestInRSCRATCH(js.compilerPC, inst, BitSet32{RSCRATCH});
    WriteExitDestInRSCRATCH(inst.LK_3, next);
  }
  SetJumpTarget(skip);

  EmitBranchNotTaken(inst, true);
}

void Jit64::bclrx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITBranchOff);

  const BranchSkips skips = EmitBranchConditionTests(inst);

  MOV(32, R(RSCRATCH), PPCSTATE_LR);
  // With the BLR optimization the low bits need no masking here: only
  // aligned return addresses are ever predicted, so a matching prediction
  // implies an aligned LR, and the mispredict path masks pc itself.
  if (!m_enable_blr_optimization)
    AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
  if (inst.LK)
    MOV(32, PPCSTATE_LR, Imm32(js.compilerPC + 4));

  {
    // The taken path leaves the block; the fork keeps the not-taken path's
    // cache state intact.
    RCForkGuard taken_gprs = gpr.Fork();
    RCForkGuard taken_fprs = fpr.Fork();
    gpr.Flush();
    fpr.Flush();

    const u32 here = js.compilerPC;
    if (!js.op->branchIsIdleLoop)
    {
      // Destination is already in RSCRATCH, which the watch must not clobber.
      WriteBranchWatchDestInRSCRATCH(here, inst, BitSet32{RSCRATCH});
      WriteBLRExit();
    }
    else
    {
      const u32 loop_head = js.op->branchTo;
      WriteBranchWatch<true>(here, loop_head, inst, {});
      WriteIdleExit(loop_head);
    }
  }

  BindBranchSkips(skips);
  EmitBranchNotTaken(inst, false);
}
