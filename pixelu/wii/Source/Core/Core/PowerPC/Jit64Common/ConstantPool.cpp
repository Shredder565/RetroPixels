// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64Common/ConstantPool.h"

#include <cstdint>
#include <cstring>

#include "Common/Assert.h"

ConstantPool::ConstantPool() = default;

ConstantPool::~ConstantPool() = default;

void ConstantPool::Init(void* memory, size_t size)
{
  m_begin = static_cast<u8*>(memory);
  m_end = m_begin + size;
  Clear();
}

void ConstantPool::Clear()
{
  m_next = m_begin;
  m_copies.clear();
}

void ConstantPool::Shutdown()
{
  m_begin = nullptr;
  m_end = nullptr;
  m_next = nullptr;
  m_copies.clear();
}

const void* ConstantPool::GetConstant(const void* value, size_t element_size, size_t num_elements,
                                      size_t index)
{
  const size_t bytes = element_size * num_elements;

  auto it = m_copies.find(value);
  if (it == m_copies.end())
  {
    const uintptr_t next = reinterpret_cast<uintptr_t>(m_next);
    const uintptr_t aligned = (next + (ALIGNMENT - 1)) & ~uintptr_t{ALIGNMENT - 1};
    u8* where = m_next + (aligned - next);
    const bool fits = m_next != nullptr && where <= m_end &&
                      static_cast<size_t>(m_end - where) >= bytes;
    ASSERT_MSG(DYNA_REC, fits, "Constant pool has run out of space.");
    if (!fits)
      return nullptr;

    std::memcpy(where, value, bytes);
    m_next = where + bytes;
    it = m_copies.emplace(value, Copy{where, bytes}).first;
  }

  ASSERT_MSG(DYNA_REC, it->second.bytes == bytes, "Constant has incorrect size in constant pool.");
  return it->second.data + element_size * index;
}
