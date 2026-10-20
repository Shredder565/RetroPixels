// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/PPCCache.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace PowerPC
{
namespace
{
constexpr u32 BLOCK_BYTES = CACHE_BLOCK_SIZE * sizeof(u32);
constexpr u32 NO_WAY = 0xFF;

// The pseudo-LRU state of a set is a three-level binary tree over the eight
// ways, stored in seven bits:
//
//   level 0:  bit 0 chooses ways 0-3 (0) or 4-7 (1)
//   level 1:  bit 1 chooses 0-1 / 2-3,  bit 2 chooses 4-5 / 6-7
//   level 2:  bit 3 chooses 0 / 1, bit 4 chooses 2 / 3,
//             bit 5 chooses 4 / 5, bit 6 chooses 6 / 7
//
// A zero bit points to the left subtree, a one bit to the right. The victim
// is found by following the bits from the root. Using a way flips the three
// bits on its path to point away from it.
struct TreePath
{
  u32 bits[3];   // tree bit at each level
  u32 sides[3];  // 0 = the way is to the left at that level, 1 = right
};

constexpr TreePath PathTo(u32 way)
{
  const u32 half = way >> 2;
  const u32 quarter = (way >> 1) & 1;
  const u32 leaf = way & 1;
  return {{0, 1 + half, 3 + (way >> 1)}, {half, quarter, leaf}};
}

struct PlruUpdate
{
  u8 mask;
  u8 value;
};

// What touching each way does to the tree bits.
constexpr std::array<PlruUpdate, CACHE_WAYS> PLRU_UPDATE = [] {
  std::array<PlruUpdate, CACHE_WAYS> table{};
  for (u32 way = 0; way < CACHE_WAYS; ++way)
  {
    const TreePath path = PathTo(way);
    u32 mask = 0;
    u32 value = 0;
    for (u32 level = 0; level < 3; ++level)
    {
      mask |= 1u << path.bits[level];
      if (path.sides[level] == 0)
        value |= 1u << path.bits[level];
    }
    table[way] = {static_cast<u8>(mask), static_cast<u8>(value)};
  }
  return table;
}();

// The way the tree bits point at, for every tree state.
constexpr std::array<u8, 128> PLRU_VICTIM = [] {
  std::array<u8, 128> table{};
  for (u32 state = 0; state < table.size(); ++state)
  {
    u32 node = 0;
    u32 way = 0;
    for (u32 level = 0; level < 3; ++level)
    {
      const u32 right = (state >> node) & 1;
      way = (way << 1) | right;
      // Children of node n in this numbering: level 1 nodes are 1 and 2,
      // level 2 nodes are 3..6.
      node = level == 0 ? 1 + right : 3 + way;
    }
    table[state] = static_cast<u8>(way);
  }
  return table;
}();

static_assert(PLRU_UPDATE[0].mask == 0b0001011 && PLRU_UPDATE[0].value == 0b0001011);
static_assert(PLRU_UPDATE[7].mask == 0b1000101 && PLRU_UPDATE[7].value == 0);
static_assert(PLRU_VICTIM[0] == 0 && PLRU_VICTIM[127] == 7);

constexpr u32 WayBit(u32 way)
{
  return 1u << way;
}

// The lookup-table slot for the block containing a physical address.
u8& LookupSlot(Cache& cache, Memory::MemoryManager& memory, u32 address)
{
  if ((address & CACHE_VMEM_BIT) != 0)
    return cache.lookup_table_vmem[(address & memory.GetFakeVMemMask()) / BLOCK_BYTES];
  if ((address & CACHE_EXRAM_BIT) != 0)
    return cache.lookup_table_ex[(address & memory.GetExRamMask()) / BLOCK_BYTES];
  return cache.lookup_table[(address & memory.GetRamMask()) / BLOCK_BYTES];
}

void ForgetBlock(Cache& cache, Memory::MemoryManager& memory, u32 set, u32 way)
{
  LookupSlot(cache, memory, cache.addrs[set][way]) = NO_WAY;
}

bool IsValid(const Cache& cache, u32 set, u32 way)
{
  return (cache.valid[set] & WayBit(way)) != 0;
}

bool IsModified(const Cache& cache, u32 set, u32 way)
{
  return (cache.modified[set] & WayBit(way)) != 0;
}

void WriteBack(Cache& cache, Memory::MemoryManager& memory, u32 set, u32 way, u32 address)
{
  memory.CopyToEmu(address, cache.data[set][way].data(), BLOCK_BYTES);
}

void Drop(Cache& cache, u32 set, u32 way)
{
  cache.valid[set] &= ~WayBit(way);
  cache.modified[set] &= ~WayBit(way);
}

u32 SetOf(u32 address)
{
  return (address / BLOCK_BYTES) % CACHE_SETS;
}
}  // namespace

InstructionCache::~InstructionCache()
{
  if (m_config_callback_id)
    Config::RemoveConfigChangedCallback(*m_config_callback_id);
}

void Cache::Reset()
{
  valid.fill(0);
  plru.fill(0);
  modified.fill(0);
  std::ranges::fill(lookup_table, NO_WAY);
  std::ranges::fill(lookup_table_ex, NO_WAY);
  std::ranges::fill(lookup_table_vmem, NO_WAY);
}

void InstructionCache::Reset(JitInterface& jit_interface)
{
  Cache::Reset();
  jit_interface.ClearSafe();
}

void Cache::Init(Memory::MemoryManager& memory)
{
  for (auto& set : data)
    for (auto& block : set)
      block.fill(0);
  for (auto& set : addrs)
    set.fill(0);
  lookup_table.resize(memory.GetRamSize() / BLOCK_BYTES);
  lookup_table_ex.resize(memory.GetExRamSize() / BLOCK_BYTES);
  lookup_table_vmem.resize(memory.GetFakeVMemSize() / BLOCK_BYTES);
  Reset();
}

void InstructionCache::Init(Memory::MemoryManager& memory)
{
  if (!m_config_callback_id)
    m_config_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();

  Cache::Init(memory);
}

std::pair<u32, u32> Cache::GetCache(Memory::MemoryManager& memory, u32 addr, bool locked)
{
  const u32 block_address = addr & ~(BLOCK_BYTES - 1);
  const u32 set = SetOf(block_address);
  u8& slot = LookupSlot(*this, memory, block_address);
  u32 way = slot;

  if (way == NO_WAY && !locked)
  {
    // Fill an empty way if there is one, otherwise replace the pseudo-LRU
    // victim (writing it back first if it was modified).
    if (valid[set] != 0xFF)
      way = static_cast<u32>(std::countr_one(valid[set]));
    else
      way = PLRU_VICTIM[plru[set]];

    if (IsValid(*this, set, way))
    {
      if (IsModified(*this, set, way))
        WriteBack(*this, memory, set, way, addrs[set][way]);
      ForgetBlock(*this, memory, set, way);
    }

    memory.CopyFromEmu(data[set][way].data(), block_address, BLOCK_BYTES);
    slot = static_cast<u8>(way);
    addrs[set][way] = block_address;
    valid[set] |= WayBit(way);
    modified[set] &= ~WayBit(way);
  }

  if (way != NO_WAY)
    plru[set] = (plru[set] & ~PLRU_UPDATE[way].mask) | PLRU_UPDATE[way].value;

  return {set, way};
}

void Cache::Store(Memory::MemoryManager& memory, u32 addr)
{
  const auto [set, way] = GetCache(memory, addr, true);
  if (way == NO_WAY)
    return;

  if (IsValid(*this, set, way) && IsModified(*this, set, way))
    WriteBack(*this, memory, set, way, addr & ~(BLOCK_BYTES - 1));
  modified[set] &= ~WayBit(way);
}

void Cache::FlushAll(Memory::MemoryManager& memory)
{
  for (u32 set = 0; set < CACHE_SETS; ++set)
  {
    for (u32 way = 0; way < CACHE_WAYS; ++way)
    {
      if (IsValid(*this, set, way) && IsModified(*this, set, way))
        WriteBack(*this, memory, set, way, addrs[set][way]);
    }
  }
  Reset();
}

void Cache::Invalidate(Memory::MemoryManager& memory, u32 addr)
{
  const auto [set, way] = GetCache(memory, addr, true);
  if (way == NO_WAY || !IsValid(*this, set, way))
    return;

  ForgetBlock(*this, memory, set, way);
  Drop(*this, set, way);
}

void Cache::Flush(Memory::MemoryManager& memory, u32 addr)
{
  const auto [set, way] = GetCache(memory, addr, true);
  if (way == NO_WAY || !IsValid(*this, set, way))
    return;

  if (IsModified(*this, set, way))
    WriteBack(*this, memory, set, way, addr & ~(BLOCK_BYTES - 1));
  ForgetBlock(*this, memory, set, way);
  Drop(*this, set, way);
}

void Cache::Touch(Memory::MemoryManager& memory, u32 addr, bool store)
{
  // Loads and stores bring a block in the same way.
  GetCache(memory, addr, false);
}

void Cache::Read(Memory::MemoryManager& memory, u32 addr, void* buffer, u32 len, bool locked)
{
  u8* out = static_cast<u8*>(buffer);
  while (len != 0)
  {
    const u32 offset = addr & (BLOCK_BYTES - 1);
    const u32 chunk = std::min(len, BLOCK_BYTES - offset);
    const auto [set, way] = GetCache(memory, addr, locked);

    if (way == NO_WAY)
      memory.CopyFromEmu(out, addr, chunk);
    else
      std::memcpy(out, reinterpret_cast<const u8*>(data[set][way].data()) + offset, chunk);

    addr += chunk;
    out += chunk;
    len -= chunk;
  }
}

void Cache::Write(Memory::MemoryManager& memory, u32 addr, const void* buffer, u32 len,
                  bool locked)
{
  const u8* in = static_cast<const u8*>(buffer);
  while (len != 0)
  {
    const u32 offset = addr & (BLOCK_BYTES - 1);
    const u32 chunk = std::min(len, BLOCK_BYTES - offset);
    const auto [set, way] = GetCache(memory, addr, locked);

    if (way == NO_WAY)
    {
      memory.CopyToEmu(addr, in, chunk);
    }
    else
    {
      std::memcpy(reinterpret_cast<u8*>(data[set][way].data()) + offset, in, chunk);
      modified[set] |= WayBit(way);
    }

    addr += chunk;
    in += chunk;
    len -= chunk;
  }
}

void Cache::DoState(Memory::MemoryManager& memory, PointerWrap& p)
{
  // The lookup tables are not saved. When loading, only the slots of blocks
  // that are valid now or after the load are touched, rather than refilling
  // all of the (several MiB of) tables.
  const auto for_each_valid_block = [this](auto&& fn) {
    for (u32 set = 0; set < CACHE_SETS; ++set)
      for (u32 way = 0; way < CACHE_WAYS; ++way)
        if (IsValid(*this, set, way))
          fn(set, way);
  };

  if (p.IsReadMode())
    for_each_valid_block([&](u32 set, u32 way) { ForgetBlock(*this, memory, set, way); });

  p.DoArray(data);
  p.DoArray(plru);
  p.DoArray(valid);
  p.DoArray(addrs);
  p.DoArray(modified);

  if (p.IsReadMode())
  {
    for_each_valid_block([&](u32 set, u32 way) {
      LookupSlot(*this, memory, addrs[set][way]) = static_cast<u8>(way);
    });
  }
}

u32 InstructionCache::ReadInstruction(Memory::MemoryManager& memory,
                                      PowerPC::PowerPCState& ppc_state, u32 addr)
{
  if (m_disable_icache || !HID0(ppc_state).ICE)
    return memory.Read_U32(addr);

  u32 word;
  Read(memory, addr, &word, sizeof(word), HID0(ppc_state).ILOCK);
  return Common::swap32(word);
}

void InstructionCache::Invalidate(Memory::MemoryManager& memory, JitInterface& jit_interface,
                                  u32 addr)
{
  // The 750CL manual (sections 3.4.1.5 and 3.4.2.6) says icbi invalidates
  // whether or not the cache is enabled or locked, and that it clears every
  // way of the addressed set, not just the matching block.
  const u32 set = SetOf(addr);
  for (u32 way = 0; way < CACHE_WAYS; ++way)
  {
    if (IsValid(*this, set, way))
      ForgetBlock(*this, memory, set, way);
  }
  valid[set] = 0;
  modified[set] = 0;

  jit_interface.InvalidateICacheLine(addr);
}

void InstructionCache::RefreshConfig()
{
  m_disable_icache = Config::Get(Config::MAIN_DISABLE_ICACHE);
}
}  // namespace PowerPC
