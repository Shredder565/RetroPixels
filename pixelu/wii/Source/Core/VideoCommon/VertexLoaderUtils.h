// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Common/Inline.h"
#include "Common/Swap.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderManager.h"

// Helpers shared by the C++ loader's component readers. GX vertex data is big-endian; the host
// vertex buffer is native-endian.
namespace VertexData
{
// Reads a big-endian value at p (no alignment needed).
template <typename T>
DOLPHIN_FORCE_INLINE T LoadBE(const u8* p)
{
  T raw;
  std::memcpy(&raw, p, sizeof(T));
  return Common::FromBigEndian(raw);
}

// Reads a value at p exactly as stored, without byte swapping.
template <typename T>
DOLPHIN_FORCE_INLINE T LoadRaw(const u8* p)
{
  T raw;
  std::memcpy(&raw, p, sizeof(T));
  return raw;
}

// Writes value at out and moves out past it.
template <typename T>
DOLPHIN_FORCE_INLINE void Emit(u8*& out, T value)
{
  std::memcpy(out, &value, sizeof(T));
  out += sizeof(T);
}

// Converts one big-endian component to float: fixed-point formats are scaled, floats are not.
template <typename T>
DOLPHIN_FORCE_INLINE float Component(const u8* p, float scale)
{
  if constexpr (std::is_same_v<T, float>)
    return LoadBE<float>(p);
  else
    return static_cast<float>(LoadBE<T>(p)) * scale;
}

// How a component finds its data.
enum class Source
{
  // In the vertex itself.
  Inline,
  // At the read position, without consuming it (a position the VCD marks absent).
  Peek,
  // In a vertex array, through an 8-bit index.
  Index8,
  // In a vertex array, through a 16-bit index.
  Index16,
};

template <Source S>
constexpr bool IsIndexSource = S == Source::Index8 || S == Source::Index16;

template <Source S>
using IndexType = std::conditional_t<S == Source::Index16, u16, u8>;

// Element address of an array entry. The product wraps at 32 bits, as the hardware's would.
DOLPHIN_FORCE_INLINE const u8* ArrayEntry(CPArray array, u32 index)
{
  return VertexLoaderManager::cached_arraybases[array] +
         index * g_main_cp_state.array_strides[array];
}

// Consumes an index from the vertex and returns it.
template <Source S>
DOLPHIN_FORCE_INLINE u32 TakeIndex(VertexLoaderCursor& cursor)
{
  using I = IndexType<S>;
  const I index = LoadBE<I>(cursor.src);
  cursor.src += sizeof(I);
  return index;
}

// Returns where an inline component of `bytes` bytes starts, consuming it.
template <Source S>
DOLPHIN_FORCE_INLINE const u8* TakeInline(VertexLoaderCursor& cursor, size_t bytes)
{
  const u8* const where = cursor.src;
  if constexpr (S == Source::Inline)
    cursor.src += bytes;
  return where;
}
}  // namespace VertexData
