// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Common/SmallVector.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoaderBase.h"

// Where the C++ loader is while it converts one batch of vertices.
struct VertexLoaderCursor
{
  const u8* src;
  u8* dst;
  // Vertices still to come after the current one; 0 while converting the last vertex.
  int remaining;
  // Set by an indexed position read whose index is all ones: the vertex is dropped.
  bool drop_vertex;
  // Texture matrix indices read at the start of the vertex, by texture coordinate slot.
  std::array<u8, 8> texmtx;
};

// One conversion step of the C++ loader, with the parameters it was built with.
struct VertexLoaderStep
{
  using Fn = void (*)(VertexLoaderCursor& cursor, const VertexLoaderStep& step);

  Fn run;
  // Multiplier for fixed-point components (1 / 2^frac).
  float scale;
  // Vertex array an indexed component reads from.
  CPArray array;
  // Texture coordinate slot, for the texture matrix steps.
  u8 slot;
};

// The portable vertex loader: a list of steps, each converting one component.
class VertexLoader : public VertexLoaderBase
{
public:
  VertexLoader(const TVtxDesc& vtx_desc, const VAT& vtx_attr);

  int RunVertices(const u8* src, u8* dst, int count) override;

private:
  void AddStep(VertexLoaderStep::Fn fn, float scale = 1.0f, CPArray array = CPArray::Position,
               u8 slot = 0);
  void BuildSteps();
  int AppendTexCoords(int offset);

  // The longest list: position matrix, 8 texture matrix reads, position, normal, 2 colours,
  // and 8 texture coordinates with their texture matrix writes.
  Common::SmallVector<VertexLoaderStep, 29> m_steps;
  bool m_position_indexed = false;
};
