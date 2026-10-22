// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Jit64Common/EmuCodeBlock.h"

struct TrampolineInfo;

// Trampolines are slow-path copies of fastmem accesses that faulted. With the
// MMU on far more accesses fault, so the region is larger.
constexpr size_t TRAMPOLINE_CODE_SIZE = 1024 * 1024 * 8;
constexpr size_t TRAMPOLINE_CODE_SIZE_MMU = 1024 * 1024 * 32;

// Minimum length of a fastmem access: room for the patched-in JMP rel32.
constexpr int BACKPATCH_SIZE = 5;

class TrampolineCache : public EmuCodeBlock
{
public:
  explicit TrampolineCache(Jit64& jit) : EmuCodeBlock(jit) {}

  // Emits a slow-path version of the access described by `info` that jumps
  // back to the instruction after it, and returns its entry point.
  const u8* GenerateTrampoline(const TrampolineInfo& info);
  void ClearCodeSpace();

private:
  const u8* GenerateReadTrampoline(const TrampolineInfo& info);
  const u8* GenerateWriteTrampoline(const TrampolineInfo& info);
};
