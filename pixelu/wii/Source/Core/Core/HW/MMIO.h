// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <string>
#include <tuple>
#include <type_traits>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/MMIOHandlers.h"

namespace Core
{
class System;
}

namespace MMIO
{
// Register blocks, each 64 KiB:
//  - 0x0C00xxxx: Flipper/Hollywood registers (CP, PE, VI, PI, MI, DSP, DI, SI, EXI, AI)
//  - 0x0D00xxxx: Hollywood-only registers (IPC, GPIO, ...) and Wii aliases of DI/SI/EXI/AI
//  - 0x0D80xxxx: the same registers as 0x0D00xxxx
// so internally there are two blocks, told apart by address bit 24.
enum Block
{
  GC_BLOCK = 0,
  WII_BLOCK = 1,

  NUM_BLOCKS
};
const u32 BLOCK_SIZE = 0x10000;
const u32 NUM_MMIOS = NUM_BLOCKS * BLOCK_SIZE;

// Whether a physical address is a register access. The write-gather pipe
// address is excluded: GPFifo handles it separately.
inline bool IsMMIOAddress(u32 address, bool is_wii)
{
  if (address == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS)
    return false;

  const u32 block = address & 0xFFFF0000;
  if (block == 0x0C000000)
    return true;
  return is_wii && (block == 0x0D000000 || block == 0x0D800000);
}

// Index of an MMIO address in the handler tables: the block number (bit 24)
// above the low 16 address bits.
inline u32 UniqueID(u32 address)
{
  DEBUG_ASSERT_MSG(MEMMAP,
                   (address & 0xFFFF0000) == 0x0C000000 || (address & 0xFFFF0000) == 0x0D000000 ||
                       (address & 0xFFFF0000) == 0x0D800000,
                   "UniqueID of non-MMIO address {:08x}", address);

  return (((address >> 24) & 1) << 16) | (address & 0xFFFF);
}

// Helpers for registers that expose the two halves of a 32-bit variable as
// separate 16-bit registers (host is little-endian).
namespace Utils
{
// Lock-free std::atomic<u32> has the layout of a plain u32.
inline u32* PlainValue(std::atomic<u32>* ptr)
{
  static_assert(std::atomic<u32>::is_always_lock_free && sizeof(std::atomic<u32>) == sizeof(u32));
  return std::bit_cast<u32*>(ptr);
}
inline u16* LowPart(u32* ptr) { return reinterpret_cast<u16*>(ptr); }
inline u16* HighPart(u32* ptr) { return reinterpret_cast<u16*>(ptr) + 1; }
inline u16* LowPart(std::atomic<u32>* ptr) { return LowPart(PlainValue(ptr)); }
inline u16* HighPart(std::atomic<u32>* ptr) { return HighPart(PlainValue(ptr)); }
}  // namespace Utils

// The register map: one read and one write handler per address and size.
class Mapping
{
public:
  // Installs handling methods; the slot takes ownership of them.
  template <typename Unit>
  void RegisterRead(u32 addr, ReadHandlingMethod<Unit>* read)
  {
    GetHandlerForRead<Unit>(addr).ResetMethod(read);
  }
  template <typename Unit>
  void RegisterWrite(u32 addr, WriteHandlingMethod<Unit>* write)
  {
    GetHandlerForWrite<Unit>(addr).ResetMethod(write);
  }
  template <typename Unit>
  void Register(u32 addr, ReadHandlingMethod<Unit>* read, WriteHandlingMethod<Unit>* write)
  {
    GetHandlerForRead<Unit>(addr).ResetMethod(read);
    GetHandlerForWrite<Unit>(addr).ResetMethod(write);
  }

  // Performs an access (used by the MMU slow paths and by emulator code).
  template <typename Unit>
  Unit Read(Core::System& system, u32 addr)
  {
    return GetHandlerForRead<Unit>(addr).Read(system, addr);
  }

  template <typename Unit>
  void Write(Core::System& system, u32 addr, Unit val)
  {
    GetHandlerForWrite<Unit>(addr).Write(system, addr, val);
  }

  // The handler slot that serves an address (used by the JIT to inline reads).
  template <typename Unit>
  ReadHandler<Unit>& GetHandlerForRead(u32 addr)
  {
    return Table<ReadHandler<Unit>>()[UniqueID(addr) / sizeof(Unit)];
  }

  template <typename Unit>
  WriteHandler<Unit>& GetHandlerForWrite(u32 addr)
  {
    return Table<WriteHandler<Unit>>()[UniqueID(addr) / sizeof(Unit)];
  }

private:
  template <typename Handler, typename Unit>
  using Slots = std::array<Handler, NUM_MMIOS / sizeof(Unit)>;

  // Picks the table for a handler type; only the six handler types exist.
  template <typename Handler>
  auto& Table()
  {
    if constexpr (std::is_same_v<Handler, ReadHandler<u8>>)
      return m_read8;
    else if constexpr (std::is_same_v<Handler, ReadHandler<u16>>)
      return m_read16;
    else if constexpr (std::is_same_v<Handler, ReadHandler<u32>>)
      return m_read32;
    else if constexpr (std::is_same_v<Handler, WriteHandler<u8>>)
      return m_write8;
    else if constexpr (std::is_same_v<Handler, WriteHandler<u16>>)
      return m_write16;
    else if constexpr (std::is_same_v<Handler, WriteHandler<u32>>)
      return m_write32;
    else
      static_assert(!sizeof(Handler), "MMIO accesses are 8, 16 or 32 bits wide");
  }

  // An access of size N bytes uses slot UniqueID / N, so wider accesses need fewer slots.
  Slots<ReadHandler<u8>, u8> m_read8;
  Slots<ReadHandler<u16>, u16> m_read16;
  Slots<ReadHandler<u32>, u32> m_read32;
  Slots<WriteHandler<u8>, u8> m_write8;
  Slots<WriteHandler<u16>, u16> m_write16;
  Slots<WriteHandler<u32>, u32> m_write32;
};

// 64-bit register accesses do not exist; these only let generic callers compile.
template <>
inline u64 Mapping::Read<u64>(Core::System&, u32)
{
  DEBUG_ASSERT(false);
  return 0;
}
template <>
inline void Mapping::Write<u64>(Core::System&, u32, u64)
{
  DEBUG_ASSERT(false);
}
}  // namespace MMIO
