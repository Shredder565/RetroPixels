// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/RangeSet.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCAnalyst.h"

class JitBase;

// The part of a block that generated code reads through offsetof, so it must
// stay standard-layout. feature_flags and effectiveAddress are adjacent on
// purpose: dispatchers compare both with one 64-bit load.
struct JitBlockData
{
  // Host code of the block in the near and far caches, as [begin, end).
  u8* near_begin;
  u8* near_end;
  u8* far_begin;
  u8* far_end;

  // Where Dispatch() enters the block.
  u8* normalEntry;

  // MSR-derived features the block was compiled for.
  CPUEmuFeatureFlags feature_flags;
  // Guest PC of the first instruction.
  u32 effectiveAddress;
  // Translated address of the first instruction; blocks are indexed by it,
  // which matches how the PowerPC instruction cache works.
  u32 physicalAddress;
  // Number of guest instructions compiled into the block.
  u32 originalSize;
  // Slot in the fast lookup table that refers to this block.
  size_t fast_block_map_index;
};
static_assert(std::is_standard_layout_v<JitBlockData>, "JitBlockData must have a standard layout");

// Compiled host code for the guest code starting at one address. A block is
// identified by its effective address, the translation-related MSR features
// and its physical address together.
struct JitBlock : public JitBlockData
{
  // Run statistics, kept only while profiling.
  struct ProfileData
  {
    using Clock = std::chrono::steady_clock;

    static void BeginProfiling(ProfileData* data);
    static void EndProfiling(ProfileData* data, u32 downcount_amount);

    std::size_t run_count = 0;
    u64 cycles_spent = 0;
    Clock::duration time_spent = {};

  private:
    Clock::time_point time_start;
  };

  explicit JitBlock(bool profiling_enabled)
      : profile_data(profiling_enabled ? std::make_unique<ProfileData>() : nullptr)
  {
  }

  bool OverlapsPhysicalRange(u32 address, u32 length) const;

  // An exit of the block to a fixed guest address, which can be patched to
  // jump straight into the target block.
  struct LinkData
  {
    u8* exitPtrs;  // the patchable exit sequence
#ifdef _M_ARM_64
    const u8* exitFarcode;
#endif
    u32 exitAddress;
    bool linkStatus;  // currently patched to a block
    bool call;        // the exit is a call (guest bl), not a jump
  };
  std::vector<LinkData> linkData;

  // Physical addresses of every guest instruction in the block.
  Common::RangeSet<u32> physical_addresses;

  // Debugging only: address and instruction of every analysed op (repeats
  // included), for showing what the block was compiled from.
  std::vector<std::pair<u32, UGeckoInstruction>> original_buffer;

  std::unique_ptr<ProfileData> profile_data;
};

typedef void (*CompiledCode)();

// One bit per 32-byte guest cache line of the 4 GiB physical space, set while
// some block covers the line. Generated code tests it directly (bit n is bit
// n % 32 of word n / 32).
class ValidBlockBitSet final
{
public:
  enum
  {
    VALID_BLOCK_MASK_SIZE = (1ULL << 32) / 32,
    VALID_BLOCK_ALLOC_ELEMENTS = VALID_BLOCK_MASK_SIZE / 32
  };

  ValidBlockBitSet();

  void Set(u32 bit) { m_words[bit >> 5] |= Bit(bit); }
  void Clear(u32 bit) { m_words[bit >> 5] &= ~Bit(bit); }
  void ClearAll();
  bool Test(u32 bit) const { return (m_words[bit >> 5] & Bit(bit)) != 0; }

  u32* Words() const { return m_words.get(); }

private:
  static constexpr u32 Bit(u32 bit) { return u32{1} << (bit & 31); }

  std::unique_ptr<u32[]> m_words;
};

// Bookkeeping for compiled blocks: lookup by address, linking, and
// invalidation when guest code changes. Backends supply the code patching.
class JitBaseBlockCache
{
public:
  // Size of the direct lookup table: one host pointer per 4-byte guest
  // address for each of the 8 feature-flag combinations.
  static constexpr u64 FAST_BLOCK_MAP_SIZE = 0x10'0000'0000;
  // Hashed table used when the direct table could not be reserved.
  static constexpr u32 FAST_BLOCK_MAP_FALLBACK_ELEMENTS = 0x10000;
  static constexpr u32 FAST_BLOCK_MAP_FALLBACK_MASK = FAST_BLOCK_MAP_FALLBACK_ELEMENTS - 1;

  explicit JitBaseBlockCache(JitBase& jit);
  virtual ~JitBaseBlockCache();

  virtual void Init();
  void Shutdown();
  void Clear();
  void Reset();

  // Direct table (null if not in use): entry points indexed by
  // (feature_flags << 30) | (pc >> 2).
  u8** GetEntryPoints();
  // Fallback table: blocks indexed by (pc >> 2) & FAST_BLOCK_MAP_FALLBACK_MASK.
  JitBlock** GetFastBlockMapFallback();

  void RunOnBlocks(const Core::CPUThreadGuard& guard,
                   const std::function<void(const JitBlock&)>& f) const;
  void WipeBlockProfilingData(const Core::CPUThreadGuard& guard);
  std::size_t GetBlockCount() const { return m_block_count; }

  JitBlock* AllocateBlock(u32 em_address);
  void FinalizeBlock(JitBlock& block, bool block_link, const PPCAnalyst::CodeBlock& code_block,
                     const PPCAnalyst::CodeBuffer& code_buffer);

  // Exact lookup, used when the fast tables miss. Null if no such block.
  JitBlock* GetBlockFromStartAddress(u32 em_address, CPUEmuFeatureFlags feature_flags);

  // Entry point for the current PC and features, or null when no block
  // exists yet (C++ version of what the assembly dispatchers do).
  const u8* Dispatch();

  void InvalidateICache(u32 address, u32 length, bool forced);
  void InvalidateICacheLine(u32 address);
  void ErasePhysicalRange(u32 address, u32 length);
  void EraseSingleBlock(const JitBlock& block);

  u32* GetBlockBitSet() const;

protected:
  virtual void DestroyBlock(JitBlock& block);

  JitBase& m_jit;

private:
  virtual void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) = 0;
  virtual void WriteDestroyBlock(const JitBlock& block);

  // Granularity of the physical-range index used for invalidation.
  static constexpr u32 RANGE_BUCKET_SHIFT = 9;

  size_t LookupSlot(u32 pc, u32 feature_flags) const;
  void PublishBlock(JitBlock& block, size_t slot);
  void RetractBlock(const JitBlock& block);
  JitBlock* PromoteBlock(u32 pc, CPUEmuFeatureFlags feature_flags);

  void ResolveExits(JitBlock& block);
  void LinkNewBlock(JitBlock& block);
  void UnlinkBlock(const JitBlock& block);
  void InvalidatePhysical(u32 physical_address, u32 address, u32 length, bool forced);

  void IndexRanges(JitBlock& block);
  void UnindexRanges(JitBlock& block, u32 keep_bucket);
  bool RemoveFromBlockMap(const JitBlock& block);

  // Blocks by physical start address, in creation order for equal addresses.
  std::map<u32, std::list<JitBlock>> m_blocks;
  std::size_t m_block_count = 0;

  // For every exit address, the blocks having an exit to it.
  std::unordered_map<u32, std::unordered_set<JitBlock*>> m_incoming;

  // Blocks touching each bucket of physical addresses.
  std::map<u32, std::unordered_set<JitBlock*>> m_range_index;

  ValidBlockBitSet m_valid_lines;

  Common::LazyMemoryRegion m_entry_points_arena;
  u8** m_entry_points_ptr = nullptr;
  std::array<JitBlock*, FAST_BLOCK_MAP_FALLBACK_ELEMENTS> m_fast_block_map_fallback{};
};
