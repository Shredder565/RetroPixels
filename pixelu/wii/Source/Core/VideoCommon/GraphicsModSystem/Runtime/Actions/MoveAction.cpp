// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/MoveAction.h"

namespace
{
// Overwrites *out with json[key] when that member is a number.
void ReadAxis(const picojson::value& json, const std::string& key, float* out)
{
  const picojson::value& axis = json.get(key);
  if (axis.is<double>())
    *out = static_cast<float>(axis.get<double>());
}

void Translate(GraphicsModActionData::Projection* projection, const Common::Vec3& offset)
{
  if (projection == nullptr || projection->matrix == nullptr) [[unlikely]]
    return;
  Common::Matrix44& matrix = *projection->matrix;
  matrix = matrix * Common::Matrix44::Translate(offset);
}
}  // namespace

std::unique_ptr<MoveAction> MoveAction::Create(const picojson::value& json_data)
{
  Common::Vec3 offset;
  ReadAxis(json_data, "X", &offset.x);
  ReadAxis(json_data, "Y", &offset.y);
  ReadAxis(json_data, "Z", &offset.z);
  return std::make_unique<MoveAction>(offset);
}

MoveAction::MoveAction(Common::Vec3 position_offset) : m_offset(position_offset)
{
}

void MoveAction::OnProjection(GraphicsModActionData::Projection* projection)
{
  Translate(projection, m_offset);
}

void MoveAction::OnProjectionAndTexture(GraphicsModActionData::Projection* projection)
{
  Translate(projection, m_offset);
}
