// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/CustomShaderCache.h"

#include <utility>

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

// How the two kinds of pipeline differ.
struct CustomShaderCache::SpecializedTraits
{
  using PipelineUid = VideoCommon::GXPipelineUid;
  using ShaderUid = PixelShaderUid;

  // Specialized pipelines are created even if a shader stage is missing.
  static constexpr bool needs_both_stages = false;
  static constexpr const char* shader_label = "Custom Pixel Shader";

  static auto& LaneOf(CustomShaderCache& cache) { return cache.m_specialized; }
  static ShaderCode Generate(APIType api, const ShaderHostConfig& host, const ShaderUid& uid)
  {
    return GeneratePixelShaderCode(api, host, uid.GetUidData(), {});
  }
};

struct CustomShaderCache::UberTraits
{
  using PipelineUid = VideoCommon::GXUberPipelineUid;
  using ShaderUid = UberShader::PixelShaderUid;

  static constexpr bool needs_both_stages = true;
  static constexpr const char* shader_label = "Custom Uber Pixel Shader";

  static auto& LaneOf(CustomShaderCache& cache) { return cache.m_uber; }
  static ShaderCode Generate(APIType api, const ShaderHostConfig& host, const ShaderUid& uid)
  {
    return GenPixelShader(api, host, uid.GetUidData());
  }
};

// Compiles one pixel shader and stores it in its cache slot.
template <typename Traits>
class CustomShaderCache::ShaderJob final : public VideoCommon::AsyncShaderCompiler::WorkItem
{
public:
  using Slot = typename InstanceCache<typename Traits::ShaderUid, AbstractShader>::Slot;

  ShaderJob(CustomShaderCache* owner, const typename Traits::ShaderUid& uid,
            CustomShaderInstance instance, Slot* slot)
      : m_owner(owner), m_uid(uid), m_instance(std::move(instance)), m_slot(slot)
  {
  }

  bool Compile() override
  {
    // The custom shader instance does not change the generated code yet.
    const ShaderCode code = Traits::Generate(m_owner->m_api_type, m_owner->m_host_config, m_uid);
    m_result = g_gfx->CreateShaderFromSource(ShaderStage::Pixel, code.GetBuffer(), nullptr,
                                             Traits::shader_label);
    return true;
  }

  void Retrieve() override
  {
    m_slot->pending = false;
    m_slot->value = std::move(m_result);
  }

private:
  CustomShaderCache* m_owner;
  typename Traits::ShaderUid m_uid;
  CustomShaderInstance m_instance;
  Slot* m_slot;
  std::unique_ptr<AbstractShader> m_result;
};

// Creates one pipeline once its pixel shader is available. Until then the job is re-queued
// every time it is retrieved.
template <typename Traits>
class CustomShaderCache::PipelineJob final : public VideoCommon::AsyncShaderCompiler::WorkItem
{
public:
  using Slot = typename InstanceCache<typename Traits::PipelineUid, AbstractPipeline>::Slot;

  PipelineJob(CustomShaderCache* owner, typename Traits::PipelineUid uid,
              CustomShaderInstance instance, Slot* slot, AbstractPipelineConfig config)
      : m_owner(owner), m_uid(std::move(uid)), m_instance(std::move(instance)), m_slot(slot),
        m_config(std::move(config))
  {
    m_ready = ResolvePixelShader();
  }

  bool Compile() override
  {
    if (!m_ready)
      return true;
    if constexpr (Traits::needs_both_stages)
    {
      if (m_config.pixel_shader == nullptr || m_config.vertex_shader == nullptr)
        return false;
    }
    m_result = g_gfx->CreatePipeline(m_config);
    return true;
  }

  void Retrieve() override
  {
    if (m_ready)
    {
      m_slot->pending = false;
      m_slot->value = std::move(m_result);
      return;
    }

    auto& compiler = *Traits::LaneOf(*m_owner).compiler;
    compiler.QueueWorkItem(
        compiler.template CreateWorkItem<PipelineJob>(m_owner, m_uid, m_instance, m_slot, m_config),
        0);
  }

private:
  // True once the pixel shader stage is settled. A compiled shader replaces the config's pixel
  // shader; a failed one leaves the config's own shader in place.
  bool ResolvePixelShader()
  {
    typename Traits::ShaderUid ps_uid = m_uid.ps_uid;
    ClearUnusedPixelShaderUidBits(m_owner->m_api_type, m_owner->m_host_config, &ps_uid);

    const auto* shader = Traits::LaneOf(*m_owner).shaders.Find(ps_uid, m_instance);
    if (shader == nullptr)
    {
      m_owner->QueueShader<Traits>(ps_uid, m_instance);
      return false;
    }
    if (!shader->pending && shader->value != nullptr)
      m_config.pixel_shader = shader->value.get();
    return !shader->pending;
  }

  CustomShaderCache* m_owner;
  typename Traits::PipelineUid m_uid;
  CustomShaderInstance m_instance;
  Slot* m_slot;
  AbstractPipelineConfig m_config;
  std::unique_ptr<AbstractPipeline> m_result;
  bool m_ready = false;
};

CustomShaderCache::CustomShaderCache()
{
  m_api_type = g_backend_info.api_type;
  m_host_config.bits = ShaderHostConfig::GetCurrent().bits;

  // One worker per kind keeps custom shader compiles from competing with the main caches.
  m_specialized.compiler = g_gfx->CreateAsyncShaderCompiler();
  m_specialized.compiler->StartWorkerThreads(1);
  m_uber.compiler = g_gfx->CreateAsyncShaderCompiler();
  m_uber.compiler->StartWorkerThreads(1);

  m_frame_end_hook = GetVideoEvents().after_frame_event.Register(
      [this](Core::System&) { RetrieveAsyncShaders(); });
}

CustomShaderCache::~CustomShaderCache()
{
  for (VideoCommon::AsyncShaderCompiler* compiler :
       {m_specialized.compiler.get(), m_uber.compiler.get()})
  {
    if (compiler != nullptr)
    {
      compiler->StopWorkerThreads();
      compiler->ClearAllWork();
    }
  }

  // Release pipelines before the shaders they were built from.
  m_uber.pipelines = {};
  m_specialized.pipelines = {};
  m_uber.shaders = {};
  m_specialized.shaders = {};
}

void CustomShaderCache::RetrieveAsyncShaders()
{
  m_specialized.compiler->RetrieveWorkItems();
  m_uber.compiler->RetrieveWorkItems();
}

void CustomShaderCache::Reload()
{
  for (VideoCommon::AsyncShaderCompiler* compiler :
       {m_specialized.compiler.get(), m_uber.compiler.get()})
  {
    while (compiler->HasPendingWork() || compiler->HasCompletedWork())
      compiler->RetrieveWorkItems();
  }

  m_specialized.shaders = {};
  m_uber.shaders = {};
  m_specialized.pipelines = {};
  m_uber.pipelines = {};
}

template <typename Traits>
std::optional<const AbstractPipeline*>
CustomShaderCache::Lookup(const typename Traits::PipelineUid& uid,
                          const CustomShaderInstance& custom_shaders,
                          const AbstractPipelineConfig& pipeline_config)
{
  auto& lane = Traits::LaneOf(*this);
  if (const auto* slot = lane.pipelines.Find(uid, custom_shaders))
  {
    if (slot->pending)
      return std::nullopt;
    return slot->value.get();
  }

  // The job's constructor may queue the pixel shader first, so that compile runs before the
  // pipeline job on the same compiler.
  auto* slot = lane.pipelines.Add(uid, custom_shaders);
  auto job = lane.compiler->template CreateWorkItem<PipelineJob<Traits>>(this, uid, custom_shaders,
                                                                         slot, pipeline_config);
  lane.compiler->QueueWorkItem(std::move(job), 0);
  return std::nullopt;
}

template <typename Traits>
void CustomShaderCache::QueueShader(const typename Traits::ShaderUid& uid,
                                    const CustomShaderInstance& custom_shaders)
{
  auto& lane = Traits::LaneOf(*this);
  auto* slot = lane.shaders.Add(uid, custom_shaders);
  auto job =
      lane.compiler->template CreateWorkItem<ShaderJob<Traits>>(this, uid, custom_shaders, slot);
  lane.compiler->QueueWorkItem(std::move(job), 0);
}

std::optional<const AbstractPipeline*>
CustomShaderCache::GetPipelineAsync(const VideoCommon::GXPipelineUid& uid,
                                    const CustomShaderInstance& custom_shaders,
                                    const AbstractPipelineConfig& pipeline_config)
{
  return Lookup<SpecializedTraits>(uid, custom_shaders, pipeline_config);
}

std::optional<const AbstractPipeline*>
CustomShaderCache::GetPipelineAsync(const VideoCommon::GXUberPipelineUid& uid,
                                    const CustomShaderInstance& custom_shaders,
                                    const AbstractPipelineConfig& pipeline_config)
{
  return Lookup<UberTraits>(uid, custom_shaders, pipeline_config);
}
