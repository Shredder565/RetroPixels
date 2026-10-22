// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

// Block cache for the cached interpreter. Blocks are never linked to each other. When a block is
// destroyed, its code range is remembered so the interpreter can reuse it the next time it
// generates code. It cannot be reused right away, because the destroyed block may still be
// running.
class CachedInterpreterBlockCache final : public JitBaseBlockCache
{
public:
  explicit CachedInterpreterBlockCache(JitBase& jit);

  void Init() override;

  void DestroyBlock(JitBlock& block) override;

  // Code ranges of destroyed blocks, as [begin, end) pairs, not yet handed back for reuse.
  const std::vector<std::pair<u8*, u8*>>& GetRangesToFree() const { return m_pending_free; }
  void ClearRangesToFree();

private:
  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override;
  void WriteDestroyBlock(const JitBlock& block) override;

  std::vector<std::pair<u8*, u8*>> m_pending_free;
};
