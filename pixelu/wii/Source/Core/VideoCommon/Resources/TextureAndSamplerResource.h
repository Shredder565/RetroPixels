// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "VideoCommon/Resources/Resource.h"

#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Assets/TextureAsset.h"

namespace VideoCommon
{
// Uploads a texture asset to a GPU texture (taken from the shared texture pool) and keeps the
// sampler settings that came with it.
class TextureAndSamplerResource final : public Resource
{
public:
  explicit TextureAndSamplerResource(Resource::ResourceContext resource_context);
  void MarkAsActive() override;
  void MarkAsPending() override;

  class Data
  {
  public:
    AbstractTexture* GetTexture() const { return m_texture.get(); }
    const SamplerState& GetSampler() const { return m_texture_and_sampler_data->sampler; }

  private:
    friend class TextureAndSamplerResource;

    std::shared_ptr<TextureAndSamplerData> m_texture_and_sampler_data;
    std::unique_ptr<AbstractTexture> m_texture;
    TextureConfig m_config;
  };

  // The most recently uploaded texture, or null before the first upload completes.
  const std::shared_ptr<Data>& GetData() const;

private:
  void ResetData() override;
  TaskComplete CollectPrimaryData() override;
  TaskComplete ProcessData() override;

  void OnUnloadRequested() override;

  // Owned by the asset cache.
  TextureAndSamplerAsset* m_asset = nullptr;

  // m_live is what callers see; m_staging is being built and replaces it once uploaded.
  std::shared_ptr<Data> m_live;
  std::shared_ptr<Data> m_staging;
};
}  // namespace VideoCommon
