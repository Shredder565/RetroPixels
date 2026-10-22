// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/PowerPC.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <type_traits>

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

// Savestate helpers: arrays (plain or std::array) go through DoArray, everything else
// through Do, in the order the arguments are given.
template <typename T>
struct IsStdArray : std::false_type
{
};
template <typename T, size_t N>
struct IsStdArray<std::array<T, N>> : std::true_type
{
};

template <typename... Fields>
void DoFields(PointerWrap& p, Fields&... fields)
{
  const auto one = [&p]<typename F>(F& field) {
    if constexpr (std::is_array_v<F> || IsStdArray<F>::value)
      p.DoArray(field);
    else
      p.Do(field);
  };
  (one(fields), ...);
}

// The register file as savestates hold it. The decrementer and time base are not
// part of it: CoreTiming and SystemTimers save those.
void DoRegisterFile(PointerWrap& p, PowerPCState& s)
{
  DoFields(p, s.gpr, s.pc, s.npc, s.cr.fields, s.msr, s.fpscr, s.Exceptions, s.downcount,
           s.xer_ca, s.xer_so_ov, s.xer_stringctrl, s.ps, s.sr, s.spr, s.tlb, s.pagetable_base,
           s.pagetable_mask, s.pagetable_update_pending, s.reserve, s.reserve_address);
}

// SPRs with a non-zero value out of reset (PVR is set separately: it depends on the console).
struct SprResetValue
{
  u32 index;
  u32 value;
};
constexpr SprResetValue kSprResetValues[] = {
    // PLL configuration: core clock at three times the bus.
    {SPR_HID1, 0x80000000},
    // The chip ID (ECID) of one real console.
    {SPR_ECID_U, 0x0d96e200},
    {SPR_ECID_M, 0x1840c00d},
    {SPR_ECID_L, 0x82bb08e8},
};

// Every CR field starts out with LT, GT, EQ and SO clear: negative (not GT), low half
// non-zero (not EQ), bits 62 and 59 clear.
constexpr u64 CR_FIELD_RESET = 0x8000000000000001;

void ClearArchitectedRegisters(PowerPCState& s, bool is_wii)
{
  std::ranges::fill(s.gpr, 0u);
  std::ranges::fill(s.ps, PairedSingle{});
  std::ranges::fill(s.sr, 0u);
  std::ranges::fill(s.cr.fields, CR_FIELD_RESET);
  std::ranges::fill(s.spr, 0u);
  for (const SprResetValue& r : kSprResetValues)
    s.spr[r.index] = r.value;
  s.spr[SPR_PVR] = is_wii ? PVR_BROADWAY : PVR_GEKKO;
  s.SetXER({});
  s.fpscr.Hex = 0;
  s.pc = s.npc = 0;
  s.Exceptions = 0;
  s.reserve_address = 0;
  s.reserve = false;
}
}  // namespace

// A paired single keeps both halves as raw IEEE double bits.
void PairedSingle::SetPS0(double value)
{
  std::memcpy(&ps0, &value, sizeof(ps0));
}

void PairedSingle::SetPS1(double value)
{
  std::memcpy(&ps1, &value, sizeof(ps1));
}

double PairedSingle::PS0AsDouble() const
{
  double d;
  std::memcpy(&d, &ps0, sizeof(d));
  return d;
}

double PairedSingle::PS1AsDouble() const
{
  double d;
  std::memcpy(&d, &ps1, sizeof(d));
  return d;
}

PowerPCManager::PowerPCManager(Core::System& system)
    : m_breakpoints(system), m_memchecks(system), m_debug_interface(system, m_symbol_db),
      m_system(system)
{
}

PowerPCManager::~PowerPCManager() = default;

void PowerPCManager::DoState(PointerWrap& p)
{
  // Loading a state only has to rebuild the segment-derived fastmem mappings when the
  // segment registers actually change.
  const auto old_segments = m_ppc_state.sr;
  DoRegisterFile(p, m_ppc_state);

  auto& memory = m_system.GetMemory();
  for (Cache* cache : std::array<Cache*, 2>{&m_ppc_state.iCache, &m_ppc_state.dCache})
    cache->DoState(memory, p);

  // Checked only now: a failed read above switches the wrapper out of read mode.
  const bool loading = p.IsReadMode();
  auto& mmu = m_system.GetMMU();
  mmu.DoState(p, loading && old_segments != m_ppc_state.sr);
  if (loading)
  {
    // The state may have been saved with the data cache on and be loaded with it off.
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
  ClearArchitectedRegisters(m_ppc_state, m_system.IsWii());

  // The MMU's BAT lookup tables follow the (now cleared) BAT SPRs.
  auto& mmu = m_system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();

  auto& timers = m_system.GetSystemTimers();
  TU(m_ppc_state) = TL(m_ppc_state) = 0;
  timers.TimeBaseSet();

  // MSR[IP] is set on real hardware after reset and the boot ROM clears it. That ROM
  // code is skipped here, so MSR starts at zero.
  m_ppc_state.msr.Hex = 0;
  // The decrementer starts at its maximum and counts down from there.
  m_ppc_state.spr[SPR_DEC] = ~0u;
  timers.DecrementerSet();

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
  // Follow the accurate-cache setting from the start and whenever it changes.
  m_registered_config_callback_id =
      CPUThreadConfigCallback::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();

  auto& core_timing = m_system.GetCoreTiming();
  m_invalidate_cache_thread_safe =
      core_timing.RegisterEvent("invalidateEmulatedCache", InvalidateCacheThreadSafe);

  Reset();
  InitializeCPUCore(cpu_core);

  // Both caches are sized for the memory layout of the console being emulated.
  Memory::MemoryManager& memory = m_system.GetMemory();
  m_ppc_state.iCache.Init(memory);
  m_ppc_state.dCache.Init(memory);
}

void PowerPCManager::Reset()
{
  // Translation state first: no page table, empty TLB.
  PowerPCState& s = m_ppc_state;
  s.pagetable_base = s.pagetable_mask = 0;
  s.pagetable_update_pending = false;
  s.tlb = {};

  ResetRegisters();

  s.iCache.Reset(m_system.GetJitInterface());
  s.dCache.Reset();
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
  // Whatever was injected before goes away first.
  const bool replacing = m_cpu_core_base_is_injected;
  if (replacing)
    m_cpu_core_base->Shutdown();

  if (new_cpu != nullptr)
  {
    new_cpu->Init();
    m_cpu_core_base = new_cpu;
    m_cpu_core_base_is_injected = true;
  }
  else if (replacing)
  {
    // Back to the core the current mode asks for.
    m_cpu_core_base_is_injected = false;
    ApplyMode();
  }
}

void PowerPCManager::RunLoop()
{
  m_cpu_core_base->Run();
  Host_UpdateDisasmDialog();
}

void PowerPCManager::SingleStep()
{
  m_cpu_core_base->SingleStep();
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
