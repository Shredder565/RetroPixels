// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/JitInterface.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/PowerPC/CPUCoreBase.h"
#include "Core/PowerPC/CachedInterpreter/CachedInterpreter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif

#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif

namespace
{
// "DR", "IR" and "PERFMON" for the set feature flags, joined by '|'.
std::string FeatureFlagNames(CPUEmuFeatureFlags flags)
{
  std::string names;
  const auto add = [&](CPUEmuFeatureFlags bit, std::string_view name) {
    if (!(flags & bit))
      return;
    if (!names.empty())
      names += '|';
    names += name;
  };
  add(FEATURE_FLAG_MSR_DR, "DR");
  add(FEATURE_FLAG_MSR_IR, "IR");
  add(FEATURE_FLAG_PERFMON, "PERFMON");
  return names;
}

double PercentOf(double part, double whole)
{
  return whole == 0 ? 0.0 : 100.0 * part / whole;
}

double PerRun(double total, u64 runs)
{
  return runs == 0 ? 0.0 : total / runs;
}
// The recompilers this build has. Anything else (the plain interpreter, or a core for
// another host architecture) gives nullptr.
std::unique_ptr<JitBase> MakeRecompiler(PowerPC::CPUCore core, Core::System& system)
{
  using PowerPC::CPUCore;
  if (core == CPUCore::CachedInterpreter)
    return std::make_unique<CachedInterpreter>(system);
#ifdef _M_X86_64
  if (core == CPUCore::JIT64)
    return std::make_unique<Jit64>(system);
#endif
#ifdef _M_ARM_64
  if (core == CPUCore::JITARM64)
    return std::make_unique<JitArm64>(system);
#endif
  return nullptr;
}

template <typename F>
void WithBlockCache(JitBase* jit, F&& f)
{
  if (jit != nullptr)
    f(*jit->GetBlockCache());
}
}  // namespace

JitInterface::JitInterface(Core::System& system) : m_system(system)
{
}

JitInterface::~JitInterface() = default;

CPUCoreBase* JitInterface::InitJitCore(PowerPC::CPUCore core)
{
  // An unsupported core leaves no recompiler behind; the caller falls back to its default.
  m_jit = MakeRecompiler(core, m_system);
  if (m_jit == nullptr)
    return nullptr;
  m_jit->Init();
  return m_jit.get();
}

CPUCoreBase* JitInterface::GetCore() const
{
  return m_jit.get();
}

void JitInterface::SetJit(std::unique_ptr<JitBase> jit)
{
  m_jit = std::move(jit);
}

void JitInterface::DoState(PointerWrap& p)
{
  // Nothing of the recompiler is saved; loading a state just drops the compiled code.
  if (p.IsReadMode() && m_jit)
    m_jit->ClearCache();
}

#ifndef _ARCH_32
bool JitInterface::WantsPageTableMappings() const
{
  return m_jit && m_jit->WantsPageTableMappings();
}
#endif

void JitInterface::UpdateMembase()
{
  if (!m_jit)
    return;

#ifdef _M_ARM_64
  // JitArm64 uses the non-arena paths whenever fastmem as a whole is off.
  const bool use_arena = m_jit->jo.fastmem;
#else
  const bool use_arena = m_jit->jo.fastmem_arena;
#endif
  auto& state = m_system.GetPPCState();
  auto& memory = m_system.GetMemory();
  if (state.msr.DR)
    state.mem_ptr = use_arena ? memory.GetLogicalBase() : memory.GetLogicalPageMappingsBase();
  else
    state.mem_ptr = use_arena ? memory.GetPhysicalBase() : memory.GetPhysicalPageMappingsBase();
}

void JitInterface::JitBlockLogDump(const Core::CPUThreadGuard& guard, std::FILE* file) const
{
  std::fputs(
      "ppcFeatureFlags\tppcAddress\tppcSize\thostNearSize\thostFarSize\trunCount\tcyclesSpent"
      "\tcyclesAverage\tcyclesPercent\ttimeSpent(ns)\ttimeAverage(ns)\ttimePercent\tsymbol\n",
      file);
  if (!m_jit)
    return;

  JitBaseBlockCache& blocks = *m_jit->GetBlockCache();
  const auto symbol_name = [this](const JitBlock& block) -> std::string_view {
    const Common::Symbol* symbol = m_jit->m_ppc_symbol_db.GetSymbolFromAddr(block.effectiveAddress);
    return symbol ? std::string_view{symbol->name} : std::string_view{};
  };
  // Columns shared by both variants.
  const auto block_columns = [](const JitBlock& block) {
    return fmt::format("{}\t{:08x}\t{}\t{}\t{}", FeatureFlagNames(block.feature_flags),
                       block.effectiveAddress, block.originalSize * sizeof(UGeckoInstruction),
                       block.near_end - block.near_begin, block.far_end - block.far_begin);
  };

  if (!m_jit->IsProfilingEnabled())
  {
    blocks.RunOnBlocks(guard, [&](const JitBlock& block) {
      fmt::println(file, "{}\t-\t-\t-\t-\t-\t-\t-\t\"{}\"", block_columns(block),
                   symbol_name(block));
    });
    return;
  }

  using Duration = JitBlock::ProfileData::Clock::duration;
  u64 total_cycles = 0;
  Duration total_time{};
  blocks.RunOnBlocks(guard, [&](const JitBlock& block) {
    total_cycles += block.profile_data->cycles_spent;
    total_time += block.profile_data->time_spent;
  });

  blocks.RunOnBlocks(guard, [&](const JitBlock& block) {
    const JitBlock::ProfileData& data = *block.profile_data;
    const double time_ns =
        std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(data.time_spent)
            .count();
    const double cycles_percent =
        total_cycles == 0 ? 0.0 : PercentOf(static_cast<double>(data.cycles_spent), total_cycles);
    const double time_percent =
        total_time == Duration{} ?
            0.0 :
            PercentOf(static_cast<double>(data.time_spent.count()), total_time.count());

    fmt::println(file, "{}\t{}\t{}\t{:.6f}\t{:.6f}\t{}\t{:.6f}\t{:.6f}\t\"{}\"", block_columns(block),
                 data.run_count, data.cycles_spent,
                 PerRun(static_cast<double>(data.cycles_spent), data.run_count), cycles_percent,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(data.time_spent).count(),
                 PerRun(time_ns, data.run_count), time_percent, symbol_name(block));
  });
}

// Fault handling: with no recompiler loaded, no fault is ours.
bool JitInterface::HandleStackFault()
{
  return m_jit && m_jit->HandleStackFault();
}

bool JitInterface::HandleFault(uintptr_t access_address, SContext* ctx)
{
  return m_jit && m_jit->HandleFault(access_address, ctx);
}

// Block cache access. Everything here is a no-op without a recompiler.
std::size_t JitInterface::GetBlockCount() const
{
  return m_jit ? m_jit->GetBlockCache()->GetBlockCount() : 0;
}

void JitInterface::RunOnBlocks(const Core::CPUThreadGuard& guard,
                               const std::function<void(const JitBlock&)>& f) const
{
  WithBlockCache(m_jit.get(), [&](JitBaseBlockCache& cache) { cache.RunOnBlocks(guard, f); });
}

void JitInterface::WipeBlockProfilingData(const Core::CPUThreadGuard& guard)
{
  WithBlockCache(m_jit.get(),
                 [&](JitBaseBlockCache& cache) { cache.WipeBlockProfilingData(guard); });
}

void JitInterface::ClearSafe()
{
  WithBlockCache(m_jit.get(), [](JitBaseBlockCache& cache) { cache.Clear(); });
}

void JitInterface::EraseSingleBlock(const JitBlock& block)
{
  if (JitBase* jit = m_jit.get())
    jit->EraseSingleBlock(block);
}

void JitInterface::ClearCache(const Core::CPUThreadGuard&)
{
  if (JitBase* jit = m_jit.get())
    jit->ClearCache();
}

std::vector<JitInterface::MemoryStats> JitInterface::GetMemoryStats() const
{
  if (!m_jit)
    return {};
  return m_jit->GetMemoryStats();
}

std::size_t JitInterface::DisassembleNearCode(const JitBlock& block, std::ostream& stream) const
{
  return m_jit ? m_jit->DisassembleNearCode(block, stream) : 0;
}

std::size_t JitInterface::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const
{
  return m_jit ? m_jit->DisassembleFarCode(block, stream) : 0;
}

void JitInterface::InvalidateICache(u32 address, u32 size, bool forced)
{
  if (m_jit)
    m_jit->GetBlockCache()->InvalidateICache(address, size, forced);
}

void JitInterface::InvalidateICacheLine(u32 address)
{
  if (m_jit)
    m_jit->GetBlockCache()->InvalidateICacheLine(address);
}

void JitInterface::InvalidateICacheLines(u32 address, u32 count)
{
  // Guest loops of "dcbX addr; addr += 32; --count" are folded into one
  // range invalidation. A count of 0 (the loop wrapping) or one covering the
  // whole 4 GiB space invalidates everything from the first line on.
  constexpr u32 LINE_SIZE = 32;
  constexpr u32 LINES_IN_ADDRESS_SPACE = static_cast<u32>(0x1'0000'0000 / LINE_SIZE);

  if (count == 1)
  {
    InvalidateICacheLine(address);
    return;
  }

  const u32 first_line = address & ~(LINE_SIZE - 1);
  if (count == 0 || count >= LINES_IN_ADDRESS_SPACE)
    InvalidateICache(first_line, 0xFFFFFFFF, false);
  else
    InvalidateICache(first_line, LINE_SIZE * count, false);
}

void JitInterface::InvalidateICacheLineFromJIT(JitInterface& jit_interface, u32 address)
{
  jit_interface.InvalidateICacheLine(address);
}

void JitInterface::InvalidateICacheLinesFromJIT(JitInterface& jit_interface, u32 address, u32 count)
{
  jit_interface.InvalidateICacheLines(address, count);
}

void JitInterface::CompileExceptionCheck(ExceptionType type)
{
  if (!m_jit)
    return;

  std::unordered_set<u32>* addresses = nullptr;
  switch (type)
  {
  case ExceptionType::FIFOWrite:
    addresses = &m_jit->js.fifoWriteAddresses;
    break;
  case ExceptionType::PairedQuantize:
    addresses = &m_jit->js.pairedQuantizeAddresses;
    break;
  case ExceptionType::SpeculativeConstants:
    addresses = &m_jit->js.noSpeculativeConstantsAddresses;
    break;
  }

  const u32 pc = m_system.GetPPCState().pc;
  if (pc == 0 || addresses->contains(pc))
    return;

  if (type == ExceptionType::FIFOWrite)
  {
    // Only stores can write to the FIFO; the code at pc may have changed
    // since it was compiled.
    ASSERT(Core::IsCPUThread());
    Core::CPUThreadGuard guard(m_system);
    const OpType op_type =
        PPCTables::GetOpInfo(PowerPC::MMU::HostRead<u32>(guard, pc), pc)->type;
    if (op_type != OpType::Store && op_type != OpType::StoreFP && op_type != OpType::StorePS)
      return;
  }

  addresses->insert(pc);
  // Recompile the block containing pc so it includes the check.
  m_jit->GetBlockCache()->InvalidateICache(pc, 4, true);
}

void JitInterface::CompileExceptionCheckFromJIT(JitInterface& jit_interface, ExceptionType type)
{
  jit_interface.CompileExceptionCheck(type);
}

void JitInterface::Shutdown()
{
  if (!m_jit)
    return;
  m_jit->Shutdown();
  m_jit.reset();
}
