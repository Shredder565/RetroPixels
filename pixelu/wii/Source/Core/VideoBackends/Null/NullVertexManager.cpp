// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Null/NullVertexManager.h"

namespace Null
{
VertexManager::VertexManager() = default;
VertexManager::~VertexManager() = default;

void VertexManager::DrawCurrentBatch(u32, u32, u32)
{
  // There is no device to draw with.
}

}  // namespace Null
