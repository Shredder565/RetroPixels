// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/CachedInterpreter/CachedInterpreterEmitter.h"

#include <cstdint>
#include <cstring>

#include "Common/Assert.h"

void CachedInterpreterEmitter::Write(AnyCallback callback, const void* operands, std::size_t size)
{
  DEBUG_ASSERT(reinterpret_cast<std::uintptr_t>(m_code) % alignof(AnyCallback) == 0);
  const std::size_t record_size = sizeof(AnyCallback) + size;
  // Keep the end of the range out of reach so a full region is never written right up to it.
  if (m_code + record_size >= m_code_end)
  {
    m_write_failed = true;
    return;
  }
  std::memcpy(m_code, &callback, sizeof(AnyCallback));
  if (size != 0)
    std::memcpy(m_code + sizeof(AnyCallback), operands, size);
  m_code += record_size;
}

s32 CachedInterpreterEmitter::PoisonCallback(PowerPC::PowerPCState&, const void*)
{
  ASSERT_MSG(DYNA_REC, false, "Cached interpreter executed poisoned (unused or freed) code.");
  return 0;
}

void CachedInterpreterCodeBlock::PoisonMemory()
{
  DEBUG_ASSERT(reinterpret_cast<std::uintptr_t>(region) % alignof(AnyCallback) == 0);
  DEBUG_ASSERT(region_size % sizeof(AnyCallback) == 0);
  const AnyCallback poison = AnyCallbackCast(PoisonCallback);
  auto* const slots = reinterpret_cast<AnyCallback*>(region);
  const std::size_t count = region_size / sizeof(AnyCallback);
  for (std::size_t i = 0; i < count; ++i)
    slots[i] = poison;
}
