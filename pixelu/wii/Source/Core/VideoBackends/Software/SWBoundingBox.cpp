// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/SWBoundingBox.h"

#include <algorithm>
#include <array>

#include "Common/CommonTypes.h"

namespace BBoxManager
{
namespace
{
std::array<u16, 4> s_box{};  // indexed by Coordinate
}

u16 GetCoordinate(Coordinate coordinate)
{
  return s_box[static_cast<size_t>(coordinate)];
}

void SetCoordinate(Coordinate coordinate, u16 value)
{
  s_box[static_cast<size_t>(coordinate)] = value;
}

void Update(u16 left, u16 right, u16 top, u16 bottom)
{
  auto& box = s_box;
  box[size_t(Coordinate::Left)] = std::min(box[size_t(Coordinate::Left)], left);
  box[size_t(Coordinate::Right)] = std::max(box[size_t(Coordinate::Right)], right);
  box[size_t(Coordinate::Top)] = std::min(box[size_t(Coordinate::Top)], top);
  box[size_t(Coordinate::Bottom)] = std::max(box[size_t(Coordinate::Bottom)], bottom);
}
}  // namespace BBoxManager

namespace SW
{
std::vector<BBoxType> SWBoundingBox::Read(u32 index, u32 length)
{
  std::vector<BBoxType> out;
  out.reserve(length);
  for (u32 n = index; n < index + length; ++n)
    out.push_back(BBoxManager::GetCoordinate(static_cast<BBoxManager::Coordinate>(n)));
  return out;
}

void SWBoundingBox::Write(u32 index, std::span<const BBoxType> values)
{
  u32 n = index;
  for (const BBoxType value : values)
    BBoxManager::SetCoordinate(static_cast<BBoxManager::Coordinate>(n++), static_cast<u16>(value));
}
}  // namespace SW
