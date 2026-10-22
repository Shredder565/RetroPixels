// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}
namespace MMIO
{
class Mapping;
}
class PointerWrap;

// The memory interface (MI, 0x0C004000): MEM1 protection regions, the
// protection interrupt registers and the access counters. Nothing here
// affects emulation; software reads back what it wrote.
namespace MemoryInterface
{
class MemoryInterfaceManager
{
public:
  MemoryInterfaceManager();
  MemoryInterfaceManager(const MemoryInterfaceManager&) = delete;
  MemoryInterfaceManager(MemoryInterfaceManager&&) = delete;
  MemoryInterfaceManager& operator=(const MemoryInterfaceManager&) = delete;
  MemoryInterfaceManager& operator=(MemoryInterfaceManager&&) = delete;
  ~MemoryInterfaceManager();

  void Init();
  void Shutdown();

  void DoState(PointerWrap& p);
  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

private:
  // The halfword registers in savestate order (the .cpp lists the slot of
  // each register). The last slot is unused.
  static constexpr size_t NUM_SLOTS = 36;
  std::array<u16, NUM_SLOTS> m_slots{};
};
}  // namespace MemoryInterface
