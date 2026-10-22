// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/BoundingBox.h"

namespace Null
{
// Bounding box readback without a GPU: every coordinate reads as zero and writes are dropped.
class NullBoundingBox final : public BoundingBox
{
public:
  bool Initialize() override { return true; }

protected:
  std::vector<BBoxType> Read(u32, u32 length) override { return std::vector<BBoxType>(length, 0); }
  void Write(u32, std::span<const BBoxType>) override {}
};

}  // namespace Null
