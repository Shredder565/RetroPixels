// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/CachedInterpreter/CachedInterpreterBlockCache.h"

#include "Core/PowerPC/CachedInterpreter/CachedInterpreterEmitter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"

CachedInterpreterBlockCache::CachedInterpreterBlockCache(JitBase& jit) : JitBaseBlockCache{jit}
{
}

void CachedInterpreterBlockCache::Init()
{
  JitBaseBlockCache::Init();
  m_pending_free.clear();
}

void CachedInterpreterBlockCache::ClearRangesToFree()
{
  m_pending_free.clear();
}

void CachedInterpreterBlockCache::DestroyBlock(JitBlock& block)
{
  JitBaseBlockCache::DestroyBlock(block);
  if (block.near_end != block.near_begin)
    m_pending_free.emplace_back(block.near_begin, block.near_end);
}

void CachedInterpreterBlockCache::WriteLinkBlock(const JitBlock::LinkData&, const JitBlock*)
{
  // Blocks always return to the dispatcher, so there is nothing to patch.
}

void CachedInterpreterBlockCache::WriteDestroyBlock(const JitBlock& block)
{
  // Overwrite the entry so a stale pointer to the block traps instead of running old code.
  CachedInterpreterEmitter emitter(block.normalEntry, block.near_end);
  emitter.Write(CachedInterpreterEmitter::PoisonCallback);
}
