// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// Uniform byte-level views of the emulated address spaces, used by the
// debugger's memory view and search.
namespace AddressSpace
{
enum class Type
{
  Effective,
  Auxiliary,
  Physical,
  Mem1,
  Mem2,
  Fake,
};

struct Accessors
{
  using iterator = const u8*;

  virtual bool IsValidAddress(const Core::CPUThreadGuard& guard, u32 address) const = 0;
  virtual u8 ReadU8(const Core::CPUThreadGuard& guard, u32 address) const = 0;
  virtual void WriteU8(const Core::CPUThreadGuard& guard, u32 address, u8 value) = 0;

  // The defaults below are built from ReadU8/WriteU8. Reads are big-endian.
  virtual u16 ReadU16(const Core::CPUThreadGuard& guard, u32 address) const;
  virtual void WriteU16(const Core::CPUThreadGuard& guard, u32 address, u16 value);
  virtual u32 ReadU32(const Core::CPUThreadGuard& guard, u32 address) const;
  virtual void WriteU32(const Core::CPUThreadGuard& guard, u32 address, u32 value);
  virtual u64 ReadU64(const Core::CPUThreadGuard& guard, u32 address) const;
  virtual void WriteU64(const Core::CPUThreadGuard& guard, u32 address, u64 value);
  virtual float ReadF32(const Core::CPUThreadGuard& guard, u32 address) const;

  // Contiguous host memory behind the space, if there is any (else null).
  virtual iterator begin() const;
  virtual iterator end() const;

  // Finds the needle starting at or after (forward) / before (backward) the
  // given offset.
  virtual std::optional<u32> Search(const Core::CPUThreadGuard& guard, u32 haystack_offset,
                                    const u8* needle_start, std::size_t needle_size,
                                    bool forward) const;
  virtual ~Accessors();
};

// Never null; before Init() every space is empty.
Accessors* GetAccessors(Type address_space);

void Init();
void Shutdown();

}  // namespace AddressSpace
