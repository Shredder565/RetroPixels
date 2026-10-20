// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

struct OutputVertexData;

// Clip-space stage of the software pipeline: trivial rejection, face culling, clipping against
// the view volume, the perspective divide, and the expansion of lines and points into triangles.
namespace Clipper
{
void Init();

void ProcessTriangle(OutputVertexData* v0, OutputVertexData* v1, OutputVertexData* v2);
void ProcessLine(OutputVertexData* v0, OutputVertexData* v1);
void ProcessPoint(OutputVertexData* v);

// True when all three vertices lie outside the same clip plane.
bool IsTriviallyRejected(const OutputVertexData* v0, const OutputVertexData* v1,
                         const OutputVertexData* v2);
bool IsBackface(const OutputVertexData* v0, const OutputVertexData* v1, const OutputVertexData* v2);

// Computes screenPosition from projectedPosition and the XF viewport.
void PerspectiveDivide(OutputVertexData* vertex);
}  // namespace Clipper
