// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

struct InputVertexData;
struct OutputVertexData;

// The XF stage of the software pipeline: position and normal transforms, per-vertex lighting
// and texture coordinate generation, driven by XF memory.
namespace TransformUnit
{
void TransformPosition(const InputVertexData* src, OutputVertexData* dst);
void TransformNormal(const InputVertexData* src, OutputVertexData* dst);
void TransformColor(const InputVertexData* src, OutputVertexData* dst);
void TransformTexCoord(const InputVertexData* src, OutputVertexData* dst);
}  // namespace TransformUnit
