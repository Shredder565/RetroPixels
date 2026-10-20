// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Inline.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoader.h"

// Normals, optionally followed by a tangent and a binormal (NTB). Indexed NTB data either uses
// one index for all three vectors or, with NormalIndex3, one index per vector.
class VertexLoader_Normal
{
public:
  // Bytes the normal (and tangent/binormal) take in a GX vertex.
  static DOLPHIN_FORCE_INLINE u32 GetSize(VertexComponentFormat type, ComponentFormat format,
                                          NormalComponentCount elements, bool index3)
  {
    const bool ntb = elements == NormalComponentCount::NTB;
    switch (type)
    {
    case VertexComponentFormat::Direct:
      return ElementBytes(format) * (ntb ? 9u : 3u);
    case VertexComponentFormat::Index8:
      return (ntb && index3) ? 3u : 1u;
    case VertexComponentFormat::Index16:
      return (ntb && index3) ? 6u : 2u;
    default:
      return 0;
    }
  }

  // The C++ loader step that converts the normal data of this kind (nullptr when absent).
  static VertexLoaderStep::Fn GetReader(VertexComponentFormat type, ComponentFormat format,
                                        NormalComponentCount elements, bool index3);

private:
  // Formats 5..7 are treated as float.
  static constexpr u32 ElementBytes(ComponentFormat format)
  {
    const u32 code = static_cast<u32>(format);
    return code >= 4 ? 4u : (code >= 2 ? 2u : 1u);
  }
};
