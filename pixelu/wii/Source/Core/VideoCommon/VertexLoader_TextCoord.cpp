// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoader_TextCoord.h"

#include "Common/CommonTypes.h"

#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderUtils.h"

namespace
{
using VertexData::Source;

// Reads `Count` components of type T for the texture coordinate whose array is step.array.
template <typename T, u32 Count, Source S>
void ReadTexCoord(VertexLoaderCursor& cursor, const VertexLoaderStep& step)
{
  const u8* data;
  if constexpr (VertexData::IsIndexSource<S>)
    data = VertexData::ArrayEntry(step.array, VertexData::TakeIndex<S>(cursor));
  else
    data = VertexData::TakeInline<S>(cursor, Count * sizeof(T));

  for (u32 c = 0; c < Count; c++)
    VertexData::Emit(cursor.dst, VertexData::Component<T>(data + c * sizeof(T), step.scale));
}

template <typename T, Source S>
VertexLoaderStep::Fn PickCount(TexComponentCount elements)
{
  return elements == TexComponentCount::ST ? ReadTexCoord<T, 2, S> : ReadTexCoord<T, 1, S>;
}

template <Source S>
VertexLoaderStep::Fn PickFormat(ComponentFormat format, TexComponentCount elements)
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

VertexLoaderStep::Fn VertexLoader_TextCoord::GetReader(VertexComponentFormat type,
                                                       ComponentFormat format,
                                                       TexComponentCount elements)
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
    return nullptr;
  }
}
