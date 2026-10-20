// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoader_Normal.h"

#include <array>

#include "Common/CommonTypes.h"

#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexLoaderUtils.h"

namespace
{
using VertexData::Source;

// Converts one 3-component vector (normal, tangent or binormal number `which`) at data. The last
// vertex of a batch also stores it in the matching cache, for formats that lack these vectors.
template <typename T>
void ConvertVector(VertexLoaderCursor& cursor, const u8* data, u32 which, float scale)
{
  std::array<float, 4>* const caches[3] = {&VertexLoaderManager::normal_cache,
                                           &VertexLoaderManager::tangent_cache,
                                           &VertexLoaderManager::binormal_cache};
  for (u32 c = 0; c < 3; c++)
  {
    const float value = VertexData::Component<T>(data + c * sizeof(T), scale);
    if (cursor.remaining == 0)
      (*caches[which])[c] = value;
    VertexData::Emit(cursor.dst, value);
  }
}

// The normal alone, or normal, tangent and binormal stored back to back, from one place.
template <typename T, u32 Vectors, Source S>
void ReadNormals(VertexLoaderCursor& cursor, const VertexLoaderStep& step)
{
  constexpr u32 vector_bytes = 3 * sizeof(T);
  const u8* data;
  if constexpr (VertexData::IsIndexSource<S>)
    data = VertexData::ArrayEntry(step.array, VertexData::TakeIndex<S>(cursor));
  else
    data = VertexData::TakeInline<S>(cursor, Vectors * vector_bytes);

  for (u32 v = 0; v < Vectors; v++)
    ConvertVector<T>(cursor, data + v * vector_bytes, v, step.scale);
}

// Normal, tangent and binormal through three indices. Each index selects an NTB record; the
// tangent and binormal are still taken at their own offset within the record they select.
template <typename T, Source S>
void ReadNormalsThreeIndices(VertexLoaderCursor& cursor, const VertexLoaderStep& step)
{
  constexpr u32 vector_bytes = 3 * sizeof(T);
  for (u32 v = 0; v < 3; v++)
  {
    const u8* const record = VertexData::ArrayEntry(step.array, VertexData::TakeIndex<S>(cursor));
    ConvertVector<T>(cursor, record + v * vector_bytes, v, step.scale);
  }
}

template <typename T, Source S>
VertexLoaderStep::Fn PickLayout(NormalComponentCount elements, bool index3)
{
  if (elements != NormalComponentCount::NTB)
    return ReadNormals<T, 1, S>;
  if constexpr (VertexData::IsIndexSource<S>)
  {
    if (index3)
      return ReadNormalsThreeIndices<T, S>;
  }
  return ReadNormals<T, 3, S>;
}

template <Source S>
VertexLoaderStep::Fn PickFormat(ComponentFormat format, NormalComponentCount elements,
                                bool index3)
{
  switch (format)
  {
  case ComponentFormat::UByte:
    return PickLayout<u8, S>(elements, index3);
  case ComponentFormat::Byte:
    return PickLayout<s8, S>(elements, index3);
  case ComponentFormat::UShort:
    return PickLayout<u16, S>(elements, index3);
  case ComponentFormat::Short:
    return PickLayout<s16, S>(elements, index3);
  default:
    return PickLayout<float, S>(elements, index3);
  }
}
}  // namespace

VertexLoaderStep::Fn VertexLoader_Normal::GetReader(VertexComponentFormat type,
                                                    ComponentFormat format,
                                                    NormalComponentCount elements, bool index3)
{
  switch (type)
  {
  case VertexComponentFormat::Direct:
    return PickFormat<Source::Inline>(format, elements, index3);
  case VertexComponentFormat::Index8:
    return PickFormat<Source::Index8>(format, elements, index3);
  case VertexComponentFormat::Index16:
    return PickFormat<Source::Index16>(format, elements, index3);
  default:
    return nullptr;
  }
}
