// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Memmap.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tuple>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MemArena.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/MemoryInterface.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/PixelEngine.h"

namespace Memory
{
namespace
{
// Layout of the fastmem arena (see Memmap.h).
constexpr size_t PPC_VIEW_SIZE = 0x1'0000'0000;
constexpr size_t GUARD_SIZE = 0x8000'0000;
constexpr size_t ARENA_SIZE = 2 * PPC_VIEW_SIZE + 3 * GUARD_SIZE;

constexpr u32 FAKE_VMEM_SIZE = 0x02000000;
constexpr u32 L1_CACHE_SIZE = 0x00040000;

// The part of [start, start + length) that falls inside a memory region, in
// 32-bit address arithmetic.
struct Overlap
{
  u32 begin;
  u32 end;
};

std::optional<Overlap> FindOverlap(const PhysicalMemoryRegion& region, u32 start, u32 length)
{
  const u32 region_end = region.physical_address + region.size;
  const u32 begin = std::max(region.physical_address, start);
  const u32 end = std::min(region_end, start + length);
  if (begin >= end)
    return std::nullopt;
  return Overlap{begin, end};
}
}  // namespace

MemoryManager::MemoryManager(Core::System& system)
    : m_page_size(static_cast<u32>(m_arena.GetPageSize())),
      m_guest_pages_per_host_page(m_page_size / PowerPC::HW_PAGE_SIZE),
      m_host_page_type(GetHostPageTypeForPageSize(m_page_size)), m_system(system)
{
}

MemoryManager::~MemoryManager() = default;

MemoryManager::HostPageType MemoryManager::GetHostPageTypeForPageSize(u32 page_size)
{
  if (!std::has_single_bit(page_size))
    return HostPageType::Unsupported;
  return page_size <= PowerPC::HW_PAGE_SIZE ? HostPageType::SmallPages :
                                              HostPageType::LargePages;
}

void MemoryManager::InitMMIO(Core::System& system)
{
  m_mmio_mapping = std::make_unique<MMIO::Mapping>();
  MMIO::Mapping* const mmio = m_mmio_mapping.get();

  // GameCube register block.
  system.GetCommandProcessor().RegisterMMIO(mmio, 0x0C000000);
  system.GetPixelEngine().RegisterMMIO(mmio, 0x0C001000);
  system.GetVideoInterface().RegisterMMIO(mmio, 0x0C002000);
  system.GetProcessorInterface().RegisterMMIO(mmio, 0x0C003000);
  system.GetMemoryInterface().RegisterMMIO(mmio, 0x0C004000);
  system.GetDSP().RegisterMMIO(mmio, 0x0C005000);
  system.GetDVDInterface().RegisterMMIO(mmio, 0x0C006000, false);
  system.GetSerialInterface().RegisterMMIO(mmio, 0x0C006400);
  system.GetExpansionInterface().RegisterMMIO(mmio, 0x0C006800);
  system.GetAudioInterface().RegisterMMIO(mmio, 0x0C006C00);

  if (!system.IsWii())
    return;

  // Hollywood register block.
  system.GetWiiIPC().RegisterMMIO(mmio, 0x0D000000);
  system.GetDVDInterface().RegisterMMIO(mmio, 0x0D006000, true);
  system.GetSerialInterface().RegisterMMIO(mmio, 0x0D006400);
  system.GetExpansionInterface().RegisterMMIO(mmio, 0x0D006800);
  system.GetAudioInterface().RegisterMMIO(mmio, 0x0D006C00);
}

void MemoryManager::Init()
{
  if (Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE))
  {
    m_ram_size_real = Config::Get(Config::MAIN_MEM1_SIZE);
  }
  else
  {
    // The simulated memory size from the disc header lets a game written for
    // a development unit see its larger MEM1. It is never below retail size
    // and never above what the memory controller can address.
    m_ram_size_real =
        std::clamp(m_system.GetSimulatedMemorySize(), MEM1_SIZE_RETAIL, MEM1_SIZE_MAX);
  }
  m_ram_size = MathUtil::NextPowerOf2(m_ram_size_real);
  m_ram_mask = m_ram_size - 1;

  m_fakevmem_size = FAKE_VMEM_SIZE;
  m_fakevmem_mask = m_fakevmem_size - 1;
  m_l1_cache_size = L1_CACHE_SIZE;
  m_l1_cache_mask = m_l1_cache_size - 1;

  m_exram_size_real = Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE) ?
                          Config::Get(Config::MAIN_MEM2_SIZE) :
                          MEM2_SIZE_RETAIL;
  m_exram_size = MathUtil::NextPowerOf2(m_exram_size_real);
  m_exram_mask = m_exram_size - 1;

  using Region = PhysicalMemoryRegion;
  m_physical_regions = {{
      {&m_ram, 0x00000000, m_ram_size, Region::ALWAYS, 0, false},
      {&m_l1_cache, 0xE0000000, m_l1_cache_size, Region::ALWAYS, 0, false},
      {&m_fake_vmem, 0x7E000000, m_fakevmem_size, Region::FAKE_VMEM, 0, false},
      {&m_exram, 0x10000000, m_exram_size, Region::WII_ONLY, 0, false},
  }};

  const bool wii = m_system.IsWii();
  // Fake VMEM only exists on GameCube without MMU emulation.
  const bool use_fake_vmem = !wii && !m_system.IsMMUMode();

  // Lay the active regions out one after another in the shared segment.
  u32 segment_size = 0;
  for (Region& region : m_physical_regions)
  {
    if ((region.flags & Region::WII_ONLY) && !wii)
      continue;
    if ((region.flags & Region::FAKE_VMEM) && !use_fake_vmem)
      continue;
    region.active = true;
    region.shm_position = segment_size;
    segment_size += region.size;
  }
  m_arena.GrabSHMSegment(segment_size, "dolphin-emu");

  m_physical_page_mappings.fill(nullptr);
  for (const Region& region : m_physical_regions)
  {
    if (!region.active)
      continue;

    u8* const view = static_cast<u8*>(m_arena.CreateView(region.shm_position, region.size));
    *region.out_pointer = view;
    if (view == nullptr)
    {
      PanicAlertFmt("Memory::Init(): could not map the memory block at physical address "
                    "0x{:08X} (size 0x{:08X}).",
                    region.physical_address, region.size);
      exit(0);
    }

    for (u32 offset = 0; offset < region.size; offset += PowerPC::BAT_PAGE_SIZE)
    {
      const u32 page = (region.physical_address + offset) >> PowerPC::BAT_INDEX_SHIFT;
      m_physical_page_mappings[page] = view + offset;
    }
  }

  m_physical_page_mappings_base = reinterpret_cast<u8*>(m_physical_page_mappings.data());
  m_logical_page_mappings_base = reinterpret_cast<u8*>(m_logical_page_mappings.data());

  Clear();

  INFO_LOG_FMT(MEMMAP, "Emulated memory ready; MEM1 at {}", fmt::ptr(m_ram));
  m_is_initialized = true;
}

bool MemoryManager::IsAddressInFastmemArea(const u8* address) const
{
  return address >= m_fastmem_arena && address < m_fastmem_arena + m_fastmem_arena_size;
}

bool MemoryManager::InitFastmemArena()
{
  m_fastmem_arena = m_arena.ReserveMemoryRegion(ARENA_SIZE);
  if (m_fastmem_arena == nullptr)
  {
    PanicAlertFmt("Memory::InitFastmemArena(): could not reserve address space for fastmem.");
    return false;
  }

  m_physical_base = m_fastmem_arena + GUARD_SIZE;
  m_logical_base = m_physical_base + PPC_VIEW_SIZE + GUARD_SIZE;

  // The physical window always shows every active region at its address.
  for (const PhysicalMemoryRegion& region : m_physical_regions)
  {
    if (!region.active)
      continue;

    u8* const target = m_physical_base + region.physical_address;
    void* const view = m_arena.MapInMemoryRegion(region.shm_position, region.size, target, true);
    if (view != target)
    {
      PanicAlertFmt("Memory::InitFastmemArena(): could not map the memory block at physical "
                    "address 0x{:08X} (size 0x{:08X}) into the fastmem arena.",
                    region.physical_address, region.size);
      return false;
    }
  }

  m_is_fastmem_arena_initialized = true;
  m_fastmem_arena_size = ARENA_SIZE;
  return true;
}

void MemoryManager::UnmapViews(std::map<u32, LogicalMemoryView>& views)
{
  for (const auto& entry : views)
    m_arena.UnmapFromMemoryRegion(entry.second.mapped_pointer, entry.second.mapped_size);
  views.clear();
}

void MemoryManager::UpdateDBATMappings(const PowerPC::BatTable& dbat_table)
{
  UnmapViews(m_dbat_mapped_entries);
  RemoveAllPageTableMappings();
  m_logical_page_mappings.fill(nullptr);

  const u32 page_count = static_cast<u32>(dbat_table.size());
  u32 page = 0;
  while (page < page_count)
  {
    if (!(dbat_table[page] & PowerPC::BAT_PHYSICAL_BIT))
    {
      page++;
      continue;
    }

    // Collect a run of BAT pages that map to consecutive physical addresses.
    const u32 logical_start = page << PowerPC::BAT_INDEX_SHIFT;
    const u32 physical_start = dbat_table[page] & PowerPC::BAT_RESULT_MASK;
    u32 run_size = PowerPC::BAT_PAGE_SIZE;
    page++;
    while (page < page_count && (dbat_table[page] & PowerPC::BAT_PHYSICAL_BIT) &&
           (dbat_table[page] & PowerPC::BAT_RESULT_MASK) == physical_start + run_size)
    {
      run_size += PowerPC::BAT_PAGE_SIZE;
      page++;
    }

    for (const PhysicalMemoryRegion& region : m_physical_regions)
    {
      if (!region.active)
        continue;
      const std::optional<Overlap> overlap = FindOverlap(region, physical_start, run_size);
      if (!overlap)
        continue;

      const u32 offset_in_region = overlap->begin - region.physical_address;
      const u32 logical = logical_start + (overlap->begin - physical_start);
      const u32 size = overlap->end - overlap->begin;

      if (m_is_fastmem_arena_initialized)
      {
        void* const view = m_arena.MapInMemoryRegion(region.shm_position + offset_in_region, size,
                                                     m_logical_base + logical, true);
        if (view == nullptr)
        {
          PanicAlertFmt("Memory::UpdateDBATMappings(): could not map physical 0x{:08X} "
                        "(size 0x{:08X}) at logical 0x{:08X} in the fastmem arena.",
                        overlap->begin, size, logical_start);
          continue;
        }
        m_dbat_mapped_entries.emplace(logical, LogicalMemoryView{view, size});
      }

      u8* const host = *region.out_pointer + offset_in_region;
      const u32 first_page = logical / PowerPC::BAT_PAGE_SIZE;
      for (u32 i = 0; i < size / PowerPC::BAT_PAGE_SIZE; i++)
        m_logical_page_mappings[first_page + i] = host + i * PowerPC::BAT_PAGE_SIZE;
    }
  }
}

void MemoryManager::AddPageTableMapping(u32 logical_address, u32 translated_address, bool writeable)
{
  if (!m_is_fastmem_arena_initialized)
    return;

  if (m_host_page_type == HostPageType::SmallPages)
  {
    AddHostPageTableMapping(logical_address, translated_address, writeable,
                            PowerPC::HW_PAGE_SIZE);
  }
  else if (m_host_page_type == HostPageType::LargePages)
  {
    TryAddLargePageTableMapping(logical_address, translated_address, writeable);
  }
}

void MemoryManager::TryAddLargePageTableMapping(u32 logical_address, u32 translated_address,
                                                bool writeable)
{
  const bool readable_complete =
      TryAddLargePageTableMapping(logical_address, translated_address, m_large_readable_pages);
  const bool writeable_complete =
      writeable &&
      TryAddLargePageTableMapping(logical_address, translated_address, m_large_writeable_pages);

  if (!readable_complete && !writeable_complete)
    return;

  const u32 host_page_mask = ~(m_page_size - 1);
  AddHostPageTableMapping(logical_address & host_page_mask, translated_address & host_page_mask,
                          writeable_complete, m_page_size);
}

bool MemoryManager::TryAddLargePageTableMapping(u32 logical_address, u32 translated_address,
                                                std::map<u32, std::vector<u32>>& map)
{
  std::vector<u32>& guest_pages = map[logical_address & ~(m_page_size - 1)];
  if (guest_pages.empty())
    guest_pages.assign(m_guest_pages_per_host_page, INVALID_MAPPING);

  const u32 index = (logical_address & (m_page_size - 1)) / PowerPC::HW_PAGE_SIZE;
  guest_pages[index] = translated_address;
  return CanCreateHostMappingForGuestPages(guest_pages);
}

bool MemoryManager::CanCreateHostMappingForGuestPages(std::span<const u32> entries) const
{
  // The guest pages must be the consecutive pages of one aligned host page.
  const u32 first = entries[0];
  if (first & (m_page_size - 1))
    return false;

  for (size_t i = 1; i < m_guest_pages_per_host_page; i++)
  {
    if (entries[i] != first + i * PowerPC::HW_PAGE_SIZE)
      return false;
  }
  return true;
}

void MemoryManager::AddHostPageTableMapping(u32 logical_address, u32 translated_address,
                                            bool writeable, u32 logical_size)
{
  for (const PhysicalMemoryRegion& region : m_physical_regions)
  {
    if (!region.active)
      continue;
    const std::optional<Overlap> overlap = FindOverlap(region, translated_address, logical_size);
    if (!overlap)
      continue;

    const u32 shm_offset = region.shm_position + (overlap->begin - region.physical_address);
    u8* const target = m_logical_base + logical_address + (overlap->begin - translated_address);
    const u32 size = overlap->end - overlap->begin;

    const auto existing = m_page_table_mapped_entries.find(logical_address);
    if (existing != m_page_table_mapped_entries.end())
    {
      // Already mapped: only the protection may change.
      const LogicalMemoryView& view = existing->second;
      if (view.mapped_pointer == target && view.mapped_size == size &&
          !m_arena.ChangeMappingProtection(target, size, writeable))
      {
        PanicAlertFmt("Memory::AddPageTableMapping(): could not change the protection of "
                      "physical 0x{:08X} (size 0x{:08X}) at logical 0x{:08X}.",
                      overlap->begin, size, logical_address);
      }
      continue;
    }

    void* const view = m_arena.MapInMemoryRegion(shm_offset, size, target, writeable);
    if (view == nullptr)
    {
      PanicAlertFmt("Memory::AddPageTableMapping(): could not map physical 0x{:08X} "
                    "(size 0x{:08X}) at logical 0x{:08X} in the fastmem arena.",
                    overlap->begin, size, logical_address);
      continue;
    }
    m_page_table_mapped_entries.emplace(logical_address, LogicalMemoryView{view, size});
  }
}

void MemoryManager::RemovePageTableMappings(const std::set<u32>& mappings)
{
  if (m_host_page_type == HostPageType::SmallPages)
  {
    for (const u32 logical_address : mappings)
      RemoveHostPageTableMapping(logical_address);
  }
  else if (m_host_page_type == HostPageType::LargePages)
  {
    for (const u32 logical_address : mappings)
      RemoveLargePageTableMapping(logical_address);
  }
}

void MemoryManager::RemoveLargePageTableMapping(u32 logical_address)
{
  RemoveLargePageTableMapping(logical_address, m_large_readable_pages);
  RemoveLargePageTableMapping(logical_address, m_large_writeable_pages);
  RemoveHostPageTableMapping(logical_address & ~(m_page_size - 1));
}

void MemoryManager::RemoveLargePageTableMapping(u32 logical_address,
                                                std::map<u32, std::vector<u32>>& map)
{
  const auto found = map.find(logical_address & ~(m_page_size - 1));
  if (found == map.end())
    return;
  found->second[(logical_address & (m_page_size - 1)) / PowerPC::HW_PAGE_SIZE] = INVALID_MAPPING;
}

void MemoryManager::RemoveHostPageTableMapping(u32 logical_address)
{
  const auto found = m_page_table_mapped_entries.find(logical_address);
  if (found == m_page_table_mapped_entries.end())
    return;
  m_arena.UnmapFromMemoryRegion(found->second.mapped_pointer, found->second.mapped_size);
  m_page_table_mapped_entries.erase(found);
}

void MemoryManager::RemoveAllPageTableMappings()
{
  UnmapViews(m_page_table_mapped_entries);
  m_large_readable_pages.clear();
  m_large_writeable_pages.clear();
}

void MemoryManager::DoState(PointerWrap& p)
{
  // The memory layout is stored first. A state made with other memory sizes
  // (or another MMU setting) cannot be loaded into this session.
  const bool have_fake_vmem = m_fake_vmem != nullptr;
  const bool have_exram = m_exram != nullptr;
  const auto current = std::make_tuple(GetRamSize(), GetL1CacheSize(), have_fake_vmem,
                                       have_fake_vmem ? GetFakeVMemSize() : 0u, have_exram,
                                       have_exram ? GetExRamSize() : 0u);

  auto stored = current;
  p.Do(std::get<0>(stored));
  p.Do(std::get<1>(stored));
  p.Do(std::get<2>(stored));
  p.Do(std::get<3>(stored));
  p.Do(std::get<4>(stored));
  p.Do(std::get<5>(stored));

  if (stored != current)
  {
    Core::DisplayMessage("State is incompatible with current memory settings (MMU and/or memory "
                         "overrides). Aborting load state.",
                         3000);
    p.SetVerifyMode();
    return;
  }

  p.DoArray(m_ram, GetRamSize());
  p.DoArray(m_l1_cache, GetL1CacheSize());
  p.DoMarker("Memory RAM");
  if (have_fake_vmem)
    p.DoArray(m_fake_vmem, GetFakeVMemSize());
  p.DoMarker("Memory FakeVMEM");
  if (have_exram)
    p.DoArray(m_exram, GetExRamSize());
  p.DoMarker("Memory EXRAM");
}

void MemoryManager::Shutdown()
{
  ShutdownFastmemArena();

  m_is_initialized = false;
  for (const PhysicalMemoryRegion& region : m_physical_regions)
  {
    if (!region.active)
      continue;
    m_arena.ReleaseView(*region.out_pointer, region.size);
    *region.out_pointer = nullptr;
  }
  m_arena.ReleaseSHMSegment();
  m_mmio_mapping.reset();
  INFO_LOG_FMT(MEMMAP, "Emulated memory released");
}

void MemoryManager::ShutdownFastmemArena()
{
  if (!m_is_fastmem_arena_initialized)
    return;

  for (const PhysicalMemoryRegion& region : m_physical_regions)
  {
    if (region.active)
      m_arena.UnmapFromMemoryRegion(m_physical_base + region.physical_address, region.size);
  }
  UnmapViews(m_dbat_mapped_entries);
  UnmapViews(m_page_table_mapped_entries);

  m_arena.ReleaseMemoryRegion();

  m_large_readable_pages.clear();
  m_large_writeable_pages.clear();

  m_fastmem_arena = nullptr;
  m_fastmem_arena_size = 0;
  m_physical_base = nullptr;
  m_logical_base = nullptr;

  m_is_fastmem_arena_initialized = false;
}

void MemoryManager::Clear()
{
  const std::array<std::pair<u8*, u32>, 4> blocks{{
      {m_ram, GetRamSize()},
      {m_l1_cache, GetL1CacheSize()},
      {m_fake_vmem, GetFakeVMemSize()},
      {m_exram, GetExRamSize()},
  }};
  for (const auto& [pointer, size] : blocks)
  {
    if (pointer != nullptr)
      std::memset(pointer, 0, size);
  }
}

u8* MemoryManager::GetPointerForRange(u32 address, size_t size) const
{
  const std::span<u8> block = GetSpanForAddress(address);

  // An invalid start address has already raised an alert in GetSpanForAddress.
  if (block.data() == nullptr)
    return nullptr;

  if (block.size() < size)
  {
    PanicAlertFmt("Oversized range in GetPointerForRange. {:x} bytes at {:#010x}", size, address);
    return nullptr;
  }
  return block.data();
}

void MemoryManager::CopyFromEmu(void* data, u32 address, size_t size) const
{
  if (size == 0)
    return;

  const u8* const source = GetPointerForRange(address, size);
  if (source == nullptr)
  {
    PanicAlertFmt("Invalid range in CopyFromEmu. {:x} bytes from {:#010x}", size, address);
    return;
  }
  std::memcpy(data, source, size);
}

void MemoryManager::CopyToEmu(u32 address, const void* data, size_t size)
{
  if (size == 0)
    return;

  u8* const target = GetPointerForRange(address, size);
  if (target == nullptr)
  {
    PanicAlertFmt("Invalid range in CopyToEmu. {:x} bytes to {:#010x}", size, address);
    return;
  }
  std::memcpy(target, data, size);
}

void MemoryManager::Memset(u32 address, u8 value, size_t size)
{
  if (size == 0)
    return;

  u8* const target = GetPointerForRange(address, size);
  if (target == nullptr)
  {
    PanicAlertFmt("Invalid range in Memset. {:x} bytes at {:#010x}", size, address);
    return;
  }
  std::memset(target, value, size);
}

std::string MemoryManager::GetString(u32 em_address, size_t size)
{
  std::string text;
  if (size != 0)
  {
    // Fixed-size field: stops at the first NUL, if any.
    text.resize(size);
    CopyFromEmu(text.data(), em_address, size);
    text.resize(strnlen(text.data(), size));
    return text;
  }

  // NUL-terminated string, read one byte at a time.
  for (u8 c = Read_U8(em_address); c != 0; c = Read_U8(++em_address))
    text.push_back(static_cast<char>(c));
  return text;
}

std::span<u8> MemoryManager::GetSpanForAddress(u32 address) const
{
  // Devices see physical memory mirrored every 1 GiB.
  address &= 0x3FFFFFFF;

  if (address < GetRamSizeReal())
    return std::span<u8>(m_ram + address, GetRamSizeReal() - address);

  if (m_exram != nullptr && (address >> 28) == 0x1 && (address & 0x0FFFFFFF) < GetExRamSizeReal())
  {
    const u32 offset = address & GetExRamMask();
    return std::span<u8>(m_exram + offset, GetExRamSizeReal() - offset);
  }

  const auto& ppc_state = m_system.GetPPCState();
  PanicAlertFmt("Unknown Pointer {:#010x} PC {:#010x} LR {:#010x}", address, ppc_state.pc,
                LR(ppc_state));
  return {};
}

u8 MemoryManager::Read_U8(u32 address) const
{
  u8 value = 0;
  CopyFromEmu(&value, address, sizeof(value));
  return value;
}

u16 MemoryManager::Read_U16(u32 address) const
{
  u16 value = 0;
  CopyFromEmu(&value, address, sizeof(value));
  return Common::swap16(value);
}

u32 MemoryManager::Read_U32(u32 address) const
{
  u32 value = 0;
  CopyFromEmu(&value, address, sizeof(value));
  return Common::swap32(value);
}

u64 MemoryManager::Read_U64(u32 address) const
{
  u64 value = 0;
  CopyFromEmu(&value, address, sizeof(value));
  return Common::swap64(value);
}

u32 MemoryManager::Read_U32_Swap(u32 address) const
{
  u32 value = 0;
  CopyFromEmu(&value, address, sizeof(value));
  return value;
}

void MemoryManager::Write_U8(u8 value, u32 address)
{
  CopyToEmu(address, &value, sizeof(value));
}

void MemoryManager::Write_U16(u16 value, u32 address)
{
  const u16 big_endian = Common::swap16(value);
  CopyToEmu(address, &big_endian, sizeof(big_endian));
}

void MemoryManager::Write_U32(u32 value, u32 address)
{
  const u32 big_endian = Common::swap32(value);
  CopyToEmu(address, &big_endian, sizeof(big_endian));
}

void MemoryManager::Write_U64(u64 value, u32 address)
{
  const u64 big_endian = Common::swap64(value);
  CopyToEmu(address, &big_endian, sizeof(big_endian));
}

void MemoryManager::Write_U32_Swap(u32 value, u32 address)
{
  CopyToEmu(address, &value, sizeof(value));
}

void MemoryManager::Write_U64_Swap(u64 value, u32 address)
{
  CopyToEmu(address, &value, sizeof(value));
}

}  // namespace Memory
