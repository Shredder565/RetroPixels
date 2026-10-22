// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/MemoryInterface.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Core/HW/MMIO.h"

namespace MemoryInterface
{
namespace
{
// Savestate slot of each halfword register. The savestate holds:
//   slots 0-7    protection regions 0-3 (first page, last page)
//   slot  8      protection type
//   slots 9, 10  interrupt mask, interrupt flags
//   slot  11     unknown register at 0x20
//   slots 12, 13 protection address, low then high halfword
//   slots 14-33  access counters 0-9, each low then high halfword
//   slot  34     unknown register at 0x5A
//   slot  35     padding
// The register at 0x22 holds the high halfword of the protection address and
// the one at 0x24 the low halfword; each counter likewise has its high
// halfword at the lower address.
struct RegisterSlot
{
  u32 offset;
  u32 slot;
};

constexpr RegisterSlot FIXED_REGISTERS[] = {
    {0x00, 0},  {0x02, 1},  {0x04, 2},  {0x06, 3},  {0x08, 4},  {0x0A, 5},
    {0x0C, 6},  {0x0E, 7},  {0x10, 8},  {0x1C, 9},  {0x1E, 10}, {0x20, 11},
    {0x22, 13}, {0x24, 12}, {0x5A, 34},
};

constexpr u32 COUNTER_BASE = 0x32;
constexpr u32 COUNTER_FIRST_SLOT = 14;
constexpr u32 NUM_COUNTERS = 10;
}  // namespace

MemoryInterfaceManager::MemoryInterfaceManager() = default;

MemoryInterfaceManager::~MemoryInterfaceManager() = default;

void MemoryInterfaceManager::Init()
{
  m_slots.fill(0);
}

void MemoryInterfaceManager::Shutdown()
{
  Init();
}

void MemoryInterfaceManager::DoState(PointerWrap& p)
{
  p.DoArray(m_slots);
}

void MemoryInterfaceManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  const auto map_halfword = [&](u32 offset, u32 slot) {
    mmio->Register(base | offset, MMIO::DirectRead<u16>(&m_slots[slot]),
                   MMIO::DirectWrite<u16>(&m_slots[slot]));
  };

  for (const RegisterSlot& reg : FIXED_REGISTERS)
    map_halfword(reg.offset, reg.slot);

  for (u32 counter = 0; counter < NUM_COUNTERS; ++counter)
  {
    const u32 offset = COUNTER_BASE + 4 * counter;
    const u32 low_slot = COUNTER_FIRST_SLOT + 2 * counter;
    map_halfword(offset, low_slot + 1);
    map_halfword(offset + 2, low_slot);
  }

  // Word accesses are split into two halfword accesses.
  for (u32 offset = 0; offset < 0x1000; offset += 4)
  {
    mmio->Register(base | offset,
                   MMIO::ReadToSmaller<u32>(mmio, base | offset, base | (offset + 2)),
                   MMIO::WriteToSmaller<u32>(mmio, base | offset, base | (offset + 2)));
  }
}

}  // namespace MemoryInterface
