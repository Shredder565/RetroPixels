// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/JitAsm.h"

#include <climits>
#include <cstddef>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/JitRegister.h"
#include "Common/x64ABI.h"
#include "Common/x64Emitter.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/System.h"

using namespace Gen;

namespace
{
constexpr size_t ROUTINE_CONST_POOL_SIZE = 4096;
}  // namespace

Jit64AsmRoutineManager::Jit64AsmRoutineManager(Jit64& jit) : CommonAsmRoutines(jit)
{
}

void Jit64AsmRoutineManager::Init()
{
  m_const_pool.Init(AllocChildCodeSpace(ROUTINE_CONST_POOL_SIZE), ROUTINE_CONST_POOL_SIZE);
  Generate();
  WriteProtect(true);
}

void Jit64AsmRoutineManager::Regenerate()
{
  UnWriteProtect(false);
  ResetCodePtr();
  Generate();
  WriteProtect(true);
}

void Jit64AsmRoutineManager::ResetStack(X64CodeBlock& emitter)
{
  emitter.MOV(64, R(RSP), PPCSTATE(stored_stack_pointer));
}

void Jit64AsmRoutineManager::EmitBlockLookup()
{
  JitBaseBlockCache& block_cache = *m_jit.GetBlockCache();
  u8** const entry_points = block_cache.GetEntryPoints();

  if (entry_points != nullptr)
  {
    // Flat table of entry pointers indexed by (feature_flags << 30 | pc >> 2).
    // With 8-byte entries the byte offset is (feature_flags << 32 | pc) * 2.
    MOV(32, R(RSCRATCH2), PPCSTATE(feature_flags));
    SHL(64, R(RSCRATCH2), Imm8(32));
    MOV(32, R(RSCRATCH_EXTRA), PPCSTATE(pc));
    OR(64, R(RSCRATCH_EXTRA), R(RSCRATCH2));
    MOV(64, R(RSCRATCH2), Imm64(reinterpret_cast<u64>(entry_points)));
    MOV(64, R(RSCRATCH), MComplex(RSCRATCH2, RSCRATCH_EXTRA, SCALE_2, 0));

    TEST(64, R(RSCRATCH), R(RSCRATCH));
    const FixupBranch missing = J_CC(CC_Z);
    JMPptr(R(RSCRATCH));
    SetJumpTarget(missing);
    return;
  }

  // Hashed table of JitBlockData pointers indexed by (pc >> 2) & mask. With
  // 8-byte entries the byte offset is (pc & (mask << 2)) * 2. A hit still has
  // to be checked against the block's address and feature flags.
  const u64 table = reinterpret_cast<u64>(block_cache.GetFastBlockMapFallback());
  MOV(32, R(RSCRATCH), PPCSTATE(pc));
  MOV(32, R(RSCRATCH_EXTRA), R(RSCRATCH));
  AND(32, R(RSCRATCH), Imm32(JitBaseBlockCache::FAST_BLOCK_MAP_FALLBACK_MASK << 2));
  if (table <= static_cast<u64>(INT_MAX))
  {
    MOV(64, R(RSCRATCH), MScaled(RSCRATCH, SCALE_2, static_cast<s32>(table)));
  }
  else
  {
    MOV(64, R(RSCRATCH2), Imm64(table));
    MOV(64, R(RSCRATCH), MComplex(RSCRATCH2, RSCRATCH, SCALE_2, 0));
  }

  TEST(64, R(RSCRATCH), R(RSCRATCH));
  const FixupBranch missing = J_CC(CC_Z);

  // effectiveAddress directly follows feature_flags, so one 64-bit compare
  // checks both.
  static_assert(offsetof(JitBlockData, feature_flags) + 4 ==
                offsetof(JitBlockData, effectiveAddress));
  MOV(32, R(RSCRATCH2), PPCSTATE(feature_flags));
  SHL(64, R(RSCRATCH_EXTRA), Imm8(32));
  OR(64, R(RSCRATCH2), R(RSCRATCH_EXTRA));
  CMP(64, R(RSCRATCH2), MDisp(RSCRATCH, static_cast<s32>(offsetof(JitBlockData, feature_flags))));
  const FixupBranch wrong_block = J_CC(CC_NE);
  JMPptr(MDisp(RSCRATCH, static_cast<s32>(offsetof(JitBlockData, normalEntry))));

  SetJumpTarget(missing);
  SetJumpTarget(wrong_block);

  // The C++ dispatcher also looks at blocks the hashed table does not hold.
  ABI_PushRegistersAndAdjustStack({}, 0);
  MOV(64, R(ABI_PARAM1), Imm64(reinterpret_cast<u64>(&m_jit)));
  ABI_CallFunction(JitBase::Dispatch);
  ABI_PopRegistersAndAdjustStack({}, 0);
  TEST(64, R(ABI_RETURN), R(ABI_RETURN));
  const FixupBranch still_missing = J_CC(CC_Z);
  JMPptr(R(ABI_RETURN));
  SetJumpTarget(still_missing);
}

// The run loop:
//
//   enter_code:   save callee-saved registers and RSP, set up RPPCSTATE.
//   outer loop:   CoreTiming::GlobalAdvance, reload RMEM, then dispatch.
//   dispatcher*:  entry points used by block exits (see below); look up the
//                 block for pc and jump to it, compiling it first if needed.
//   do_timing:    the slice is used up; go round the outer loop unless the
//                 CPU is no longer running.
//   dispatcher_exit: restore and return to Run().
void Jit64AsmRoutineManager::Generate()
{
  const bool debugging = Config::IsDebuggingEnabled();
  auto& ppc_state = m_jit.m_ppc_state;
  auto& cpu = m_jit.m_system.GetCPU();

  enter_code = AlignCode16();
  // The extra 8 bytes give this frame a shadow area of its own for calls.
  ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, /*frame*/ 16);
  MOV(64, R(RPPCSTATE), Imm64(reinterpret_cast<u64>(&ppc_state) + PPCSTATE_BIAS));
  MOV(64, PPCSTATE(stored_stack_pointer), R(RSP));
  // A return prediction that never matches, below every real one.
  MOV(64, MDisp(RSP, 8), Imm32(0xFFFFFFFF));

  const u8* const outer_loop = GetCodePtr();
  ABI_PushRegistersAndAdjustStack({}, 0);
  ABI_CallFunction(CoreTiming::GlobalAdvance);
  ABI_PopRegistersAndAdjustStack({}, 0);
  // GlobalAdvance may have taken an exception, which can change MSR[DR].
  MOV(64, R(RMEM), PPCSTATE(mem_ptr));
  const FixupBranch to_lookup = J(debugging ? Jump::Near : Jump::Short);

  // WriteBLRExit lands here when the host stack's prediction was wrong,
  // with the block's cycle count in RSCRATCH2. The predictions on the stack
  // are worthless now, so drop them all.
  dispatcher_mispredicted_blr = GetCodePtr();
  AND(32, PPCSTATE(pc), Imm32(0xFFFFFFFC));
  ResetStack(*this);
  SUB(32, PPCSTATE(downcount), R(RSCRATCH2));

  // Entered with the flags of the downcount subtraction.
  dispatcher = GetCodePtr();
  const FixupBranch out_of_cycles = J_CC(CC_LE, Jump::Near);

  // Entered once the downcount is known to be positive.
  dispatcher_no_timing_check = GetCodePtr();
  FixupBranch stopped{};
  if (debugging)
  {
    MOV(64, R(RSCRATCH), ImmPtr(cpu.GetStatePtr()));
    CMP(32, MatR(RSCRATCH), Imm32(std::to_underlying(CPU::State::Running)));
    stopped = J_CC(CC_NE, Jump::Near);
  }

  SetJumpTarget(to_lookup);
  dispatcher_no_check = GetCodePtr();
  EmitBlockLookup();

  // No block yet: compile one. The stack is reset first because compiling
  // may clear the cache, and because a stack-overflow recovery on Windows
  // must not fault again while the JIT runs.
  ResetStack(*this);
  ABI_PushRegistersAndAdjustStack({}, 0);
  MOV(64, R(ABI_PARAM1), Imm64(reinterpret_cast<u64>(&m_jit)));
  MOV(32, R(ABI_PARAM2), PPCSTATE(pc));
  ABI_CallFunction(JitTrampoline);
  ABI_PopRegistersAndAdjustStack({}, 0);
  // Compiling may have raised an ISI, which can change MSR[DR].
  MOV(64, R(RMEM), PPCSTATE(mem_ptr));
  JMP(dispatcher_no_check);

  SetJumpTarget(out_of_cycles);
  do_timing = GetCodePtr();
  // CoreTiming's exception check expects npc to be the next instruction.
  MOV(32, R(RSCRATCH), PPCSTATE(pc));
  MOV(32, PPCSTATE(npc), R(RSCRATCH));
  MOV(64, R(RSCRATCH), ImmPtr(cpu.GetStatePtr()));
  CMP(32, MatR(RSCRATCH), Imm32(std::to_underlying(CPU::State::Running)));
  J_CC(CC_E, outer_loop);

  dispatcher_exit = GetCodePtr();
  if (debugging)
    SetJumpTarget(stopped);
  // Unwind whatever the BLR optimization left on the stack.
  ResetStack(*this);
  ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8, 16);
  RET();

  Common::JitRegister::Register(enter_code, GetCodePtr(), "JIT_Loop");

  GenerateCommon();
}

void Jit64AsmRoutineManager::GenerateCommon()
{
  frsqrte = AlignCode4();
  GenFrsqrte();
  fres = AlignCode4();
  GenFres();
  mfcr = AlignCode4();
  GenMfcr();
  cdts = AlignCode4();
  GenConvertDoubleToSingle();
  fmadds_eft = AlignCode4();
  GenerateFmaddsEft();
  ps_madd_eft = AlignCode4();
  GeneratePsMaddEft();

  GenQuantizedLoads();
  GenQuantizedSingleLoads();
  GenQuantizedStores();
  GenQuantizedSingleStores();
}
