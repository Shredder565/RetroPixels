// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <utility>
#include <vector>

#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

// Jit64's block cache: knows how to patch x86 exits and entry points, and
// remembers the code ranges of destroyed blocks so the JIT can reuse them.
class JitBlockCache : public JitBaseBlockCache
{
public:
  explicit JitBlockCache(JitBase& jit);

  void Init() override;

  void DestroyBlock(JitBlock& block) override;

  const std::vector<std::pair<u8*, u8*>>& GetRangesToFreeNear() const;
  const std::vector<std::pair<u8*, u8*>>& GetRangesToFreeFar() const;

  void ClearRangesToFree();

private:
  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override;
  void WriteDestroyBlock(const JitBlock& block) override;

  std::vector<std::pair<u8*, u8*>> m_freed_near;
  std::vector<std::pair<u8*, u8*>> m_freed_far;
};
