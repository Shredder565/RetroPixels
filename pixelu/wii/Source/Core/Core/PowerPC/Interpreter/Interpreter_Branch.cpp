// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Branches, system call, return from interrupt and the HLE hook instruction.

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/HLE/HLE.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
// BO field bits (PowerPC numbering 0..4, here as values of the 5-bit field).
constexpr u32 BO_IGNORE_CONDITION = 0x10;  // branch regardless of the CR bit
constexpr u32 BO_BRANCH_IF_TRUE = 0x08;    // condition is "CR bit set"
constexpr u32 BO_IGNORE_COUNTER = 0x04;    // do not decrement or test CTR
constexpr u32 BO_BRANCH_IF_CTR_ZERO = 0x02;

bool ConditionHolds(const PowerPC::PowerPCState& ppc_state, u32 bo, u32 bi)
{
  if (bo & BO_IGNORE_CONDITION)
    return true;
  return ppc_state.cr.GetBit(bi) == ((bo & BO_BRANCH_IF_TRUE) ? 1u : 0u);
}

// Decrements CTR unless BO says not to, and tells whether the counter test passes.
bool CounterHolds(PowerPC::PowerPCState& ppc_state, u32 bo)
{
  if (bo & BO_IGNORE_COUNTER)
    return true;
  CTR(ppc_state)--;
  return (CTR(ppc_state) == 0) == ((bo & BO_BRANCH_IF_CTR_ZERO) != 0);
}

// Takes a branch to `target` (or records the fall-through) and ends the block.
void Resolve(Interpreter& interpreter, PowerPC::PowerPCState& ppc_state,
             Core::BranchWatch& branch_watch, UGeckoInstruction inst, bool taken, u32 target,
             bool link)
{
  const u32 here = ppc_state.pc;
  if (taken)
  {
    ppc_state.npc = target;
    if (link)
      LR(ppc_state) = here + 4;
    if (branch_watch.GetRecordingActive())
      branch_watch.HitTrue(here, target, inst, ppc_state.msr.IR);
  }
  else if (branch_watch.GetRecordingActive())
  {
    branch_watch.HitFalse(here, here + 4, inst, ppc_state.msr.IR);
  }
}
}  // namespace

void Interpreter::bx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  u32 target = static_cast<u32>(SignExt26(inst.LI << 2));
  if (!inst.AA)
    target += ppc_state.pc;
  Resolve(interpreter, ppc_state, interpreter.m_branch_watch, inst, true, target, inst.LK);
  interpreter.m_end_block = true;
}

void Interpreter::bcx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const bool counter_ok = CounterHolds(ppc_state, inst.BO);
  const bool taken = counter_ok && ConditionHolds(ppc_state, inst.BO, inst.BI);
  u32 target = static_cast<u32>(SignExt16(static_cast<s16>(inst.BD << 2)));
  if (!inst.AA)
    target += ppc_state.pc;
  Resolve(interpreter, ppc_state, interpreter.m_branch_watch, inst, taken, target, inst.LK);
  interpreter.m_end_block = true;
}

// bcctr never touches CTR (the decrementing forms are invalid).
void Interpreter::bcctrx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  DEBUG_ASSERT_MSG(POWERPC, (inst.BO_2 & BO_IGNORE_COUNTER) != 0,
                   "bcctr with a CTR-decrementing BO field");
  const bool taken = ConditionHolds(ppc_state, inst.BO_2, inst.BI_2);
  Resolve(interpreter, ppc_state, interpreter.m_branch_watch, inst, taken,
          CTR(ppc_state) & ~3u, inst.LK_3);
  interpreter.m_end_block = true;
}

void Interpreter::bclrx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const bool counter_ok = CounterHolds(ppc_state, inst.BO_2);
  const bool taken = counter_ok && ConditionHolds(ppc_state, inst.BO_2, inst.BI_2);
  // The target is the LR value from before this instruction links.
  Resolve(interpreter, ppc_state, interpreter.m_branch_watch, inst, taken, LR(ppc_state) & ~3u,
          inst.LK_3);
  interpreter.m_end_block = true;
}

// The HLE hook pseudo-instruction: `inst` carries the hook index.
void Interpreter::HLEFunction(Interpreter& interpreter, UGeckoInstruction inst)
{
  interpreter.m_end_block = true;
  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(interpreter.m_system);
  HLE::Execute(guard, interpreter.m_ppc_state.pc, inst.hex);
}

// rfi: MSR takes the saved bits from SRR1 (the Gekko restores more than the architecture
// lists), MSR[13] is cleared, and execution continues at SRR0.
void Interpreter::rfi(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  if (ppc_state.msr.PR)
  {
    GenerateProgramException(ppc_state, ProgramExceptionCause::PrivilegedInstruction);
    return;
  }

  constexpr u32 RESTORED_BITS = 0x87C0FFFF;
  constexpr u32 MSR_BIT_13 = 0x00040000;
  u32 msr = (ppc_state.msr.Hex & ~RESTORED_BITS) | (SRR1(ppc_state) & RESTORED_BITS);
  ppc_state.msr.Hex = msr & ~MSR_BIT_13;
  ppc_state.npc = SRR0(ppc_state);
  interpreter.m_system.GetPowerPC().MSRUpdated();
  interpreter.m_end_block = true;
}

// sc: taken immediately as a system call exception.
void Interpreter::sc(Interpreter& interpreter, UGeckoInstruction inst)
{
  interpreter.m_ppc_state.Exceptions |= EXCEPTION_SYSCALL;
  interpreter.m_system.GetPowerPC().CheckExceptions();
  interpreter.m_end_block = true;
}
