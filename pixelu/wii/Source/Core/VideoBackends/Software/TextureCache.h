// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "VideoBackends/Software/SWTexture.h"
#include "VideoBackends/Software/TextureEncoder.h"
#include "VideoCommon/TextureCacheBase.h"

namespace SW
{
// The software renderer only performs EFB copies to RAM, with its own encoder. Copies straight
// into texture cache entries are not emulated.
class TextureCache : public TextureCacheBase
{
protected:
  void CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params, u32 native_width,
               u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
               const MathUtil::Rectangle<int>& src_rect, bool scale_by_half, bool, float y_scale,
               float gamma, bool, bool, const std::array<u32, 3>&) override
  {
    TextureEncoder::Encode(dst, params, native_width, bytes_per_row, num_blocks_y, memory_stride,
                           src_rect, scale_by_half, y_scale, gamma);
  }

  void CopyEFBToCacheEntry(RcTcacheEntry&, bool, const MathUtil::Rectangle<int>&, bool, bool,
                           EFBCopyFormat, bool, float, bool, bool,
                           const std::array<u32, 3>&) override
  {
  }
};

}  // namespace SW
