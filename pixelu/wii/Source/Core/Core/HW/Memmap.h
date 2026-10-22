// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/MemArena.h"
#include "Common/Swap.h"
#include "Core/PowerPC/MMU.h"

class PointerWrap;
namespace Core
{
class System;
}
namespace MMIO
{
class Mapping;
}

namespace Memory
{
constexpr u32 MEM1_BASE_ADDR = 0x80000000U;
constexpr u32 MEM2_BASE_ADDR = 0x90000000U;
constexpr u32 MEM1_SIZE_RETAIL = 0x01800000U;
constexpr u32 MEM1_SIZE_GDEV = 0x04000000U;
constexpr u32 MEM1_SIZE_MAX = 0x04000000U;
constexpr u32 MEM2_SIZE_RETAIL = 0x04000000U;
constexpr u32 MEM2_SIZE_NDEV = 0x08000000U;
constexpr u32 MEM2_SIZE_MAX = 0x08000000U;

// One block of emulated memory backed by the shared memory segment.
struct PhysicalMemoryRegion
{
  u8** out_pointer;
  u32 physical_address;
  u32 size;
  enum : u32
  {
    ALWAYS = 0,
    FAKE_VMEM = 1,
    WII_ONLY = 2,
  } flags;
  u32 shm_position;
  bool active;
};

// A view mapped into the logical half of the fastmem arena.
struct LogicalMemoryView
{
  void* mapped_pointer;
  u32 mapped_size;
};

// Owns emulated RAM and the fastmem arena.
//
// Backing memory: MEM1 (24 MiB retail, allocated as the next power of two),
// MEM2 (Wii only), the locked-L1 scratch area at 0xE0000000 and, on GameCube
// without MMU emulation, a 32 MiB "fake VMEM" at 0x7E000000 that stands in
// for ARAM paging done by some games' libraries.
//
// Fastmem: two 4 GiB windows, each with 2 GiB of guard space on both sides
// (JIT address arithmetic can overshoot by up to 2 GiB). The physical window
// mirrors the console's physical map; the logical window follows the DBATs
// and, with MMU emulation, the page table. Unmapped addresses fault and the
// JIT falls back to the slow path.
//
// Without fastmem the JITs use the page-mapping tables instead: one host
// pointer per 128 KiB BAT page, or null.
class MemoryManager
{
public:
  explicit MemoryManager(Core::System& system);
  MemoryManager(const MemoryManager& other) = delete;
  MemoryManager(MemoryManager&& other) = delete;
  MemoryManager& operator=(const MemoryManager& other) = delete;
  MemoryManager& operator=(MemoryManager&& other) = delete;
  ~MemoryManager();

  u32 GetRamSizeReal() const { return m_ram_size_real; }
  u32 GetRamSize() const { return m_ram_size; }
  u32 GetRamMask() const { return m_ram_mask; }
  u32 GetFakeVMemSize() const { return m_fakevmem_size; }
  u32 GetFakeVMemMask() const { return m_fakevmem_mask; }
  u32 GetL1CacheSize() const { return m_l1_cache_size; }
  u32 GetL1CacheMask() const { return m_l1_cache_mask; }
  u32 GetExRamSizeReal() const { return m_exram_size_real; }
  u32 GetExRamSize() const { return m_exram_size; }
  u32 GetExRamMask() const { return m_exram_mask; }

  bool IsAddressInFastmemArea(const u8* address) const;
  u8* GetPhysicalBase() const { return m_physical_base; }
  u8* GetLogicalBase() const { return m_logical_base; }
  u8* GetPhysicalPageMappingsBase() const { return m_physical_page_mappings_base; }
  u8* GetLogicalPageMappingsBase() const { return m_logical_page_mappings_base; }

  u32 GetHostPageSize() const { return m_page_size; }

  // References, because AddressSpace keeps pointers to these pointers.
  u8*& GetRAM() { return m_ram; }
  u8*& GetEXRAM() { return m_exram; }
  u8* GetL1Cache() { return m_l1_cache; }
  u8*& GetFakeVMEM() { return m_fake_vmem; }

  MMIO::Mapping* GetMMIOMapping() const { return m_mmio_mapping.get(); }

  bool IsInitialized() const { return m_is_initialized; }
  void InitMMIO(Core::System& system);
  void Init();
  void Shutdown();
  bool InitFastmemArena();
  void ShutdownFastmemArena();
  void DoState(PointerWrap& p);

  void UpdateDBATMappings(const PowerPC::BatTable& dbat_table);
  void AddPageTableMapping(u32 logical_address, u32 translated_address, bool writeable);
  void RemovePageTableMappings(const std::set<u32>& mappings);
  void RemoveAllPageTableMappings();

  void Clear();

  // Access by physical address, for emulated hardware (DMA engines and the
  // like) and emulator code. Addresses are taken modulo 1 GiB.
  std::string GetString(u32 em_address, size_t size = 0);

  // The rest of the memory block that contains `address`, or an empty span
  // (and a panic alert) if it is not memory.
  std::span<u8> GetSpanForAddress(u32 address) const;

  // Host pointer for [address, address + size), or null unless the whole
  // range lies in one memory block.
  u8* GetPointerForRange(u32 address, size_t size) const;
  void CopyFromEmu(void* data, u32 address, size_t size) const;
  void CopyToEmu(u32 address, const void* data, size_t size);
  void Memset(u32 address, u8 value, size_t size);
  u8 Read_U8(u32 address) const;
  u16 Read_U16(u32 address) const;
  u32 Read_U32(u32 address) const;
  u64 Read_U64(u32 address) const;
  u32 Read_U32_Swap(u32 address) const;
  void Write_U8(u8 var, u32 address);
  void Write_U16(u16 var, u32 address);
  void Write_U32(u32 var, u32 address);
  void Write_U64(u64 var, u32 address);
  void Write_U32_Swap(u32 var, u32 address);
  void Write_U64_Swap(u64 var, u32 address);

  // Copies of big-endian arrays, swapping each element.
  template <typename T>
  void CopyFromEmuSwapped(T* data, u32 address, size_t size) const
  {
    const T* source = reinterpret_cast<const T*>(GetPointerForRange(address, size));
    if (source == nullptr)
      return;
    for (size_t i = 0; i < size / sizeof(T); i++)
      data[i] = Common::FromBigEndian(source[i]);
  }

  template <typename T>
  void CopyToEmuSwapped(u32 address, const T* data, size_t size)
  {
    T* target = reinterpret_cast<T*>(GetPointerForRange(address, size));
    if (target == nullptr)
      return;
    for (size_t i = 0; i < size / sizeof(T); i++)
      target[i] = Common::FromBigEndian(data[i]);
  }

private:
  enum class HostPageType
  {
    // Host pages of 4 KiB or less: guest pages map one to one.
    SmallPages,
    // Larger host pages: a host page is mapped once all its guest pages agree.
    LargePages,
    // Page size not a power of two, or the needed APIs are missing.
    Unsupported,
  };

  static HostPageType GetHostPageTypeForPageSize(u32 page_size);

  void UnmapViews(std::map<u32, LogicalMemoryView>& views);

  void TryAddLargePageTableMapping(u32 logical_address, u32 translated_address, bool writeable);
  bool TryAddLargePageTableMapping(u32 logical_address, u32 translated_address,
                                   std::map<u32, std::vector<u32>>& map);
  bool CanCreateHostMappingForGuestPages(std::span<const u32> entries) const;
  void AddHostPageTableMapping(u32 logical_address, u32 translated_address, bool writeable,
                               u32 logical_size);
  void RemoveLargePageTableMapping(u32 logical_address);
  void RemoveLargePageTableMapping(u32 logical_address, std::map<u32, std::vector<u32>>& map);
  void RemoveHostPageTableMapping(u32 logical_address);

  // Fastmem arena: [guard][physical 4 GiB][guard][logical 4 GiB][guard].
  u8* m_fastmem_arena = nullptr;
  size_t m_fastmem_arena_size = 0;
  u8* m_physical_base = nullptr;
  u8* m_logical_base = nullptr;

  // Pointer tables used when fastmem is not available (see the class comment).
  u8* m_physical_page_mappings_base = nullptr;
  u8* m_logical_page_mappings_base = nullptr;

  // Host views of the backing memory.
  u8* m_ram = nullptr;
  u8* m_exram = nullptr;
  u8* m_l1_cache = nullptr;
  u8* m_fake_vmem = nullptr;

  // *_size is what is allocated (a power of two), *_size_real what the
  // emulated software is told it has.
  u32 m_ram_size_real = 0;
  u32 m_ram_size = 0;
  u32 m_ram_mask = 0;
  u32 m_fakevmem_size = 0;
  u32 m_fakevmem_mask = 0;
  u32 m_l1_cache_size = 0;
  u32 m_l1_cache_mask = 0;
  u32 m_exram_size_real = 0;
  u32 m_exram_size = 0;
  u32 m_exram_mask = 0;

  bool m_is_fastmem_arena_initialized = false;
  bool m_is_initialized = false;

  std::unique_ptr<MMIO::Mapping> m_mmio_mapping;

  Common::MemArena m_arena;

  const u32 m_page_size;
  const u32 m_guest_pages_per_host_page;
  const HostPageType m_host_page_type;

  // RAM, locked L1, fake VMEM, MEM2 (in this order; it decides the layout of
  // the shared memory segment).
  std::array<PhysicalMemoryRegion, 4> m_physical_regions{};

  // Views in the logical window, keyed by the logical address they start at.
  std::map<u32, LogicalMemoryView> m_dbat_mapped_entries;
  std::map<u32, LogicalMemoryView> m_page_table_mapped_entries;

  std::array<void*, PowerPC::BAT_PAGE_COUNT> m_physical_page_mappings{};
  std::array<void*, PowerPC::BAT_PAGE_COUNT> m_logical_page_mappings{};

  // LargePages only: per host page, the translation of each guest page in it
  // (INVALID_MAPPING if none), separately for readable and writeable pages.
  static constexpr u32 INVALID_MAPPING = 0xFFFFFFFF;
  std::map<u32, std::vector<u32>> m_large_readable_pages;
  std::map<u32, std::vector<u32>> m_large_writeable_pages;

  Core::System& m_system;
};
}  // namespace Memory
