// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

namespace VideoCommon
{
// Upper bounds on texture sampler slots shared by every backend.
constexpr u32 MAX_PIXEL_SHADER_SAMPLERS = 16;
constexpr u32 MAX_COMPUTE_SHADER_SAMPLERS = 8;
}  // namespace VideoCommon
