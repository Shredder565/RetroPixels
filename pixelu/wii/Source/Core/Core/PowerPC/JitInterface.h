// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Owner of the active recompiler (Jit64, JitArm64 or the cached
// interpreter) and the one entry point the rest of the core uses to reach
// it. Every call is a no-op (or returns an empty result) when no recompiler
// is active.

#pragma once

#include <cstddef>
#include <cstdio>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/MachineContext.h"

class CPUCoreBase;
class PointerWrap;
class JitBase;
struct JitBlock;

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core
namespace PowerPC
{
enum class CPUCore;
}

class JitInterface
{
public:
  explicit JitInterface(Core::System& system);
  JitInterface(const JitInterface&) = delete;
  JitInterface(JitInterface&&) = delete;
  JitInterface& operator=(const JitInterface&) = delete;
  JitInterface& operator=(JitInterface&&) = delete;
  ~JitInterface();

  // Loading a state throws away all compiled code.
  void DoState(PointerWrap& p);

  // Creates and initialises the recompiler for core. Returns null (and has
  // no recompiler) for cores that are not recompilers.
  CPUCoreBase* InitJitCore(PowerPC::CPUCore core);
  CPUCoreBase* GetCore() const;

#ifdef _ARCH_32
  constexpr bool WantsPageTableMappings() const { return false; }
#else
  bool WantsPageTableMappings() const;
#endif

  // Points PowerPCState::mem_ptr at the view matching MSR[DR] and the
  // recompiler's fastmem mode.
  void UpdateMembase();
  // Writes one tab-separated line per block (with profiling data if enabled).
  void JitBlockLogDump(const Core::CPUThreadGuard& guard, std::FILE* file) const;
  void WipeBlockProfilingData(const Core::CPUThreadGuard& guard);
  void RunOnBlocks(const Core::CPUThreadGuard& guard,
                   const std::function<void(const JitBlock&)>& f) const;
  std::size_t GetBlockCount() const;

  // Host fault handling for fastmem accesses and stack guard pages.
  bool HandleFault(uintptr_t access_address, SContext* ctx);
  bool HandleStackFault();

  // Discards all compiled code.
  void ClearCache(const Core::CPUThreadGuard& guard);

  // Forgets the compiled blocks without freeing their code, so it may be
  // called from inside compiled code.
  void ClearSafe();

  // For the debugger's JIT block view only: removing blocks from elsewhere
  // would leave that view with dangling references.
  void EraseSingleBlock(const JitBlock& block);

  // Per code region: name, free bytes, fragmentation ratio.
  using MemoryStats = std::pair<std::string_view, std::pair<std::size_t, double>>;
  std::vector<MemoryStats> GetMemoryStats() const;

  // Disassemble a block's host code; return the instruction count.
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const;
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const;

  // Drops blocks overlapping the range. forced: recompile even though the
  // code did not change (e.g. to add an exception check).
  void InvalidateICache(u32 address, u32 size, bool forced);
  void InvalidateICacheLine(u32 address);
  // count consecutive 32-byte lines starting at address (0 = all of memory).
  void InvalidateICacheLines(u32 address, u32 count);
  static void InvalidateICacheLineFromJIT(JitInterface& jit_interface, u32 address);
  static void InvalidateICacheLinesFromJIT(JitInterface& jit_interface, u32 address, u32 count);

  // Run-time discoveries that make the instruction at PC need a slower,
  // checked code path; its block is recompiled with it.
  enum class ExceptionType
  {
    FIFOWrite,
    PairedQuantize,
    SpeculativeConstants
  };
  void CompileExceptionCheck(ExceptionType type);
  static void CompileExceptionCheckFromJIT(JitInterface& jit_interface, ExceptionType type);

  // Test hook (page fault unit test).
  void SetJit(std::unique_ptr<JitBase> jit);

  void Shutdown();

private:
  std::unique_ptr<JitBase> m_jit;
  Core::System& m_system;
};
