// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "VideoCommon/TextureCacheBase.h"

namespace Null
{
// EFB copies have no pixels to copy, so both kinds are ignored.
class TextureCache final : public TextureCacheBase
{
protected:
  void CopyEFB(AbstractStagingTexture*, const EFBCopyParams&, u32, u32, u32, u32,
               const MathUtil::Rectangle<int>&, bool, bool, float, float, bool, bool,
               const std::array<u32, 3>&) override
  {
  }

  void CopyEFBToCacheEntry(RcTcacheEntry&, bool, const MathUtil::Rectangle<int>&, bool, bool,
                           EFBCopyFormat, bool, float, bool, bool,
                           const std::array<u32, 3>&) override
  {
  }
};

}  // namespace Null
