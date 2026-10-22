// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/OpcodeDecoding.h"

// Turns GX primitives into 16-bit index lists for the host's triangle, line and point
// primitives. Vertices are numbered in the order they were added since Start().
class IndexGenerator
{
public:
  // Picks up the backend/config choices: primitive restart and vertex shader line/point
  // expansion. Called again when the configuration changes.
  void Init();
  // Starts a new batch writing indices to index_ptr.
  void Start(u16* index_ptr);

  void AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices);
  // Appends ready-made indices that refer to the next num_vertices vertices.
  void AddExternalIndices(const u16* indices, u32 num_indices, u32 num_vertices);

  // Vertices added since Start().
  u32 GetNumVerts() const { return m_next_vertex; }
  // Indices written since Start().
  u32 GetIndexLen() const { return static_cast<u32>(m_cursor - m_start); }
  // How many more vertices of this primitive fit before indices run out of 16 bits.
  u32 GetRemainingIndices(OpcodeDecoder::Primitive primitive) const;

private:
  u16* m_cursor = nullptr;
  u16* m_start = nullptr;
  u32 m_next_vertex = 0;
  bool m_restart = false;
  bool m_vs_expand = false;
};
