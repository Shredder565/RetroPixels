// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Inline.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoader.h"

// Texture coordinates: 1 (s) or 2 (s, t) components of any component format.
class VertexLoader_TextCoord
{
public:
  // Bytes one texture coordinate takes in a GX vertex.
  static DOLPHIN_FORCE_INLINE u32 GetSize(VertexComponentFormat type, ComponentFormat format,
                                          TexComponentCount elements)
  {
    switch (type)
    {
    case VertexComponentFormat::Direct:
      return ElementBytes(format) * (elements == TexComponentCount::ST ? 2u : 1u);
    case VertexComponentFormat::Index8:
      return 1;
    case VertexComponentFormat::Index16:
      return 2;
    default:
      return 0;
    }
  }

  // The C++ loader step that converts a texture coordinate of this kind (nullptr when absent).
  static VertexLoaderStep::Fn GetReader(VertexComponentFormat type, ComponentFormat format,
                                        TexComponentCount elements);

private:
  // Formats 5..7 are treated as float.
  static constexpr u32 ElementBytes(ComponentFormat format)
  {
    const u32 code = static_cast<u32>(format);
    return code >= 4 ? 4u : (code >= 2 ? 2u : 1u);
  }
};
