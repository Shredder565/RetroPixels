// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The emulated CPU: its architectural state (PowerPCState, which the JITs
// address by offset) and PowerPCManager, which owns the state, the debugger
// objects tied to it, and the active execution engine, and which delivers
// exceptions.

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <tuple>
#include <type_traits>
#include <vector>

#include "Common/CommonTypes.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/ConditionRegister.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCCache.h"
#include "Core/PowerPC/PPCSymbolDB.h"

class CPUCoreBase;
class PointerWrap;
namespace CoreTiming
{
struct EventType;
}

namespace PowerPC
{
// Execution engines. The numbers are stored in users' configuration, so
// they never change; the missing ones belonged to engines that no longer
// exist.
enum class CPUCore
{
  Interpreter = 0,
  JIT64 = 1,
  JITARM64 = 4,
  CachedInterpreter = 5,
};

enum class CoreMode
{
  Interpreter,
  JIT,
};

// Software TLB: separate data and instruction TLBs, each two-way set
// associative with 64 sets (128 entries).
constexpr size_t TLB_SIZE = 128;
constexpr size_t NUM_TLBS = 2;
constexpr size_t TLB_WAYS = 2;
constexpr size_t DATA_TLB_INDEX = 0;
constexpr size_t INST_TLB_INDEX = 1;

struct TLBEntry
{
  using WayArray = std::array<u32, TLB_WAYS>;

  static constexpr u32 INVALID_TAG = 0xffffffff;

  WayArray tag{INVALID_TAG, INVALID_TAG};
  WayArray paddr{};
  WayArray vsid{};
  WayArray pte{};
  // Which way was used last (the other one is replaced next).
  u32 recent = 0;

  void Invalidate() { tag.fill(INVALID_TAG); }
};

// One floating-point register. ps0 is the full 64-bit double; ps1 is the
// second paired-single slot, also kept as a double.
struct PairedSingle
{
  // Raw bits (u64 overloads) or a double value (stored as its bits).
  void SetPS0(u64 value) { ps0 = value; }
  void SetPS1(u64 value) { ps1 = value; }
  void SetPS0(double value);
  void SetPS1(double value);

  void SetBoth(u64 lhs, u64 rhs) { ps0 = lhs, ps1 = rhs; }
  void SetBoth(double lhs, double rhs) { SetPS0(lhs), SetPS1(rhs); }
  void Fill(u64 value) { ps0 = ps1 = value; }
  void Fill(double value) { SetBoth(value, value); }

  u64 PS0AsU64() const { return ps0; }
  u64 PS1AsU64() const { return ps1; }
  // The low word of each slot.
  u32 PS0AsU32() const { return static_cast<u32>(ps0); }
  u32 PS1AsU32() const { return static_cast<u32>(ps1); }
  double PS0AsDouble() const;
  double PS1AsDouble() const;

  u64 ps0 = 0;
  u64 ps1 = 0;
};
// The JITs use offsetof on it.
static_assert(std::is_standard_layout_v<PairedSingle>, "PairedSingle must be standard layout");

// The whole architectural state of the CPU.
//
// The x64 JIT keeps a pointer to this struct (biased by 0x80) in a register
// and reaches fields with one-byte displacements where it can, so the fields
// it touches most (pc, npc, gpr, cr, msr, fpscr, the XER bytes, downcount,
// Exceptions, ...) sit in the first 256 bytes, and the 512-byte FPR array
// starts right after them. The layout is pinned by the static_asserts below
// the struct.
struct PowerPCState
{
  u32 pc = 0;
  u32 npc = 0;

  // Host stack pointer saved by the JIT's BLR optimisation.
  u8* stored_stack_pointer = nullptr;

  // Write-gather pipe position and buffer, for the JIT's fast path.
  u8* gather_pipe_ptr = nullptr;
  u8* gather_pipe_base_ptr = nullptr;

  u32 gpr[32]{};

  ConditionRegister cr{};

  UReg_MSR msr;
  UReg_FPSCR fpscr;

  CPUEmuFeatureFlags feature_flags;

  // Pending EXCEPTION_* bits.
  u32 Exceptions = 0;

  // Cycles left until the next CoreTiming event (kept here so the JIT can
  // reach it cheaply).
  int downcount = 0;

  // XER is kept split up: the carry, the SO/OV pair as (SO << 1) | OV, and
  // the string byte count/compare bits (Broadway implements bits 16-23 even
  // without lscbx).
  u8 xer_ca = 0;
  u8 xer_so_ov = 0;
  u16 xer_stringctrl = 0;

  // lwarx/stwcx. reservation.
  u32 reserve_address;
  bool reserve;

  bool pagetable_update_pending = false;
  bool m_enable_dcache = false;

  // The FPRs, as pairs of doubles (SSE-friendly).
  alignas(16) PairedSingle ps[32];

  std::array<u32, 16> sr{};

  // All SPRs, indexed by SPR number. 8-byte aligned so TBL/TBU can be read
  // as one 64-bit value.
  alignas(8) u32 spr[1024]{};

  // Base of the fastmem view currently in use.
  u8* mem_ptr = nullptr;

  // Decoded SDR1.
  u32 pagetable_base = 0;
  u32 pagetable_mask = 0;

  std::array<std::array<TLBEntry, TLB_SIZE / TLB_WAYS>, NUM_TLBS> tlb;

  InstructionCache iCache;
  Cache dCache;

  // CR1 after a floating-point instruction with Rc set: FX, FEX, VX, OX.
  void UpdateCR1()
  {
    const u32 field = (fpscr.FX << 3) | (fpscr.FEX << 2) | (fpscr.VX << 1) | fpscr.OX;
    cr.SetField(1, field);
  }

  void SetCarry(u32 ca) { xer_ca = ca; }

  u32 GetCarry() const { return xer_ca; }

  UReg_XER GetXER() const
  {
    const u32 value = u32{xer_stringctrl} | (u32{xer_ca} << XER_CA_SHIFT) |
                      (u32{xer_so_ov} << XER_OV_SHIFT);
    return UReg_XER{value};
  }

  void SetXER(UReg_XER new_xer)
  {
    xer_stringctrl = static_cast<u16>(new_xer.BYTE_COUNT | (new_xer.BYTE_CMP << 8));
    xer_ca = static_cast<u8>(new_xer.CA);
    xer_so_ov = static_cast<u8>((new_xer.SO << 1) | new_xer.OV);
  }

  u32 GetXER_SO() const { return xer_so_ov >> 1; }

  // SO is sticky: this can only set it.
  void SetXER_SO(bool value) { xer_so_ov |= static_cast<u8>(value) << 1; }

  u32 GetXER_OV() const { return xer_so_ov & XER_OV_MASK; }

  // Setting OV also sets SO.
  void SetXER_OV(bool value)
  {
    xer_so_ov = static_cast<u8>((xer_so_ov & ~XER_OV_MASK) | static_cast<u8>(value));
    SetXER_SO(value);
  }

  void UpdateFPRFDouble(double dvalue);
  void UpdateFPRFSingle(float fvalue);
};

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
// Field offsets the JITs and savestates were built around.
static_assert(offsetof(PowerPCState, pc) == 0);
static_assert(offsetof(PowerPCState, npc) == 4);
static_assert(offsetof(PowerPCState, stored_stack_pointer) == 8);
static_assert(offsetof(PowerPCState, gather_pipe_ptr) == 16);
static_assert(offsetof(PowerPCState, gather_pipe_base_ptr) == 24);
static_assert(offsetof(PowerPCState, gpr) == 32);
static_assert(offsetof(PowerPCState, cr) == 160);
static_assert(offsetof(PowerPCState, msr) == 224);
static_assert(offsetof(PowerPCState, fpscr) == 228);
static_assert(offsetof(PowerPCState, feature_flags) == 232);
static_assert(offsetof(PowerPCState, Exceptions) == 236);
static_assert(offsetof(PowerPCState, downcount) == 240);
static_assert(offsetof(PowerPCState, xer_ca) == 244);
static_assert(offsetof(PowerPCState, xer_so_ov) == 245);
static_assert(offsetof(PowerPCState, xer_stringctrl) == 246);
static_assert(offsetof(PowerPCState, reserve_address) == 248);
static_assert(offsetof(PowerPCState, reserve) == 252);
static_assert(offsetof(PowerPCState, pagetable_update_pending) == 253);
static_assert(offsetof(PowerPCState, m_enable_dcache) == 254);
static_assert(offsetof(PowerPCState, ps) == 256);
static_assert(offsetof(PowerPCState, sr) == 768);
static_assert(offsetof(PowerPCState, spr) == 832);
static_assert(offsetof(PowerPCState, mem_ptr) == 4928);
static_assert(offsetof(PowerPCState, pagetable_base) == 4936);
static_assert(offsetof(PowerPCState, pagetable_mask) == 4940);
static_assert(offsetof(PowerPCState, tlb) == 4944);
static_assert(offsetof(PowerPCState, iCache) == 9552);
static_assert(offsetof(PowerPCState, dCache) ==
              offsetof(PowerPCState, iCache) + sizeof(InstructionCache));
static_assert(alignof(PowerPCState) == 16);
static_assert(sizeof(ConditionRegister) == 64);
static_assert(sizeof(PairedSingle) == 16);
static_assert(sizeof(TLBEntry) == 36);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

std::span<const CPUCore> AvailableCPUCores();
CPUCore DefaultCPUCore();

class PowerPCManager
{
public:
  explicit PowerPCManager(Core::System& system);
  PowerPCManager(const PowerPCManager& other) = delete;
  PowerPCManager(PowerPCManager&& other) = delete;
  PowerPCManager& operator=(const PowerPCManager& other) = delete;
  PowerPCManager& operator=(PowerPCManager&& other) = delete;
  ~PowerPCManager();

  void Init(CPUCore cpu_core);
  void Reset();
  void Shutdown();
  void DoState(PointerWrap& p);
  // icbi from a thread other than the CPU thread: done on the CPU thread
  // through CoreTiming while emulation runs.
  void ScheduleInvalidateCacheThreadSafe(u32 address);

  CoreMode GetMode() const;
  // CPU thread only, or with the CPU paused (CPU::PauseAndLock) or not yet
  // started.
  void SetMode(CoreMode _coreType);
  const char* GetCPUName() const;

  // Replaces the execution engine with an outside one until called again
  // with nullptr. The injected core is Init()ed when added and Shutdown()
  // when removed, and GetMode() reports Interpreter meanwhile. Same thread
  // rules as SetMode(), and never from inside the CPU run loop itself.
  void InjectExternalCPUCore(CPUCoreBase* core);

  // These need the CPU execution lock (CPU thread or CPU::PauseAndLock).
  void SingleStep();
  void CheckExceptions();
  void CheckExternalExceptions();
  // Logs a breakpoint hit at PC if asked to; returns whether it would stop.
  bool CheckBreakPoints();
  // Same, and actually stops emulation (and hands control to GDB if
  // attached). Returns whether it stopped.
  bool CheckAndHandleBreakPoints();
  void RunLoop();

  u64 ReadFullTimeBaseValue() const;
  void WriteFullTimeBaseValue(u64 value);

  // Call after anything changes MSR.
  void MSRUpdated();

  PowerPCState& GetPPCState() { return m_ppc_state; }
  const PowerPCState& GetPPCState() const { return m_ppc_state; }
  BreakPoints& GetBreakPoints() { return m_breakpoints; }
  const BreakPoints& GetBreakPoints() const { return m_breakpoints; }
  MemChecks& GetMemChecks() { return m_memchecks; }
  const MemChecks& GetMemChecks() const { return m_memchecks; }
  PPCDebugInterface& GetDebugInterface() { return m_debug_interface; }
  const PPCDebugInterface& GetDebugInterface() const { return m_debug_interface; }
  PPCSymbolDB& GetSymbolDB() { return m_symbol_db; }
  const PPCSymbolDB& GetSymbolDB() const { return m_symbol_db; }
  Core::BranchWatch& GetBranchWatch() { return m_branch_watch; }
  const Core::BranchWatch& GetBranchWatch() const { return m_branch_watch; }

private:
  // Picks m_cpu_core_base for m_mode (unless a core is injected).
  void ApplyMode();
  void InitializeCPUCore(CPUCore cpu_core);
  // Re-reads the accurate-cache setting.
  void RefreshConfig();
  void ResetRegisters();

  PowerPCState m_ppc_state;

  CPUCoreBase* m_cpu_core_base = nullptr;
  bool m_cpu_core_base_is_injected = false;
  CoreMode m_mode = CoreMode::Interpreter;

  BreakPoints m_breakpoints;
  MemChecks m_memchecks;
  PPCSymbolDB m_symbol_db;
  PPCDebugInterface m_debug_interface;
  Core::BranchWatch m_branch_watch;

  CPUThreadConfigCallback::ConfigChangedCallbackID m_registered_config_callback_id;

  CoreTiming::EventType* m_invalidate_cache_thread_safe = nullptr;

  Core::System& m_system;
};

void UpdatePerformanceMonitor(u32 cycles, u32 num_load_stores, u32 num_fp_inst,
                              PowerPCState& ppc_state);

// Plain functions the JITs can call.
void CheckExceptionsFromJIT(PowerPCManager& power_pc);
void CheckExternalExceptionsFromJIT(PowerPCManager& power_pc);
void CheckAndHandleBreakPointsFromJIT(PowerPCManager& power_pc);

// Typed views of SPRs.
#define HID0(ppc_state) ((UReg_HID0&)(ppc_state).spr[SPR_HID0])
#define HID2(ppc_state) ((UReg_HID2&)(ppc_state).spr[SPR_HID2])
#define HID4(ppc_state) ((UReg_HID4&)(ppc_state).spr[SPR_HID4])
#define DMAU(ppc_state) (*(UReg_DMAU*)&(ppc_state).spr[SPR_DMAU])
#define DMAL(ppc_state) (*(UReg_DMAL*)&(ppc_state).spr[SPR_DMAL])
#define MMCR0(ppc_state) ((UReg_MMCR0&)(ppc_state).spr[SPR_MMCR0])
#define MMCR1(ppc_state) ((UReg_MMCR1&)(ppc_state).spr[SPR_MMCR1])
#define THRM1(ppc_state) ((UReg_THRM12&)(ppc_state).spr[SPR_THRM1])
#define THRM2(ppc_state) ((UReg_THRM12&)(ppc_state).spr[SPR_THRM2])
#define THRM3(ppc_state) ((UReg_THRM3&)(ppc_state).spr[SPR_THRM3])

#define LR(ppc_state) (ppc_state).spr[SPR_LR]
#define CTR(ppc_state) (ppc_state).spr[SPR_CTR]
#define SRR0(ppc_state) (ppc_state).spr[SPR_SRR0]
#define SRR1(ppc_state) (ppc_state).spr[SPR_SRR1]
#define GQR(ppc_state, x) (ppc_state).spr[SPR_GQR0 + (x)]
#define TL(ppc_state) (ppc_state).spr[SPR_TL]
#define TU(ppc_state) (ppc_state).spr[SPR_TU]

void RoundingModeUpdated(PowerPCState& ppc_state);
void MMCRUpdated(PowerPCState& ppc_state);
void RecalculateAllFeatureFlags(PowerPCState& ppc_state);

}  // namespace PowerPC
