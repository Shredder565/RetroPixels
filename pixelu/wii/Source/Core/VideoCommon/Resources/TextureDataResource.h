// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "VideoCommon/Resources/Resource.h"

#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/Assets/TextureAsset.h"

namespace VideoCommon
{
// Exposes the decoded pixels of a texture asset on the CPU side, without creating a GPU
// texture (used by the hires texture replacement path).
class TextureDataResource final : public Resource
{
public:
  explicit TextureDataResource(Resource::ResourceContext resource_context);

  std::shared_ptr<CustomTextureData> GetData() const;
  // When the data returned by GetData() was loaded.
  CustomAsset::TimeType GetLoadTime() const;

  void MarkAsActive() override;
  void MarkAsPending() override;

private:
  TaskComplete CollectPrimaryData() override;

  // Owned by the asset cache.
  TextureAsset* m_asset = nullptr;

  std::shared_ptr<CustomTextureData> m_pixels;
  CustomAsset::TimeType m_loaded_at = {};
};
}  // namespace VideoCommon
