// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cmath>

#include "Common/Assert.h"
#include "Common/MathUtil.h"

namespace SW
{
// Nearest-neighbour blit between two images of T pixels: every destination pixel of dstrect
// takes the source pixel at the same relative position in srcrect (rounded to the nearest).
template <typename T>
void CopyRegion(const T* const source, const MathUtil::Rectangle<int>& srcrect, const int src_width,
                const int src_height, T* destination, const MathUtil::Rectangle<int>& dstrect,
                const int dst_width, const int dst_height)
{
  ASSERT(srcrect.top >= 0 && srcrect.bottom <= src_height);
  ASSERT(srcrect.left >= 0 && srcrect.right <= src_width);
  ASSERT(dstrect.top >= 0 && dstrect.bottom <= dst_height);
  ASSERT(dstrect.left >= 0 && dstrect.right <= dst_width);

  const int out_w = dstrect.GetWidth();
  const int out_h = dstrect.GetHeight();
  const double step_x = srcrect.GetWidth() / static_cast<double>(out_w);
  const double step_y = srcrect.GetHeight() / static_cast<double>(out_h);

  for (int row = 0; row < out_h; ++row)
  {
    const int src_y = srcrect.top + static_cast<int>(std::round(row * step_y));
    const T* const src_line = source + src_y * src_width;
    T* const dst_line = destination + (dstrect.top + row) * dst_width;
    for (int col = 0; col < out_w; ++col)
    {
      const int src_x = srcrect.left + static_cast<int>(std::round(col * step_x));
      dst_line[dstrect.left + col] = src_line[src_x];
    }
  }
}

// Scales a whole src_width x src_height image into a whole dst_width x dst_height one.
template <typename T>
void CopyRegion(const T* const source, const int src_width, const int src_height, T* destination,
                const int dst_width, const int dst_height)
{
  const MathUtil::Rectangle<int> whole_src{0, 0, src_width, src_height};
  const MathUtil::Rectangle<int> whole_dst{0, 0, dst_width, dst_height};
  CopyRegion(source, whole_src, src_width, src_height, destination, whole_dst, dst_width,
             dst_height);
}
}  // namespace SW
