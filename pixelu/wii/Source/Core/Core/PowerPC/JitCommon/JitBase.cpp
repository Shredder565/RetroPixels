// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/JitCommon/JitBase.h"

#include <algorithm>
#include <array>
#include <utility>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/Thread.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/MemTools.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <unistd.h>
#endif

// About the stack guard:
//
// With the BLR optimization every guest "bl" also pushes a host return
// address, so a guest that calls without ever returning grows the host stack
// without bound. The overflow may happen inside C++ code called from a block,
// so it cannot simply be undone where it happens. Instead a guard area is
// placed SAFE_STACK_SIZE bytes from the end of the stack: the first touch
// raises a fault, the fault handler turns the optimization off, schedules a
// full cache clear and lets execution go on in the space below the guard.
//
// On POSIX systems the guard is a read-protected range handled like a
// fastmem fault. On Windows the system's own guard pages are used:
// SetThreadStackGuarantee makes the stack-overflow exception arrive while
// SAFE_STACK_SIZE bytes are still left, and _resetstkoflw re-arms it after
// the cleanup.

const std::array<std::pair<bool JitBase::*, const Config::Info<bool>*>, 25> JitBase::JIT_SETTINGS{{
    {&JitBase::m_accurate_cpu_cache_enabled, &Config::MAIN_ACCURATE_CPU_CACHE},
    {&JitBase::m_accurate_fmadds, &Config::MAIN_ACCURATE_FMADDS},
    {&JitBase::m_accurate_nans, &Config::MAIN_ACCURATE_NANS},
    {&JitBase::bJITBranchOff, &Config::MAIN_DEBUG_JIT_BRANCH_OFF},
    {&JitBase::m_enable_profiling, &Config::MAIN_DEBUG_JIT_ENABLE_PROFILING},
    {&JitBase::bJITFloatingPointOff, &Config::MAIN_DEBUG_JIT_FLOATING_POINT_OFF},
    {&JitBase::bJITIntegerOff, &Config::MAIN_DEBUG_JIT_INTEGER_OFF},
    {&JitBase::bJITLoadStoreFloatingOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF},
    {&JitBase::bJITLoadStorelbzxOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF},
    {&JitBase::bJITLoadStorelwzOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF},
    {&JitBase::bJITLoadStorelXzOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF},
    {&JitBase::bJITLoadStoreOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_OFF},
    {&JitBase::bJITLoadStorePairedOff, &Config::MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF},
    {&JitBase::bJITOff, &Config::MAIN_DEBUG_JIT_OFF},
    {&JitBase::bJITPairedOff, &Config::MAIN_DEBUG_JIT_PAIRED_OFF},
    {&JitBase::bJITRegisterCacheOff, &Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF},
    {&JitBase::bJITSystemRegistersOff, &Config::MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF},
    {&JitBase::m_enable_div_by_zero_exceptions, &Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS},
    {&JitBase::m_enable_debugging, &Config::MAIN_ENABLE_DEBUGGING},
    {&JitBase::m_fastmem_enabled, &Config::MAIN_FASTMEM},
    {&JitBase::m_enable_float_exceptions, &Config::MAIN_FLOAT_EXCEPTIONS},
    {&JitBase::m_fprf, &Config::MAIN_FPRF},
    {&JitBase::m_enable_branch_following, &Config::MAIN_JIT_FOLLOW_BRANCH},
    {&JitBase::m_low_dcbz_hack, &Config::MAIN_LOW_DCBZ_HACK},
    {&JitBase::m_page_table_fastmem_enabled, &Config::MAIN_PAGE_TABLE_FASTMEM},
}};

JitBase::JitBase(Core::System& system)
    : m_code_buffer(code_buffer_size), m_system(system), m_ppc_state(system.GetPPCState()),
      m_mmu(system.GetMMU()), m_branch_watch(system.GetPowerPC().GetBranchWatch()),
      m_ppc_symbol_db(system.GetPPCSymbolDB())
{
  // RefreshConfig itself runs from the backend's Init and ClearCache; a
  // settings change only has to throw the compiled code away.
  m_registered_config_callback_id = CPUThreadConfigCallback::AddConfigChangedCallback([this] {
    if (DoesConfigNeedRefresh())
      ClearCache();
  });
}

JitBase::~JitBase()
{
  CPUThreadConfigCallback::RemoveConfigChangedCallback(m_registered_config_callback_id);
}

bool JitBase::DoesConfigNeedRefresh() const
{
  for (const auto& [member, info] : JIT_SETTINGS)
  {
    if (this->*member != Config::Get(*info))
      return true;
  }
  return false;
}

void JitBase::RefreshConfig()
{
  const bool had_page_table_mappings = WantsPageTableMappings();

  for (const auto& [member, info] : JIT_SETTINGS)
    this->*member = Config::Get(*info);

  // With the data cache emulated, memory must go through the cache code, and
  // the dcbz workaround has nothing left to work around.
  if (m_accurate_cpu_cache_enabled)
  {
    m_fastmem_enabled = false;
    m_low_dcbz_hack = false;
  }

  analyzer.SetDebuggingEnabled(m_enable_debugging);
  analyzer.SetBranchFollowingEnabled(m_enable_branch_following);
  analyzer.SetFloatExceptionsEnabled(m_enable_float_exceptions);
  analyzer.SetDivByZeroExceptionsEnabled(m_enable_div_by_zero_exceptions);

  const bool watchpoints = m_system.GetPowerPC().GetMemChecks().HasAny();

  // Watchpoints on untranslated accesses cannot be caught by fastmem faults.
  const bool fastmem_usable = m_ppc_state.msr.DR || !watchpoints;
  jo.fastmem = m_fastmem_enabled && jo.fastmem_arena && fastmem_usable &&
               EMM::IsExceptionHandlerSupported();
  jo.memcheck = watchpoints || m_system.IsMMUMode() || m_system.IsPauseOnPanicMode();
  jo.fp_exceptions = m_enable_float_exceptions;
  jo.div_by_zero_exceptions = m_enable_div_by_zero_exceptions;

  // The MMU only keeps host mappings for page table entries while some core
  // wants them, so tell it when that changes. During start-up the MMU is not
  // ready for this yet.
  if (WantsPageTableMappings() != had_page_table_mappings && Core::IsRunning(m_system))
    m_system.GetMMU().PageTableUpdated();
}

bool JitBase::WantsPageTableMappings() const
{
  return m_page_table_fastmem_enabled && jo.fastmem;
}

void JitBase::InitFastmemArena()
{
  const bool wanted = Config::Get(Config::MAIN_FASTMEM_ARENA);
  jo.fastmem_arena = wanted && m_system.GetMemory().InitFastmemArena();
}

void JitBase::InitBLROptimization()
{
  m_enable_blr_optimization =
      EMM::IsExceptionHandlerSupported() && jo.enableBlocklink && !IsDebuggingEnabled();
  m_cleanup_after_stackfault = false;
}

void JitBase::ProtectStack()
{
  if (!m_enable_blr_optimization)
    return;

  const auto give_up = [this] {
    m_stack_guard = nullptr;
    m_enable_blr_optimization = false;
  };

#ifdef _WIN32
  ULONG guarantee = SAFE_STACK_SIZE;
  if (!SetThreadStackGuarantee(&guarantee))
  {
    PanicAlertFmt("Failed to set thread stack guarantee");
    give_up();
  }
#else
  const auto [stack_low, stack_size] = Common::GetCurrentThreadStack();
  const uintptr_t low = reinterpret_cast<uintptr_t>(stack_low);
  // Any local lives in the current frame, which is somewhere inside the stack.
  int probe = 0;
  const uintptr_t here = reinterpret_cast<uintptr_t>(&probe);
  if (here < low || here - low >= stack_size)
  {
    PanicAlertFmt("Failed to get correct stack base");
    give_up();
    return;
  }

  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0)
  {
    PanicAlertFmt("Failed to get page size");
    give_up();
    return;
  }

  const uintptr_t guard = Common::AlignUp(low + GUARD_OFFSET, static_cast<uintptr_t>(page));
  // The guard and the room still needed below the current frame must fit.
  if (guard >= here || here - guard < GUARD_SIZE + MIN_UNSAFE_STACK_SIZE)
  {
    PanicAlertFmt("Stack is too small for BLR optimization (size {:x}, base {:x}, current stack "
                  "pointer {:x}, alignment {:x})",
                  stack_size, low, here, page);
    give_up();
    return;
  }

  m_stack_guard = reinterpret_cast<u8*>(guard);
  if (!Common::ReadProtectMemory(m_stack_guard, GUARD_SIZE))
    give_up();
#endif
}

void JitBase::UnprotectStack()
{
#ifndef _WIN32
  if (m_stack_guard == nullptr)
    return;
  Common::UnWriteProtectMemory(m_stack_guard, GUARD_SIZE);
  m_stack_guard = nullptr;
#endif
}

bool JitBase::HandleStackFault()
{
  // Only a fault caused by our own guard, on the CPU thread, can be recovered
  // from; anything else is a real overflow and is left to crash.
  if (!Core::IsCPUThread() || !m_enable_blr_optimization)
    return false;

  WARN_LOG_FMT(POWERPC, "BLR cache disabled due to excessive BL in the emulated program.");

  UnprotectStack();
  m_enable_blr_optimization = false;

  // The compiled CALLs cannot be thrown away while one of them is running.
  // Invalidate everything so no block is entered again, force the next exit
  // to go through the dispatcher's timing check and clear the cache there.
  GetBlockCache()->InvalidateICache(0, 0xffffffff, true);
  m_system.GetCoreTiming().ForceExceptionCheck(0);
  m_cleanup_after_stackfault = true;
  return true;
}

void JitBase::CleanUpAfterStackFault()
{
  if (!m_cleanup_after_stackfault)
    return;

  ClearCache();
  m_cleanup_after_stackfault = false;
#ifdef _WIN32
  // The overflow consumed the guard page; put it back.
  _resetstkoflw();
#endif
}

bool JitBase::CanMergeNextInstructions(int count) const
{
  if (js.instructionsLeft < count || m_system.GetCPU().IsStepping())
    return false;

  // A breakpoint between the instructions would see host flags clobbered.
  if (!IsDebuggingEnabled())
    return true;
  const auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
  for (int offset = 1; offset <= count; ++offset)
  {
    if (breakpoints.IsAddressBreakPoint(js.op[offset].address))
      return false;
  }
  return true;
}

bool JitBase::ShouldHandleFPExceptionForInstruction(const PPCAnalyst::CodeOp* op) const
{
  const u64 flags = op->opinfo->flags;
  if (jo.fp_exceptions)
    return (flags & FL_FLOAT_EXCEPTION) != 0;
  if (jo.div_by_zero_exceptions)
    return (flags & FL_FLOAT_DIV) != 0;
  return false;
}

// Entry points called from the generated dispatcher.

const u8* JitBase::Dispatch(JitBase& jit)
{
  return jit.GetBlockCache()->Dispatch();
}

void JitTrampoline(JitBase& jit, u32 em_address)
{
  jit.Jit(em_address);
}
