// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/PowerPC.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FPURoundMode.h"
#include "Common/FloatUtils.h"
#include "Common/Logging/Log.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/PowerPC/CPUCoreBase.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"

namespace PowerPC
{
namespace
{
// Processor version register values: Gekko 2.4e (retail GameCube) and
// Broadway (retail Wii).
constexpr u32 PVR_GEKKO = 0x00083214;
constexpr u32 PVR_BROADWAY = 0x00087102;

// MSR bits copied to SRR1 when an interrupt is taken, and the bits an
// interrupt clears: POW, EE, PR, FP, FE0, SE, BE, FE1, IR, DR, PM and RI.
// ME, ILE and IP are kept, and LE is loaded from ILE.
constexpr u32 SRR1_MSR_BITS = 0x87C0FFFF;
constexpr u32 MSR_CLEARED_ON_INTERRUPT = 0x0004EF36;

// SRR1 bit 1 (IBM numbering) for an ISI: the translation was not found.
constexpr u32 SRR1_ISI_PAGE_FAULT = 1u << 30;

// Interrupt vectors (with MSR[IP] = 0).
constexpr u32 VECTOR_DSI = 0x00000300;
constexpr u32 VECTOR_ISI = 0x00000400;
constexpr u32 VECTOR_EXTERNAL = 0x00000500;
constexpr u32 VECTOR_ALIGNMENT = 0x00000600;
constexpr u32 VECTOR_PROGRAM = 0x00000700;
constexpr u32 VECTOR_FP_UNAVAILABLE = 0x00000800;
constexpr u32 VECTOR_DECREMENTER = 0x00000900;
constexpr u32 VECTOR_SYSCALL = 0x00000C00;
constexpr u32 VECTOR_PERFORMANCE_MONITOR = 0x00000F00;

void InvalidateCacheThreadSafe(Core::System& system, u64 userdata, s64)
{
  system.GetPPCState().iCache.Invalidate(system.GetMemory(), system.GetJitInterface(),
                                         static_cast<u32>(userdata));
  Host_JitCacheInvalidation();
}

// The part every interrupt shares: save the return address and MSR, switch
// to the handler's MSR and jump to the vector.
void EnterInterrupt(PowerPCState& state, u32 return_address, u32 srr1, u32 vector)
{
  SRR0(state) = return_address;
  SRR1(state) = srr1;
  state.msr.LE = state.msr.ILE;
  state.msr.Hex &= ~MSR_CLEARED_ON_INTERRUPT;
  state.pc = vector;
  state.npc = vector;
}

u32 SavedMSR(const PowerPCState& state)
{
  return state.msr.Hex & SRR1_MSR_BITS;
}

// MSR[DR] and MSR[IR] are bits 4 and 5; the feature flags put them at bits
// 0 and 1.
static_assert(UReg_MSR{}.DR.StartBit() == 4);
static_assert(UReg_MSR{}.IR.StartBit() == 5);
static_assert(FEATURE_FLAG_MSR_DR == 1 << 0);
static_assert(FEATURE_FLAG_MSR_IR == 1 << 1);

u32 TranslationFlags(const PowerPCState& state)
{
  return (state.msr.Hex >> 4) & (FEATURE_FLAG_MSR_DR | FEATURE_FLAG_MSR_IR);
}

bool PerformanceMonitorActive(const PowerPCState& state)
{
  return state.spr[SPR_MMCR0] != 0 || state.spr[SPR_MMCR1] != 0;
}
}  // namespace

double PairedSingle::PS0AsDouble() const
{
  return std::bit_cast<double>(ps0);
}

double PairedSingle::PS1AsDouble() const
{
  return std::bit_cast<double>(ps1);
}

void PairedSingle::SetPS0(double value)
{
  ps0 = std::bit_cast<u64>(value);
}

void PairedSingle::SetPS1(double value)
{
  ps1 = std::bit_cast<u64>(value);
}

PowerPCManager::PowerPCManager(Core::System& system)
    : m_breakpoints(system), m_memchecks(system), m_debug_interface(system, m_symbol_db),
      m_system(system)
{
}

PowerPCManager::~PowerPCManager() = default;

void PowerPCManager::DoState(PointerWrap& p)
{
  // The decrementer and time base are saved by CoreTiming/SystemTimers, not
  // here.
  const std::array<u32, 16> segments_before = m_ppc_state.sr;

  p.DoArray(m_ppc_state.gpr);
  p.Do(m_ppc_state.pc);
  p.Do(m_ppc_state.npc);
  p.DoArray(m_ppc_state.cr.fields);
  p.Do(m_ppc_state.msr);
  p.Do(m_ppc_state.fpscr);
  p.Do(m_ppc_state.Exceptions);
  p.Do(m_ppc_state.downcount);
  p.Do(m_ppc_state.xer_ca);
  p.Do(m_ppc_state.xer_so_ov);
  p.Do(m_ppc_state.xer_stringctrl);
  p.DoArray(m_ppc_state.ps);
  p.DoArray(m_ppc_state.sr);
  p.DoArray(m_ppc_state.spr);
  p.DoArray(m_ppc_state.tlb);
  p.Do(m_ppc_state.pagetable_base);
  p.Do(m_ppc_state.pagetable_mask);
  p.Do(m_ppc_state.pagetable_update_pending);
  p.Do(m_ppc_state.reserve);
  p.Do(m_ppc_state.reserve_address);

  auto& memory = m_system.GetMemory();
  m_ppc_state.iCache.DoState(memory, p);
  m_ppc_state.dCache.DoState(memory, p);

  auto& mmu = m_system.GetMMU();
  if (!p.IsReadMode())
  {
    mmu.DoState(p, false);
  }
  else
  {
    mmu.DoState(p, segments_before != m_ppc_state.sr);

    // A state saved with the data cache on may be loaded with it off.
    if (!m_ppc_state.m_enable_dcache)
    {
      INFO_LOG_FMT(POWERPC, "Flushing data cache");
      m_ppc_state.dCache.FlushAll(memory);
    }

    RoundingModeUpdated(m_ppc_state);
    RecalculateAllFeatureFlags(m_ppc_state);

    mmu.IBATUpdated();
    mmu.DBATUpdated();
  }

  m_system.GetJitInterface().DoState(p);
}

void PowerPCManager::ResetRegisters()
{
  std::ranges::fill(m_ppc_state.ps, PairedSingle{});
  std::ranges::fill(m_ppc_state.sr, 0u);
  std::ranges::fill(m_ppc_state.gpr, 0u);
  std::ranges::fill(m_ppc_state.spr, 0u);

  m_ppc_state.spr[SPR_PVR] = m_system.IsWii() ? PVR_BROADWAY : PVR_GEKKO;
  // HID1 reports the PLL configuration: core clock at three times the bus.
  m_ppc_state.spr[SPR_HID1] = 0x80000000;
  // The chip ID (ECID) of one real console.
  m_ppc_state.spr[SPR_ECID_U] = 0x0d96e200;
  m_ppc_state.spr[SPR_ECID_M] = 0x1840c00d;
  m_ppc_state.spr[SPR_ECID_L] = 0x82bb08e8;

  m_ppc_state.fpscr.Hex = 0;
  m_ppc_state.pc = 0;
  m_ppc_state.npc = 0;
  m_ppc_state.Exceptions = 0;

  m_ppc_state.reserve = false;
  m_ppc_state.reserve_address = 0;

  // Every CR field starts out with LT, GT, EQ and SO clear: negative (not
  // GT), low half non-zero (not EQ), bits 62 and 59 clear.
  std::ranges::fill(m_ppc_state.cr.fields, u64{0x8000000000000001});
  m_ppc_state.SetXER({});

  auto& mmu = m_system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();

  auto& system_timers = m_system.GetSystemTimers();
  TL(m_ppc_state) = 0;
  TU(m_ppc_state) = 0;
  system_timers.TimeBaseSet();

  // Real hardware comes out of reset with MSR[IP] set, but the boot ROM that
  // would clear it is not run, so start with 0.
  m_ppc_state.msr.Hex = 0;
  m_ppc_state.spr[SPR_DEC] = 0xFFFFFFFF;
  system_timers.DecrementerSet();

  RoundingModeUpdated(m_ppc_state);
  RecalculateAllFeatureFlags(m_ppc_state);
}

void PowerPCManager::InitializeCPUCore(CPUCore cpu_core)
{
  // The interpreter is always set up: boot code and the debugger use it
  // whatever the main engine is.
  auto& interpreter = m_system.GetInterpreter();
  interpreter.Init();

  if (cpu_core == CPUCore::Interpreter)
  {
    m_cpu_core_base = &interpreter;
  }
  else
  {
    auto& jit = m_system.GetJitInterface();
    m_cpu_core_base = jit.InitJitCore(cpu_core);
    if (m_cpu_core_base == nullptr)
    {
      WARN_LOG_FMT(POWERPC, "CPU core {} not available. Falling back to default.",
                   static_cast<int>(cpu_core));
      m_cpu_core_base = jit.InitJitCore(DefaultCPUCore());
    }
  }

  m_mode = m_cpu_core_base == &interpreter ? CoreMode::Interpreter : CoreMode::JIT;
}

std::span<const CPUCore> AvailableCPUCores()
{
  static constexpr std::array cores{
#ifdef _M_X86_64
      CPUCore::JIT64,
#endif
      CPUCore::CachedInterpreter,
      CPUCore::Interpreter,
  };
  return cores;
}

CPUCore DefaultCPUCore()
{
#ifdef _M_X86_64
  return CPUCore::JIT64;
#else
  return CPUCore::CachedInterpreter;
#endif
}

void PowerPCManager::RefreshConfig()
{
  const bool was_enabled = m_ppc_state.m_enable_dcache;
  m_ppc_state.m_enable_dcache = Config::Get(Config::MAIN_ACCURATE_CPU_CACHE);

  // Turning the data cache off must not lose writes that only live in it.
  if (was_enabled && !m_ppc_state.m_enable_dcache)
  {
    INFO_LOG_FMT(POWERPC, "Flushing data cache");
    m_ppc_state.dCache.FlushAll(m_system.GetMemory());
  }
}

void PowerPCManager::Init(CPUCore cpu_core)
{
  m_registered_config_callback_id =
      CPUThreadConfigCallback::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();

  m_invalidate_cache_thread_safe =
      m_system.GetCoreTiming().RegisterEvent("invalidateEmulatedCache", InvalidateCacheThreadSafe);

  Reset();

  InitializeCPUCore(cpu_core);
  auto& memory = m_system.GetMemory();
  m_ppc_state.iCache.Init(memory);
  m_ppc_state.dCache.Init(memory);
}

void PowerPCManager::Reset()
{
  m_ppc_state.pagetable_base = 0;
  m_ppc_state.pagetable_mask = 0;
  m_ppc_state.pagetable_update_pending = false;
  m_ppc_state.tlb = {};

  ResetRegisters();
  m_ppc_state.iCache.Reset(m_system.GetJitInterface());
  m_ppc_state.dCache.Reset();
  m_system.GetMMU().Reset();
}

void PowerPCManager::ScheduleInvalidateCacheThreadSafe(u32 address)
{
  const bool running = m_system.GetCPU().GetState() == CPU::State::Running;
  if (running && !Core::IsCPUThread())
  {
    m_system.GetCoreTiming().ScheduleEvent(0, m_invalidate_cache_thread_safe, address,
                                           CoreTiming::FromThread::NON_CPU);
    return;
  }

  m_ppc_state.iCache.Invalidate(m_system.GetMemory(), m_system.GetJitInterface(), address);
  Host_JitCacheInvalidation();
}

void PowerPCManager::Shutdown()
{
  CPUThreadConfigCallback::RemoveConfigChangedCallback(m_registered_config_callback_id);
  InjectExternalCPUCore(nullptr);
  m_system.GetJitInterface().Shutdown();
  m_system.GetInterpreter().Shutdown();
  m_cpu_core_base = nullptr;
}

CoreMode PowerPCManager::GetMode() const
{
  return m_cpu_core_base_is_injected ? CoreMode::Interpreter : m_mode;
}

void PowerPCManager::ApplyMode()
{
  auto& interpreter = m_system.GetInterpreter();
  if (m_mode == CoreMode::JIT)
  {
    // The JIT's block cache simply fills up again from here.
    m_cpu_core_base = m_system.GetJitInterface().GetCore();
    if (m_cpu_core_base == nullptr)
      m_cpu_core_base = &interpreter;
  }
  else
  {
    m_cpu_core_base = &interpreter;
  }
}

void PowerPCManager::SetMode(CoreMode new_mode)
{
  if (new_mode == m_mode)
    return;

  m_mode = new_mode;

  // An injected core stays in charge; the mode applies once it is removed.
  if (!m_cpu_core_base_is_injected)
    ApplyMode();
}

const char* PowerPCManager::GetCPUName() const
{
  return m_cpu_core_base->GetName();
}

void PowerPCManager::InjectExternalCPUCore(CPUCoreBase* new_cpu)
{
  if (m_cpu_core_base_is_injected)
    m_cpu_core_base->Shutdown();

  if (new_cpu == nullptr)
  {
    if (m_cpu_core_base_is_injected)
    {
      m_cpu_core_base_is_injected = false;
      ApplyMode();
    }
    return;
  }

  new_cpu->Init();
  m_cpu_core_base = new_cpu;
  m_cpu_core_base_is_injected = true;
}

void PowerPCManager::SingleStep()
{
  m_cpu_core_base->SingleStep();
}

void PowerPCManager::RunLoop()
{
  m_cpu_core_base->Run();
  Host_UpdateDisasmDialog();
}

u64 PowerPCManager::ReadFullTimeBaseValue() const
{
  // TBL and TBU are adjacent SPRs (268, 269), so on a little-endian host
  // they form the 64-bit time base directly.
  u64 value;
  std::memcpy(&value, &TL(m_ppc_state), sizeof(value));
  return value;
}

void PowerPCManager::WriteFullTimeBaseValue(u64 value)
{
  std::memcpy(&TL(m_ppc_state), &value, sizeof(value));
}

void UpdatePerformanceMonitor(u32 cycles, u32 num_load_stores, u32 num_fp_inst,
                              PowerPCState& ppc_state)
{
  // Only the events games are known to use are counted: event 1 (processor
  // cycles) on every counter, event 11 on PMC2 (completed loads/stores) and
  // PMC3 (completed FPU instructions).
  const UReg_MMCR0 mmcr0 = MMCR0(ppc_state);
  const UReg_MMCR1 mmcr1 = MMCR1(ppc_state);

  if (mmcr0.PMC1SELECT == 1)
    ppc_state.spr[SPR_PMC1] += cycles;

  if (mmcr0.PMC2SELECT == 1)
    ppc_state.spr[SPR_PMC2] += cycles;
  else if (mmcr0.PMC2SELECT == 11)
    ppc_state.spr[SPR_PMC2] += num_load_stores;

  if (mmcr1.PMC3SELECT == 1)
    ppc_state.spr[SPR_PMC3] += cycles;
  else if (mmcr1.PMC3SELECT == 11)
    ppc_state.spr[SPR_PMC3] += num_fp_inst;

  if (mmcr1.PMC4SELECT == 1)
    ppc_state.spr[SPR_PMC4] += cycles;

  // A counter whose top bit is set raises the interrupt if enabled: PMC1
  // through PMC1INTCONTROL, the others through PMCINTCONTROL.
  const auto negative = [&](int spr) { return (ppc_state.spr[spr] & 0x80000000) != 0; };
  const bool pmc1 = mmcr0.PMC1INTCONTROL && negative(SPR_PMC1);
  const bool others = mmcr0.PMCINTCONTROL &&
                      (negative(SPR_PMC2) || negative(SPR_PMC3) || negative(SPR_PMC4));
  if (pmc1 || others)
    ppc_state.Exceptions |= EXCEPTION_PERFORMANCE_MONITOR;
}

void PowerPCManager::CheckExceptions()
{
  // Only one exception is taken per call, in this fixed order (which is not
  // the order the 750CL documents, but is what the emulator has always
  // done). External interrupts are left to CheckExternalExceptions.
  PowerPCState& s = m_ppc_state;
  const u32 pending = s.Exceptions;

  if (pending & EXCEPTION_ISI)
  {
    EnterInterrupt(s, s.npc, SavedMSR(s) | SRR1_ISI_PAGE_FAULT, VECTOR_ISI);
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_ISI");
    s.Exceptions &= ~EXCEPTION_ISI;
  }
  else if (pending & EXCEPTION_PROGRAM)
  {
    // The cause bits of SRR1 were already set when the exception was raised.
    EnterInterrupt(s, s.pc, SRR1(s) | SavedMSR(s), VECTOR_PROGRAM);
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_PROGRAM");
    s.Exceptions &= ~EXCEPTION_PROGRAM;
  }
  else if (pending & EXCEPTION_SYSCALL)
  {
    EnterInterrupt(s, s.npc, SavedMSR(s), VECTOR_SYSCALL);
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_SYSCALL (PC={:08x})", s.pc);
    s.Exceptions &= ~EXCEPTION_SYSCALL;
  }
  else if (pending & EXCEPTION_FPU_UNAVAILABLE)
  {
    // Common: the OS switches FPU contexts lazily. The instruction is retried.
    EnterInterrupt(s, s.pc, SavedMSR(s), VECTOR_FP_UNAVAILABLE);
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_FPU_UNAVAILABLE");
    s.Exceptions &= ~EXCEPTION_FPU_UNAVAILABLE;
  }
  else if (pending & EXCEPTION_FAKE_MEMCHECK_HIT)
  {
    // A watchpoint stopped the access; there is no guest-visible exception.
    s.Exceptions &= ~(EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT);
  }
  else if (pending & EXCEPTION_DSI)
  {
    // DSISR and DAR were filled in when the fault was raised.
    EnterInterrupt(s, s.pc, SavedMSR(s), VECTOR_DSI);
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_DSI");
    s.Exceptions &= ~EXCEPTION_DSI;
  }
  else if (pending & EXCEPTION_ALIGNMENT)
  {
    EnterInterrupt(s, s.pc, SavedMSR(s), VECTOR_ALIGNMENT);
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_ALIGNMENT");
    s.Exceptions &= ~EXCEPTION_ALIGNMENT;
  }
  else
  {
    CheckExternalExceptions();
    return;
  }

  MSRUpdated();
}

void PowerPCManager::CheckExternalExceptions()
{
  PowerPCState& s = m_ppc_state;
  const u32 pending = s.Exceptions;

  // Asynchronous interrupts wait until MSR[EE] is set.
  if (pending == 0 || !s.msr.EE)
    return;

  u32 vector;
  if (pending & EXCEPTION_EXTERNAL_INT)
  {
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_EXTERNAL_INT");
    DEBUG_ASSERT_MSG(POWERPC, s.msr.RI, "EXTERNAL_INT unrecoverable???");
    vector = VECTOR_EXTERNAL;
    s.Exceptions &= ~EXCEPTION_EXTERNAL_INT;
  }
  else if (pending & EXCEPTION_PERFORMANCE_MONITOR)
  {
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_PERFORMANCE_MONITOR");
    vector = VECTOR_PERFORMANCE_MONITOR;
    s.Exceptions &= ~EXCEPTION_PERFORMANCE_MONITOR;
  }
  else if (pending & EXCEPTION_DECREMENTER)
  {
    DEBUG_LOG_FMT(POWERPC, "EXCEPTION_DECREMENTER");
    vector = VECTOR_DECREMENTER;
    s.Exceptions &= ~EXCEPTION_DECREMENTER;
  }
  else
  {
    DEBUG_ASSERT_MSG(POWERPC, 0, "Unknown EXT interrupt: Exceptions == {:08x}", pending);
    ERROR_LOG_FMT(POWERPC, "Unknown EXTERNAL INTERRUPT exception: Exceptions == {:08x}",
                  pending);
    return;
  }

  EnterInterrupt(s, s.npc, SavedMSR(s), vector);
  MSRUpdated();
}

bool PowerPCManager::CheckBreakPoints()
{
  const u32 pc = m_ppc_state.pc;
  const TBreakPoint* bp = m_breakpoints.GetBreakpoint(pc);
  if (!m_breakpoints.IsBreakingEnabled() || bp == nullptr || !bp->is_enabled)
    return false;
  if (!EvaluateCondition(m_system, bp->condition))
    return false;

  if (bp->log_on_hit)
  {
    const auto& r = m_ppc_state.gpr;
    NOTICE_LOG_FMT(MEMMAP,
                   "BP {:08x} {}({:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} "
                   "{:08x}) LR={:08x}",
                   pc, m_symbol_db.GetDescription(pc), r[3], r[4], r[5], r[6], r[7], r[8], r[9],
                   r[10], r[11], r[12], LR(m_ppc_state));
  }
  return bp->break_on_hit;
}

bool PowerPCManager::CheckAndHandleBreakPoints()
{
  if (!CheckBreakPoints())
    return false;

  m_system.GetCPU().Break();
  if (GDBStub::IsActive())
    GDBStub::TakeControl();
  return true;
}

void PowerPCManager::MSRUpdated()
{
  const u32 perfmon = m_ppc_state.feature_flags & FEATURE_FLAG_PERFMON;
  m_ppc_state.feature_flags =
      static_cast<CPUEmuFeatureFlags>(perfmon | TranslationFlags(m_ppc_state));

  // Page table writes made while translation was off are acted on once it
  // is back on.
  if (m_ppc_state.msr.DR && m_ppc_state.pagetable_update_pending)
    m_system.GetMMU().PageTableUpdated();

  m_system.GetJitInterface().UpdateMembase();
}

void PowerPCState::UpdateFPRFDouble(double dvalue)
{
  fpscr.FPRF = Common::ClassifyDouble(dvalue);
}

void PowerPCState::UpdateFPRFSingle(float fvalue)
{
  fpscr.FPRF = Common::ClassifyFloat(fvalue);
}

void RoundingModeUpdated(PowerPCState& ppc_state)
{
  // The host's rounding mode is per thread.
  ASSERT(Core::IsCPUThread());

  Common::FPU::SetSIMDMode(ppc_state.fpscr.RN, ppc_state.fpscr.NI);
}

void MMCRUpdated(PowerPCState& ppc_state)
{
  u32 flags = ppc_state.feature_flags & ~FEATURE_FLAG_PERFMON;
  if (PerformanceMonitorActive(ppc_state))
    flags |= FEATURE_FLAG_PERFMON;
  ppc_state.feature_flags = static_cast<CPUEmuFeatureFlags>(flags);
}

void RecalculateAllFeatureFlags(PowerPCState& ppc_state)
{
  u32 flags = TranslationFlags(ppc_state);
  if (PerformanceMonitorActive(ppc_state))
    flags |= FEATURE_FLAG_PERFMON;
  ppc_state.feature_flags = static_cast<CPUEmuFeatureFlags>(flags);
}

void CheckExceptionsFromJIT(PowerPCManager& power_pc)
{
  power_pc.CheckExceptions();
}

void CheckExternalExceptionsFromJIT(PowerPCManager& power_pc)
{
  power_pc.CheckExternalExceptions();
}

void CheckAndHandleBreakPointsFromJIT(PowerPCManager& power_pc)
{
  power_pc.CheckAndHandleBreakPoints();
}
}  // namespace PowerPC
