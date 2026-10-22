// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Inline.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoader.h"

// Vertex positions: 2 or 3 components of any component format, direct or indexed.
class VertexLoader_Position
{
public:
  // Bytes the position takes in a GX vertex.
  static DOLPHIN_FORCE_INLINE u32 GetSize(VertexComponentFormat type, ComponentFormat format,
                                          CoordComponentCount elements)
  {
    switch (type)
    {
    case VertexComponentFormat::Direct:
      return ElementBytes(format) * (elements == CoordComponentCount::XYZ ? 3u : 2u);
    case VertexComponentFormat::Index8:
      return 1;
    case VertexComponentFormat::Index16:
      return 2;
    default:
      return 0;
    }
  }

  // The C++ loader step that converts a position of this kind.
  static VertexLoaderStep::Fn GetReader(VertexComponentFormat type, ComponentFormat format,
                                        CoordComponentCount elements);

private:
  // Formats 5..7 are treated as float.
  static constexpr u32 ElementBytes(ComponentFormat format)
  {
    const u32 code = static_cast<u32>(format);
    return code >= 4 ? 4u : (code >= 2 ? 2u : 1u);
  }
};
