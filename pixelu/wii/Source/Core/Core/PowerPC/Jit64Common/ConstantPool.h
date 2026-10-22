// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <unordered_map>

#include "Common/CommonTypes.h"

// Copies of host constants placed inside the code region, so generated code
// can always reach them with a 32-bit RIP-relative displacement.
class ConstantPool
{
public:
  static constexpr size_t CONST_POOL_SIZE = 1024 * 32;
  static constexpr size_t ALIGNMENT = 16;

  ConstantPool();
  ~ConstantPool();

  void Init(void* memory, size_t size);
  void Clear();
  void Shutdown();

  // Returns the pool copy of element `index` of the array at `value`
  // (num_elements items of element_size bytes), copying the whole array in
  // on first use. Constants are identified by their host address.
  const void* GetConstant(const void* value, size_t element_size, size_t num_elements,
                          size_t index);

private:
  struct Copy
  {
    u8* data;
    size_t bytes;
  };

  u8* m_begin = nullptr;
  u8* m_end = nullptr;
  u8* m_next = nullptr;
  std::unordered_map<const void*, Copy> m_copies;
};
