// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"

namespace Common
{
// A block of memory that an emitter (T, for example Gen::XEmitter) writes code or data into.
// Deriving from CodeBlock<Emitter> makes the emitter's methods available directly.
//
// A block can hand out the tail of its space to "child" blocks. The parent owns the whole
// allocation; children only borrow a slice of it and must not free it. region_size is the part
// the parent itself still uses, total_region_size is the whole allocation.
template <class T, bool executable = true>
class CodeBlock : public T
{
private:
  // Fills the whole usable region with a byte pattern that traps when executed.
  virtual void PoisonMemory() = 0;

protected:
  u8* region = nullptr;
  size_t region_size = 0;
  size_t total_region_size = 0;

  bool m_is_child = false;
  std::vector<CodeBlock*> m_children;

public:
  CodeBlock() = default;
  virtual ~CodeBlock()
  {
    if (region != nullptr)
      FreeCodeSpace();
  }

  CodeBlock(const CodeBlock&) = delete;
  CodeBlock& operator=(const CodeBlock&) = delete;
  CodeBlock(CodeBlock&&) = delete;
  CodeBlock& operator=(CodeBlock&&) = delete;

  // Must be called before anything is emitted.
  void AllocCodeSpace(size_t size)
  {
    total_region_size = size;
    region_size = size;
    void* memory;
    if constexpr (executable)
      memory = Common::AllocateExecutableMemory(size);
    else
      memory = Common::AllocateMemoryPages(size);
    region = static_cast<u8*>(memory);
    T::SetCodePtr(region, region + size);
  }

  // Overwrites everything with trap bytes and starts emitting from the beginning again.
  void ClearCodeSpace()
  {
    PoisonMemory();
    ResetCodePtr();
  }

  // Releases the allocation (parents only). Children that borrowed from it become empty.
  void FreeCodeSpace()
  {
    ASSERT(!m_is_child);
    Common::FreeMemoryPages(region, total_region_size);
    const auto forget_region = [](CodeBlock& block) {
      block.region = nullptr;
      block.region_size = 0;
      block.total_region_size = 0;
    };
    forget_region(*this);
    for (CodeBlock* child : m_children)
      forget_region(*child);
  }

  bool IsInSpace(const u8* ptr) const { return ptr >= region && ptr < region + region_size; }
  bool IsInSpaceOrChildSpace(const u8* ptr) const
  {
    return ptr >= region && ptr < region + total_region_size;
  }

  void WriteProtect(bool allow_execute)
  {
    Common::WriteProtectMemory(region, region_size, allow_execute);
  }
  void UnWriteProtect(bool allow_execute)
  {
    Common::UnWriteProtectMemory(region, region_size, allow_execute);
  }

  void ResetCodePtr() { T::SetCodePtr(region, region + region_size); }

  size_t GetSpaceLeft() const
  {
    const size_t used = static_cast<size_t>(T::GetCodePtr() - region);
    ASSERT(used < region_size);
    return region_size - used;
  }

  // True when less room is left than any single block could need.
  bool IsAlmostFull() const { return GetSpaceLeft() < 0x10000; }

  bool HasChildren() const { return total_region_size != region_size; }

  // Takes child_size bytes off the end of this block's usable space and returns their start.
  u8* AllocChildCodeSpace(size_t child_size)
  {
    ASSERT_MSG(DYNA_REC, child_size <= GetSpaceLeft(), "Not enough space left for a child block.");
    region_size -= child_size;
    ResetCodePtr();
    return region + region_size;
  }

  // Gives child_size bytes off the end of this block to child, which then emits into them.
  void AddChildCodeSpace(CodeBlock* child, size_t child_size)
  {
    u8* const start = AllocChildCodeSpace(child_size);
    child->m_is_child = true;
    child->region = start;
    child->region_size = child_size;
    child->total_region_size = child_size;
    child->ResetCodePtr();
    m_children.push_back(child);
  }
};
}  // namespace Common
