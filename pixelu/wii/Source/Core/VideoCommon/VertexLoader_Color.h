// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Inline.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoader.h"

// Vertex colours in the six GX colour formats, converted to RGBA8 (R in the lowest byte).
class VertexLoader_Color
{
public:
  // Bytes one colour takes in a GX vertex. Formats 6 and 7 do not exist and count as 0.
  static DOLPHIN_FORCE_INLINE u32 GetSize(VertexComponentFormat type, ColorFormat format)
  {
    if (format > ColorFormat::RGBA8888)
    {
      PanicAlertFmt("Invalid color format {}", format);
      return 0;
    }
    switch (type)
    {
    case VertexComponentFormat::Direct:
      return DirectBytes(format);
    case VertexComponentFormat::Index8:
      return 1;
    case VertexComponentFormat::Index16:
      return 2;
    default:
      return 0;
    }
  }

  // The C++ loader step that converts a colour of this kind (nullptr when absent or invalid).
  static VertexLoaderStep::Fn GetReader(VertexComponentFormat type, ColorFormat format);

private:
  static constexpr u32 DirectBytes(ColorFormat format)
  {
    switch (format)
    {
    case ColorFormat::RGB565:
    case ColorFormat::RGBA4444:
      return 2;
    case ColorFormat::RGB888:
    case ColorFormat::RGBA6666:
      return 3;
    default:
      return 4;
    }
  }
};
