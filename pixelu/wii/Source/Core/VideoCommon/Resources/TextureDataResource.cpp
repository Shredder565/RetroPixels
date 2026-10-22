// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/TextureDataResource.h"

#include "VideoCommon/Assets/CustomAssetCache.h"

namespace VideoCommon
{
TextureDataResource::TextureDataResource(Resource::ResourceContext resource_context)
    : Resource(std::move(resource_context))
{
  const ResourceContext& ctx = m_resource_context;
  m_asset =
      ctx.asset_cache->CreateAsset<TextureAsset>(ctx.primary_asset_id, ctx.asset_library, this);
}

std::shared_ptr<CustomTextureData> TextureDataResource::GetData() const
{
  return m_pixels;
}

CustomAsset::TimeType TextureDataResource::GetLoadTime() const
{
  return m_loaded_at;
}

void TextureDataResource::MarkAsActive()
{
  m_resource_context.asset_cache->MarkAssetActive(m_asset);
}

void TextureDataResource::MarkAsPending()
{
  m_resource_context.asset_cache->MarkAssetPending(m_asset);
}

Resource::TaskComplete TextureDataResource::CollectPrimaryData()
{
  // Take the timestamp first so it can never be newer than the data it is paired with.
  const CustomAsset::TimeType stamp = m_asset->GetLastLoadedTime();
  std::shared_ptr<CustomTextureData> pixels = m_asset->GetData();
  if (pixels == nullptr)
    return TaskComplete::No;

  m_pixels = std::move(pixels);
  m_loaded_at = stamp;
  return TaskComplete::Yes;
}
}  // namespace VideoCommon
