// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/JitCommon/JitCache.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/JitRegister.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"

namespace
{
constexpr u32 LINE_SIZE = 32;

// Calls f(bucket) for every bucket of 2^shift bytes touched by [begin, end).
template <typename F>
void ForEachBucket(u32 begin, u32 end, u32 shift, F&& f)
{
  const u64 last = u64{end};
  for (u64 bucket = u64{begin} >> shift << shift; bucket < last; bucket += u64{1} << shift)
    f(static_cast<u32>(bucket));
}
}  // namespace

bool JitBlock::OverlapsPhysicalRange(u32 address, u32 length) const
{
  return physical_addresses.overlaps(address, address + length);
}

void JitBlock::ProfileData::BeginProfiling(ProfileData* data)
{
  ++data->run_count;
  data->time_start = Clock::now();
}

void JitBlock::ProfileData::EndProfiling(ProfileData* data, u32 downcount_amount)
{
  data->time_spent += Clock::now() - data->time_start;
  data->cycles_spent += downcount_amount;
}

ValidBlockBitSet::ValidBlockBitSet() : m_words(new u32[VALID_BLOCK_ALLOC_ELEMENTS])
{
  ClearAll();
}

void ValidBlockBitSet::ClearAll()
{
  std::memset(m_words.get(), 0, sizeof(u32) * VALID_BLOCK_ALLOC_ELEMENTS);
}

JitBaseBlockCache::JitBaseBlockCache(JitBase& jit) : m_jit{jit}
{
}

JitBaseBlockCache::~JitBaseBlockCache() = default;

void JitBaseBlockCache::Init()
{
  Common::JitRegister::Init(Config::Get(Config::MAIN_PERF_MAP_DIR));

  m_entry_points_ptr = nullptr;
#ifdef _ARCH_64
  // The direct table only reserves address space; pages appear on first write.
  if (Config::Get(Config::MAIN_LARGE_ENTRY_POINTS_MAP))
    m_entry_points_ptr = static_cast<u8**>(m_entry_points_arena.Create(FAST_BLOCK_MAP_SIZE));
#endif

  Clear();
}

void JitBaseBlockCache::Shutdown()
{
  Common::JitRegister::Shutdown();
  m_entry_points_arena.Release();
}

// Throws every block away (cache full, savestates, settings changes).
void JitBaseBlockCache::Clear()
{
#if defined(_DEBUG) || defined(DEBUGFAST)
  Core::DisplayMessage("Clearing code cache.", 3000);
#endif
  auto& js = m_jit.js;
  js.fifoWriteAddresses.clear();
  js.pairedQuantizeAddresses.clear();
  js.noSpeculativeConstantsAddresses.clear();

  for (auto& [address, list] : m_blocks)
  {
    for (JitBlock& block : list)
      DestroyBlock(block);
  }
  m_blocks.clear();
  m_block_count = 0;
  m_incoming.clear();
  m_range_index.clear();
  m_valid_lines.ClearAll();

  if (m_entry_points_ptr != nullptr)
    m_entry_points_arena.Clear();
}

void JitBaseBlockCache::RunOnBlocks(const Core::CPUThreadGuard&,
                                    const std::function<void(const JitBlock&)>& f) const
{
  for (const auto& [address, list] : m_blocks)
  {
    for (const JitBlock& block : list)
      f(block);
  }
}

void JitBaseBlockCache::WipeBlockProfilingData(const Core::CPUThreadGuard&)
{
  for (auto& [address, list] : m_blocks)
  {
    for (JitBlock& block : list)
    {
      if (block.profile_data)
        *block.profile_data = JitBlock::ProfileData{};
    }
  }
  Host_JitProfileDataWiped();
}

size_t JitBaseBlockCache::LookupSlot(u32 pc, u32 feature_flags) const
{
  if (m_entry_points_ptr == nullptr)
    return (pc >> 2) & FAST_BLOCK_MAP_FALLBACK_MASK;
  return (size_t{feature_flags} << 30) | (pc >> 2);
}

// Makes `block` what the fast tables return for its slot.
void JitBaseBlockCache::PublishBlock(JitBlock& block, size_t slot)
{
  if (m_entry_points_ptr != nullptr)
  {
    m_entry_points_arena.EnsureMemoryPageWritable(slot * sizeof(u8*));
    m_entry_points_ptr[slot] = block.normalEntry;
  }
  else
  {
    m_fast_block_map_fallback[slot] = &block;
  }
  block.fast_block_map_index = slot;
}

// Removes `block` from the fast tables if its slot still refers to it.
void JitBaseBlockCache::RetractBlock(const JitBlock& block)
{
  const size_t slot = block.fast_block_map_index;
  if (m_entry_points_ptr != nullptr)
  {
    if (m_entry_points_ptr[slot] == block.normalEntry)
      m_entry_points_ptr[slot] = nullptr;
  }
  else if (m_fast_block_map_fallback[slot] == &block)
  {
    m_fast_block_map_fallback[slot] = nullptr;
  }
}

JitBlock* JitBaseBlockCache::AllocateBlock(u32 em_address)
{
  const u32 physical = m_jit.m_mmu.JitCache_TranslateAddress(em_address).address;
  JitBlock& block = m_blocks[physical].emplace_back(m_jit.IsProfilingEnabled());
  ++m_block_count;
  block.effectiveAddress = em_address;
  block.physicalAddress = physical;
  block.feature_flags = m_jit.m_ppc_state.feature_flags;
  block.fast_block_map_index = 0;
  block.linkData.clear();
  return &block;
}

void JitBaseBlockCache::IndexRanges(JitBlock& block)
{
  for (const auto& [begin, end] : block.physical_addresses)
  {
    ForEachBucket(begin, end, 5, [this](u32 line) { m_valid_lines.Set(line / LINE_SIZE); });
    ForEachBucket(begin, end, RANGE_BUCKET_SHIFT,
                  [this, &block](u32 bucket) { m_range_index[bucket].insert(&block); });
  }
}

// Drops `block` from every range bucket except `keep_bucket` (pass a value
// that is not a bucket start to drop it from all of them).
void JitBaseBlockCache::UnindexRanges(JitBlock& block, u32 keep_bucket)
{
  for (const auto& [begin, end] : block.physical_addresses)
  {
    DEBUG_ASSERT(begin != end);
    ForEachBucket(begin, end, RANGE_BUCKET_SHIFT, [this, &block, keep_bucket](u32 bucket) {
      if (bucket != keep_bucket)
        m_range_index[bucket].erase(&block);
    });
  }
}

void JitBaseBlockCache::FinalizeBlock(JitBlock& block, bool block_link,
                                      const PPCAnalyst::CodeBlock& code_block,
                                      const PPCAnalyst::CodeBuffer& code_buffer)
{
  PublishBlock(block, LookupSlot(block.effectiveAddress, block.feature_flags));

  block.physical_addresses = code_block.m_physical_addresses;
  block.originalSize = code_block.m_num_instructions;

  if (m_jit.IsDebuggingEnabled())
  {
    block.original_buffer.clear();
    block.original_buffer.reserve(block.originalSize);
    for (u32 i = 0; i < block.originalSize; ++i)
      block.original_buffer.emplace_back(code_buffer[i].address, code_buffer[i].inst);
  }

  IndexRanges(block);

  if (block_link)
  {
    for (const JitBlock::LinkData& exit : block.linkData)
      m_incoming[exit.exitAddress].insert(&block);
    LinkNewBlock(block);
  }

  // Name the code for host profilers.
  const u8* code = block.normalEntry;
  const u32 code_size = static_cast<u32>(block.near_end - block.normalEntry);
  const Common::Symbol* symbol =
      Common::JitRegister::IsEnabled() ?
          m_jit.m_ppc_symbol_db.GetSymbolFromAddr(block.effectiveAddress) :
          nullptr;
  if (symbol != nullptr)
  {
    Common::JitRegister::Register(code, code_size, "JIT_PPC_{}_{:08x}", symbol->function_name,
                                  block.physicalAddress);
  }
  else
  {
    Common::JitRegister::Register(code, code_size, "JIT_PPC_{:08x}", block.physicalAddress);
  }
}

JitBlock* JitBaseBlockCache::GetBlockFromStartAddress(u32 addr, CPUEmuFeatureFlags feature_flags)
{
  u32 physical = addr;
  if ((feature_flags & FEATURE_FLAG_MSR_IR) != 0)
  {
    const auto translated = m_jit.m_mmu.JitCache_TranslateAddress(addr);
    if (!translated.valid)
      return nullptr;
    physical = translated.address;
  }

  const auto it = m_blocks.find(physical);
  if (it == m_blocks.end())
    return nullptr;
  for (JitBlock& block : it->second)
  {
    if (block.effectiveAddress == addr && block.feature_flags == feature_flags)
      return &block;
  }
  return nullptr;
}

// Re-enters an existing block into its fast-table slot after a miss.
JitBlock* JitBaseBlockCache::PromoteBlock(u32 pc, CPUEmuFeatureFlags feature_flags)
{
  JitBlock* block = GetBlockFromStartAddress(pc, feature_flags);
  if (block == nullptr)
    return nullptr;

  RetractBlock(*block);
  PublishBlock(*block, LookupSlot(pc, feature_flags));
  return block;
}

const u8* JitBaseBlockCache::Dispatch()
{
  const u32 pc = m_jit.m_ppc_state.pc;
  const CPUEmuFeatureFlags flags = m_jit.m_ppc_state.feature_flags;
  const size_t slot = LookupSlot(pc, flags);

  JitBlock* block;
  if (m_entry_points_ptr != nullptr)
  {
    if (u8* entry = m_entry_points_ptr[slot])
      return entry;
    block = PromoteBlock(pc, flags);
  }
  else
  {
    block = m_fast_block_map_fallback[slot];
    if (block == nullptr || block->effectiveAddress != pc || block->feature_flags != flags)
      block = PromoteBlock(pc, flags);
  }
  return block != nullptr ? block->normalEntry : nullptr;
}

void JitBaseBlockCache::InvalidateICacheLine(u32 address)
{
  const u32 line = address & ~(LINE_SIZE - 1);
  const auto translated = m_jit.m_mmu.JitCache_TranslateAddress(line);
  if (translated.valid)
    InvalidatePhysical(translated.address, line, LINE_SIZE, false);
}

void JitBaseBlockCache::InvalidateICache(u32 initial_address, u32 initial_length, bool forced)
{
  // Translation may differ from one page to the next, so handle the range one
  // translation unit (BAT block or hardware page) at a time.
  u32 address = initial_address;
  u32 remaining = initial_length;
  while (remaining != 0)
  {
    const auto translated = m_jit.m_mmu.JitCache_TranslateAddress(address);
    const bool bat = translated.valid && translated.translated && translated.from_bat;
    const u32 unit_shift = bat ? PowerPC::BAT_INDEX_SHIFT : PowerPC::HW_PAGE_INDEX_SHIFT;
    const u32 unit_mask = ~((u32{1} << unit_shift) - 1);

    const u32 last = address + (remaining - 1);
    const bool single_unit = (address & unit_mask) == (last & unit_mask);
    const u32 chunk =
        single_unit ? remaining : ((address + (u32{1} << unit_shift)) & unit_mask) - address;

    if (translated.valid)
      InvalidatePhysical(translated.address, address, chunk, forced);
    if (single_unit)
      return;
    address += chunk;
    remaining -= chunk;
  }
}

void JitBaseBlockCache::InvalidatePhysical(u32 physical_address, u32 address, u32 length,
                                           bool forced)
{
  // A single aligned cache line (the dcb* case) can be checked against the
  // valid-line bits first; nothing needs doing when no block covers it.
  if (length == LINE_SIZE && (physical_address & (LINE_SIZE - 1)) == 0)
  {
    const u32 line = physical_address / LINE_SIZE;
    if (!m_valid_lines.Test(line))
      return;
    m_valid_lines.Clear(line);
  }
  else if (length > LINE_SIZE)
  {
    // Lines lying wholly inside the range will be free of blocks afterwards.
    const u32 first = (physical_address + (LINE_SIZE - 1)) / LINE_SIZE;
    const u32 end = (physical_address + length) / LINE_SIZE;
    for (u32 line = first; line < end; ++line)
      m_valid_lines.Clear(line);
  }

  ErasePhysicalRange(physical_address, length);

  // Real code changes also forget what was learned about these instructions
  // (FIFO writes, quantizer use, speculation failures); keeping it would put
  // checks where they no longer belong.
  if (forced)
    return;
  auto& js = m_jit.js;
  for (u32 pc = address; pc < address + length; pc += 4)
  {
    js.fifoWriteAddresses.erase(pc);
    js.pairedQuantizeAddresses.erase(pc);
    js.noSpeculativeConstantsAddresses.erase(pc);
  }
}

bool JitBaseBlockCache::RemoveFromBlockMap(const JitBlock& block)
{
  const auto it = m_blocks.find(block.physicalAddress);
  if (it == m_blocks.end())
    return false;
  auto& list = it->second;
  for (auto entry = list.begin(); entry != list.end(); ++entry)
  {
    if (&*entry == &block)
    {
      list.erase(entry);
      --m_block_count;
      if (list.empty())
        m_blocks.erase(it);
      return true;
    }
  }
  return false;
}

void JitBaseBlockCache::ErasePhysicalRange(u32 address, u32 length)
{
  const u32 first_bucket = address >> RANGE_BUCKET_SHIFT << RANGE_BUCKET_SHIFT;
  const u64 range_end = u64{address} + length;

  auto bucket = m_range_index.lower_bound(first_bucket);
  while (bucket != m_range_index.end() && u64{bucket->first} < range_end)
  {
    auto& members = bucket->second;
    for (auto it = members.begin(); it != members.end();)
    {
      JitBlock* block = *it;
      if (!block->OverlapsPhysicalRange(address, length))
      {
        ++it;
        continue;
      }
      // Other buckets may now become empty; they are left for later reuse.
      UnindexRanges(*block, bucket->first);
      DestroyBlock(*block);
      RemoveFromBlockMap(*block);
      it = members.erase(it);
    }

    if (members.empty())
      bucket = m_range_index.erase(bucket);
    else
      ++bucket;
  }
}

void JitBaseBlockCache::EraseSingleBlock(const JitBlock& block)
{
  const auto it = m_blocks.find(block.physicalAddress);
  if (it == m_blocks.end())
    return;
  const auto entry = std::ranges::find_if(
      it->second, [&block](const JitBlock& candidate) { return &candidate == &block; });
  if (entry == it->second.end()) [[unlikely]]
    return;

  JitBlock& target = *entry;
  for (const auto& [begin, end] : target.physical_addresses)
  {
    ForEachBucket(begin, end, RANGE_BUCKET_SHIFT,
                  [this, &target](u32 b) { m_range_index[b].erase(&target); });
  }
  DestroyBlock(target);
  RemoveFromBlockMap(target);  // `block` dangles from here on
}

u32* JitBaseBlockCache::GetBlockBitSet() const
{
  return m_valid_lines.Words();
}

void JitBaseBlockCache::WriteDestroyBlock(const JitBlock&)
{
}

// Patches every unlinked exit of `block` whose target already exists.
void JitBaseBlockCache::ResolveExits(JitBlock& block)
{
  for (JitBlock::LinkData& exit : block.linkData)
  {
    if (exit.linkStatus)
      continue;
    if (const JitBlock* target = GetBlockFromStartAddress(exit.exitAddress, block.feature_flags))
    {
      WriteLinkBlock(exit, target);
      exit.linkStatus = true;
    }
  }
}

// Links a new block's exits, and the exits of other blocks that lead to it.
void JitBaseBlockCache::LinkNewBlock(JitBlock& block)
{
  ResolveExits(block);
  const auto sources = m_incoming.find(block.effectiveAddress);
  if (sources == m_incoming.end())
    return;
  for (JitBlock* source : sources->second)
  {
    if (source->feature_flags == block.feature_flags)
      ResolveExits(*source);
  }
}

void JitBaseBlockCache::UnlinkBlock(const JitBlock& block)
{
  for (const JitBlock::LinkData& exit : block.linkData)
    WriteLinkBlock(exit, nullptr);

  const auto sources = m_incoming.find(block.effectiveAddress);
  if (sources == m_incoming.end())
    return;
  for (JitBlock* source : sources->second)
  {
    if (source->feature_flags != block.feature_flags)
      continue;
    for (JitBlock::LinkData& exit : source->linkData)
    {
      if (exit.exitAddress != block.effectiveAddress)
        continue;
      WriteLinkBlock(exit, nullptr);
      exit.linkStatus = false;
    }
  }
}

void JitBaseBlockCache::DestroyBlock(JitBlock& block)
{
  RetractBlock(block);
  UnlinkBlock(block);

  for (const JitBlock::LinkData& exit : block.linkData)
  {
    const auto sources = m_incoming.find(exit.exitAddress);
    if (sources == m_incoming.end())
      continue;
    sources->second.erase(&block);
    if (sources->second.empty())
      m_incoming.erase(sources);
  }

  // Lets the backend make any remaining jump into the block trap.
  WriteDestroyBlock(block);
}

// Accessors for the dispatcher's lookup tables.
JitBlock** JitBaseBlockCache::GetFastBlockMapFallback()
{
  return m_fast_block_map_fallback.data();
}

u8** JitBaseBlockCache::GetEntryPoints()
{
  return m_entry_points_ptr;
}

void JitBaseBlockCache::Reset()
{
  Shutdown();
  Init();
}
