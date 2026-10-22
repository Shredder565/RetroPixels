// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/Inline.h"
#include "Common/Swap.h"

// A cursor over a byte range of GX command or vertex data. Reads are big-endian by default
// (the console's byte order); writes are native order unless asked otherwise. No bounds checks
// are made: callers compare size() against what they intend to consume.
class DataReader
{
public:
  DataReader() = default;
  DataReader(u8* src, u8* end_) : m_cursor(src), m_limit(end_) {}

  u8* GetPointer() { return m_cursor; }
  const u8* GetPointer() const { return m_cursor; }

  DataReader& operator=(u8* src)
  {
    m_cursor = src;
    return *this;
  }

  // Bytes left between the cursor and the end of the range.
  size_t size() const { return static_cast<size_t>(m_limit - m_cursor); }

  template <typename T, bool swapped = true>
  DOLPHIN_FORCE_INLINE T Peek(int offset = 0) const
  {
    T value;
    std::memcpy(&value, m_cursor + offset, sizeof(T));
    if constexpr (swapped)
      value = Common::FromBigEndian(value);
    return value;
  }

  template <typename T, bool swapped = true>
  DOLPHIN_FORCE_INLINE T Read()
  {
    T value = Peek<T, swapped>();
    m_cursor += sizeof(T);
    return value;
  }

  template <typename T, bool swapped = false>
  DOLPHIN_FORCE_INLINE void Write(T data)
  {
    if constexpr (swapped)
      data = Common::FromBigEndian(data);
    std::memcpy(m_cursor, &data, sizeof(T));
    m_cursor += sizeof(T);
  }

  template <typename T = u8>
  void Skip(size_t data = 1)
  {
    m_cursor += data * sizeof(T);
  }

private:
  u8* __restrict m_cursor = nullptr;
  u8* m_limit = nullptr;
};
