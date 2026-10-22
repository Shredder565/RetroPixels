// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>

#include "Common/HookableEvent.h"
#include "Common/WorkQueueThread.h"

#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/Assets/CustomAssetCache.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/Resources/MaterialResource.h"
#include "VideoCommon/Resources/ShaderResource.h"
#include "VideoCommon/Resources/TextureAndSamplerResource.h"
#include "VideoCommon/Resources/TextureDataResource.h"
#include "VideoCommon/Resources/TexturePool.h"

namespace VideoCommon
{
// Owns every custom resource, the asset cache they load through and the services they share.
// Each Get* call returns the resource for that asset (creating it on first use) after advancing
// its build; callers check IsDataProcessed() before using the data.
class CustomResourceManager
{
public:
  void Initialize();
  void Shutdown();

  void Reset();

  // Request that an asset be reloaded
  void MarkAssetDirty(const CustomAssetLibrary::AssetID& asset_id);

  void XFBTriggered();
  void SetHostConfig(const ShaderHostConfig& host_config);

  TextureDataResource*
  GetTextureDataFromAsset(const CustomAssetLibrary::AssetID& asset_id,
                          std::shared_ptr<VideoCommon::CustomAssetLibrary> library);
  MaterialResource*
  GetDrawMaterialFromAsset(const CustomAssetLibrary::AssetID& asset_id,
                           const GXPipelineUid& pipeline_uid,
                           std::shared_ptr<VideoCommon::CustomAssetLibrary> library);
  MaterialResource*
  GetPostProcessingMaterialFromAsset(const CustomAssetLibrary::AssetID& asset_id,
                                     std::shared_ptr<VideoCommon::CustomAssetLibrary> library);

  ShaderResource* GetShaderFromAsset(const CustomAssetLibrary::AssetID& asset_id,
                                     std::size_t shader_key,
                                     const std::optional<GXPipelineUid>& pipeline_uid,
                                     const std::string& preprocessor_settings,
                                     std::shared_ptr<VideoCommon::CustomAssetLibrary> library);

  TextureAndSamplerResource*
  GetTextureAndSamplerFromAsset(const CustomAssetLibrary::AssetID& asset_id,
                                std::shared_ptr<VideoCommon::CustomAssetLibrary> library);

private:
  Resource::ResourceContext MakeContext(const CustomAssetLibrary::AssetID& asset_id,
                                        std::shared_ptr<VideoCommon::CustomAssetLibrary> library);

  template <typename T>
  using Owned = std::unique_ptr<T>;
  template <typename T>
  using ByAsset = std::map<CustomAssetLibrary::AssetID, T>;
  // Second level key: pipeline hash for draw materials, shader key for shaders.
  template <typename T>
  using ByAssetAndKey = ByAsset<std::map<std::size_t, Owned<T>>>;

  CustomAssetCache m_cache;
  TexturePool m_pool;
  Common::AsyncWorkThreadSP m_worker;
  std::unique_ptr<AsyncShaderCompiler> m_compiler;

  ByAssetAndKey<MaterialResource> m_draw_materials;
  ByAssetAndKey<ShaderResource> m_shaders;
  ByAsset<Owned<TextureDataResource>> m_texture_data;
  ByAsset<Owned<TextureAndSamplerResource>> m_textures;
  ByAsset<Owned<MaterialResource>> m_post_materials;

  ShaderHostConfig m_host_config;

  Common::EventHook m_frame_hook;

  // Placeholders bound in material texture slots that have no texture.
  std::unique_ptr<AbstractTexture> m_placeholder_transparent;
  std::unique_ptr<AbstractTexture> m_placeholder_color;
  std::unique_ptr<AbstractTexture> m_placeholder_cubemap;
  std::unique_ptr<AbstractTexture> m_placeholder_array;
};
}  // namespace VideoCommon
