// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/ScaleAction.h"

namespace
{
void ReadFactor(const picojson::value& json, const std::string& key, float* out)
{
  const picojson::value& factor = json.get(key);
  if (factor.is<double>())
    *out = static_cast<float>(factor.get<double>());
}

// Scales the x and y diagonal entries of the projection.
void ScaleProjection(GraphicsModActionData::Projection* projection, const Common::Vec3& factors)
{
  if (projection == nullptr || projection->matrix == nullptr) [[unlikely]]
    return;
  auto& m = projection->matrix->data;
  m[0] *= factors.x;
  m[5] *= factors.y;
}
}  // namespace

std::unique_ptr<ScaleAction> ScaleAction::Create(const picojson::value& json_data)
{
  Common::Vec3 factors;
  ReadFactor(json_data, "X", &factors.x);
  ReadFactor(json_data, "Y", &factors.y);
  ReadFactor(json_data, "Z", &factors.z);
  return std::make_unique<ScaleAction>(factors);
}

ScaleAction::ScaleAction(Common::Vec3 scale) : m_factors(scale)
{
}

void ScaleAction::BeforeEFB(GraphicsModActionData::PreEFB* efb)
{
  if (efb == nullptr || efb->scaled_width == nullptr || efb->scaled_height == nullptr) [[unlikely]]
    return;

  // Only positive factors resize; the result is truncated to whole texels.
  if (m_factors.x > 0)
    *efb->scaled_width = static_cast<u32>(static_cast<float>(efb->texture_width) * m_factors.x);
  if (m_factors.y > 0)
    *efb->scaled_height = static_cast<u32>(static_cast<float>(efb->texture_height) * m_factors.y);
}

void ScaleAction::OnProjection(GraphicsModActionData::Projection* projection)
{
  ScaleProjection(projection, m_factors);
}

void ScaleAction::OnProjectionAndTexture(GraphicsModActionData::Projection* projection)
{
  ScaleProjection(projection, m_factors);
}
