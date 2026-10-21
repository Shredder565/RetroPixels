// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/CustomResourceManager.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/PipelineUtils.h"
#include "VideoCommon/Resources/InvalidTextures.h"
#include "VideoCommon/VideoEvents.h"

namespace VideoCommon
{
namespace
{
constexpr const char* WORKER_NAME = "resource-worker";

// Creates the resource in `slot` on first use, then lets it make progress.
template <typename T, typename Create>
T* ProcessSlot(std::unique_ptr<T>& slot, Create&& create)
{
  if (slot == nullptr)
    slot = create();
  slot->Process();
  return slot.get();
}
}  // namespace

void CustomResourceManager::Initialize()
{
  m_cache.Initialize();
  m_worker.Reset(WORKER_NAME);
  m_host_config.bits = ShaderHostConfig::GetCurrent().bits;

  m_compiler = g_gfx->CreateAsyncShaderCompiler();
  m_compiler->StartWorkerThreads(1);

  // Asset loads are published once per presented frame.
  m_frame_hook =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { XFBTriggered(); });

  m_placeholder_array = CreateInvalidArrayTexture();
  m_placeholder_color = CreateInvalidColorTexture();
  m_placeholder_cubemap = CreateInvalidCubemapTexture();
  m_placeholder_transparent = CreateInvalidTransparentTexture();
}

void CustomResourceManager::Shutdown()
{
  if (m_compiler != nullptr)
  {
    m_compiler->StopWorkerThreads();
    m_compiler->ClearAllWork();
  }
  m_cache.Shutdown();
  m_worker.Shutdown();
  Reset();
}

void CustomResourceManager::Reset()
{
  // Materials go first since they reference shaders and textures.
  m_draw_materials.clear();
  m_post_materials.clear();
  m_shaders.clear();
  m_texture_data.clear();
  m_textures.clear();

  // Not recreated here: resources built after a Reset see null placeholders until the next
  // Initialize.
  m_placeholder_transparent.reset();
  m_placeholder_color.reset();
  m_placeholder_cubemap.reset();
  m_placeholder_array.reset();

  m_cache.Reset();
  m_pool.Reset();
  m_worker.Reset(WORKER_NAME);
}

void CustomResourceManager::MarkAssetDirty(const CustomAssetLibrary::AssetID& asset_id)
{
  m_cache.MarkAssetDirty(asset_id);
}

void CustomResourceManager::XFBTriggered()
{
  m_cache.Update();
}

void CustomResourceManager::SetHostConfig(const ShaderHostConfig& host_config)
{
  for (auto& per_asset : m_shaders)
  {
    for (auto& entry : per_asset.second)
    {
      ShaderResource& shader = *entry.second;
      shader.SetHostConfig(host_config);
      // Rebuild the shader (and whatever uses it) the next time it is requested.
      static_cast<Resource&>(shader).Restart(false);
    }
  }
  m_host_config.bits = host_config.bits;
}

TextureDataResource* CustomResourceManager::GetTextureDataFromAsset(
    const CustomAssetLibrary::AssetID& asset_id,
    std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  return ProcessSlot(m_texture_data[asset_id], [&] {
    return std::make_unique<TextureDataResource>(MakeContext(asset_id, std::move(library)));
  });
}

MaterialResource* CustomResourceManager::GetDrawMaterialFromAsset(
    const CustomAssetLibrary::AssetID& asset_id, const GXPipelineUid& pipeline_uid,
    std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  auto& slot = m_draw_materials[asset_id][PipelineToHash(pipeline_uid)];
  return ProcessSlot(slot, [&] {
    return std::make_unique<MaterialResource>(MakeContext(asset_id, std::move(library)),
                                              pipeline_uid);
  });
}

MaterialResource* CustomResourceManager::GetPostProcessingMaterialFromAsset(
    const CustomAssetLibrary::AssetID& asset_id,
    std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  return ProcessSlot(m_post_materials[asset_id], [&] {
    return std::make_unique<MaterialResource>(MakeContext(asset_id, std::move(library)));
  });
}

ShaderResource* CustomResourceManager::GetShaderFromAsset(
    const CustomAssetLibrary::AssetID& asset_id, std::size_t shader_key,
    const std::optional<GXPipelineUid>& pipeline_uid, const std::string& preprocessor_settings,
    std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  return ProcessSlot(m_shaders[asset_id][shader_key], [&] {
    return std::make_unique<ShaderResource>(MakeContext(asset_id, std::move(library)), pipeline_uid,
                                            preprocessor_settings, m_host_config);
  });
}

TextureAndSamplerResource* CustomResourceManager::GetTextureAndSamplerFromAsset(
    const CustomAssetLibrary::AssetID& asset_id,
    std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  return ProcessSlot(m_textures[asset_id], [&] {
    return std::make_unique<TextureAndSamplerResource>(MakeContext(asset_id, std::move(library)));
  });
}

Resource::ResourceContext
CustomResourceManager::MakeContext(const CustomAssetLibrary::AssetID& asset_id,
                                   std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  Resource::ResourceContext context{};
  context.primary_asset_id = asset_id;
  context.asset_library = std::move(library);
  context.asset_cache = &m_cache;
  context.resource_manager = this;
  context.texture_pool = &m_pool;
  context.worker_queue = &m_worker;
  context.shader_compiler = m_compiler.get();
  context.invalid_array_texture = m_placeholder_array.get();
  context.invalid_color_texture = m_placeholder_color.get();
  context.invalid_cubemap_texture = m_placeholder_cubemap.get();
  context.invalid_transparent_texture = m_placeholder_transparent.get();
  return context;
}
}  // namespace VideoCommon
