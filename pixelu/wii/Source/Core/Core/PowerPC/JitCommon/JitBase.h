// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <iosfwd>
#include <map>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/Config/ConfigInfo.h"
#include "Common/x64Emitter.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/ConfigManager.h"
#include "Core/MachineContext.h"
#include "Core/PowerPC/CPUCoreBase.h"
#include "Core/PowerPC/JitCommon/JitAsmCommon.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"

namespace Core
{
class BranchWatch;
class System;
}  // namespace Core
namespace PowerPC
{
class MMU;
struct PowerPCState;
}  // namespace PowerPC
class PPCSymbolDB;

// Placed at the start of every instruction handler; empty in normal builds.
#define INSTRUCTION_START

// Hands the current instruction to the interpreter and leaves the handler.
#define FALLBACK_IF(cond)                                                                          \
  do                                                                                               \
  {                                                                                                \
    if (cond)                                                                                      \
    {                                                                                              \
      FallBackToInterpreter(inst);                                                                 \
      return;                                                                                      \
    }                                                                                              \
  } while (0)

// Falls back when the whole JIT or the given instruction group is switched off.
#define JITDISABLE(setting) FALLBACK_IF(bJITOff || setting)

// State and settings shared by the recompiling CPU cores.
class JitBase : public CPUCoreBase
{
protected:
  // Where the guest carry bit currently lives while a block is compiled.
  enum class CarryFlag
  {
    InPPCState,
    InHostCarry,
#ifdef _M_X86_64
    InHostCarryInverted,
#endif
#ifdef _M_ARM_64
    ConstantTrue,
    ConstantFalse,
#endif
  };

  // Native stack budget for the BLR optimization (calls are mirrored on the
  // host stack). A guard region sits GUARD_OFFSET bytes above the stack's
  // lowest address; touching it disables the optimization, and the space below
  // the guard is what C++ code may still use after that.
  static constexpr size_t SAFE_STACK_SIZE = 256 * 1024;
  static constexpr size_t MIN_UNSAFE_STACK_SIZE = 192 * 1024;
  static constexpr size_t MIN_STACK_SIZE = SAFE_STACK_SIZE + MIN_UNSAFE_STACK_SIZE;
  static constexpr size_t GUARD_SIZE = 64 * 1024;
  static constexpr size_t GUARD_OFFSET = SAFE_STACK_SIZE - GUARD_SIZE;

  // Settings derived from the configuration when the cache is (re)built.
  struct JitOptions
  {
    bool enableBlocklink;
    bool optimizeGatherPipe;
    bool accurateSinglePrecision;
    bool fastmem;
    bool fastmem_arena;
    bool memcheck;
    bool fp_exceptions;
    bool div_by_zero_exceptions;
  };

  // Per-block compiler state.
  struct JitState
  {
    u32 compilerPC;
    u32 blockStart;
    int instructionsLeft;
    u32 downcountAmount;
    u32 numLoadStoreInst;
    u32 numFloatingPointInst;

    // Set by a fastmem access that needs an exception check after it.
    u8* fastmemLoadStore;
    // A load/store already branched to the exception path; the check after
    // the instruction only has to bind exceptionHandler.
    bool fixupExceptionHandler;
    Gen::FixupBranch exceptionHandler;

    bool assumeNoPairedQuantize;
    BitSet8 constantGqrValid;
    std::array<u32, 8> constantGqr;
    bool firstFPInstructionFound;
    bool isLastInstruction;
    int skipInstructions;
    CarryFlag carryFlag;

    // While a fastmem trampoline is generated: where its exception exit goes.
    bool generatingTrampoline = false;
    u8* trampolineExceptionHandler;

    bool mustCheckFifo;
    u32 fifoBytesSinceCheck;

    PPCAnalyst::BlockStats st;
    PPCAnalyst::BlockRegStats gpa;
    PPCAnalyst::BlockRegStats fpa;
    PPCAnalyst::CodeOp* op;
    BitSet32 fpr_is_store_safe;

    JitBlock* curBlock;

    // Instructions that turned out to need a slower but safer translation;
    // they survive cache clears.
    std::unordered_set<u32> fifoWriteAddresses;
    std::unordered_set<u32> pairedQuantizeAddresses;
    std::unordered_set<u32> noSpeculativeConstantsAddresses;
  };

  PPCAnalyst::CodeBlock code_block;
  PPCAnalyst::CodeBuffer m_code_buffer;
  PPCAnalyst::PPCAnalyzer analyzer;

  CPUThreadConfigCallback::ConfigChangedCallbackID m_registered_config_callback_id;

  // Debug switches: each sends one instruction group to the interpreter.
  bool bJITOff = false;
  bool bJITLoadStoreOff = false;
  bool bJITLoadStorelXzOff = false;
  bool bJITLoadStorelwzOff = false;
  bool bJITLoadStorelbzxOff = false;
  bool bJITLoadStoreFloatingOff = false;
  bool bJITLoadStorePairedOff = false;
  bool bJITFloatingPointOff = false;
  bool bJITIntegerOff = false;
  bool bJITPairedOff = false;
  bool bJITSystemRegistersOff = false;
  bool bJITBranchOff = false;
  bool bJITRegisterCacheOff = false;

  bool m_enable_profiling = false;
  bool m_enable_debugging = false;
  bool m_enable_branch_following = false;
  bool m_enable_float_exceptions = false;
  bool m_enable_div_by_zero_exceptions = false;
  bool m_low_dcbz_hack = false;
  bool m_fprf = false;
  bool m_accurate_nans = false;
  bool m_accurate_fmadds = false;
  bool m_fastmem_enabled = false;
  bool m_page_table_fastmem_enabled = false;
  bool m_accurate_cpu_cache_enabled = false;

  bool m_enable_blr_optimization = false;
  bool m_cleanup_after_stackfault = false;
  u8* m_stack_guard = nullptr;

  // Every member above that mirrors a configuration value, with its setting.
  static const std::array<std::pair<bool JitBase::*, const Config::Info<bool>*>, 25> JIT_SETTINGS;

  bool DoesConfigNeedRefresh() const;
  void RefreshConfig();

  void InitFastmemArena();

  void InitBLROptimization();
  void ProtectStack();
  void UnprotectStack();
  void CleanUpAfterStackFault();

  // Whether the next `count` instructions can be compiled together with the
  // current one (no stepping, no breakpoint in between, enough left).
  bool CanMergeNextInstructions(int count) const;

  bool HasConstantCarry() const
  {
#ifdef _M_ARM_64
    return js.carryFlag == CarryFlag::ConstantTrue || js.carryFlag == CarryFlag::ConstantFalse;
#else
    return false;
#endif
  }

  // Whether `op` must be followed by an FP exception check.
  bool ShouldHandleFPExceptionForInstruction(const PPCAnalyst::CodeOp* op) const;

public:
  explicit JitBase(Core::System& system);
  JitBase(const JitBase&) = delete;
  JitBase(JitBase&&) = delete;
  JitBase& operator=(const JitBase&) = delete;
  JitBase& operator=(JitBase&&) = delete;
  ~JitBase() override;

  bool IsProfilingEnabled() const { return m_enable_profiling && m_enable_debugging; }
  bool IsDebuggingEnabled() const { return m_enable_debugging; }
  bool IsBranchWatchEnabled() const
  {
    return m_system.GetPowerPC().GetBranchWatch().GetRecordingActive();
  }

  // Called from generated code: the host address to continue at for PPC_PC.
  static const u8* Dispatch(JitBase& jit);
  virtual JitBaseBlockCache* GetBlockCache() = 0;

  virtual void Jit(u32 em_address) = 0;

  virtual void EraseSingleBlock(const JitBlock& block) = 0;

  // Memory region name, free size, and fragmentation ratio
  using MemoryStats = std::pair<std::string_view, std::pair<std::size_t, double>>;
  virtual std::vector<MemoryStats> GetMemoryStats() const = 0;

  virtual std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const = 0;
  virtual std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const = 0;

  virtual const CommonAsmRoutinesBase* GetAsmRoutines() = 0;

  virtual bool WantsPageTableMappings() const;

  virtual bool HandleFault(uintptr_t access_address, SContext* ctx) = 0;
  bool HandleStackFault();

  // Largest number of instructions analysed for one block.
  static constexpr std::size_t code_buffer_size = 32000;

  JitOptions jo{};
  JitState js{};

  Core::System& m_system;
  PowerPC::PowerPCState& m_ppc_state;
  PowerPC::MMU& m_mmu;
  Core::BranchWatch& m_branch_watch;
  PPCSymbolDB& m_ppc_symbol_db;
};

// Called from generated code when no block exists for an address.
void JitTrampoline(JitBase& jit, u32 em_address);
