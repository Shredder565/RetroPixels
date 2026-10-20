// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include <bit>
#include <cstdlib>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/HostDisassembler.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/x64ABI.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/Host.h"
#include "Core/MachineContext.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/Jit64/JitAsm.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/FarCodeCache.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/Jit64Common/TrampolineCache.h"
#include "Core/PowerPC/JitCommon/ConstantPropagation.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace Gen;
using namespace PowerPC;

namespace
{
// MSR[FP], MSR[EE].
constexpr u32 MSR_FP_BIT = 1 << 13;
constexpr u32 MSR_EE_BIT = 0x0008000;

// The emulated address space a fastmem base points at.
constexpr uintptr_t GUEST_ADDRESS_SPACE = 0x1'0000'0000;

// Restores the source register of a store that byte-swapped it in place
// before faulting, so the trampoline can swap it again.
void UnswapRegister(u64* reg, int bits)
{
  switch (bits)
  {
  case 8:
    break;
  case 16:
    *reg = Common::swap16(static_cast<u16>(*reg));
    break;
  case 32:
    *reg = Common::swap32(static_cast<u32>(*reg));
    break;
  case 64:
    *reg = Common::swap64(*reg);
    break;
  default:
    DEBUG_ASSERT(false);
    break;
  }
}
}  // namespace

Jit64::Jit64(Core::System& system)
    : JitBase(system), QuantizedMemoryRoutines(*this),
      m_disassembler(HostDisassembler::Factory(HostDisassembler::Platform::x86_64))
{
}

Jit64::~Jit64() = default;

// ---------------------------------------------------------------------------
// Host faults

bool Jit64::HandleFault(uintptr_t access_address, SContext* ctx)
{
  // A touch of the stack guard means the BLR optimization overflowed the
  // host stack.
  const uintptr_t guard_begin = reinterpret_cast<uintptr_t>(m_stack_guard);
  const bool in_guard = access_address >= guard_begin && access_address < guard_begin + GUARD_SIZE;
  if (m_enable_blr_optimization && in_guard)
    return HandleStackFault();

  auto& memory = m_system.GetMemory();
  if (!memory.IsAddressInFastmemArea(reinterpret_cast<u8*>(access_address)))
    return false;

  // A fastmem access hit an unmapped page: replace it with a slow-path
  // trampoline. Only accesses to I/O or unmapped memory get here, so the
  // cost of the trampolines does not matter.
  const auto& ppc_state = m_system.GetPPCState();
  const uintptr_t base = reinterpret_cast<uintptr_t>(ppc_state.msr.DR ? memory.GetLogicalBase() :
                                                                        memory.GetPhysicalBase());
  if (access_address < base || access_address >= base + GUEST_ADDRESS_SPACE)
  {
    WARN_LOG_FMT(DYNA_REC,
                 "Jit64 address calculation overflowed! Please report if this happens a lot. "
                 "PC {:#018x}, access address {:#018x}, memory base {:#018x}, MSR.DR {}",
                 ctx->CTX_PC, access_address, base, ppc_state.msr.DR);
  }
  return BackPatch(ctx);
}

bool Jit64::BackPatch(SContext* ctx)
{
  u8* const fault_pc = reinterpret_cast<u8*>(ctx->CTX_PC);
  if (!IsInSpace(fault_pc))
    return false;

  const auto entry = m_back_patch_info.find(fault_pc);
  if (entry == m_back_patch_info.end())
  {
    PanicAlertFmt("BackPatch: no register use entry for address {}", fmt::ptr(fault_pc));
    return false;
  }
  const TrampolineInfo& info = entry->second;

  // With memchecks, the instruction's exception exit (in far code) is reused
  // by the trampoline.
  u8* exception_exit = nullptr;
  if (jo.memcheck)
  {
    const auto handler = m_exception_handler_at_loc.find(fault_pc);
    if (handler != m_exception_handler_at_loc.end())
      exception_exit = handler->second;
  }

  js.generatingTrampoline = true;
  js.trampolineExceptionHandler = exception_exit;
  js.compilerPC = info.pc;
  const u8* const trampoline = trampolines.GenerateTrampoline(info);
  js.generatingTrampoline = false;
  js.trampolineExceptionHandler = nullptr;

  // Overwrite the access (padded to at least BACKPATCH_SIZE bytes) with a
  // jump to the trampoline, which returns to the instruction after it.
  u8* const patch_end = info.start + info.len;
  XEmitter patcher(info.start, patch_end);
  patcher.JMP(trampoline);
  while (patcher.GetCodePtr() < patch_end)
    patcher.INT3();

  // Undo what the faulting sequence already did to registers so the
  // trampoline starts from the original state.
  if (info.nonAtomicSwapStoreSrc != INVALID_REG)
    UnswapRegister(ContextRN(ctx, static_cast<int>(info.nonAtomicSwapStoreSrc)), info.accessSize * 8);
  if (info.offsetAddedToAddress)
  {
    u64* const address_reg = ContextRN(ctx, static_cast<int>(info.op_arg.GetSimpleReg()));
    *address_reg = static_cast<u32>(*address_reg - static_cast<u64>(static_cast<s64>(info.offset)));
  }

  ctx->CTX_PC = reinterpret_cast<std::remove_cvref_t<decltype(ctx->CTX_PC)>>(trampoline);
  return true;
}

// ---------------------------------------------------------------------------
// Lifetime

void Jit64::Init()
{
  InitFastmemArena();
  RefreshConfig();
  EnableBlockLink();

  jo.optimizeGatherPipe = true;
  jo.accurateSinglePrecision = true;
  js.fastmemLoadStore = nullptr;
  js.compilerPC = 0;

  gpr.SetEmitter(this);
  fpr.SetEmitter(this);

  // One allocation holds, in order: near code, the asm routines, the
  // trampolines, far code and the constant pool. Memchecks put far more
  // code into trampolines and far code.
  const size_t routines_size = asm_routines.CODE_SIZE;
  const size_t trampolines_size = jo.memcheck ? TRAMPOLINE_CODE_SIZE_MMU : TRAMPOLINE_CODE_SIZE;
  const size_t far_size = jo.memcheck ? FARCODE_SIZE_MMU : FARCODE_SIZE;
  const size_t pool_size = ConstantPool::CONST_POOL_SIZE;
  AllocCodeSpace(CODE_SIZE + routines_size + trampolines_size + far_size + pool_size);
  AddChildCodeSpace(&asm_routines, routines_size);
  AddChildCodeSpace(&trampolines, trampolines_size);
  AddChildCodeSpace(&m_far_code, far_size);
  m_const_pool.Init(AllocChildCodeSpace(pool_size), pool_size);
  ResetCodePtr();

  InitBLROptimization();
  m_stack_guard = nullptr;

  blocks.Init();
  // The shared routines must not use far code, which is wiped on every
  // cache clear, so far code is only switched on after generating them.
  asm_routines.Init();
  m_far_code.Init();
  Clear();

  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;
  EnableOptimization();

  ResetFreeMemoryRanges();
}

void Jit64::ClearCache()
{
  blocks.Clear();
  blocks.ClearRangesToFree();
  trampolines.ClearCodeSpace();
  m_far_code.ClearCodeSpace();
  m_const_pool.Clear();
  ClearCodeSpace();
  Clear();
  RefreshConfig();
  asm_routines.Regenerate();
  ResetFreeMemoryRanges();
  Host_JitCacheInvalidation();
}

void Jit64::Shutdown()
{
  FreeCodeSpace();
  m_system.GetMemory().ShutdownFastmemArena();

  blocks.Shutdown();
  m_far_code.Shutdown();
  m_const_pool.Shutdown();
}

// Hands the code ranges of blocks destroyed since the last compile back to
// the allocators.
void Jit64::FreeRanges()
{
  for (const auto& [begin, end] : blocks.GetRangesToFreeNear())
    m_free_ranges_near.insert(begin, end);
  for (const auto& [begin, end] : blocks.GetRangesToFreeFar())
    m_free_ranges_far.insert(begin, end);
  blocks.ClearRangesToFree();
}

void Jit64::ResetFreeMemoryRanges()
{
  m_free_ranges_near.clear();
  m_free_ranges_near.insert(region, region + region_size);
  m_free_ranges_far.clear();
  m_free_ranges_far.insert(m_far_code.GetWritableCodePtr(), m_far_code.GetWritableCodeEnd());
}

void Jit64::EraseSingleBlock(const JitBlock& block)
{
  blocks.EraseSingleBlock(block);
  FreeRanges();
}

std::vector<JitBase::MemoryStats> Jit64::GetMemoryStats() const
{
  return {{"near", m_free_ranges_near.get_stats()}, {"far", m_free_ranges_far.get_stats()}};
}

std::size_t Jit64::DisassembleNearCode(const JitBlock& block, std::ostream& stream) const
{
  return m_disassembler->Disassemble(block.normalEntry, block.near_end, stream);
}

std::size_t Jit64::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const
{
  return m_disassembler->Disassemble(block.far_begin, block.far_end, stream);
}

// ---------------------------------------------------------------------------
// Instructions without their own translation

void Jit64::FallBackToInterpreter(UGeckoInstruction inst)
{
  // The interpreter works on ppcState, so everything goes back there. Some
  // registers may have been discarded already; that is fine.
  FlushCarry();
  gpr.Flush(BitSet32(0xFFFFFFFF), RegCache::FlushMode::Full,
            RegCache::IgnoreDiscardedRegisters::Yes);
  fpr.Flush(BitSet32(0xFFFFFFFF), RegCache::FlushMode::Full,
            RegCache::IgnoreDiscardedRegisters::Yes);

  const PPCAnalyst::CodeOp& op = *js.op;
  if (op.canEndBlock)
  {
    MOV(32, PPCSTATE(pc), Imm32(js.compilerPC));
    MOV(32, PPCSTATE(npc), Imm32(js.compilerPC + 4));
  }

  ABI_PushRegistersAndAdjustStack({}, 0);
  ABI_CallFunctionPC(Interpreter::GetInterpreterOp(inst), &m_system.GetInterpreter(), inst.hex);
  ABI_PopRegistersAndAdjustStack({}, 0);

  // The outputs are in ppcState now, whatever the caches thought of them.
  gpr.Reset(op.regsOut);
  fpr.Reset(op.GetFregsOut());
  m_constant_propagation.ClearGPRs(op.regsOut);

  if ((op.opinfo->flags & FL_SET_MSR) != 0)
    EmitUpdateMembase();

  if (op.canEndBlock)
  {
    // The interpreter reports a taken branch or exception through npc.
    MOV(32, R(RSCRATCH), PPCSTATE(npc));
    FixupBranch fall_through{};
    if (!js.isLastInstruction)
    {
      CMP(32, R(RSCRATCH), Imm32(js.compilerPC + 4));
      fall_through = J_CC(CC_Z);
    }
    MOV(32, PPCSTATE(pc), R(RSCRATCH));
    WriteExceptionExit();
    if (!js.isLastInstruction)
      SetJumpTarget(fall_through);
    return;
  }

  if (!ShouldHandleFPExceptionForInstruction(js.op))
    return;

  // Enabled FP exceptions raise a program exception.
  TEST(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_PROGRAM));
  const FixupBranch raised = J_CC(CC_NZ, Jump::Near);
  SwitchToFarCode();
  SetJumpTarget(raised);
  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();
    MOV(32, PPCSTATE(pc), Imm32(op.address));
    WriteExceptionExit();
  }
  SwitchToNearCode();
}

void Jit64::HLEFunction(u32 hook_index)
{
  gpr.Flush();
  fpr.Flush();
  ABI_PushRegistersAndAdjustStack({}, 0);
  ABI_CallFunctionCCP(HLE::ExecuteFromJIT, js.compilerPC, hook_index, &m_system);
  ABI_PopRegistersAndAdjustStack({}, 0);
}

void Jit64::DoNothing(UGeckoInstruction)
{
}

// Debug aid (m_im_here_debug): logs block entries, every 1024th repeat of
// an address.
void Jit64::ImHere(Jit64& jit)
{
  const u32 pc = jit.m_ppc_state.pc;
  if (jit.m_im_here_log)
  {
    static File::IOFile log_file;
    if (!log_file)
      log_file.Open("log64.txt", "w");
    log_file.WriteString(fmt::format("{0:08x}\n", pc));
  }

  const auto seen = jit.m_been_here.find(pc);
  if (seen != jit.m_been_here.end())
  {
    ++seen->second;
    if ((seen->second & 1023) != 0)
      return;
  }
  INFO_LOG_FMT(DYNA_REC, "I'm here - PC = {:08x} , LR = {:08x}", pc, LR(jit.m_ppc_state));
  jit.m_been_here[pc] = 1;
}

// ---------------------------------------------------------------------------
// Block exits

bool Jit64::Cleanup()
{
  bool emitted = false;

  // A gather pipe that filled up during the block is flushed before leaving.
  if (jo.optimizeGatherPipe && js.fifoBytesSinceCheck > 0)
  {
    MOV(64, R(RSCRATCH), PPCSTATE(gather_pipe_ptr));
    SUB(64, R(RSCRATCH), PPCSTATE(gather_pipe_base_ptr));
    CMP(64, R(RSCRATCH), Imm32(GPFifo::GATHER_PIPE_SIZE));
    const FixupBranch not_full = J_CC(CC_L);
    ABI_PushRegistersAndAdjustStack({}, 0);
    ABI_CallFunctionP(GPFifo::UpdateGatherPipe, &m_system.GetGPFifo());
    ABI_PopRegistersAndAdjustStack({}, 0);
    SetJumpTarget(not_full);
    emitted = true;
  }

  if ((m_ppc_state.feature_flags & FEATURE_FLAG_PERFMON) != 0)
  {
    ABI_PushRegistersAndAdjustStack({}, 0);
    ABI_CallFunctionCCCP(PowerPC::UpdatePerformanceMonitor, js.downcountAmount, js.numLoadStoreInst,
                         js.numFloatingPointInst, &m_ppc_state);
    ABI_PopRegistersAndAdjustStack({}, 0);
    emitted = true;
  }

  if (IsProfilingEnabled())
  {
    ABI_PushRegistersAndAdjustStack({}, 0);
    ABI_CallFunctionPC(&JitBlock::ProfileData::EndProfiling, js.curBlock->profile_data.get(),
                       js.downcountAmount);
    ABI_PopRegistersAndAdjustStack({}, 0);
    emitted = true;
  }

  return emitted;
}

// The BLR optimization keeps guest calls on the host stack: each call pushes
// (feature_flags << 32 | return address), which a later blr compares against
// to return with a host RET.
void Jit64::PushBLRPrediction(u32 after)
{
  const u64 prediction = static_cast<u64>(m_ppc_state.feature_flags) << 32 | after;
  MOV(64, R(RSCRATCH2), Imm64(prediction));
  PUSH(RSCRATCH2);
}

void Jit64::FakeBLCall(u32 after)
{
  if (!m_enable_blr_optimization)
    return;

  // A followed bl: keep the host stack in step with a CALL that "returns"
  // to an exit for the instruction after the bl.
  PushBLRPrediction(after);
  const FixupBranch into_callee = CALL();
  POP(RSCRATCH2);
  JustWriteExit(after, false, 0);
  SetJumpTarget(into_callee);
}

void Jit64::EmitUpdateMembase()
{
  MOV(64, R(RMEM), PPCSTATE(mem_ptr));
}

// After a write to MSR: refresh the memory base and feature flags from
// MSR[DR] and MSR[IR], and process a pending page table update once data
// translation is on.
void Jit64::MSRUpdated(const OpArg& msr, X64Reg scratch_reg)
{
  ASSERT(!msr.IsSimpleReg(scratch_reg));

  constexpr u32 dr_mask = 1 << UReg_MSR{}.DR.StartBit();
  static_assert(UReg_MSR{}.DR.StartBit() == 4);
  static_assert(UReg_MSR{}.IR.StartBit() == 5);
  static_assert(FEATURE_FLAG_MSR_DR == 1 << 0);
  static_assert(FEATURE_FLAG_MSR_IR == 1 << 1);

  auto& memory = m_system.GetMemory();
  const bool known = msr.IsImm();
  const bool known_dr = known && UReg_MSR(msr.Imm32()).DR;

  if (known)
  {
    MOV(64, R(RMEM), ImmPtr(known_dr ? memory.GetLogicalBase() : memory.GetPhysicalBase()));
  }
  else
  {
    MOV(64, R(RMEM), ImmPtr(memory.GetLogicalBase()));
    MOV(64, R(scratch_reg), ImmPtr(memory.GetPhysicalBase()));
    TEST(32, msr, Imm32(dr_mask));
    CMOVcc(64, RMEM, R(scratch_reg), CC_Z);
  }
  MOV(64, PPCSTATE(mem_ptr), R(RMEM));

  // Feature flag bits 0-1 mirror MSR bits 4-5.
  const u32 kept_flags = m_ppc_state.feature_flags & ~0x3u;
  if (known)
  {
    MOV(32, PPCSTATE(feature_flags), Imm32(kept_flags | ((msr.Imm32() >> 4) & 0x3)));
  }
  else
  {
    MOV(32, R(scratch_reg), msr);
    SHR(32, R(scratch_reg), Imm8(4));
    AND(32, R(scratch_reg), Imm32(0x3));
    if (kept_flags != 0)
      OR(32, R(scratch_reg), Imm32(kept_flags));
    MOV(32, PPCSTATE(feature_flags), R(scratch_reg));
  }

  if (known && !known_dr)
    return;

  gpr.Flush();
  fpr.Flush();
  FixupBranch translation_off{};
  if (!known)
  {
    TEST(32, msr, Imm32(dr_mask));
    translation_off = J_CC(CC_Z);
  }
  CMP(8, PPCSTATE(pagetable_update_pending), Imm8(0));
  const FixupBranch nothing_pending = J_CC(CC_E);
  ABI_CallFunctionP(&PowerPC::MMU::PageTableUpdatedFromJit, &m_system.GetMMU());
  SetJumpTarget(nothing_pending);
  if (!known)
    SetJumpTarget(translation_off);
}

void Jit64::WriteExit(u32 destination, bool bl, u32 after)
{
  const bool push_prediction = bl && m_enable_blr_optimization;

  Cleanup();
  if (push_prediction)
    PushBLRPrediction(after);
  SUB(32, PPCSTATE(downcount), Imm32(js.downcountAmount));
  JustWriteExit(destination, push_prediction, after);
}

// Emits a linkable exit to `destination` (the downcount must already be
// updated). A call exit runs the callee as a host CALL and continues with an
// exit to `after` when it returns.
void Jit64::JustWriteExit(u32 destination, bool bl, u32 after)
{
  JitBlock::LinkData link;
  link.exitAddress = destination;
  link.linkStatus = false;
  link.call = bl;

  MOV(32, PPCSTATE(pc), Imm32(destination));

  if (!bl)
  {
    J_CC(CC_LE, asm_routines.do_timing);
    link.exitPtrs = GetWritableCodePtr();
    // Always the 5-byte form, so a link can be patched over it in place.
    JMP(asm_routines.dispatcher_no_timing_check, true);
    js.curBlock->linkData.push_back(link);
    return;
  }

  // Out of cycles: run the timing code first, then skip the call.
  const FixupBranch out_of_cycles = J_CC(CC_LE, Jump::Near);
  SwitchToFarCode();
  SetJumpTarget(out_of_cycles);
  CALL(asm_routines.do_timing);
  const FixupBranch skip_call = J(Jump::Near);
  SwitchToNearCode();

  link.exitPtrs = GetWritableCodePtr();
  CALL(asm_routines.dispatcher_no_timing_check);

  SetJumpTarget(skip_call);
  POP(RSCRATCH);
  JustWriteExit(after, false, 0);

  js.curBlock->linkData.push_back(link);
}

void Jit64::WriteExitDestInRSCRATCH(bool bl, u32 after)
{
  const bool push_prediction = bl && m_enable_blr_optimization;

  MOV(32, PPCSTATE(pc), R(RSCRATCH));
  Cleanup();
  if (push_prediction)
    PushBLRPrediction(after);
  SUB(32, PPCSTATE(downcount), Imm32(js.downcountAmount));

  if (!push_prediction)
  {
    JMP(asm_routines.dispatcher);
    return;
  }
  CALL(asm_routines.dispatcher);
  POP(RSCRATCH);
  JustWriteExit(after, false, 0);
}

// blr with the BLR optimization: if the target matches the prediction on
// the host stack, return with RET; otherwise take the mispredict path,
// which expects the downcount amount in RSCRATCH2.
void Jit64::WriteBLRExit()
{
  if (!m_enable_blr_optimization)
  {
    WriteExitDestInRSCRATCH();
    return;
  }

  MOV(32, PPCSTATE(pc), R(RSCRATCH));
  if (Cleanup())
    MOV(32, R(RSCRATCH), PPCSTATE(pc));

  if (m_ppc_state.feature_flags != 0)
  {
    MOV(32, R(RSCRATCH2), Imm32(m_ppc_state.feature_flags));
    SHL(64, R(RSCRATCH2), Imm8(32));
    OR(64, R(RSCRATCH), R(RSCRATCH2));
  }
  MOV(32, R(RSCRATCH2), Imm32(js.downcountAmount));
  CMP(64, R(RSCRATCH), MDisp(RSP, 8));
  J_CC(CC_NE, asm_routines.dispatcher_mispredicted_blr);
  SUB(32, PPCSTATE(downcount), R(RSCRATCH2));
  RET();
}

void Jit64::EmitCheckExceptionsAndDispatch(void (*check)(PowerPC::PowerPCManager&))
{
  ABI_PushRegistersAndAdjustStack({}, 0);
  ABI_CallFunctionP(check, &m_system.GetPowerPC());
  ABI_PopRegistersAndAdjustStack({}, 0);
  // Taking an exception may have changed MSR[DR].
  EmitUpdateMembase();
  SUB(32, PPCSTATE(downcount), Imm32(js.downcountAmount));
  JMP(asm_routines.dispatcher);
}

void Jit64::WriteRfiExitDestInRSCRATCH()
{
  MOV(32, PPCSTATE(pc), R(RSCRATCH));
  MOV(32, PPCSTATE(npc), R(RSCRATCH));
  Cleanup();
  EmitCheckExceptionsAndDispatch(PowerPC::CheckExceptionsFromJIT);
}

void Jit64::WriteIdleExit(u32 destination)
{
  ABI_PushRegistersAndAdjustStack({}, 0);
  ABI_CallFunction(CoreTiming::GlobalIdle);
  ABI_PopRegistersAndAdjustStack({}, 0);
  MOV(32, PPCSTATE(pc), Imm32(destination));
  WriteExceptionExit();
}

void Jit64::WriteExceptionExit()
{
  Cleanup();
  MOV(32, R(RSCRATCH), PPCSTATE(pc));
  MOV(32, PPCSTATE(npc), R(RSCRATCH));
  EmitCheckExceptionsAndDispatch(PowerPC::CheckExceptionsFromJIT);
}

void Jit64::WriteExternalExceptionExit()
{
  Cleanup();
  MOV(32, R(RSCRATCH), PPCSTATE(pc));
  MOV(32, PPCSTATE(npc), R(RSCRATCH));
  EmitCheckExceptionsAndDispatch(PowerPC::CheckExternalExceptionsFromJIT);
}

// ---------------------------------------------------------------------------
// Running

void Jit64::Run()
{
  ProtectStack();
  m_system.GetJitInterface().UpdateMembase();
  std::bit_cast<CompiledCode>(asm_routines.enter_code)();
  UnprotectStack();
}

void Jit64::SingleStep()
{
  // Single stepping compiles one-instruction blocks (see Jit), so this is
  // the same as running.
  ProtectStack();
  m_system.GetJitInterface().UpdateMembase();
  std::bit_cast<CompiledCode>(asm_routines.enter_code)();
  UnprotectStack();
}

void Jit64::Trace()
{
  std::string gpr_text;
  std::string fpr_text;
#ifdef JIT_LOG_GPR
  for (size_t index = 0; index < std::size(m_ppc_state.gpr); ++index)
    gpr_text += fmt::format("r{:02d}: {:08x} ", index, m_ppc_state.gpr[index]);
#endif
#ifdef JIT_LOG_FPR
  for (size_t index = 0; index < std::size(m_ppc_state.ps); ++index)
    fpr_text += fmt::format("f{:02d}: {:016x} ", index, m_ppc_state.ps[index].PS0AsU64());
#endif

  DEBUG_LOG_FMT(DYNA_REC,
                "JIT64 PC: {:08x} SRR0: {:08x} SRR1: {:08x} FPSCR: {:08x} "
                "MSR: {:08x} LR: {:08x} {} {}",
                m_ppc_state.pc, SRR0(m_ppc_state), SRR1(m_ppc_state), m_ppc_state.fpscr.Hex,
                m_ppc_state.msr.Hex, m_ppc_state.spr[8], gpr_text, fpr_text);
}

// ---------------------------------------------------------------------------
// Compilation

void Jit64::Jit(u32 em_address)
{
  Jit(em_address, true);
}

void Jit64::Jit(u32 em_address, bool clear_cache_and_retry_on_failure)
{
  CleanUpAfterStackFault();

  const bool no_block_cache = SConfig::GetInstance().bJITNoBlockCache;
  if (no_block_cache || trampolines.IsAlmostFull())
  {
    if (!no_block_cache)
      WARN_LOG_FMT(DYNA_REC, "flushing trampoline code cache, please report if this happens a lot");
    ClearCache();
  }
  FreeRanges();

  std::size_t max_instructions = m_code_buffer.size();
  if (IsDebuggingEnabled())
  {
    EnableBlockLink();
    EnableOptimization();

    if (!IsProfilingEnabled())
    {
      // Single stepping: one instruction per block, no linking and no
      // analysis that looks past the instruction.
      if (m_system.GetCPU().IsStepping())
      {
        max_instructions = 1;
        jo.enableBlocklink = false;
        for (const auto option : {PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE,
                                  PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_MERGE,
                                  PPCAnalyst::PPCAnalyzer::OPTION_CROR_MERGE,
                                  PPCAnalyst::PPCAnalyzer::OPTION_CARRY_MERGE,
                                  PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_FOLLOW})
        {
          analyzer.ClearOption(option);
        }
      }
      Trace();
    }
  }

  const u32 next_pc = analyzer.Analyze(em_address, &code_block, &m_code_buffer, max_instructions);

  if (code_block.m_memory_exception)
  {
    // The block's address does not translate: instruction storage interrupt.
    m_ppc_state.npc = next_pc;
    m_ppc_state.Exceptions |= EXCEPTION_ISI;
    m_system.GetPowerPC().CheckExceptions();
    m_system.GetJitInterface().UpdateMembase();
    WARN_LOG_FMT(POWERPC, "ISI exception at {:#010x}", next_pc);
    return;
  }

  if (SetEmitterStateToFreeCodeRegion())
  {
    u8* const near_begin = GetWritableCodePtr();
    u8* const far_begin = m_far_code.GetWritableCodePtr();

    JitBlock* const block = blocks.AllocateBlock(em_address);
    if (DoJit(em_address, block, next_pc))
    {
      u8* const near_end = GetWritableCodePtr();
      u8* const far_end = m_far_code.GetWritableCodePtr();
      if (near_begin != near_end)
        m_free_ranges_near.erase(near_begin, near_end);
      if (far_begin != far_end)
        m_free_ranges_far.erase(far_begin, far_end);

      // Remembered so the ranges can be reused once the block is destroyed.
      block->near_begin = near_begin;
      block->near_end = near_end;
      block->far_begin = far_begin;
      block->far_end = far_end;

      blocks.FinalizeBlock(*block, jo.enableBlocklink, code_block, m_code_buffer);
#ifdef JIT_LOG_GENERATED_CODE
      LogGeneratedCode();
#endif
      return;
    }
  }

  // Out of code space: start over with an empty cache, once.
  if (clear_cache_and_retry_on_failure)
  {
    WARN_LOG_FMT(DYNA_REC, "flushing code caches, please report if this happens a lot");
    ClearCache();
    Jit(em_address, false);
    return;
  }

  PanicAlertFmtT("JIT failed to find code space after a cache clear. This should never happen. "
                 "Please report this incident on the bug tracker. Dolphin will now exit.");
  std::exit(-1);
}

bool Jit64::SetEmitterStateToFreeCodeRegion()
{
  const auto near_range = m_free_ranges_near.by_size_begin();
  if (near_range == m_free_ranges_near.by_size_end())
  {
    WARN_LOG_FMT(DYNA_REC, "Failed to find free memory region in near code region.");
    return false;
  }
  SetCodePtr(near_range.from(), near_range.to());

  const auto far_range = m_free_ranges_far.by_size_begin();
  if (far_range == m_free_ranges_far.by_size_end())
  {
    WARN_LOG_FMT(DYNA_REC, "Failed to find free memory region in far code region.");
    return false;
  }
  m_far_code.SetCodePtr(far_range.from(), far_range.to());
  return true;
}

const u8* Jit64::EmitAssumptionFailureStub(u32 exception_type)
{
  // Records that the block's assumption failed and recompiles it without.
  SwitchToFarCode();
  const u8* const stub = GetCodePtr();
  MOV(32, PPCSTATE(pc), Imm32(js.blockStart));
  ABI_PushRegistersAndAdjustStack({}, 0);
  ABI_CallFunctionPC(JitInterface::CompileExceptionCheckFromJIT, &m_system.GetJitInterface(),
                     exception_type);
  ABI_PopRegistersAndAdjustStack({}, 0);
  JMP(asm_routines.dispatcher_no_check);
  SwitchToNearCode();
  return stub;
}

// GQRs a block reads but never writes are assumed to keep their current
// values, which lets paired loads and stores be inlined. The block entry
// verifies the assumption.
void Jit64::EmitConstantGQRChecks()
{
  const BitSet8 assumed = ComputeStaticGQRs(code_block);
  if (!assumed)
    return;

  const u8* const recompile = EmitAssumptionFailureStub(
      static_cast<u32>(JitInterface::ExceptionType::PairedQuantize));
  for (const int gqr : assumed)
  {
    const u32 value = GQR(m_ppc_state, gqr);
    js.constantGqr[static_cast<size_t>(gqr)] = value;
    CMP_or_TEST(32, PPCSTATE_SPR(SPR_GQR0 + gqr), Imm32(value));
    J_CC(CC_NZ, recompile);
  }
  js.constantGqrValid = assumed;
}

// Input registers that currently hold a gather pipe or hardware register
// address are assumed constant, so stores through them can be compiled for
// that address. Typically the address was loaded in a previous block. The
// block entry verifies the assumption.
void Jit64::IntializeSpeculativeConstants()
{
  const u8* recompile = nullptr;
  for (const auto preg : code_block.m_gpr_inputs)
  {
    const u32 value = m_ppc_state.gpr[preg];
    const bool looks_like_io = m_mmu.IsOptimizableGatherPipeWrite(value) ||
                               m_mmu.IsOptimizableGatherPipeWrite(value - 0x8000) ||
                               value == 0xCC000000;
    if (!looks_like_io)
      continue;

    if (recompile == nullptr)
    {
      recompile = EmitAssumptionFailureStub(
          static_cast<u32>(JitInterface::ExceptionType::SpeculativeConstants));
    }
    CMP(32, PPCSTATE_GPR(preg), Imm32(value));
    J_CC(CC_NZ, recompile);
    // Not dirty: ppcState already holds this value.
    gpr.SetImmediate32(static_cast<preg_t>(preg), value, false);
  }
}

// Between instructions: run the gather pipe check when the previous
// instruction may have filled the pipe, and take a pending CP/PE interrupt.
void Jit64::EmitGatherPipeInterruptCheck(const PPCAnalyst::CodeOp& op, size_t index)
{
  // Stores through registers that hit the pipe were found by earlier runs.
  bool check_interrupt = js.fifoWriteAddresses.contains(m_code_buffer[index - 1].address);

  // Stores to a known pipe address are counted at compile time.
  if (jo.optimizeGatherPipe &&
      (js.fifoBytesSinceCheck >= GPFifo::GATHER_PIPE_SIZE || js.mustCheckFifo))
  {
    js.fifoBytesSinceCheck = 0;
    js.mustCheckFifo = false;
    const BitSet32 live = CallerSavedRegistersInUse();
    ABI_PushRegistersAndAdjustStack(live, 0);
    ABI_CallFunctionP(GPFifo::FastCheckGatherPipe, &m_system.GetGPFifo());
    ABI_PopRegistersAndAdjustStack(live, 0);
    check_interrupt = true;
  }

  if (!check_interrupt)
    return;

  // The real CP interrupt is asynchronous; checking here is an approximation.
  TEST(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_EXTERNAL_INT));
  const FixupBranch pending = J_CC(CC_NZ, Jump::Near);

  SwitchToFarCode();
  SetJumpTarget(pending);
  TEST(32, PPCSTATE(msr), Imm32(MSR_EE_BIT));
  const FixupBranch disabled = J_CC(CC_Z, Jump::Near);
  MOV(64, R(RSCRATCH), ImmPtr(&m_system.GetProcessorInterface().m_interrupt_cause));
  TEST(32, MatR(RSCRATCH),
       Imm32(ProcessorInterface::INT_CAUSE_CP | ProcessorInterface::INT_CAUSE_PE_TOKEN |
             ProcessorInterface::INT_CAUSE_PE_FINISH));
  const FixupBranch not_graphics = J_CC(CC_Z, Jump::Near);
  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();
    MOV(32, PPCSTATE(pc), Imm32(op.address));
    WriteExternalExceptionExit();
  }
  SwitchToNearCode();
  SetJumpTarget(not_graphics);
  SetJumpTarget(disabled);
}

void Jit64::EmitBreakpointCheck(const PPCAnalyst::CodeOp& op)
{
  auto& cpu = m_system.GetCPU();
  auto& power_pc = m_system.GetPowerPC();
  if (!IsDebuggingEnabled() || !power_pc.GetBreakPoints().IsAddressBreakPoint(op.address) ||
      cpu.IsStepping())
  {
    return;
  }

  gpr.Flush();
  fpr.Flush();
  MOV(32, PPCSTATE(pc), Imm32(op.address));
  ABI_PushRegistersAndAdjustStack({}, 0);
  ABI_CallFunctionP(PowerPC::CheckAndHandleBreakPointsFromJIT, &power_pc);
  ABI_PopRegistersAndAdjustStack({}, 0);

  // Leave the JIT if the breakpoint stopped the CPU.
  MOV(64, R(RSCRATCH), ImmPtr(cpu.GetStatePtr()));
  CMP(32, MatR(RSCRATCH), Imm32(std::to_underlying(CPU::State::Running)));
  const FixupBranch still_running = J_CC(CC_E);
  Cleanup();
  MOV(32, PPCSTATE(npc), Imm32(op.address));
  SUB(32, PPCSTATE(downcount), Imm32(js.downcountAmount));
  JMP(asm_routines.dispatcher_exit);
  SetJumpTarget(still_running);
}

// The first FPU instruction of a block checks MSR[FP] and raises an
// FP-unavailable exception when it is clear.
void Jit64::EmitFPUnavailableCheck(const PPCAnalyst::CodeOp& op)
{
  TEST(32, PPCSTATE(msr), Imm32(MSR_FP_BIT));
  const FixupBranch fp_disabled = J_CC(CC_Z, Jump::Near);

  SwitchToFarCode();
  SetJumpTarget(fp_disabled);
  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();
    // The exception handler reads PC.
    MOV(32, PPCSTATE(pc), Imm32(op.address));
    OR(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_FPU_UNAVAILABLE));
    WriteExceptionExit();
  }
  SwitchToNearCode();

  js.firstFPInstructionFound = true;
}

void Jit64::CompileWithConstantPropagation(PPCAnalyst::CodeOp& op)
{
  const JitCommon::ConstantPropagationResult folded =
      m_constant_propagation.EvaluateInstruction(op.inst, op.opinfo->flags);

  if (!folded.instruction_fully_executed)
  {
    // Bind inputs that later instructions read again now, while registers
    // are free, so they are not loaded twice. Keep at least one register
    // spare for the output (PreloadRegisters stops at two free).
    gpr.PreloadRegisters(op.regsIn & (op.gprWillBeRead | op.gprWillBeWritten) &
                         ~op.gprDiscardable);
    fpr.PreloadRegisters(op.fregsIn & op.fprInXmm & ~op.fprDiscardable);
    CompileInstruction(op);
  }

  m_constant_propagation.Apply(folded);
  if (folded.gpr >= 0)
    gpr.SetImmediate32(static_cast<preg_t>(folded.gpr), folded.gpr_value);

  if (!folded.instruction_fully_executed)
    return;

  // The instruction was evaluated at compile time; only its side effects
  // on XER and CR0 are left to emit. RC goes last since it may merge with a
  // following branch.
  if (folded.carry)
    FinalizeCarry(*folded.carry);
  if (folded.overflow)
    GenerateConstantOverflow(*folded.overflow);
  if (folded.compute_rc)
    FinalizeImmediateRC(static_cast<s32>(folded.gpr_value));
}

// With memchecks, a load/store that raised a DSI leaves the block from far
// code with the registers as they were before the instruction.
void Jit64::EmitMemoryExceptionExit(const PPCAnalyst::CodeOp& op)
{
  ASSERT_MSG(DYNA_REC, !(js.fastmemLoadStore && js.fixupExceptionHandler),
             "Fastmem loadstores shouldn't have exception handler fixups (PC={:x})!", op.address);

  // A fastmem access needs no test: a fault backpatches it, and the
  // trampoline jumps to the exit emitted here.
  FixupBranch raised{};
  const bool needs_test = js.fastmemLoadStore == nullptr && !js.fixupExceptionHandler;
  if (needs_test)
  {
    TEST(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_DSI));
    raised = J_CC(CC_NZ, Jump::Near);
  }

  SwitchToFarCode();
  if (js.fastmemLoadStore != nullptr)
    m_exception_handler_at_loc[js.fastmemLoadStore] = GetWritableCodePtr();
  else
    SetJumpTarget(js.fixupExceptionHandler ? js.exceptionHandler : raised);

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Revert();
    fpr.Revert();
    gpr.Flush();
    fpr.Flush();
    MOV(32, PPCSTATE(pc), Imm32(op.address));
    WriteExceptionExit();
  }
  SwitchToNearCode();
}

// Drops or writes back registers according to the analyzer's liveness info.
void Jit64::FlushAfterInstruction(const PPCAnalyst::CodeOp& op)
{
  if (!bJITRegisterCacheOff)
  {
    gpr.Discard(op.gprDiscardable);
    fpr.Discard(op.fprDiscardable);
  }

  // Registers this instruction touched that the block will not use in a
  // host register again go back to ppcState.
  const BitSet32 gpr_unneeded = ~(op.gprWillBeRead | op.gprWillBeWritten);
  const BitSet32 fpr_unneeded = ~(op.fprWillBeRead | op.fprWillBeWritten);
  gpr.Flush(gpr_unneeded & (op.regsIn | op.regsOut), RegCache::FlushMode::Full);
  fpr.Flush(fpr_unneeded & (op.fregsIn | op.GetFregsOut()), RegCache::FlushMode::Full);

  // Outputs that are not overwritten later are stored now but stay bound.
  gpr.Flush(~op.gprWillBeWritten & op.regsOut, RegCache::FlushMode::Undirty);
  fpr.Flush(~op.fprWillBeWritten & op.GetFregsOut(), RegCache::FlushMode::Undirty);
}

bool Jit64::DoJit(u32 em_address, JitBlock* b, u32 nextPC)
{
  js.firstFPInstructionFound = false;
  js.isLastInstruction = false;
  js.blockStart = em_address;
  js.fifoBytesSinceCheck = 0;
  js.mustCheckFifo = false;
  js.curBlock = b;
  js.numLoadStoreInst = 0;
  js.numFloatingPointInst = 0;

  b->normalEntry = AlignCode4();

  if (m_im_here_debug)
  {
    ABI_PushRegistersAndAdjustStack({}, 0);
    ABI_CallFunctionP(ImHere, this);
    ABI_PopRegistersAndAdjustStack({}, 0);
  }

  if (IsProfilingEnabled())
    ABI_CallFunctionP(&JitBlock::ProfileData::BeginProfiling, b->profile_data.get());

#if defined(_DEBUG) || defined(DEBUGFAST) || defined(NAN_CHECK)
  // Keeps PC meaningful in debugger stack traces.
  MOV(32, PPCSTATE(pc), Imm32(js.blockStart));
#endif

  gpr.Start();
  fpr.Start();
  m_constant_propagation.Clear();

  js.downcountAmount = 0;
  js.skipInstructions = 0;
  js.carryFlag = CarryFlag::InPPCState;
  js.constantGqrValid = BitSet8();

  // Both assumptions are dropped for blocks where they failed before.
  if (!js.pairedQuantizeAddresses.contains(js.blockStart))
    EmitConstantGQRChecks();
  if (!js.noSpeculativeConstantsAddresses.contains(js.blockStart))
    IntializeSpeculativeConstants();

  const u32 count = code_block.m_num_instructions;
  for (u32 index = 0; index < count; ++index)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[index];

    js.compilerPC = op.address;
    js.op = &op;
    js.fpr_is_store_safe = op.fprIsStoreSafeBeforeInst;
    js.instructionsLeft = static_cast<int>(count - 1 - index);
    js.downcountAmount += op.opinfo->num_cycles;
    js.fastmemLoadStore = nullptr;
    js.fixupExceptionHandler = false;
    if (index == count - 1)
      js.isLastInstruction = true;

    if (index != 0)
      EmitGatherPipeInterruptCheck(op, index);

    if (HandleFunctionHooking(op.address))
      break;

    if (op.skip)
    {
      // Only a followed blr is ever skipped; it still counts as a branch.
      if (IsBranchWatchEnabled())
      {
        ASSERT(op.inst.hex == 0x4e800020);
        WriteBranchWatch<true>(op.address, op.branchTo, op.inst, CallerSavedRegistersInUse());
      }
    }
    else
    {
      const u64 flags = op.opinfo->flags;

      EmitBreakpointCheck(op);
      if ((flags & FL_USE_FPU) != 0 && !js.firstFPInstructionFound)
        EmitFPUnavailableCheck(op);

      if (bJITRegisterCacheOff)
      {
        gpr.Flush();
        fpr.Flush();
        m_constant_propagation.Clear();
        CompileInstruction(op);
      }
      else
      {
        CompileWithConstantPropagation(op);
      }

      js.fpr_is_store_safe = op.fprIsStoreSafeAfterInst;

      if (jo.memcheck && (flags & FL_LOADSTORE) != 0)
        EmitMemoryExceptionExit(op);

      gpr.Commit();
      fpr.Commit();
      FlushAfterInstruction(op);

      if ((flags & FL_LOADSTORE) != 0)
        ++js.numLoadStoreInst;
      if ((flags & FL_USE_FPU) != 0)
        ++js.numFloatingPointInst;
    }

#if defined(_DEBUG) || defined(DEBUGFAST)
    if (!gpr.SanityCheck() || !fpr.SanityCheck())
    {
      NOTICE_LOG_FMT(DYNA_REC, "Unflushed register: {}",
                     Common::GekkoDisassembler::Disassemble(op.inst.hex, em_address));
    }
#endif

    index += static_cast<u32>(js.skipInstructions);
    js.skipInstructions = 0;
  }

  // The analyzer cut the block short: continue at the next instruction.
  if (code_block.m_broken)
  {
    gpr.Flush();
    fpr.Flush();
    WriteExit(nextPC);
  }

  // The block owns its alignment padding, so a link to a block placed right
  // after it can turn the final exit into NOPs (JitBlockCache::WriteLinkBlock).
  AlignCode4();

  const bool near_full = HasWriteFailed();
  const bool far_full = m_far_code.HasWriteFailed();
  if (near_full)
    WARN_LOG_FMT(DYNA_REC, "JIT ran out of space in near code region during code generation.");
  if (far_full)
    WARN_LOG_FMT(DYNA_REC, "JIT ran out of space in far code region during code generation.");
  return !near_full && !far_full;
}

BitSet8 Jit64::ComputeStaticGQRs(const PPCAnalyst::CodeBlock& cb) const
{
  return cb.m_gqr_used & ~cb.m_gqr_modified;
}

BitSet32 Jit64::CallerSavedRegistersInUse(BitSet32 additional_registers) const
{
  // XMM registers occupy bits 16-31.
  const BitSet32 live = gpr.RegistersInUse() | (fpr.RegistersInUse() << 16) | additional_registers;
  return live & ABI_ALL_CALLER_SAVED;
}

void Jit64::EnableBlockLink()
{
  jo.enableBlocklink = !SConfig::GetInstance().bJITNoBlockLinking;
}

void Jit64::EnableOptimization()
{
  for (const auto option : {PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE,
                            PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_MERGE,
                            PPCAnalyst::PPCAnalyzer::OPTION_CROR_MERGE,
                            PPCAnalyst::PPCAnalyzer::OPTION_CARRY_MERGE,
                            PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_FOLLOW})
  {
    analyzer.SetOption(option);
  }
}

// Memory watchpoint conditions can read registers, so before a slow access
// the registers they use must be in ppcState. The flush is only for the
// slow path; the fork keeps the cache state of the fast path.
void Jit64::FlushRegistersBeforeSlowAccess()
{
  MemChecks& mem_checks = m_system.GetPowerPC().GetMemChecks();
  if (!mem_checks.HasAny())
    return;

  const BitSet32 gprs = mem_checks.GetGPRsUsedInConditions();
  const BitSet32 fprs = mem_checks.GetFPRsUsedInConditions();
  if (!gprs && !fprs)
    return;

  RCForkGuard gpr_guard = gpr.Fork();
  RCForkGuard fpr_guard = fpr.Fork();
  gpr.Flush(gprs);
  fpr.Flush(fprs);
}

bool Jit64::HandleFunctionHooking(u32 address)
{
  const auto hook = HLE::TryReplaceFunction(m_ppc_symbol_db, address, PowerPC::CoreMode::JIT);
  if (!hook)
    return false;

  HLEFunction(hook.hook_index);
  if (hook.type != HLE::HookType::Replace)
    return false;

  // A replaced function returns to wherever the HLE code set npc.
  MOV(32, R(RSCRATCH), PPCSTATE(npc));
  js.downcountAmount += js.st.numCycles;
  WriteExitDestInRSCRATCH();
  return true;
}

void Jit64::LogGeneratedCode() const
{
  std::ostringstream text;

  text << "\nPPC Code Buffer:\n";
  for (const PPCAnalyst::CodeOp& op :
       std::span{m_code_buffer.data(), code_block.m_num_instructions})
  {
    fmt::print(text, "0x{:08x}\t\t{}\n", op.address,
               Common::GekkoDisassembler::Disassemble(op.inst.hex, op.address));
  }

  const JitBlock& block = *js.curBlock;
  text << "\nHost Near Code:\n";
  m_disassembler->Disassemble(block.normalEntry, block.near_end, text);
  text << "\nHost Far Code:\n";
  m_disassembler->Disassemble(block.far_begin, block.far_end, text);

  DEBUG_LOG_FMT(DYNA_REC, "{}", std::move(text).str());
}
