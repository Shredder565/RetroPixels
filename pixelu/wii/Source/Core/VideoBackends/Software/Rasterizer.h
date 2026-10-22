// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

struct OutputVertexData;

// Scan conversion for the software renderer: walks front-facing triangles in 2x2 pixel blocks,
// interpolates their attributes and hands every covered pixel to the TEV.
namespace Rasterizer
{
// Resets the depth slope that z-freeze reuses.
void Init();
// Recomputes the scissor rectangles from BP and XF state.
void ScissorChanged();

// Captures the triangle's depth slope for z-freeze (does nothing while z-freeze is on).
void UpdateZSlope(const OutputVertexData* v0, const OutputVertexData* v1,
                  const OutputVertexData* v2, s32 x_off, s32 y_off);
// Rasterizes one screen-space triangle into every scissor rectangle.
void DrawTriangleFrontFace(const OutputVertexData* v0, const OutputVertexData* v1,
                           const OutputVertexData* v2);

// Reloads the TEV konstant colours.
void SetTevKonstColors();
}  // namespace Rasterizer
