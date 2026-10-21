// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/TextureAndSamplerResource.h"

#include "VideoCommon/Assets/CustomAssetCache.h"
#include "VideoCommon/Resources/TexturePool.h"

namespace VideoCommon
{
TextureAndSamplerResource::TextureAndSamplerResource(Resource::ResourceContext resource_context)
    : Resource(std::move(resource_context))
{
  const ResourceContext& ctx = m_resource_context;
  m_asset = ctx.asset_cache->CreateAsset<TextureAndSamplerAsset>(ctx.primary_asset_id,
                                                                 ctx.asset_library, this);
}

void TextureAndSamplerResource::MarkAsActive()
{
  m_resource_context.asset_cache->MarkAssetActive(m_asset);
}

void TextureAndSamplerResource::MarkAsPending()
{
  m_resource_context.asset_cache->MarkAssetPending(m_asset);
}

const std::shared_ptr<TextureAndSamplerResource::Data>& TextureAndSamplerResource::GetData() const
{
  return m_live;
}

void TextureAndSamplerResource::ResetData()
{
  m_staging = std::make_shared<Data>();
}

Resource::TaskComplete TextureAndSamplerResource::CollectPrimaryData()
{
  Data& staging = *m_staging;
  staging.m_texture_and_sampler_data = m_asset->GetData();
  const TextureAndSamplerData* source = staging.m_texture_and_sampler_data.get();
  if (source == nullptr)
    return TaskComplete::No;

  // Without a first mip level there is nothing to size the texture from.
  const auto& slices = source->texture_data.m_slices;
  if (slices.empty() || slices.front().m_levels.empty())
    return TaskComplete::Error;
  const auto& top = slices.front().m_levels.front();

  // Only one layer and one level are requested from the pool, whatever the asset holds.
  TextureConfig& config = staging.m_config;
  config.width = top.width;
  config.height = top.height;
  config.levels = 1;
  config.layers = 1;
  config.samples = 1;
  config.format = top.format;
  config.type = source->type;
  config.flags = 0;
  return TaskComplete::Yes;
}

Resource::TaskComplete TextureAndSamplerResource::ProcessData()
{
  TexturePool& pool = *m_resource_context.texture_pool;
  Data& staging = *m_staging;

  staging.m_texture = pool.AllocateTexture(staging.m_config);
  AbstractTexture* const gpu = staging.m_texture.get();
  if (gpu == nullptr) [[unlikely]]
    return TaskComplete::Error;

  const auto& slices = staging.m_texture_and_sampler_data->texture_data.m_slices;
  u32 slice_index = 0;
  for (const auto& slice : slices)
  {
    u32 level_index = 0;
    for (const auto& level : slice.m_levels)
    {
      gpu->Load(level_index, level.width, level.height, level.row_length, level.data.data(),
                level.data.size(), slice_index);
      ++level_index;
    }
    ++slice_index;
  }

  // Publish the new upload; the one it replaces goes back to the pool.
  m_live.swap(m_staging);
  if (m_staging != nullptr)
    pool.ReleaseTexture(std::move(m_staging->m_texture));
  return TaskComplete::Yes;
}

void TextureAndSamplerResource::OnUnloadRequested()
{
  if (m_live == nullptr)
    return;
  m_resource_context.texture_pool->ReleaseTexture(std::move(m_live->m_texture));
  m_live.reset();
}
}  // namespace VideoCommon
