// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"

namespace Common
{
#ifdef _WIN32
struct WindowsMemoryRegion;

// The placeholder-capable memory APIs (Windows 10 1803+). They are looked up at runtime; when any
// is missing every pointer stays null and the arena uses plain MapViewOfFileEx instead.
struct WindowsMemoryFunctions
{
  Common::DynamicLibrary m_kernel32_handle;
  Common::DynamicLibrary m_api_ms_win_core_memory_l1_1_6_handle;
  void* m_address_UnmapViewOfFileEx = nullptr;
  void* m_address_VirtualAlloc2 = nullptr;
  void* m_address_MapViewOfFile3 = nullptr;
};
#endif

// Emulated physical memory. One shared-memory segment is the backing store; any part of it can be
// mapped as a view any number of times, which is how mirrored address ranges are emulated. A
// reserved address range (the fastmem area) can have pieces of the segment mapped into it at
// fixed addresses.
class MemArena final
{
public:
  MemArena();
  ~MemArena();
  MemArena(const MemArena&) = delete;
  MemArena(MemArena&&) = delete;
  MemArena& operator=(const MemArena&) = delete;
  MemArena& operator=(MemArena&&) = delete;

  // Creates the backing segment of `size` bytes. `base_name` names it (where the platform names
  // shared memory) and is suffixed with the process id.
  void GrabSHMSegment(size_t size, std::string_view base_name);

  // Destroys the backing segment. Release every view first.
  void ReleaseSHMSegment();

  // Maps `size` bytes of the segment, starting at `offset`, at an address of the system's choice.
  // Returns nullptr on failure.
  void* CreateView(s64 offset, size_t size);

  // Unmaps a view returned by CreateView(); `size` is the size it was created with.
  void ReleaseView(void* view, size_t size);

  // Reserves `memory_size` bytes of address space for fastmem. Only one region per arena.
  // Returns the start of the region, or nullptr on failure.
  u8* ReserveMemoryRegion(size_t memory_size);

  // Gives back the region reserved by ReserveMemoryRegion(). Unmap everything first.
  void ReleaseMemoryRegion();

  // Maps `size` bytes of the segment, starting at `offset`, at address `base` inside the reserved
  // region, read-write or read-only. Returns the mapped address (`base`), or nullptr on failure.
  void* MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable);

  // Switches a mapping made by MapInMemoryRegion() between read-write and read-only.
  // Returns false on failure.
  bool ChangeMappingProtection(void* view, size_t size, bool writeable);

  // Undoes MapInMemoryRegion(); `view` and `size` are what was mapped.
  void UnmapFromMemoryRegion(void* view, size_t size);

  // The granularity mappings must respect: the page size, or the allocation granularity when
  // that is what mapping requires.
  size_t GetPageSize() const;

private:
#ifdef _WIN32
  // Placeholder bookkeeping for the reserved region (see MemArenaWin.cpp).
  WindowsMemoryRegion* EnsureSplitRegionForMapping(void* address, size_t size);
  bool JoinRegionsAfterUnmap(void* address, size_t size);

  std::vector<WindowsMemoryRegion> m_regions;
  void* m_reserved_region = nullptr;
  void* m_memory_handle = nullptr;
  WindowsMemoryFunctions m_memory_functions;
#endif
};

// A fixed-size region that reads as zero and only gets real memory where it is written. On
// Windows that is done block by block: every block starts as a read-only view of one shared zero
// block and must be made writable (EnsureMemoryPage(s)Writable) before it is written. Elsewhere
// the OS does it on its own and those calls do nothing.
class LazyMemoryRegion final
{
public:
  LazyMemoryRegion();
  ~LazyMemoryRegion();
  LazyMemoryRegion(const LazyMemoryRegion&) = delete;
  LazyMemoryRegion(LazyMemoryRegion&&) = delete;
  LazyMemoryRegion& operator=(const LazyMemoryRegion&) = delete;
  LazyMemoryRegion& operator=(LazyMemoryRegion&&) = delete;

  // Reserves a zeroed region of at least `size` bytes. Returns its address, or nullptr.
  void* Create(size_t size);

  // Makes the whole region zero again and drops the memory behind written blocks. Only valid
  // after a successful Create().
  void Clear();

  // Frees the region; the pointer returned by Create() becomes invalid.
  void Release();

  // Makes the block containing byte `offset` writable if it is not yet.
  void EnsureMemoryPageWritable(size_t offset)
  {
#ifdef _WIN32
    const size_t block = offset / BLOCK_SIZE;
    if (!m_writable_block_handles[block])
      MakeMemoryBlockWritable(block);
#endif
  }

  // Makes every block touched by bytes [offset, offset + size) writable.
  void EnsureMemoryPagesWritable(size_t offset, size_t size)
  {
#ifdef _WIN32
    if (size == 0)
      return;
    const size_t last_block = (offset + size - 1) / BLOCK_SIZE;
    for (size_t block = offset / BLOCK_SIZE; block <= last_block; ++block)
    {
      if (!m_writable_block_handles[block])
        MakeMemoryBlockWritable(block);
    }
#endif
  }

private:
  void* m_memory = nullptr;
  size_t m_size = 0;

#ifdef _WIN32
  constexpr static size_t BLOCK_SIZE = 8 * 1024 * 1024;

  void* m_zero_block = nullptr;  // section handle of the shared zero block
  WindowsMemoryFunctions m_memory_functions;
  std::vector<void*> m_writable_block_handles;  // per block: its own section, or null

  void MakeMemoryBlockWritable(size_t block_index);
#endif
};

}  // namespace Common
