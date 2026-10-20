// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/EfbCopy.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "Common/CommonTypes.h"
#include "VideoBackends/Software/SWEfbInterface.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/VideoCommon.h"

namespace EfbCopy
{
void ClearEfb()
{
  // The clear registers hold alpha:red and green:blue; the EFB interface wants A, B, G, R bytes.
  const u32 ar = bpmem.clearcolorAR;
  const u32 word = ((ar & 0xff00) >> 8) | (bpmem.clearcolorGB << 8) | ((ar & 0xff) << 24);
  std::array<u8, 4> color;
  std::memcpy(color.data(), &word, sizeof(word));

  // The rectangle includes its right and bottom edges, cut off at the EFB's last pixel.
  const u32 x0 = bpmem.copyTexSrcXY.x;
  const u32 y0 = bpmem.copyTexSrcXY.y;
  const u32 x1 = std::min(x0 + bpmem.copyTexSrcWH.x, EFB_WIDTH - 1);
  const u32 y1 = std::min(y0 + bpmem.copyTexSrcWH.y, EFB_HEIGHT - 1);

  for (u32 y = y0; y <= y1; ++y)
  {
    for (u32 x = x0; x <= x1; ++x)
    {
      EfbInterface::SetColor(static_cast<u16>(x), static_cast<u16>(y), color.data());
      EfbInterface::SetDepth(static_cast<u16>(x), static_cast<u16>(y), bpmem.clearZValue);
    }
  }
}
}  // namespace EfbCopy
