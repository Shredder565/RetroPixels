// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include <algorithm>
#include <cstdint>

#include "Common/BitSet.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/x64ABI.h"

#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPHost.h"
#include "Core/DSP/DSPTables.h"
#include "Core/DSP/Interpreter/DSPIntTables.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"
#include "Core/DSP/Jit/x64/DSPJitTables.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
constexpr u32 BLOCK_COUNT = 0x10000;
// A block holds at most this many instructions.
constexpr int MAX_BLOCK_INSTRUCTIONS = 250;

// The generated code is several times the size of the old register-caching
// translator's, so the buffer is larger in proportion. When less than
// CODE_RESERVE bytes are left (room for the largest possible block), nothing
// more is compiled: the dispatcher stops and the whole buffer is thrown away
// when RunCycles returns.
constexpr size_t CODE_SPACE_BYTES = 16 * 1024 * 1024;
constexpr size_t CODE_RESERVE = 1024 * 1024;

void InterpretThunk(Interpreter::Interpreter& interpreter, UDSPInstruction inst)
{
  (interpreter.*Interpreter::GetOp(inst))(inst);
}

void InterpretExtThunk(Interpreter::Interpreter& interpreter, UDSPInstruction inst)
{
  (interpreter.*Interpreter::GetExtOp(inst))(inst);
}

void ApplyWriteBackThunk(Interpreter::Interpreter& interpreter)
{
  interpreter.ApplyWriteBackLog();
}

u32 CheckExceptionsThunk(DSPCore& core)
{
  return core.CheckExceptions() ? 1u : 0u;
}

template <typename T>
void CallGenerated(const u8* code)
{
  reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(code))();
}
}  // namespace

DSPEmitter::DSPEmitter(DSPCore& dsp) : m_dsp_core(dsp), m_regs(*this, dsp.DSPState())
{
  InitHandlerTables();

  m_blocks.entry.resize(BLOCK_COUNT);
  m_blocks.link.resize(BLOCK_COUNT);
  m_blocks.length.resize(BLOCK_COUNT);
  m_blocks.waiting.resize(BLOCK_COUNT);

  AllocCodeSpace(CODE_SPACE_BYTES);
  BuildDispatcher();
  BuildStub();
  ResetAllBlocks();
}

DSPEmitter::~DSPEmitter()
{
  FreeCodeSpace();
}

u16 DSPEmitter::RunCycles(u16 cycles)
{
  SDSP& state = m_dsp_core.DSPState();
  if (state.external_interrupt_waiting.exchange(false, std::memory_order_acquire))
  {
    m_dsp_core.CheckExternalInterrupt();
    m_dsp_core.CheckExceptions();
  }

  m_cycles_left = cycles;
  CallGenerated<void()>(m_enter);

  if (state.reset_dspjit_codespace)
  {
    ClearCodeSpace();
    BuildDispatcher();
    BuildStub();
    ResetAllBlocks();
    state.reset_dspjit_codespace = false;
  }

  return m_cycles_left;
}

void DSPEmitter::ClearIRAM()
{
  for (u32 address = 0; address < DSP_IRAM_SIZE; ++address)
  {
    ResetBlock(static_cast<u16>(address));
    ClearWaiting(static_cast<u16>(address));
  }
  // The code itself goes once the current run is over.
  m_dsp_core.DSPState().reset_dspjit_codespace = true;
}

void DSPEmitter::DoState(PointerWrap& p)
{
  p.Do(m_cycles_left);
}

void DSPEmitter::ResetBlock(u16 start)
{
  m_blocks.entry[start] = m_stub;
  m_blocks.link[start] = nullptr;
  m_blocks.length[start] = 0;
}

void DSPEmitter::ResetAllBlocks()
{
  for (u32 address = 0; address < BLOCK_COUNT; ++address)
  {
    ResetBlock(static_cast<u16>(address));
    m_blocks.waiting[address].clear();
  }
  m_blocks.waiting_blocks.clear();
}

void DSPEmitter::AddWaiting(u16 block, u16 target)
{
  m_blocks.waiting[block].push_back(target);
  m_blocks.waiting_blocks.insert(block);
}

void DSPEmitter::RemoveWaiting(u16 block, u16 target)
{
  auto& list = m_blocks.waiting[block];
  std::erase(list, target);
  if (list.empty())
    m_blocks.waiting_blocks.erase(block);
}

void DSPEmitter::ClearWaiting(u16 block)
{
  m_blocks.waiting[block].clear();
  m_blocks.waiting_blocks.erase(block);
}

// Layout of a run: save the host registers, then loop { stop if halted (or, on
// the DSP thread, if an interrupt is waiting); jump to the block for pc; the
// block jumps back to m_return with its cycle count in AX; subtract it } until
// the 16-bit budget runs out or would go below zero.
void DSPEmitter::BuildDispatcher()
{
  const BitSet32 saved = ABI_ALL_CALLEE_SAVED & BitSet32(0xffff);

  m_enter = AlignCode16();
  ABI_PushRegistersAndAdjustStack(saved, 8);
  MOV(64, R(STATE_REG), ImmPtr(&m_dsp_core.DSPState()));
  MOV(64, R(ENTRY_TABLE), ImmPtr(m_blocks.entry.data()));
  MOV(64, R(CYCLE_COUNTER), ImmPtr(&m_cycles_left));

  const u8* next_block = GetCodePtr();
  const bool watch_interrupts = Host::OnThread();
  FixupBranch interrupt_waiting;
  if (watch_interrupts)
  {
    CMP(8, m_regs.ExternalInterruptWaiting(), Imm8(0));
    interrupt_waiting = J_CC(CC_NE, Jump::Near);
  }
  TEST(8, m_regs.ControlReg(), Imm8(static_cast<u8>(CR_HALT)));
  FixupBranch halted = J_CC(CC_NZ, Jump::Near);
  MOVZX(32, 16, ECX, m_regs.Pc());
  JMPptr(MComplex(ENTRY_TABLE, RCX, SCALE_8, 0));

  m_return = GetCodePtr();
  SUB(16, MatR(CYCLE_COUNTER), R(EAX));
  J_CC(CC_A, next_block);

  SetJumpTarget(halted);
  if (watch_interrupts)
    SetJumpTarget(interrupt_waiting);
  m_leave = GetCodePtr();
  ABI_PopRegistersAndAdjustStack(saved, 8);
  RET();
}

// Where the dispatcher goes for a pc without code: compile, then report 0 cycles
// so the dispatcher looks the pc up again. With the code buffer full it leaves.
void DSPEmitter::BuildStub()
{
  m_stub = AlignCode16();
  MOV(64, R(ABI_PARAM1), ImmPtr(this));
  ABI_CallFunction(CompileCurrentThunk);
  TEST(32, R(ABI_RETURN), R(ABI_RETURN));
  J_CC(CC_Z, m_leave);
  XOR(32, R(EAX), R(EAX));
  JMP(m_return, true);
}

u32 DSPEmitter::CompileCurrentThunk(DSPEmitter& emitter)
{
  return emitter.CompileCurrent() ? 1u : 0u;
}

bool DSPEmitter::CompileCurrent()
{
  SDSP& state = m_dsp_core.DSPState();
  if (GetSpaceLeft() < CODE_RESERVE)
  {
    state.reset_dspjit_codespace = true;
    return false;
  }

  CompileBlock(state.pc);

  // Translate the blocks that are waiting for link targets, until a full pass
  // leaves no list behind that is still waiting. Two blocks that branch to each
  // other can each keep the other from being linkable forever; once a pass
  // produces a waiting state seen before, nothing new can come of more passes.
  std::set<u64> seen_states;
  bool again = true;
  while (again)
  {
    again = false;
    u32 index = 0;
    while (index < 0xffff)
    {
      const auto next = m_blocks.waiting_blocks.lower_bound(static_cast<u16>(index));
      if (next == m_blocks.waiting_blocks.end() || *next == 0xffff)
        break;
      index = *next;

      if (GetSpaceLeft() < CODE_RESERVE)
      {
        state.reset_dspjit_codespace = true;
        return true;
      }
      CompileBlock(m_blocks.waiting[index].front());
      if (!m_blocks.waiting[index].empty())
        again = true;
      ++index;
    }

    if (again)
    {
      u64 fingerprint = 0x84222325cbf29ce4ULL;
      const auto mix = [&fingerprint](u64 value) {
        fingerprint = (fingerprint ^ value) * 0x9e3779b97f4a7c15ULL;
        fingerprint ^= fingerprint >> 29;
      };
      for (const u16 block : m_blocks.waiting_blocks)
      {
        mix(0x10000u | block);
        for (const u16 target : m_blocks.waiting[block])
          mix(target);
      }
      for (u32 block = 0; block < BLOCK_COUNT; ++block)
      {
        if (m_blocks.link[block] != nullptr)
          mix(0x20000u | block);
      }
      if (!seen_states.insert(fingerprint).second)
      {
        WARN_LOG_FMT(DSPLLE, "Blocks around {:#06x} keep waiting for each other", state.pc);
        break;
      }
    }
  }
  return true;
}

u16 DSPEmitter::BlockEndCycles() const
{
  const auto& analyzer = m_dsp_core.DSPState().GetAnalyzer();
  if (!Host::OnThread() && analyzer.IsIdleSkip(m_block_start))
    return IDLE_SKIP_CYCLES;
  return m_blocks.length[m_block_start];
}

void DSPEmitter::EmitExit(u16 cycles)
{
  MOV(32, R(EAX), Imm32(cycles));
  JMP(m_return, true);
}

void DSPEmitter::CompileBlock(u16 start)
{
  const SDSP& state = m_dsp_core.DSPState();
  const auto& analyzer = state.GetAnalyzer();

  m_block_start = start;
  ClearWaiting(start);
  const u8* const entry = AlignCode16();

  m_pc = start;
  m_blocks.length[start] = 0;
  bool pc_needs_update = false;

  while (static_cast<int>(m_pc) < static_cast<int>(start) + MAX_BLOCK_INSTRUCTIONS)
  {
    if (analyzer.IsCheckExceptions(m_pc))
      EmitExceptionCheck(m_blocks.length[start]);

    const UDSPInstruction inst = state.ReadIMEM(m_pc);
    const DSPOPCTemplate* const tmpl = GetOpTemplate(inst);

    EmitInstruction(inst);

    ++m_blocks.length[start];
    m_pc = static_cast<u16>(m_pc + tmpl->size);
    // A branch to the next instruction of this block needs no link.
    RemoveWaiting(start, m_pc);
    pc_needs_update = true;

    if (analyzer.IsLoopEnd(static_cast<u16>(m_pc - 1)))
      EmitLoopEnd(tmpl->branch);

    if (tmpl->branch)
    {
      // The branch has set pc itself.
      pc_needs_update = false;
      if (tmpl->uncond_branch)
        break;
      if (MainHandler(inst) == nullptr)
      {
        // The interpreter ran it: leave if it went somewhere else.
        CMP(16, m_regs.Pc(), Imm16(m_pc));
        FixupBranch fell_through = J_CC(CC_E, Jump::Near);
        EmitExit(BlockEndCycles());
        SetJumpTarget(fell_through);
      }
    }

    // An idle loop starts at the next instruction; it gets a block of its own.
    if (analyzer.IsIdleSkip(m_pc))
      break;
  }

  if (pc_needs_update)
    MOV(16, m_regs.Pc(), Imm16(m_pc));

  m_blocks.entry[start] = entry;

  if (m_blocks.waiting[start].empty())
  {
    // Complete: others may link here now, and those that waited for this block
    // are translated again so that they do.
    m_blocks.link[start] = entry;
    std::vector<u16> released;
    for (const u16 block : m_blocks.waiting_blocks)
    {
      if (block == 0xffff)
        continue;
      const auto& list = m_blocks.waiting[block];
      if (std::ranges::find(list, start) != list.end())
        released.push_back(block);
    }
    for (const u16 block : released)
    {
      RemoveWaiting(block, start);
      ResetBlock(block);
    }
  }

  if (m_blocks.length[start] == 0)
  {
    ERROR_LOG_FMT(DSPLLE, "Block at {:#06x} has no instructions", start);
    m_blocks.length[start] = 1;
  }

  EmitExit(BlockEndCycles());
}

void DSPEmitter::EmitInstruction(UDSPInstruction inst)
{
  const DSPOPCTemplate* const tmpl = GetOpTemplate(inst);
  Interpreter::Interpreter* const interpreter = &m_dsp_core.GetInterpreter();

  // The extension reads its operands before the main instruction runs, and
  // writes its results after it.
  bool ext_translated = false;
  if (tmpl->extended)
  {
    if (const JITHandler ext = ExtHandler(inst))
    {
      (this->*ext)(inst);
      ext_translated = true;
    }
    else
    {
      ABI_CallFunctionPC(InterpretExtThunk, interpreter, inst);
    }
  }

  if (const JITHandler main = MainHandler(inst))
    (this->*main)(inst);
  else
    EmitInterpreterCall(inst);

  if (tmpl->extended)
  {
    if (ext_translated)
      CommitExtension();
    else
      ABI_CallFunctionP(ApplyWriteBackThunk, interpreter);
  }
}

void DSPEmitter::EmitInterpreterCall(UDSPInstruction inst)
{
  // Instructions with an immediate word fetch it through pc.
  if (GetOpTemplate(inst)->reads_pc)
    MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 1)));
  ABI_CallFunctionPC(InterpretThunk, &m_dsp_core.GetInterpreter(), inst);
}

void DSPEmitter::EmitExceptionCheck(u16 cycles)
{
  TEST(8, m_regs.Exceptions(), Imm8(0xff));
  FixupBranch none_pending = J_CC(CC_Z, Jump::Near);
  MOV(16, m_regs.Pc(), Imm16(m_pc));
  ABI_CallFunctionP(CheckExceptionsThunk, &m_dsp_core);
  TEST(32, R(ABI_RETURN), R(ABI_RETURN));
  FixupBranch not_taken = J_CC(CC_Z, Jump::Near);
  // pc now points at the handler.
  EmitExit(cycles);
  SetJumpTarget(not_taken);
  SetJumpTarget(none_pending);
}

// At the last instruction of a hardware loop: count down, and go back to the
// loop start or fall out of the loop.
void DSPEmitter::EmitLoopEnd(bool after_branch)
{
  CMP(16, m_regs.StackTop(2), Imm16(0));
  FixupBranch no_loop_address = J_CC(CC_E, Jump::Near);
  CMP(16, m_regs.StackTop(3), Imm16(0));
  FixupBranch no_loop_count = J_CC(CC_E, Jump::Near);

  if (!after_branch)
    MOV(16, m_regs.Pc(), Imm16(m_pc));
  EmitLoopCounterStep();
  EmitExit(BlockEndCycles());

  SetJumpTarget(no_loop_address);
  SetJumpTarget(no_loop_count);
}

}  // namespace DSP::JIT::x64
