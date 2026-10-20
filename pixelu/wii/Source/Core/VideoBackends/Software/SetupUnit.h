// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"
#include "VideoBackends/Software/NativeVertexFormat.h"

namespace OpcodeDecoder
{
enum class Primitive : u8;
}

// Primitive assembly: collects transformed vertices and hands complete points, lines and
// triangles to the clipper, in the vertex order the GX primitive type defines.
class SetupUnit
{
public:
  void Init(OpcodeDecoder::Primitive primitive_type);

  // Returns a cleared vertex for the transform unit to fill in.
  OutputVertexData* GetVertex();

  // Accepts the vertex last returned by GetVertex.
  void SetupVertex();

private:
  std::size_t SlotFor(u32 index) const;
  OutputVertexData* Slot(u32 index) { return &m_slots[SlotFor(index)]; }

  OpcodeDecoder::Primitive m_primitive{};
  u32 m_received = 0;  // vertices accepted since Init
  std::array<OutputVertexData, 3> m_slots;
};
