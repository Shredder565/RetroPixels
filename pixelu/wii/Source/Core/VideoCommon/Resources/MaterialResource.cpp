// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/MaterialResource.h"

#include <utility>

#include <xxh3.h>

#include "Common/MsgHandler.h"
#include "Common/VariantUtil.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Assets/CustomAssetCache.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PipelineUtils.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
namespace
{
// Material texture slots are bound after the eight samplers of the emulated GX pipeline.
constexpr u32 FIRST_CUSTOM_SAMPLER = 8;

// Applies the user's anisotropic filtering setting the same way game textures get it.
// Point-sampled textures are left alone. Once anisotropy is on, all three filters are forced
// to linear, since other filter combinations give driver-dependent results with anisotropy.
SamplerState WithUserAnisotropy(SamplerState state)
{
  const bool point_sampled =
      state.tm0.min_filter == FilterMode::Near && state.tm0.mag_filter == FilterMode::Near;
  const AnisotropicFilteringMode user = g_ActiveConfig.iMaxAnisotropy;
  if (user != AnisotropicFilteringMode::Default && !point_sampled)
    state.tm0.anisotropic_filtering = static_cast<u32>(std::to_underlying(user));

  if (state.tm0.anisotropic_filtering == 0)
    return state;
  state.tm0.min_filter = FilterMode::Linear;
  state.tm0.mag_filter = FilterMode::Linear;
  state.tm0.mipmap_filter = FilterMode::Linear;
  return state;
}

// Folds one dependency's state into the overall result: an error ends collection at once,
// anything not yet built means "try again later".
bool Accumulate(Resource::TaskComplete dependency, bool& all_ready)
{
  if (dependency == Resource::TaskComplete::Error)
    return false;
  all_ready = all_ready && dependency == Resource::TaskComplete::Yes;
  return true;
}

// A shader compiler job that runs a callable on the worker thread; nothing to retrieve.
template <typename Fn>
class CallbackJob final : public AsyncShaderCompiler::WorkItem
{
public:
  explicit CallbackJob(Fn fn) : m_fn(std::move(fn)) {}
  bool Compile() override { return m_fn(); }
  void Retrieve() override {}

private:
  Fn m_fn;
};
}  // namespace

MaterialResource::MaterialResource(Resource::ResourceContext resource_context)
    : Resource(std::move(resource_context))
{
  const ResourceContext& ctx = m_resource_context;
  m_asset =
      ctx.asset_cache->CreateAsset<MaterialAsset>(ctx.primary_asset_id, ctx.asset_library, this);
}

MaterialResource::MaterialResource(Resource::ResourceContext resource_context,
                                   GXPipelineUid pipeline_uid)
    : MaterialResource(std::move(resource_context))
{
  // The uid outlives the vertex format it was given, so keep a private copy of that format.
  m_uid.emplace(std::move(pipeline_uid));
  m_vertex_format = g_gfx->CreateNativeVertexFormat(m_uid->vertex_format->GetVertexDeclaration());
  m_uid->vertex_format = m_vertex_format.get();
}

void MaterialResource::MarkAsActive()
{
  if (m_live == nullptr) [[unlikely]]
    return;

  m_resource_context.asset_cache->MarkAssetActive(m_asset);
  for (Resource* texture : m_live->m_texture_resources)
  {
    if (texture != nullptr)
      texture->MarkAsActive();
  }
  if (m_live->m_shader != nullptr)
    m_live->m_shader->MarkAsActive();
  if (m_live->m_next != nullptr)
    m_live->m_next->MarkAsActive();
}

void MaterialResource::MarkAsPending()
{
  m_resource_context.asset_cache->MarkAssetPending(m_asset);
}

void MaterialResource::ResetData()
{
  // Stop listening to what the current build depends on; the new build registers again.
  if (m_live != nullptr)
  {
    m_live->m_shader->RemoveReference(this);
    for (Resource* texture : m_live->m_texture_resources)
    {
      if (texture != nullptr)
        texture->RemoveReference(this);
    }
    if (m_live->m_next != nullptr)
      m_live->m_next->RemoveReference(this);
  }
  m_staging = std::make_shared<Data>();
  m_build_queued = false;
}

Resource::TaskComplete MaterialResource::CollectPrimaryData()
{
  std::shared_ptr<MaterialData> material = m_asset->GetData();
  if (material == nullptr) [[unlikely]]
    return TaskComplete::No;
  m_staging->m_material = std::move(material);

  // There is nothing to draw with without a shader.
  if (m_staging->m_material->shader_asset.empty())
    return TaskComplete::Error;

  PrepareBindings(*m_staging);
  ComputeShaderKey(*m_staging, m_uid ? &*m_uid : nullptr);
  return TaskComplete::Yes;
}

Resource::TaskComplete MaterialResource::CollectDependencyData()
{
  Data& staging = *m_staging;
  const MaterialData& material = *staging.m_material;
  CustomResourceManager& manager = *m_resource_context.resource_manager;
  const auto& library = m_resource_context.asset_library;
  bool all_ready = true;

  ShaderResource* shader = manager.GetShaderFromAsset(material.shader_asset, staging.m_shader_key,
                                                      m_uid, staging.m_defines, library);
  shader->AddReference(this);
  staging.m_shader = shader;
  if (!Accumulate(shader->IsDataProcessed(), all_ready))
    return TaskComplete::Error;

  for (std::size_t slot = 0; slot < material.textures.size(); ++slot)
  {
    const CustomAssetLibrary::AssetID& texture_id = material.textures[slot].asset;
    if (texture_id.empty())
      continue;

    TextureAndSamplerResource* texture = manager.GetTextureAndSamplerFromAsset(texture_id, library);
    staging.m_texture_resources[slot] = texture;
    staging.m_texture_sources[slot] = texture->GetData();
    texture->AddReference(this);
    if (!Accumulate(texture->IsDataProcessed(), all_ready))
      return TaskComplete::Error;
  }

  const CustomAssetLibrary::AssetID& next_id = material.next_material_asset;
  if (!next_id.empty())
  {
    MaterialResource* next = m_uid ? manager.GetDrawMaterialFromAsset(next_id, *m_uid, library) :
                                     manager.GetPostProcessingMaterialFromAsset(next_id, library);
    staging.m_next = next;
    next->AddReference(this);
    if (!Accumulate(next->IsDataProcessed(), all_ready))
      return TaskComplete::Error;
  }

  return all_ready ? TaskComplete::Yes : TaskComplete::No;
}

void MaterialResource::ResolveBindings(Data& data, ShaderResource::Data& shader) const
{
  const MaterialData& material = *data.m_material;
  for (std::size_t slot = 0; slot < data.m_texture_sources.size(); ++slot)
  {
    TextureLikeReference& binding = data.m_bindings[slot];
    if (!material.textures[slot].asset.empty())
    {
      std::visit(overloaded{[&](const std::shared_ptr<TextureAndSamplerResource::Data>& source) {
                   binding.texture = source->GetTexture();
                   binding.sampler = WithUserAnisotropy(source->GetSampler());
                 }},
                 data.m_texture_sources[slot]);
      continue;
    }

    // No texture given: bind the placeholder matching what the shader declared.
    const AbstractTextureType type = shader.GetTextureType(slot);
    switch (type)
    {
    case AbstractTextureType::Texture_2D:
      binding.texture = m_resource_context.invalid_color_texture;
      break;
    case AbstractTextureType::Texture_2DArray:
      binding.texture = m_resource_context.invalid_array_texture;
      break;
    case AbstractTextureType::Texture_CubeMap:
      binding.texture = m_resource_context.invalid_cubemap_texture;
      break;
    }
    if (binding.texture == nullptr)
    {
      PanicAlertFmt("Invalid texture (texture_type={}) is not found during material "
                    "resource processing (asset_id={})",
                    type, m_resource_context.primary_asset_id);
    }
  }
}

bool MaterialResource::BuildPipeline(Data& data, const ShaderResource::Data& shader,
                                     const GXPipelineUid* uid, FramebufferState framebuffer)
{
  if (!shader.IsCompiled())
    return false;

  const MaterialData& material = *data.m_material;
  AbstractPipelineConfig config;
  config.vertex_shader = shader.GetVertexShader();
  config.pixel_shader = shader.GetPixelShader();
  config.geometry_shader = shader.GetGeometryShader();

  if (uid != nullptr)
  {
    // Replaces a game draw: start from that draw's state.
    const GXPipelineUid patched = ApplyDriverBugs(*uid);
    config.vertex_format = patched.vertex_format;
    config.rasterization_state = patched.rasterization_state;
    config.depth_state = material.depth_state.value_or(patched.depth_state);
    config.blending_state = material.blending_state.value_or(patched.blending_state);
    config.framebuffer_state = std::move(framebuffer);
    config.framebuffer_state.additional_color_attachment_count = 0;
    config.usage = AbstractPipelineUsage::GX;
  }
  else
  {
    // A full screen pass over the EFB copy. The draw-only overrides are honoured here too.
    config.vertex_format = nullptr;
    config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
    config.depth_state = material.depth_state.value_or(RenderState::GetNoDepthTestingDepthState());
    config.blending_state =
        material.blending_state.value_or(RenderState::GetNoBlendingBlendState());
    config.framebuffer_state = RenderState::GetRGBA8FramebufferState();
    config.usage = AbstractPipelineUsage::Utility;
  }
  if (material.cull_mode)
    config.rasterization_state.cull_mode = *material.cull_mode;

  data.m_pipeline = g_gfx->CreatePipeline(config);
  if (data.m_pipeline != nullptr)
    PackUniforms(data);
  return true;
}

Resource::TaskComplete MaterialResource::ProcessData()
{
  std::shared_ptr<ShaderResource::Data> shader = m_staging->m_shader->GetData();
  if (shader == nullptr) [[unlikely]]
    return TaskComplete::Error;

  ResolveBindings(*m_staging, *shader);

  if (!m_build_queued)
  {
    auto build = [data = m_staging, shader_data = std::move(shader),
                  uid = m_uid ? &*m_uid : nullptr,
                  framebuffer = g_framebuffer_manager->GetEFBFramebufferState()] {
      const bool built = BuildPipeline(*data, *shader_data, uid, framebuffer);
      data->m_build_done = true;
      return built;
    };
    AsyncShaderCompiler& compiler = *m_resource_context.shader_compiler;
    // Ordering between resources is already decided by the resource system.
    compiler.QueueWorkItem(compiler.CreateWorkItem<CallbackJob<decltype(build)>>(build), 0);
    m_build_queued = true;
  }

  if (!m_staging->m_build_done)
    return TaskComplete::No;
  if (m_staging->m_pipeline == nullptr)
    return TaskComplete::Error;

  m_live.swap(m_staging);
  return TaskComplete::Yes;
}

void MaterialResource::PrepareBindings(Data& data)
{
  const std::vector<TextureSamplerValue>& textures = data.m_material->textures;
  data.m_texture_sources.clear();
  data.m_texture_resources.clear();
  data.m_bindings.clear();

  std::string defines;
  for (u32 slot = 0; slot < static_cast<u32>(textures.size()); ++slot)
  {
    const TextureSamplerValue& texture = textures[slot];
    const bool has_asset = !texture.asset.empty();

    data.m_texture_resources.push_back(nullptr);
    data.m_texture_sources.push_back(std::shared_ptr<TextureAndSamplerResource::Data>{});

    TextureLikeReference binding{};
    binding.sampler_index = slot + FIRST_CUSTOM_SAMPLER;
    binding.texture = nullptr;
    // An empty slot always uses the default sampler.
    binding.sampler_origin =
        has_asset ? texture.sampler_origin : TextureSamplerValue::SamplerOrigin::Asset;
    binding.texture_hash = has_asset ? std::string_view(texture.texture_hash) : "";
    data.m_bindings.push_back(binding);

    defines += fmt::format("#define HAS_SAMPLER_{} {}\n", slot, has_asset ? 1 : 0);
  }
  data.m_defines = std::move(defines);
}

void MaterialResource::ComputeShaderKey(Data& data, const GXPipelineUid* uid)
{
  XXH3_state_t state;
  XXH3_INITSTATE(&state);
  XXH3_64bits_reset_withSeed(&state, XXH64_hash_t{1});
  if (uid != nullptr)
    UpdateHashWithPipeline(*uid, &state);
  XXH3_64bits_update(&state, data.m_defines.data(), data.m_defines.size());
  data.m_shader_key = static_cast<std::size_t>(XXH3_64bits_digest(&state));
}

void MaterialResource::PackUniforms(Data& data)
{
  const std::vector<MaterialProperty>& properties = data.m_material->properties;
  std::size_t total = 0;
  for (const MaterialProperty& property : properties)
    total += MaterialProperty::GetMemorySize(property);

  data.m_uniforms = Common::UniqueBuffer<u8>(total);
  u8* cursor = data.m_uniforms.data();
  for (const MaterialProperty& property : properties)
    MaterialProperty::WriteToMemory(cursor, property);
}
}  // namespace VideoCommon
