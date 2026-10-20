// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/BoundingBox.h"

// The pixel engine's bounding box registers, as the software rasterizer updates them.
namespace BBoxManager
{
enum class Coordinate
{
  Left,
  Right,
  Top,
  Bottom,
};

u16 GetCoordinate(Coordinate coordinate);
void SetCoordinate(Coordinate coordinate, u16 value);

// Grows the box to include the given extents.
void Update(u16 left, u16 right, u16 top, u16 bottom);
}  // namespace BBoxManager

namespace SW
{
class SWBoundingBox final : public BoundingBox
{
public:
  bool Initialize() override { return true; }

protected:
  std::vector<BBoxType> Read(u32 index, u32 length) override;
  void Write(u32 index, std::span<const BBoxType> values) override;
};
}  // namespace SW
