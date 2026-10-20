// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The interpreter's fetch/execute loop, exception gating and diagnostics.

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <string>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
// psq_l, psq_lu, psq_st, psq_stu: the immediate-form quantised loads/stores.
bool IsQuantizedImmediateForm(UGeckoInstruction inst)
{
  switch (inst.OPCD)
  {
  case 56:
  case 57:
  case 60:
  case 61:
    return true;
  default:
    return false;
  }
}
}  // namespace

Interpreter::Interpreter(Core::System& system, PowerPC::PowerPCState& ppc_state, PowerPC::MMU& mmu,
                         Core::BranchWatch& branch_watch, PPCSymbolDB& ppc_symbol_db)
    : m_system(system), m_ppc_state(ppc_state), m_mmu(mmu), m_branch_watch(branch_watch),
      m_ppc_symbol_db(ppc_symbol_db)
{
}

Interpreter::~Interpreter() = default;

void Interpreter::Init()
{
  m_end_block = false;
}

void Interpreter::Shutdown()
{
}

void Interpreter::ClearCache()
{
  // The interpreter keeps no translated code.
}

const char* Interpreter::GetName() const
{
#ifdef _ARCH_64
  return "Interpreter64";
#else
  return "Interpreter32";
#endif
}

// Paired-single instructions are illegal while HID2[PSE] is clear. With PSE set, the immediate
// quantised loads/stores additionally need HID2[LSQE].
bool Interpreter::IsInvalidPairedSingleExecution(UGeckoInstruction inst) const
{
  const UReg_HID2 hid2{m_ppc_state.spr[SPR_HID2]};
  const bool quantized_immediate = IsQuantizedImmediateForm(inst);
  if (!hid2.PSE)
    return inst.OPCD == 4 || quantized_immediate;
  return !hid2.LSQE && quantized_immediate;
}

void Interpreter::UpdatePC()
{
  m_last_pc = m_ppc_state.pc;
  m_ppc_state.pc = m_ppc_state.npc;
}

void Interpreter::CheckExceptions()
{
  m_system.GetPowerPC().CheckExceptions();
  m_end_block = true;
}

void Interpreter::Trace(UGeckoInstruction inst)
{
  const auto& s = m_ppc_state;
  std::string gprs;
  for (u32 i = 0; i < 32; ++i)
    gprs += fmt::format("r{:02d}: {:08x} ", i, s.gpr[i]);
  std::string fprs;
  for (u32 i = 0; i < 32; ++i)
    fprs += fmt::format("f{:02d}: {:08x} {:08x} ", i, s.ps[i].PS0AsU64(), s.ps[i].PS1AsU64());

  DEBUG_LOG_FMT(POWERPC,
                "INTER PC: {:08x} SRR0: {:08x} SRR1: {:08x} CRval: {:016x} FPSCR: {:08x} "
                "MSR: {:08x} LR: {:08x} {} {:08x} {}",
                s.pc, SRR0(s), SRR1(s), s.cr.fields[0], s.fpscr.Hex, s.msr.Hex, s.spr[SPR_LR],
                gprs, inst.hex, Common::GekkoDisassembler::Disassemble(inst.hex, s.pc));
}

// Runs an HLE replacement registered at `address`, if any. Returns true when the hook replaces
// the guest function (so the guest instruction must not run), false for start hooks.
bool Interpreter::HandleFunctionHooking(u32 address)
{
  const auto hook =
      HLE::TryReplaceFunction(m_ppc_symbol_db, address, PowerPC::CoreMode::Interpreter);
  if (!hook)
    return false;
  HLEFunction(*this, UGeckoInstruction{hook.hook_index});
  return hook.type != HLE::HookType::Start;
}

int Interpreter::SingleStepInner()
{
  if (HandleFunctionHooking(m_ppc_state.pc))
  {
    UpdatePC();
    // The cost is looked up for the previously fetched instruction, as before.
    return PPCTables::GetOpInfo(m_prev_inst, m_ppc_state.pc)->num_cycles;
  }

  m_ppc_state.npc = m_ppc_state.pc + 4;
  m_prev_inst.hex = m_mmu.Read_Opcode(m_ppc_state.pc);
  const UGeckoInstruction inst = m_prev_inst;
  const GekkoOPInfo* info = PPCTables::GetOpInfo(inst, m_ppc_state.pc);

  if (m_start_trace)
    Trace(inst);

  if (inst.hex == 0)
  {
    // A zero word means the fetch faulted (ISI is pending).
    CheckExceptions();
  }
  else if (IsInvalidPairedSingleExecution(inst))
  {
    GenerateProgramException(m_ppc_state, ProgramExceptionCause::IllegalInstruction);
    CheckExceptions();
  }
  else if (!m_ppc_state.msr.FP && (info->flags & FL_USE_FPU))
  {
    m_ppc_state.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
    CheckExceptions();
  }
  else
  {
    RunInterpreterOp(*this, inst);
    // Data storage faults are taken right away; other exceptions wait for the block end.
    if (m_ppc_state.Exceptions & EXCEPTION_DSI)
      CheckExceptions();
  }

  UpdatePC();
  PowerPC::UpdatePerformanceMonitor(info->num_cycles, (info->flags & FL_LOADSTORE) != 0,
                                    (info->flags & FL_USE_FPU) != 0, m_ppc_state);
  return info->num_cycles;
}

void Interpreter::SingleStep()
{
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.Advance();

  SingleStepInner();

  // Single stepping does not account cycles: every step is a slice of its own.
  core_timing.GetGlobals().slice_length = 1;
  m_ppc_state.downcount = 0;

  if (m_ppc_state.Exceptions != 0)
  {
    m_system.GetPowerPC().CheckExceptions();
    m_ppc_state.pc = m_ppc_state.npc;
  }
}

// Executes whole blocks (up to the next instruction that sets m_end_block) and charges their
// cycles against the downcount, mirroring how the JITs account time.
void Interpreter::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& cpu = m_system.GetCPU();
  auto& power_pc = m_system.GetPowerPC();

  while (cpu.GetState() == CPU::State::Running)
  {
    // Advance first: it closes the previous slice (or leaves slice -1 at boot).
    core_timing.Advance();

    const bool debugging = Config::IsDebuggingEnabled();
    while (m_ppc_state.downcount > 0)
    {
      m_end_block = false;
      int cycles = 0;
      do
      {
        if (debugging && power_pc.CheckAndHandleBreakPoints())
          return;
        cycles += SingleStepInner();
      } while (!m_end_block);
      m_ppc_state.downcount -= cycles;
    }
  }
}

void Interpreter::unknown_instruction(Interpreter& interpreter, UGeckoInstruction inst)
{
  ASSERT(Core::IsCPUThread());
  auto& system = interpreter.m_system;
  const auto& s = interpreter.m_ppc_state;
  Core::CPUThreadGuard guard(system);

  const u32 last_pc = interpreter.m_last_pc;
  const u32 last_word = PowerPC::MMU::HostRead<u32>(guard, last_pc);
  NOTICE_LOG_FMT(POWERPC, "Last PC = {:08x} : {}", last_pc,
                 Common::GekkoDisassembler::Disassemble(last_word, last_pc));
  Dolphin_Debugger::PrintCallstack(guard, Common::Log::LogType::POWERPC,
                                   Common::Log::LogLevel::LNOTICE);

  const std::string summary =
      fmt::format("\nIntCPU: Unknown instruction {:08x} at PC = {:08x}  last_PC = {:08x}  "
                  "LR = {:08x}\n",
                  inst.hex, s.pc, last_pc, LR(s));
  NOTICE_LOG_FMT(POWERPC, "{}", summary);
  for (u32 i = 0; i < 32; i += 4)
  {
    NOTICE_LOG_FMT(POWERPC, "r{}: {:#010x} r{}: {:#010x} r{}: {:#010x} r{}: {:#010x}", i,
                   s.gpr[i], i + 1, s.gpr[i + 1], i + 2, s.gpr[i + 2], i + 3, s.gpr[i + 3]);
  }
  ASSERT_MSG(POWERPC, 0, "{}", summary);
  if (system.IsPauseOnPanicMode())
    system.GetCPU().Break();
}
