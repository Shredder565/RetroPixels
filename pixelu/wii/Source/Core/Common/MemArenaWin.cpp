// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/MemArena.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include <fmt/format.h>

#include <windows.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

// Fastmem on Windows.
//
// With the placeholder APIs (VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFileEx) the reserved
// region is one address-space placeholder that nothing else in the process can allocate into.
// Mapping a view at a fixed address first carves an exactly sized placeholder out of a free one
// (VirtualFree + MEM_PRESERVE_PLACEHOLDER) and then replaces it with the view
// (MEM_REPLACE_PLACEHOLDER). Unmapping turns the view back into a placeholder
// (MEM_PRESERVE_PLACEHOLDER) and merges it with free neighbours (MEM_COALESCE_PLACEHOLDERS).
// m_regions tracks the placeholders and views covering the region, sorted by address, with no
// gaps.
//
// Without those APIs (old Windows) the region is only probed: an address range is reserved and
// released again, and views are later mapped there with MapViewOfFileEx, hoping nothing else has
// taken the space meanwhile.

namespace Common
{
struct WindowsMemoryRegion
{
  u8* m_start;
  size_t m_size;
  bool m_is_mapped;

  WindowsMemoryRegion(u8* start, size_t size, bool is_mapped)
      : m_start(start), m_size(size), m_is_mapped(is_mapped)
  {
  }

  u8* End() const { return m_start + m_size; }
};

namespace
{
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE process, PVOID base_address, SIZE_T size,
                                       ULONG allocation_type, ULONG page_protection,
                                       MEM_EXTENDED_PARAMETER* extended_parameters,
                                       ULONG parameter_count);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE file_mapping, HANDLE process, PVOID base_address,
                                        ULONG64 offset, SIZE_T view_size, ULONG allocation_type,
                                        ULONG page_protection,
                                        MEM_EXTENDED_PARAMETER* extended_parameters,
                                        ULONG parameter_count);
using UnmapViewOfFileExFn = BOOL(WINAPI*)(PVOID base_address, ULONG unmap_flags);
using IsApiSetImplementedFn = BOOL(APIENTRY*)(PCSTR contract);

constexpr char MEMORY_API_SET[] = "api-ms-win-core-memory-l1-1-6";

bool HasPlaceholderApi(const WindowsMemoryFunctions& fns)
{
  return fns.m_address_MapViewOfFile3 != nullptr;
}

void* ReservePlaceholder(const WindowsMemoryFunctions& fns, size_t size)
{
  return reinterpret_cast<VirtualAlloc2Fn>(fns.m_address_VirtualAlloc2)(
      nullptr, nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
}

// Replaces the placeholder at `address` (exactly `size` bytes) with a view of `section`.
void* MapOverPlaceholder(const WindowsMemoryFunctions& fns, HANDLE section, void* address,
                         u64 offset, size_t size, ULONG protection)
{
  return reinterpret_cast<MapViewOfFile3Fn>(fns.m_address_MapViewOfFile3)(
      section, nullptr, address, offset, size, MEM_REPLACE_PLACEHOLDER, protection, nullptr, 0);
}

// Unmaps the view at `address`, leaving a placeholder behind.
bool UnmapToPlaceholder(const WindowsMemoryFunctions& fns, void* address)
{
  return reinterpret_cast<UnmapViewOfFileExFn>(fns.m_address_UnmapViewOfFileEx)(
             address, MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

// Splits [address, address + size) off as its own placeholder.
bool SplitPlaceholder(void* address, size_t size)
{
  return VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

// Merges the adjacent placeholders exactly covering [address, address + size) into one.
bool CoalescePlaceholders(void* address, size_t size)
{
  return VirtualFree(address, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != FALSE;
}

// A page-file-backed section of `size` bytes (zero-filled).
HANDLE CreateAnonymousSection(size_t size, DWORD protection, LPCTSTR name)
{
  const u64 size64 = static_cast<u64>(size);
  return CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, protection,
                           static_cast<DWORD>(size64 >> 32), static_cast<DWORD>(size64), name);
}

// Looks up the placeholder APIs. Fills in all three pointers (and keeps the libraries open) or
// none.
void LoadWindowsMemoryFunctions(WindowsMemoryFunctions* fns)
{
  // The api-set may resolve to a stub on systems that lack it, so ask whether it is really there.
  DynamicLibrary kernel_base{"KernelBase.dll"};
  if (!kernel_base.IsOpen())
    return;
  const auto is_api_set_implemented =
      reinterpret_cast<IsApiSetImplementedFn>(kernel_base.GetSymbolAddress("IsApiSetImplemented"));
  if (!is_api_set_implemented || !is_api_set_implemented(MEMORY_API_SET))
    return;

  DynamicLibrary& memory_lib = fns->m_api_ms_win_core_memory_l1_1_6_handle;
  DynamicLibrary& kernel32 = fns->m_kernel32_handle;
  memory_lib.Open("api-ms-win-core-memory-l1-1-6.dll");
  kernel32.Open("Kernel32.dll");

  void* const virtual_alloc2 =
      memory_lib.IsOpen() ? memory_lib.GetSymbolAddress("VirtualAlloc2FromApp") : nullptr;
  void* const map_view3 =
      memory_lib.IsOpen() ? memory_lib.GetSymbolAddress("MapViewOfFile3FromApp") : nullptr;
  void* const unmap_view_ex =
      kernel32.IsOpen() ? kernel32.GetSymbolAddress("UnmapViewOfFileEx") : nullptr;

  if (!virtual_alloc2 || !map_view3 || !unmap_view_ex)
  {
    memory_lib.Close();
    kernel32.Close();
    return;
  }

  fns->m_address_VirtualAlloc2 = virtual_alloc2;
  fns->m_address_MapViewOfFile3 = map_view3;
  fns->m_address_UnmapViewOfFileEx = unmap_view_ex;
}
}  // namespace

MemArena::MemArena()
{
  LoadWindowsMemoryFunctions(&m_memory_functions);
}

MemArena::~MemArena()
{
  ReleaseMemoryRegion();
  ReleaseSHMSegment();
}

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  const std::string name = fmt::format("{}.{}", base_name, GetCurrentProcessId());
  m_memory_handle = CreateAnonymousSection(size, PAGE_READWRITE, UTF8ToTStr(name).c_str());
}

void MemArena::ReleaseSHMSegment()
{
  if (m_memory_handle == nullptr)
    return;
  CloseHandle(m_memory_handle);
  m_memory_handle = nullptr;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  const u64 offset64 = static_cast<u64>(offset);
  return MapViewOfFileEx(m_memory_handle, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(offset64 >> 32),
                         static_cast<DWORD>(offset64), size, nullptr);
}

void MemArena::ReleaseView(void* view, size_t size)
{
  UnmapViewOfFile(view);
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  if (m_reserved_region != nullptr)
  {
    PanicAlertFmt("This MemArena has already reserved its memory region.");
    return nullptr;
  }

  if (!HasPlaceholderApi(m_memory_functions))
  {
    NOTICE_LOG_FMT(MEMMAP, "Placeholder memory APIs are not available; using legacy fastmem "
                           "mapping.");
    // Find a free range big enough and give it straight back; views go there later.
    void* const probe = VirtualAlloc(nullptr, memory_size, MEM_RESERVE, PAGE_READWRITE);
    if (probe == nullptr)
    {
      PanicAlertFmt("Could not find {} bytes of free address space: {}", memory_size,
                    GetLastErrorString());
      return nullptr;
    }
    VirtualFree(probe, 0, MEM_RELEASE);
    return static_cast<u8*>(probe);
  }

  u8* const base = static_cast<u8*>(ReservePlaceholder(m_memory_functions, memory_size));
  if (base == nullptr)
  {
    PanicAlertFmt("Could not reserve {} bytes of address space: {}", memory_size,
                  GetLastErrorString());
    return nullptr;
  }

  m_reserved_region = base;
  m_regions.emplace_back(base, memory_size, false);
  return base;
}

void MemArena::ReleaseMemoryRegion()
{
  if (!HasPlaceholderApi(m_memory_functions) || m_reserved_region == nullptr)
    return;

  // Anything still mapped here is a bug in the caller's mapping logic.
  const auto still_mapped = std::ranges::count_if(
      m_regions, [](const WindowsMemoryRegion& region) { return region.m_is_mapped; });
  if (still_mapped != 0)
  {
    PanicAlertFmt("Releasing the fastmem region while {} mapping(s) are still in place.",
                  still_mapped);
  }

  VirtualFree(m_reserved_region, 0, MEM_RELEASE);
  m_reserved_region = nullptr;
  m_regions.clear();
}

WindowsMemoryRegion* MemArena::EnsureSplitRegionForMapping(void* start_address, size_t size)
{
  u8* const address = static_cast<u8*>(start_address);
  if (m_regions.empty())
  {
    NOTICE_LOG_FMT(MEMMAP, "Cannot map at {}: no memory region has been reserved.",
                   fmt::ptr(address));
    return nullptr;
  }

  // The region containing `address` is the last one starting at or before it.
  auto next = std::ranges::upper_bound(m_regions, address, {}, &WindowsMemoryRegion::m_start);
  if (next == m_regions.begin())
  {
    NOTICE_LOG_FMT(MEMMAP, "Cannot map at {}: below the reserved memory region.",
                   fmt::ptr(address));
    return nullptr;
  }
  const size_t index = static_cast<size_t>(next - m_regions.begin()) - 1;
  const WindowsMemoryRegion free_region = m_regions[index];

  if (free_region.m_is_mapped)
  {
    NOTICE_LOG_FMT(MEMMAP, "Cannot map 0x{:x} bytes at {}: overlaps the mapping at {}.", size,
                   fmt::ptr(address), fmt::ptr(free_region.m_start));
    return nullptr;
  }

  const size_t head = static_cast<size_t>(address - free_region.m_start);
  if (head > free_region.m_size || free_region.m_size - head < size)
  {
    NOTICE_LOG_FMT(MEMMAP,
                   "Cannot map 0x{:x} bytes at {}: the free space starting at {} is only 0x{:x} "
                   "bytes.",
                   size, fmt::ptr(address), fmt::ptr(free_region.m_start), free_region.m_size);
    return nullptr;
  }
  const size_t tail = free_region.m_size - head - size;

  // Already exactly the requested placeholder.
  if (head == 0 && tail == 0)
    return &m_regions[index];

  if (!SplitPlaceholder(address, size))
  {
    NOTICE_LOG_FMT(MEMMAP, "Splitting the placeholder at {} failed: {}", fmt::ptr(address),
                   GetLastErrorString());
    return nullptr;
  }

  // Replace the free region by up to three pieces: [head][requested][tail].
  std::vector<WindowsMemoryRegion> pieces;
  if (head != 0)
    pieces.emplace_back(free_region.m_start, head, false);
  const size_t requested_index = index + pieces.size();
  pieces.emplace_back(address, size, false);
  if (tail != 0)
    pieces.emplace_back(address + size, tail, false);

  const auto at = m_regions.erase(m_regions.begin() + index);
  m_regions.insert(at, pieces.begin(), pieces.end());
  return &m_regions[requested_index];
}

bool MemArena::JoinRegionsAfterUnmap(void* start_address, size_t size)
{
  u8* const address = static_cast<u8*>(start_address);
  if (m_regions.empty())
  {
    NOTICE_LOG_FMT(MEMMAP, "Cannot unmap at {}: no memory region has been reserved.",
                   fmt::ptr(address));
    return false;
  }

  const auto found =
      std::ranges::lower_bound(m_regions, address, {}, &WindowsMemoryRegion::m_start);
  if (found == m_regions.end() || found->m_start != address || found->m_size != size)
  {
    NOTICE_LOG_FMT(MEMMAP, "Cannot unmap 0x{:x} bytes at {}: no such mapping.", size,
                   fmt::ptr(address));
    return false;
  }
  found->m_is_mapped = false;

  // Merge with whichever neighbours are free placeholders too.
  auto first = found;
  auto last = found;
  if (first != m_regions.begin() && !std::prev(first)->m_is_mapped)
    --first;
  if (std::next(last) != m_regions.end() && !std::next(last)->m_is_mapped)
    ++last;
  if (first == last)
    return true;

  const size_t merged_size = static_cast<size_t>(last->End() - first->m_start);
  if (!CoalescePlaceholders(first->m_start, merged_size))
  {
    NOTICE_LOG_FMT(MEMMAP, "Merging the placeholders at {} failed: {}", fmt::ptr(first->m_start),
                   GetLastErrorString());
    return false;
  }

  first->m_size = merged_size;
  m_regions.erase(std::next(first), std::next(last));
  return true;
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  void* mapped;
  if (HasPlaceholderApi(m_memory_functions))
  {
    WindowsMemoryRegion* const region = EnsureSplitRegionForMapping(base, size);
    if (region == nullptr)
    {
      PanicAlertFmt("Could not prepare the fastmem region for a mapping at {}.", fmt::ptr(base));
      return nullptr;
    }

    // Always map read-write; read-only is applied afterwards so it can be lifted again later.
    mapped = MapOverPlaceholder(m_memory_functions, m_memory_handle, base,
                                static_cast<u64>(offset), size, PAGE_READWRITE);
    if (mapped == nullptr)
    {
      PanicAlertFmt("Mapping into the fastmem region failed: {}", GetLastErrorString());
      JoinRegionsAfterUnmap(base, size);  // undo the split
      return nullptr;
    }
    region->m_is_mapped = true;
  }
  else
  {
    const u64 offset64 = static_cast<u64>(offset);
    mapped =
        MapViewOfFileEx(m_memory_handle, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(offset64 >> 32),
                        static_cast<DWORD>(offset64), size, base);
    if (mapped == nullptr)
      return nullptr;
  }

  if (!writeable)
    ChangeMappingProtection(base, size, false);

  return mapped;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  DWORD previous_protection;
  if (!VirtualProtect(view, size, writeable ? PAGE_READWRITE : PAGE_READONLY,
                      &previous_protection))
  {
    PanicAlertFmt("Changing the protection of a fastmem mapping failed: {}",
                  GetLastErrorString());
    return false;
  }
  return true;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
  if (!HasPlaceholderApi(m_memory_functions))
  {
    UnmapViewOfFile(view);
    return;
  }

  if (!UnmapToPlaceholder(m_memory_functions, view))
  {
    PanicAlertFmt("Unmapping from the fastmem region failed: {}", GetLastErrorString());
    return;
  }
  if (!JoinRegionsAfterUnmap(view, size))
    PanicAlertFmt("Could not merge the fastmem region after unmapping.");
}

size_t MemArena::GetPageSize() const
{
  SYSTEM_INFO info;
  GetSystemInfo(&info);

  // Plain MapViewOfFileEx can only place views on allocation-granularity (64 KiB) boundaries.
  if (!HasPlaceholderApi(m_memory_functions))
    return std::max<size_t>(info.dwPageSize, 64 * 1024);

  return info.dwPageSize;
}

LazyMemoryRegion::LazyMemoryRegion()
{
  LoadWindowsMemoryFunctions(&m_memory_functions);
}

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);

  if (size == 0 || !HasPlaceholderApi(m_memory_functions))
    return nullptr;

  const size_t total_size = Common::AlignUp(size, BLOCK_SIZE);
  const size_t block_count = total_size / BLOCK_SIZE;

  u8* const memory = static_cast<u8*>(ReservePlaceholder(m_memory_functions, total_size));
  if (memory == nullptr)
  {
    NOTICE_LOG_FMT(MEMMAP, "Reserving {} bytes for a lazy memory region failed: {}", size,
                   GetLastErrorString());
    return nullptr;
  }

  // One placeholder per block. Splitting off each block but the last leaves the last one as the
  // remainder.
  for (size_t i = 0; i + 1 < block_count; ++i)
  {
    if (!SplitPlaceholder(memory + i * BLOCK_SIZE, BLOCK_SIZE))
    {
      NOTICE_LOG_FMT(MEMMAP, "Splitting a lazy memory region failed: {}", GetLastErrorString());
      // Blocks 0..i-1 are separate placeholders now; block i is the start of the rest.
      for (size_t j = 0; j <= i; ++j)
        VirtualFree(memory + j * BLOCK_SIZE, 0, MEM_RELEASE);
      return nullptr;
    }
  }

  // From here on Release() can undo everything, whatever state the blocks are in.
  m_memory = memory;
  m_size = total_size;
  m_writable_block_handles.assign(block_count, nullptr);

  m_zero_block = CreateAnonymousSection(BLOCK_SIZE, PAGE_READONLY, nullptr);
  if (m_zero_block == nullptr)
  {
    NOTICE_LOG_FMT(MEMMAP, "Creating the zero block failed: {}", GetLastErrorString());
    Release();
    return nullptr;
  }

  for (size_t i = 0; i < block_count; ++i)
  {
    if (!MapOverPlaceholder(m_memory_functions, m_zero_block, memory + i * BLOCK_SIZE, 0,
                            BLOCK_SIZE, PAGE_READONLY))
    {
      NOTICE_LOG_FMT(MEMMAP, "Mapping the zero block failed: {}", GetLastErrorString());
      Release();
      return nullptr;
    }
  }

  return memory;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);
  u8* const memory = static_cast<u8*>(m_memory);

  for (size_t i = 0; i < m_writable_block_handles.size(); ++i)
  {
    HANDLE& block = m_writable_block_handles[i];
    if (block == nullptr)
      continue;

    u8* const address = memory + i * BLOCK_SIZE;
    if (!UnmapToPlaceholder(m_memory_functions, address))
      PanicAlertFmt("Unmapping a written block failed: {}", GetLastErrorString());
    if (!CloseHandle(block))
      PanicAlertFmt("Freeing a written block failed: {}", GetLastErrorString());
    block = nullptr;

    if (!MapOverPlaceholder(m_memory_functions, m_zero_block, address, 0, BLOCK_SIZE,
                            PAGE_READONLY))
    {
      PanicAlertFmt("Mapping the zero block back failed: {}", GetLastErrorString());
    }
  }
}

void LazyMemoryRegion::Release()
{
  u8* const memory = static_cast<u8*>(m_memory);

  // Unmap every block (zero or written; unmapping a bare placeholder just fails) and free the
  // written blocks' sections.
  if (memory != nullptr)
  {
    for (size_t i = 0; i < m_writable_block_handles.size(); ++i)
    {
      UnmapToPlaceholder(m_memory_functions, memory + i * BLOCK_SIZE);
      if (m_writable_block_handles[i] != nullptr)
      {
        CloseHandle(m_writable_block_handles[i]);
        m_writable_block_handles[i] = nullptr;
      }
    }
  }

  if (m_zero_block != nullptr)
  {
    CloseHandle(m_zero_block);
    m_zero_block = nullptr;
  }

  if (memory != nullptr)
  {
    for (size_t i = 0; i < m_size / BLOCK_SIZE; ++i)
      VirtualFree(memory + i * BLOCK_SIZE, 0, MEM_RELEASE);
    m_memory = nullptr;
    m_size = 0;
  }
  m_writable_block_handles.clear();
}

void LazyMemoryRegion::MakeMemoryBlockWritable(size_t block_index)
{
  u8* const address = static_cast<u8*>(m_memory) + block_index * BLOCK_SIZE;

  // Get the block's own memory first, so a failure leaves the zero block in place.
  const HANDLE block = CreateAnonymousSection(BLOCK_SIZE, PAGE_READWRITE, nullptr);
  if (block == nullptr)
  {
    PanicAlertFmt("Creating a writable block failed: {}", GetLastErrorString());
    return;
  }

  if (!UnmapToPlaceholder(m_memory_functions, address))
  {
    PanicAlertFmt("Unmapping the zero block failed: {}", GetLastErrorString());
    CloseHandle(block);
    return;
  }

  if (!MapOverPlaceholder(m_memory_functions, block, address, 0, BLOCK_SIZE, PAGE_READWRITE))
  {
    PanicAlertFmt("Mapping a writable block failed: {}", GetLastErrorString());
    CloseHandle(block);
    // Keep the block readable.
    MapOverPlaceholder(m_memory_functions, m_zero_block, address, 0, BLOCK_SIZE, PAGE_READONLY);
    return;
  }

  m_writable_block_handles[block_index] = block;
}

}  // namespace Common
