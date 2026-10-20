// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Model of the 750CL's L1 caches: 32 KiB, 8-way set associative, 128 sets of
// 32-byte blocks, with the tree pseudo-LRU replacement the manual describes.
// The data cache is only modelled when "accurate CPU cache" is on; the
// instruction cache model decides what code the CPU actually sees after
// self-modifying writes.

#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

class JitInterface;
namespace Memory
{
class MemoryManager;
}
class PointerWrap;
namespace PowerPC
{
struct PowerPCState;
}

namespace PowerPC
{
constexpr u32 CACHE_SETS = 128;
constexpr u32 CACHE_WAYS = 8;
// Words per cache block.
constexpr u32 CACHE_BLOCK_SIZE = 8;

// Physical-address bits that select the Wii's MEM2 and the fake virtual
// memory region; each has its own block lookup table.
constexpr u32 CACHE_EXRAM_BIT = 0x10000000;
constexpr u32 CACHE_VMEM_BIT = 0x20000000;

struct Cache
{
  // Block contents, as raw (big-endian) memory words.
  std::array<std::array<std::array<u32, CACHE_BLOCK_SIZE>, CACHE_WAYS>, CACHE_SETS> data{};

  // The 32-byte aligned address each block holds. Hardware only keeps the
  // tag, but the set bits are the same for every block of a set, so keeping
  // the whole address changes nothing and makes write-back simpler.
  std::array<std::array<u32, CACHE_WAYS>, CACHE_SETS> addrs{};

  // Per set: the seven pseudo-LRU tree bits, and one valid and one modified
  // bit per way.
  std::array<u8, CACHE_SETS> plru{};
  std::array<u8, CACHE_SETS> valid{};
  std::array<u8, CACHE_SETS> modified{};

  // For every 32-byte block of MEM1, MEM2 and fake VMEM: the way that holds
  // it, or 0xFF. Derived from addrs/valid; only kept so lookups are fast.
  std::vector<u8> lookup_table{};
  std::vector<u8> lookup_table_ex{};
  std::vector<u8> lookup_table_vmem{};

  // dcbst: write the block back if it is modified, keep it cached.
  void Store(Memory::MemoryManager& memory, u32 addr);
  // dcbi: drop the block without writing it back.
  void Invalidate(Memory::MemoryManager& memory, u32 addr);
  // dcbf: write the block back if modified, then drop it.
  void Flush(Memory::MemoryManager& memory, u32 addr);
  // dcbt/dcbtst: bring the block in.
  void Touch(Memory::MemoryManager& memory, u32 addr, bool store);

  // Writes back every modified block and empties the cache.
  void FlushAll(Memory::MemoryManager& memory);

  // Finds the block for addr, loading it (and evicting another) unless the
  // cache is locked. Returns {set, way}; way is 0xFF if the block is not
  // cached.
  std::pair<u32, u32> GetCache(Memory::MemoryManager& memory, u32 addr, bool locked);

  void Read(Memory::MemoryManager& memory, u32 addr, void* buffer, u32 len, bool locked);
  void Write(Memory::MemoryManager& memory, u32 addr, const void* buffer, u32 len, bool locked);

  void Init(Memory::MemoryManager& memory);
  void Reset();

  void DoState(Memory::MemoryManager& memory, PointerWrap& p);
};

struct InstructionCache : public Cache
{
  std::optional<Config::ConfigChangedCallbackID> m_config_callback_id = std::nullopt;

  bool m_disable_icache = false;

  InstructionCache() = default;
  ~InstructionCache();
  u32 ReadInstruction(Memory::MemoryManager& memory, PowerPC::PowerPCState& ppc_state, u32 addr);
  // icbi: drops every way of the set addr maps to and tells the JIT.
  void Invalidate(Memory::MemoryManager& memory, JitInterface& jit_interface, u32 addr);
  void Init(Memory::MemoryManager& memory);
  void Reset(JitInterface& jit_interface);
  void RefreshConfig();
};
}  // namespace PowerPC
