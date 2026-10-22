// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <xxh3.h>

#include "VideoCommon/GXPipelineTypes.h"

namespace VideoCommon
{
// Adjusts a GX pipeline UID for what the host backend and driver can do (logic op and
// dual-source blending fallbacks, framebuffer-fetch blending, early-Z workarounds, and
// line/point expansion in the vertex shader).
GXPipelineUid ApplyDriverBugs(const GXPipelineUid& in);

// Hash of a pipeline UID that uses the vertex declaration instead of the vertex format pointer,
// so it is stable across runs.
std::size_t PipelineToHash(const GXPipelineUid& in);

// Feeds the same data into an existing XXH3 state.
void UpdateHashWithPipeline(const GXPipelineUid& in, XXH3_state_t* hash_state);

}  // namespace VideoCommon
