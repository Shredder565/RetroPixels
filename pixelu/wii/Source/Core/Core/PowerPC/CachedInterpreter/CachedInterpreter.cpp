// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/CachedInterpreter/CachedInterpreter.h"

#include <cstdlib>
#include <cstring>
#include <span>
#include <sstream>
#include <string>
#include <utility>

#include <fmt/ostream.h>

#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/Host.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
// Size of a record whose operands are of type T.
template <class T>
constexpr s32 RecordSize()
{
  return static_cast<s32>(sizeof(void*) + sizeof(T));
}
}  // namespace

CachedInterpreter::CachedInterpreter(Core::System& system) : JitBase(system), m_block_cache(*this)
{
}

CachedInterpreter::~CachedInterpreter() = default;

void CachedInterpreter::Init()
{
  RefreshConfig();

  AllocCodeSpace(CODE_SIZE);
  ResetFreeMemoryRanges();

  // Every block returns to the dispatcher.
  jo.enableBlocklink = false;

  m_block_cache.Init();

  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;
}

void CachedInterpreter::Shutdown()
{
  m_block_cache.Shutdown();
}

// ---------------------------------------------------------------------------------------------
// Execution

void CachedInterpreter::ExecuteOneBlock()
{
  const u8* record = m_block_cache.Dispatch();
  if (record == nullptr)
  {
    // No block here yet: translate it. It runs on the next dispatch.
    Jit(m_ppc_state.pc);
    return;
  }

  PowerPC::PowerPCState& ppc_state = m_ppc_state;
  const AnyCallback plain = AnyCallbackCast(Interpret<false>);
  const AnyCallback with_pc = AnyCallbackCast(Interpret<true>);
  for (;;)
  {
    AnyCallback callback;
    std::memcpy(&callback, record, sizeof(callback));
    const u8* const operands = record + sizeof(callback);

    // Most records are plain instruction calls, so call those directly instead of through the
    // pointer.
    if (callback == plain) [[likely]]
    {
      Interpret<false>(ppc_state, *reinterpret_cast<const InterpretOperands*>(operands));
      record += RecordSize<InterpretOperands>();
      continue;
    }
    if (callback == with_pc)
    {
      Interpret<true>(ppc_state, *reinterpret_cast<const InterpretOperands*>(operands));
      record += RecordSize<InterpretOperands>();
      continue;
    }

    const s32 step = callback(ppc_state, operands);
    if (step == 0)
      return;
    record += step;
  }
}

void CachedInterpreter::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  const CPU::State* const cpu_state = m_system.GetCPU().GetStatePtr();

  while (*cpu_state == CPU::State::Running)
  {
    // Starts the next timing slice (and may deliver exceptions, changing pc).
    core_timing.Advance();
    do
    {
      ExecuteOneBlock();
    } while (m_ppc_state.downcount > 0 && *cpu_state == CPU::State::Running);
  }
}

void CachedInterpreter::SingleStep()
{
  m_system.GetCoreTiming().Advance();
  ExecuteOneBlock();
}

// ---------------------------------------------------------------------------------------------
// Record callbacks

s32 CachedInterpreter::StartProfiledBlock(PowerPC::PowerPCState&,
                                          const StartProfiledBlockOperands& operands)
{
  JitBlock::ProfileData::BeginProfiling(operands.profile_data);
  return RecordSize<StartProfiledBlockOperands>();
}

template <bool profiled>
s32 CachedInterpreter::EndBlock(PowerPC::PowerPCState& ppc_state,
                                const EndBlockOperands<profiled>& operands)
{
  const u32 cycles = operands.downcount;
  ppc_state.downcount -= cycles;
  ppc_state.pc = ppc_state.npc;
  if constexpr (profiled)
    JitBlock::ProfileData::EndProfiling(operands.profile_data, cycles);
  PowerPC::UpdatePerformanceMonitor(cycles, operands.num_load_stores, operands.num_fp_inst,
                                    ppc_state);
  return 0;
}

template <bool write_pc>
s32 CachedInterpreter::Interpret(PowerPC::PowerPCState& ppc_state,
                                 const InterpretOperands& operands)
{
  if constexpr (write_pc)
  {
    // Handlers that branch or raise exceptions read the address of the current instruction.
    const u32 here = operands.current_pc;
    ppc_state.npc = here + 4;
    ppc_state.pc = here;
  }
  (*operands.func)(operands.interpreter, operands.inst);
  return RecordSize<InterpretOperands>();
}

template <bool write_pc>
s32 CachedInterpreter::InterpretAndCheckExceptions(
    PowerPC::PowerPCState& ppc_state, const InterpretAndCheckExceptionsOperands& operands)
{
  Interpret<write_pc>(ppc_state, operands);
  if (ppc_state.Exceptions & (EXCEPTION_DSI | EXCEPTION_PROGRAM))
  {
    // The faulting instruction is the one reported; the block ends here.
    ppc_state.pc = operands.current_pc;
    ppc_state.downcount -= operands.downcount;
    operands.power_pc.CheckExceptions();
    return 0;
  }
  return RecordSize<InterpretAndCheckExceptionsOperands>();
}

s32 CachedInterpreter::HLEFunction(PowerPC::PowerPCState& ppc_state,
                                   const HLEFunctionOperands& operands)
{
  ppc_state.pc = operands.current_pc;
  HLE::Execute(Core::CPUThreadGuard{operands.system}, operands.current_pc, operands.hook_index);
  return RecordSize<HLEFunctionOperands>();
}

s32 CachedInterpreter::WriteBrokenBlockNPC(PowerPC::PowerPCState& ppc_state,
                                           const WriteBrokenBlockNPCOperands& operands)
{
  ppc_state.npc = operands.current_pc;
  return RecordSize<WriteBrokenBlockNPCOperands>();
}

s32 CachedInterpreter::CheckFPU(PowerPC::PowerPCState& ppc_state, const CheckHaltOperands& operands)
{
  if (ppc_state.msr.FP)
    return RecordSize<CheckHaltOperands>();
  ppc_state.pc = operands.current_pc;
  ppc_state.downcount -= operands.downcount;
  ppc_state.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
  operands.power_pc.CheckExceptions();
  return 0;
}

s32 CachedInterpreter::CheckBreakpoint(PowerPC::PowerPCState& ppc_state,
                                       const CheckHaltOperands& operands)
{
  ppc_state.pc = operands.current_pc;
  if (!operands.power_pc.CheckAndHandleBreakPoints())
    return RecordSize<CheckHaltOperands>();
  // Reached through the manager because CheckAndHandleBreakPoints may have touched the state.
  operands.power_pc.GetPPCState().downcount -= operands.downcount;
  return 0;
}

s32 CachedInterpreter::CheckIdle(PowerPC::PowerPCState& ppc_state,
                                 const CheckIdleOperands& operands)
{
  if (ppc_state.npc == operands.idle_pc)
    operands.core_timing.Idle();
  return RecordSize<CheckIdleOperands>();
}

// ---------------------------------------------------------------------------------------------
// Code generation

bool CachedInterpreter::HandleFunctionHooking(u32 address)
{
  // The cached interpreter counts as a JIT for HLE purposes.
  const auto hook = HLE::TryReplaceFunction(m_ppc_symbol_db, address, PowerPC::CoreMode::JIT);
  if (!hook)
    return false;

  Write(HLEFunction, {m_system, address, hook.hook_index});

  // A start hook runs and then the guest code continues. A replacement ends the block, charged
  // as the whole analysed block.
  if (hook.type != HLE::HookType::Replace)
    return false;
  js.downcountAmount += js.st.numCycles;
  WriteEndBlock();
  return true;
}

void CachedInterpreter::WriteEndBlock()
{
  const EndBlockOperands<false> counts{js.downcountAmount, js.numLoadStoreInst,
                                       js.numFloatingPointInst};
  if (IsProfilingEnabled())
    Write(EndBlock<true>, {counts, js.curBlock->profile_data.get()});
  else
    Write(EndBlock<false>, counts);
}

void CachedInterpreter::WriteInstruction(const PPCAnalyst::CodeOp& op, Interpreter& interpreter,
                                         PowerPC::PowerPCManager& power_pc)
{
  const InterpretOperands call{interpreter, Interpreter::GetInterpreterOp(op.inst), op.address,
                               op.inst};
  const bool memory_fault_possible = jo.memcheck && (op.opinfo->flags & FL_LOADSTORE) != 0;
  const bool fp_exception_possible = !op.canEndBlock && ShouldHandleFPExceptionForInstruction(&op);

  if (memory_fault_possible || fp_exception_possible)
  {
    const InterpretAndCheckExceptionsOperands checked{call, power_pc, js.downcountAmount};
    if (op.canEndBlock)
      Write(InterpretAndCheckExceptions<true>, checked);
    else
      Write(InterpretAndCheckExceptions<false>, checked);
  }
  else if (op.canEndBlock)
  {
    Write(CallbackCast(Interpret<true>), call);
  }
  else
  {
    Write(CallbackCast(Interpret<false>), call);
  }
}

bool CachedInterpreter::SetEmitterStateToFreeCodeRegion()
{
  const auto largest = m_free_ranges.by_size_begin();
  if (largest == m_free_ranges.by_size_end())
  {
    WARN_LOG_FMT(DYNA_REC, "Cached interpreter: no free space left in the code region.");
    return false;
  }
  SetCodePtr(largest.from(), largest.to());
  return true;
}

void CachedInterpreter::FreeRanges()
{
  for (const auto& [begin, end] : m_block_cache.GetRangesToFree())
    m_free_ranges.insert(begin, end);
  m_block_cache.ClearRangesToFree();
}

void CachedInterpreter::ResetFreeMemoryRanges()
{
  m_free_ranges.clear();
  m_free_ranges.insert(region, region + region_size);
}

void CachedInterpreter::Jit(u32 em_address)
{
  Jit(em_address, true);
}

void CachedInterpreter::Jit(u32 em_address, bool clear_cache_and_retry_on_failure)
{
  // Builds the block at em_address in the largest free range; false if it did not fit.
  const auto emit = [this, em_address](u32 next_pc) {
    if (!SetEmitterStateToFreeCodeRegion())
      return false;
    JitBlock& block = *m_block_cache.AllocateBlock(em_address);
    u8* const start = GetWritableCodePtr();
    block.near_begin = start;
    block.normalEntry = start;
    if (!DoJit(em_address, &block, next_pc))
      return false;
    u8* const finish = GetWritableCodePtr();
    block.near_end = finish;
    block.far_begin = block.far_end = nullptr;
    // The range belongs to the block until the block is destroyed.
    if (finish != start)
      m_free_ranges.erase(start, finish);
    m_block_cache.FinalizeBlock(block, jo.enableBlocklink, code_block, m_code_buffer);
#ifdef JIT_LOG_GENERATED_CODE
    LogGeneratedCode();
#endif
    return true;
  };

  bool may_retry = clear_cache_and_retry_on_failure;
  for (;;)
  {
    const bool flush = IsAlmostFull() || SConfig::GetInstance().bJITNoBlockCache;
    if (flush)
      ClearCache();
    FreeRanges();

    const u32 next_pc =
        analyzer.Analyze(em_address, &code_block, &m_code_buffer, m_code_buffer.size());
    if (code_block.m_memory_exception)
    {
      // Not even the first instruction could be fetched: deliver an ISI instead of a block.
      m_ppc_state.Exceptions |= EXCEPTION_ISI;
      m_ppc_state.npc = next_pc;
      m_system.GetPowerPC().CheckExceptions();
      WARN_LOG_FMT(POWERPC, "ISI exception at {:#010x}", next_pc);
      return;
    }

    if (emit(next_pc))
      return;
    if (!may_retry)
      break;
    // One more attempt on an empty cache.
    WARN_LOG_FMT(DYNA_REC, "Cached interpreter ran out of code space; clearing the cache.");
    ClearCache();
    may_retry = false;
  }

  PanicAlertFmtT("JIT failed to find code space after a cache clear. This should never happen. "
                 "Please report this incident on the bug tracker. Dolphin will now exit.");
  std::exit(-1);
}

bool CachedInterpreter::DoJit(u32 em_address, JitBlock* b, u32 nextPC)
{
  // Fresh per-block compiler state.
  js.curBlock = b;
  js.blockStart = em_address;
  js.downcountAmount = 0;
  js.numLoadStoreInst = 0;
  js.numFloatingPointInst = 0;
  js.fifoBytesSinceCheck = 0;
  js.firstFPInstructionFound = false;

  PowerPC::PowerPCManager& power_pc = m_system.GetPowerPC();
  Interpreter& interpreter = m_system.GetInterpreter();
  const bool check_breakpoints = IsDebuggingEnabled() && !m_system.GetCPU().IsStepping();
  const BreakPoints& breakpoints = power_pc.GetBreakPoints();

  if (IsProfilingEnabled())
    Write(StartProfiledBlock, {b->profile_data.get()});

  const u32 count = code_block.m_num_instructions;
  for (u32 i = 0; i < count; ++i)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];
    js.op = &op;
    js.compilerPC = op.address;
    js.instructionsLeft = count - 1 - i;

    // Counters include the current instruction, so an exception raised by it charges its cycles.
    js.downcountAmount += op.opinfo->num_cycles;
    if (op.opinfo->flags & FL_LOADSTORE)
      ++js.numLoadStoreInst;
    if (op.opinfo->flags & FL_USE_FPU)
      ++js.numFloatingPointInst;

    if (HandleFunctionHooking(op.address))
      break;
    if (op.skip)
      continue;

    if (check_breakpoints && breakpoints.IsAddressBreakPoint(op.address))
      Write(CheckBreakpoint, {power_pc, op.address, js.downcountAmount});

    if (!js.firstFPInstructionFound && (op.opinfo->flags & FL_USE_FPU))
    {
      // One check per block is enough: nothing inside the block can clear MSR[FP] without
      // ending it.
      Write(CheckFPU, {power_pc, op.address, js.downcountAmount});
      js.firstFPInstructionFound = true;
    }

    WriteInstruction(op, interpreter, power_pc);

    if (op.branchIsIdleLoop)
      Write(CheckIdle, {m_system.GetCoreTiming(), js.blockStart});
    if (op.canEndBlock)
      WriteEndBlock();
  }

  if (code_block.m_broken)
  {
    Write(WriteBrokenBlockNPC, {nextPC});
    WriteEndBlock();
  }

  if (HasWriteFailed())
  {
    WARN_LOG_FMT(DYNA_REC, "Cached interpreter ran out of space while generating a block.");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Cache management and diagnostics

void CachedInterpreter::EraseSingleBlock(const JitBlock& block)
{
  m_block_cache.EraseSingleBlock(block);
  FreeRanges();
}

void CachedInterpreter::ClearCache()
{
  m_block_cache.Clear();
  m_block_cache.ClearRangesToFree();
  ClearCodeSpace();
  ResetFreeMemoryRanges();
  RefreshConfig();
  Host_JitCacheInvalidation();
}

std::vector<JitBase::MemoryStats> CachedInterpreter::GetMemoryStats() const
{
  return {{"free", m_free_ranges.get_stats()}};
}

std::size_t CachedInterpreter::DisassembleNearCode(const JitBlock& block,
                                                   std::ostream& stream) const
{
  return Disassemble(block, stream);
}

std::size_t CachedInterpreter::DisassembleFarCode(const JitBlock&, std::ostream& stream) const
{
  // All records live in the near code.
  stream << "N/A\n";
  return 0;
}

void CachedInterpreter::LogGeneratedCode() const
{
  std::ostringstream text;
  text << "\nPPC Code Buffer:\n";
  for (const PPCAnalyst::CodeOp& op :
       std::span{m_code_buffer.data(), code_block.m_num_instructions})
  {
    fmt::print(text, "0x{:08x}\t\t{}\n", op.address,
               Common::GekkoDisassembler::Disassemble(op.inst.hex, op.address));
  }
  text << "\nHost Code:\n";
  Disassemble(*js.curBlock, text);
  DEBUG_LOG_FMT(DYNA_REC, "{}", std::move(text).str());
}
