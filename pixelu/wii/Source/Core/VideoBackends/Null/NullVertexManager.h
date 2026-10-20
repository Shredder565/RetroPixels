// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

#include "VideoCommon/VertexManagerBase.h"

namespace Null
{
// Batches are built by VideoCommon as usual and then thrown away.
class VertexManager final : public VertexManagerBase
{
public:
  VertexManager();
  ~VertexManager() override;

protected:
  void DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex) override;
};
}  // namespace Null
