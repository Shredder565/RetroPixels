// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "VideoCommon/AbstractTexture.h"

namespace VideoCommon
{
// 1x1 stand-ins bound while a custom texture is missing or still loading: fully transparent,
// or magenta so the gap is easy to spot.
std::unique_ptr<AbstractTexture> CreateInvalidTransparentTexture();
std::unique_ptr<AbstractTexture> CreateInvalidColorTexture();
// Null on Apple platforms.
std::unique_ptr<AbstractTexture> CreateInvalidCubemapTexture();
std::unique_ptr<AbstractTexture> CreateInvalidArrayTexture();
}  // namespace VideoCommon
