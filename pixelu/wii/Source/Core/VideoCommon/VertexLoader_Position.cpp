// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoader_Position.h"

#include <limits>

#include "Common/CommonTypes.h"

#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexLoaderUtils.h"

namespace
{
using VertexData::Source;

// Reads `Count` components of type T and writes them as floats. The last three vertices of a
// batch also go to the position cache (used for the z-freeze slope), except dropped ones.
template <typename T, u32 Count, Source S>
void ReadPosition(VertexLoaderCursor& cursor, const VertexLoaderStep& step)
{
  const u8* data;
  if constexpr (VertexData::IsIndexSource<S>)
  {
    const u32 index = VertexData::TakeIndex<S>(cursor);
    cursor.drop_vertex = index == std::numeric_limits<VertexData::IndexType<S>>::max();
    data = VertexData::ArrayEntry(step.array, index);
  }
  else
  {
    data = VertexData::TakeInline<S>(cursor, Count * sizeof(T));
  }

  const bool cache = cursor.remaining < 3 && !cursor.drop_vertex;
  for (u32 c = 0; c < Count; c++)
  {
    const float value = VertexData::Component<T>(data + c * sizeof(T), step.scale);
    if (cache)
      VertexLoaderManager::position_cache[static_cast<size_t>(cursor.remaining)][c] = value;
    VertexData::Emit(cursor.dst, value);
  }
}

template <typename T, Source S>
VertexLoaderStep::Fn PickCount(CoordComponentCount elements)
{
  return elements == CoordComponentCount::XYZ ? ReadPosition<T, 3, S> : ReadPosition<T, 2, S>;
}

template <Source S>
VertexLoaderStep::Fn PickFormat(ComponentFormat format, CoordComponentCount elements)
{
  switch (format)
  {
  case ComponentFormat::UByte:
    return PickCount<u8, S>(elements);
  case ComponentFormat::Byte:
    return PickCount<s8, S>(elements);
  case ComponentFormat::UShort:
    return PickCount<u16, S>(elements);
  case ComponentFormat::Short:
    return PickCount<s16, S>(elements);
  default:
    return PickCount<float, S>(elements);
  }
}
}  // namespace

VertexLoaderStep::Fn VertexLoader_Position::GetReader(VertexComponentFormat type,
                                                      ComponentFormat format,
                                                      CoordComponentCount elements)
{
  switch (type)
  {
  case VertexComponentFormat::Direct:
    return PickFormat<Source::Inline>(format, elements);
  case VertexComponentFormat::Index8:
    return PickFormat<Source::Index8>(format, elements);
  case VertexComponentFormat::Index16:
    return PickFormat<Source::Index16>(format, elements);
  default:
    // A position the VCD leaves out is still converted from the bytes at the read position,
    // which are not consumed (the JIT loader does the same).
    return PickFormat<Source::Peek>(format, elements);
  }
}
