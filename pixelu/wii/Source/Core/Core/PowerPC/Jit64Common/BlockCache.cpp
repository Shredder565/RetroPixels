// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64Common/BlockCache.h"

#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"

namespace
{
// Byte length of CALL rel32.
constexpr int CALL_LEN = 5;

// When the target starts this close after the exit, the exit is padded with
// NOPs and falls through. The slack covers block-entry alignment padding.
constexpr s64 FALLTHROUGH_LIMIT = Gen::XEmitter::NEAR_JMP_LEN + 3;

void RecordRange(std::vector<std::pair<u8*, u8*>>& ranges, u8* begin, u8* end)
{
  if (begin != end)
    ranges.emplace_back(begin, end);
}
}  // namespace

JitBlockCache::JitBlockCache(JitBase& jit) : JitBaseBlockCache{jit}
{
}

void JitBlockCache::Init()
{
  JitBaseBlockCache::Init();
  ClearRangesToFree();
}

void JitBlockCache::WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest)
{
  u8* const exit = source.exitPtrs;
  const u8* const target =
      dest != nullptr ? dest->normalEntry : m_jit.GetAsmRoutines()->dispatcher_no_timing_check;

  if (source.call)
  {
    Gen::XEmitter emitter(exit, exit + CALL_LEN);
    emitter.CALL(target);
    return;
  }

  const s64 distance = target - exit;
  if (distance > 0 && distance <= FALLTHROUGH_LIMIT)
  {
    const int gap = static_cast<int>(distance);
    Gen::XEmitter emitter(exit, exit + gap);
    emitter.NOP(static_cast<size_t>(gap));
    return;
  }

  // Always the 5-byte form: the exit was reserved at that size, and a short
  // JMP would leave stale bytes behind that confuse disassemblers.
  Gen::XEmitter emitter(exit, exit + Gen::XEmitter::NEAR_JMP_LEN);
  emitter.JMP(target, true);
}

void JitBlockCache::WriteDestroyBlock(const JitBlock& block)
{
  // Code may still be running further inside the block, so only the entry is
  // poisoned; a later jump to it traps.
  Gen::XEmitter emitter(block.normalEntry, block.normalEntry + 1);
  emitter.INT3();
}

void JitBlockCache::DestroyBlock(JitBlock& block)
{
  JitBaseBlockCache::DestroyBlock(block);
  RecordRange(m_freed_near, block.near_begin, block.near_end);
  RecordRange(m_freed_far, block.far_begin, block.far_end);
}

const std::vector<std::pair<u8*, u8*>>& JitBlockCache::GetRangesToFreeNear() const
{
  return m_freed_near;
}

const std::vector<std::pair<u8*, u8*>>& JitBlockCache::GetRangesToFreeFar() const
{
  return m_freed_far;
}

void JitBlockCache::ClearRangesToFree()
{
  m_freed_near.clear();
  m_freed_far.clear();
}
