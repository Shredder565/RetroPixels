// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/AddressSpace.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#include "Core/Core.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

namespace AddressSpace
{
// ---------------------------------------------------------------------------
// Default multi-byte accessors
// ---------------------------------------------------------------------------

u16 Accessors::ReadU16(const Core::CPUThreadGuard& guard, u32 address) const
{
  const u32 hi = ReadU8(guard, address);
  const u32 lo = ReadU8(guard, address + 1);
  return static_cast<u16>((hi << 8) | lo);
}

// Note: the default writers store the low part at the lower address, which is
// the opposite of the readers. Existing behaviour; kept as is.
void Accessors::WriteU16(const Core::CPUThreadGuard& guard, u32 address, u16 value)
{
  WriteU8(guard, address, static_cast<u8>(value));
  WriteU8(guard, address + 1, static_cast<u8>(value >> 8));
}

u32 Accessors::ReadU32(const Core::CPUThreadGuard& guard, u32 address) const
{
  const u32 hi = ReadU16(guard, address);
  const u32 lo = ReadU16(guard, address + 2);
  return (hi << 16) | lo;
}

void Accessors::WriteU32(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  WriteU16(guard, address, static_cast<u16>(value));
  WriteU16(guard, address + 2, static_cast<u16>(value >> 16));
}

u64 Accessors::ReadU64(const Core::CPUThreadGuard& guard, u32 address) const
{
  const u64 hi = ReadU32(guard, address);
  const u64 lo = ReadU32(guard, address + 4);
  return (hi << 32) | lo;
}

void Accessors::WriteU64(const Core::CPUThreadGuard& guard, u32 address, u64 value)
{
  WriteU32(guard, address, static_cast<u32>(value));
  WriteU32(guard, address + 4, static_cast<u32>(value >> 32));
}

float Accessors::ReadF32(const Core::CPUThreadGuard& guard, u32 address) const
{
  return std::bit_cast<float>(ReadU32(guard, address));
}

Accessors::iterator Accessors::begin() const
{
  return nullptr;
}

Accessors::iterator Accessors::end() const
{
  return nullptr;
}

std::optional<u32> Accessors::Search(const Core::CPUThreadGuard&, u32, const u8*, std::size_t,
                                     bool) const
{
  return std::nullopt;
}

Accessors::~Accessors() = default;

namespace
{
// ---------------------------------------------------------------------------
// Searching a flat block of host memory
// ---------------------------------------------------------------------------

std::optional<u32> SearchForward(const u8* data, u64 size, u64 from, const u8* needle, u64 length)
{
  if (length > size)
    return std::nullopt;
  for (u64 pos = from; pos + length <= size; ++pos)
  {
    if (std::memcmp(data + pos, needle, length) == 0)
      return static_cast<u32>(pos);
  }
  return std::nullopt;
}

// Looks for the closest match that starts before `from`. Matches starting
// below `length - 1` are never reported (long-standing behaviour of this
// search, kept for compatibility). Candidates that would run past the end of
// the block are skipped.
std::optional<u32> SearchBackward(const u8* data, u64 size, u64 from, const u8* needle,
                                  u64 length)
{
  if (from == 0)
    return std::nullopt;
  const s64 lowest = static_cast<s64>(length) - 1;
  for (s64 pos = static_cast<s64>(from) - 1; pos >= lowest && pos >= 0; --pos)
  {
    if (static_cast<u64>(pos) + length > size)
      continue;
    if (std::memcmp(data + pos, needle, length) == 0)
      return static_cast<u32>(pos);
  }
  return std::nullopt;
}

std::optional<u32> SearchBlock(const u8* data, u64 size, u64 from, const u8* needle, u64 length,
                               bool forward)
{
  return forward ? SearchForward(data, size, from, needle, length) :
                   SearchBackward(data, size, from, needle, length);
}

// ---------------------------------------------------------------------------
// Effective addresses, through the MMU
// ---------------------------------------------------------------------------

constexpr u32 PAGE_SIZE = 0x1000;
constexpr u32 PAGE_MASK = ~(PAGE_SIZE - 1);

class EffectiveSpace final : public Accessors
{
public:
  // Every access goes through the MMU's host accessors.
  template <typename T>
  static T Get(const Core::CPUThreadGuard& guard, u32 address)
  {
    return PowerPC::MMU::HostRead<T>(guard, address);
  }
  template <typename T>
  static void Put(const Core::CPUThreadGuard& guard, u32 address, T value)
  {
    PowerPC::MMU::HostWrite<T>(guard, value, address);
  }

  bool IsValidAddress(const Core::CPUThreadGuard& guard, u32 address) const override
  {
    return PowerPC::MMU::HostIsRAMAddress(guard, address);
  }
  u8 ReadU8(const Core::CPUThreadGuard& g, u32 a) const override { return Get<u8>(g, a); }
  u16 ReadU16(const Core::CPUThreadGuard& g, u32 a) const override { return Get<u16>(g, a); }
  u32 ReadU32(const Core::CPUThreadGuard& g, u32 a) const override { return Get<u32>(g, a); }
  u64 ReadU64(const Core::CPUThreadGuard& g, u32 a) const override { return Get<u64>(g, a); }
  float ReadF32(const Core::CPUThreadGuard& g, u32 a) const override { return Get<float>(g, a); }
  void WriteU8(const Core::CPUThreadGuard& g, u32 a, u8 v) override { Put(g, a, v); }
  void WriteU16(const Core::CPUThreadGuard& g, u32 a, u16 v) override { Put(g, a, v); }
  void WriteU32(const Core::CPUThreadGuard& g, u32 a, u32 v) override { Put(g, a, v); }
  void WriteU64(const Core::CPUThreadGuard& g, u32 a, u64 v) override { Put(g, a, v); }

  // Scans address by address. Inside a mapped page every start address is
  // tried; an unmapped page is skipped as a whole, landing on the first byte
  // of the next page in the search direction. Forward searches stop when the
  // address wraps to page 0, backward ones when it wraps to the top page.
  std::optional<u32> Search(const Core::CPUThreadGuard& guard, u32 haystack_start,
                            const u8* needle_start, std::size_t needle_size,
                            bool forward) const override
  {
    const u32 stop_page = forward ? 0x00000000 : 0xfffff000;
    const u32 page_end_offset = forward ? 0x000 : 0xfff;
    const u32 step = forward ? 1u : ~0u;
    const u32 page_step = forward ? PAGE_SIZE : 0u - PAGE_SIZE;

    u32 address = haystack_start;
    while (true)
    {
      if (PowerPC::MMU::HostIsRAMAddress(guard, address))
      {
        // Try every start address up to the edge of this page.
        do
        {
          if (MatchesAt(guard, address, needle_start, needle_size))
            return address;
          address += step;
        } while ((address & ~PAGE_MASK) != page_end_offset);
      }
      else
      {
        address = (address + page_step) & PAGE_MASK;
      }

      if ((address & PAGE_MASK) == stop_page)
        return std::nullopt;
    }
  }

private:
  // Compares the needle with guest memory starting at `address`, one guest
  // page at a time. Only MEM1/MEM2 backing memory is considered.
  static bool MatchesAt(const Core::CPUThreadGuard& guard, u32 address, const u8* needle,
                        std::size_t remaining)
  {
    auto& system = guard.GetSystem();
    auto& memory = system.GetMemory();
    auto& mmu = system.GetMMU();

    u32 page = address & PAGE_MASK;
    u32 offset = address & ~PAGE_MASK;
    while (true)
    {
      if (!PowerPC::MMU::HostIsRAMAddress(guard, page))
        return false;
      const std::optional<u32> physical = mmu.GetTranslatedAddress(page);
      if (!physical)
        return false;
      // Leave out the locked-L1 area at 0xE0000000.
      if (*physical >= 0xE0000000)
        return false;

      const std::size_t chunk = std::min<std::size_t>(PAGE_SIZE - offset, remaining);
      const u8* host = memory.GetPointerForRange(*physical + offset, chunk);
      if (host == nullptr || std::memcmp(needle, host, chunk) != 0)
        return false;

      remaining -= chunk;
      needle += chunk;
      offset = 0;
      page += PAGE_SIZE;
      if (remaining == 0 || page == 0)
        return remaining == 0;
    }
  }
};

// ---------------------------------------------------------------------------
// GameCube ARAM
// ---------------------------------------------------------------------------

class AramSpace final : public Accessors
{
public:
  static constexpr u32 SIZE = 16 * 1024 * 1024;

  bool IsValidAddress(const Core::CPUThreadGuard& guard, u32 address) const override
  {
    return !guard.GetSystem().IsWii() && address < SIZE;
  }
  u8 ReadU8(const Core::CPUThreadGuard& guard, u32 address) const override
  {
    return guard.GetSystem().GetDSP().GetARAMPtr()[address];
  }
  void WriteU8(const Core::CPUThreadGuard& guard, u32 address, u8 value) override
  {
    guard.GetSystem().GetDSP().GetARAMPtr()[address] = value;
  }

  iterator begin() const override { return Core::System::GetInstance().GetDSP().GetARAMPtr(); }
  iterator end() const override { return begin() + SIZE; }

  std::optional<u32> Search(const Core::CPUThreadGuard& guard, u32 haystack_offset,
                            const u8* needle_start, std::size_t needle_size,
                            bool forward) const override
  {
    if (!IsValidAddress(guard, haystack_offset))
      return std::nullopt;
    return SearchBlock(begin(), SIZE, haystack_offset, needle_start, needle_size, forward);
  }
};

// ---------------------------------------------------------------------------
// A block of emulated RAM owned by the MemoryManager. The block pointer is
// read through each time because it changes when memory is (re)initialised.
// ---------------------------------------------------------------------------

class RamBlockSpace final : public Accessors
{
public:
  RamBlockSpace() = default;
  RamBlockSpace(u8** block, u32 size) : m_block(block), m_size(size) {}

  bool IsValidAddress(const Core::CPUThreadGuard&, u32 address) const override
  {
    return *m_block != nullptr && address < m_size;
  }
  u8 ReadU8(const Core::CPUThreadGuard&, u32 address) const override
  {
    return (*m_block)[address];
  }
  void WriteU8(const Core::CPUThreadGuard&, u32 address, u8 value) override
  {
    (*m_block)[address] = value;
  }

  iterator begin() const override { return *m_block; }
  iterator end() const override { return *m_block ? *m_block + m_size : nullptr; }

  std::optional<u32> Search(const Core::CPUThreadGuard& guard, u32 haystack_offset,
                            const u8* needle_start, std::size_t needle_size,
                            bool forward) const override
  {
    // Both the first and the last byte of a match at the start offset must
    // be inside the block.
    const u32 last = haystack_offset + static_cast<u32>(needle_size) - 1;
    if (!IsValidAddress(guard, haystack_offset) || !IsValidAddress(guard, last))
      return std::nullopt;
    return SearchBlock(*m_block, m_size, haystack_offset, needle_start, needle_size, forward);
  }

private:
  u8** m_block = nullptr;
  u32 m_size = 0;
};

// ---------------------------------------------------------------------------
// Several spaces placed at fixed offsets (the physical address map)
// ---------------------------------------------------------------------------

struct Region
{
  u32 base;
  Accessors* space;
};

class CombinedSpace final : public Accessors
{
public:
  CombinedSpace() = default;
  CombinedSpace(std::initializer_list<Region> regions) : m_regions(regions) {}

  bool IsValidAddress(const Core::CPUThreadGuard& guard, u32 address) const override
  {
    return Find(guard, address) != nullptr;
  }
  u8 ReadU8(const Core::CPUThreadGuard& guard, u32 address) const override
  {
    const Region* region = Find(guard, address);
    return region ? region->space->ReadU8(guard, address - region->base) : 0;
  }
  void WriteU8(const Core::CPUThreadGuard& guard, u32 address, u8 value) override
  {
    if (const Region* region = Find(guard, address))
      region->space->WriteU8(guard, address - region->base, value);
  }

  // Searches each region that contains the start offset, in order.
  std::optional<u32> Search(const Core::CPUThreadGuard& guard, u32 haystack_offset,
                            const u8* needle_start, std::size_t needle_size,
                            bool forward) const override
  {
    for (const Region& region : m_regions)
    {
      const u32 local = haystack_offset - region.base;
      if (!region.space->IsValidAddress(guard, local))
        continue;
      if (const auto found =
              region.space->Search(guard, local, needle_start, needle_size, forward))
      {
        return *found + region.base;
      }
    }
    return std::nullopt;
  }

private:
  const Region* Find(const Core::CPUThreadGuard& guard, u32 address) const
  {
    for (const Region& region : m_regions)
    {
      if (region.space->IsValidAddress(guard, address - region.base))
        return &region;
    }
    return nullptr;
  }

  std::vector<Region> m_regions;
};

// ---------------------------------------------------------------------------
// An empty space
// ---------------------------------------------------------------------------

class EmptySpace final : public Accessors
{
public:
  bool IsValidAddress(const Core::CPUThreadGuard&, u32) const override { return false; }
  u8 ReadU8(const Core::CPUThreadGuard&, u32) const override { return 0; }
  void WriteU8(const Core::CPUThreadGuard&, u32, u8) override {}
};

struct Spaces
{
  EffectiveSpace effective;
  AramSpace aram;
  RamBlockSpace mem1;
  RamBlockSpace mem2;
  RamBlockSpace fake_vmem;
  CombinedSpace physical_gc;
  CombinedSpace physical_wii;
  EmptySpace empty;
  bool ready = false;
};

Spaces s_spaces;
}  // namespace

Accessors* GetAccessors(Type address_space)
{
  if (!s_spaces.ready)
    return &s_spaces.empty;

  const bool is_wii = Core::System::GetInstance().IsWii();
  switch (address_space)
  {
  case Type::Effective:
    return &s_spaces.effective;
  case Type::Physical:
    return is_wii ? &s_spaces.physical_wii : &s_spaces.physical_gc;
  case Type::Mem1:
    return &s_spaces.mem1;
  case Type::Mem2:
    return is_wii ? static_cast<Accessors*>(&s_spaces.mem2) : &s_spaces.empty;
  case Type::Auxiliary:
    return is_wii ? static_cast<Accessors*>(&s_spaces.empty) : &s_spaces.aram;
  case Type::Fake:
    return &s_spaces.fake_vmem;
  }
  return &s_spaces.empty;
}

void Init()
{
  auto& memory = Core::System::GetInstance().GetMemory();

  s_spaces.mem1 = RamBlockSpace(&memory.GetRAM(), memory.GetRamSizeReal());
  s_spaces.mem2 = RamBlockSpace(&memory.GetEXRAM(), memory.GetExRamSizeReal());
  s_spaces.fake_vmem = RamBlockSpace(&memory.GetFakeVMEM(), memory.GetFakeVMemSize());
  s_spaces.physical_gc = CombinedSpace{{0x00000000, &s_spaces.mem1}};
  s_spaces.physical_wii =
      CombinedSpace{{0x00000000, &s_spaces.mem1}, {0x10000000, &s_spaces.mem2}};
  s_spaces.ready = true;
}

void Shutdown()
{
  s_spaces.ready = false;
}

}  // namespace AddressSpace
