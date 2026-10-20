// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/TextureCacheBase.h"

// EFB copies to memory: converts the EFB rectangle into the GX texture format the copy asks for
// (or YUV for the XFB) and writes it into the staging texture, block by block.
namespace TextureEncoder
{
void Encode(AbstractStagingTexture* dst, const EFBCopyParams& params, u32 native_width,
            u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
            const MathUtil::Rectangle<int>& src_rect, bool scale_by_half, float y_scale,
            float gamma);
}  // namespace TextureEncoder
