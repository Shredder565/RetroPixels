// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/ShaderCache.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/PipelineUtils.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

#include <imgui.h>

std::unique_ptr<VideoCommon::ShaderCache> g_shader_cache;

namespace VideoCommon
{
namespace
{
// Shader statistics only track vertex and pixel shaders.
void CountNewShader(ShaderStage stage)
{
  if (stage == ShaderStage::Vertex)
  {
    INCSTAT(g_stats.num_vertex_shaders_created);
    INCSTAT(g_stats.num_vertex_shaders_alive);
  }
  else if (stage == ShaderStage::Pixel)
  {
    INCSTAT(g_stats.num_pixel_shaders_created);
    INCSTAT(g_stats.num_pixel_shaders_alive);
  }
}

// Pipeline UID <-> on-disk form. The disk form is zeroed first so its padding is deterministic.
template <typename Uid, typename Disk>
void ToDisk(const Uid& uid, Disk* disk)
{
  std::memset(static_cast<void*>(disk), 0, sizeof(*disk));
  disk->vertex_decl = uid.vertex_format->GetVertexDeclaration();
  disk->vs_uid = uid.vs_uid;
  disk->gs_uid = uid.gs_uid;
  disk->ps_uid = uid.ps_uid;
  disk->rasterization_state_bits = uid.rasterization_state.hex;
  disk->depth_state_bits = uid.depth_state.hex;
  disk->blending_state_bits = uid.blending_state.hex;
}

template <typename Disk, typename Uid>
void FromDisk(const Disk& disk, Uid* uid)
{
  uid->vertex_format = VertexLoaderManager::GetOrCreateMatchingFormat(disk.vertex_decl);
  uid->vs_uid = disk.vs_uid;
  uid->gs_uid = disk.gs_uid;
  uid->ps_uid = disk.ps_uid;
  uid->rasterization_state.hex = disk.rasterization_state_bits;
  uid->depth_state.hex = disk.depth_state_bits;
  uid->blending_state.hex = disk.blending_state_bits;
}

// The cached stage for uid if it exists and is not still compiling; otherwise whatever build()
// produces.
template <typename Cache, typename Uid, typename Build>
const AbstractShader* ReadyOrBuild(Cache& cache, const Uid& uid, Build&& build)
{
  const auto found = cache.shader_map.find(uid);
  if (found == cache.shader_map.end() || found->second.pending)
    return build();
  return found->second.shader.get();
}

// Whether a stage is built. A stage nobody has asked for yet is queued with queue(); one that
// is already compiling is left alone.
template <typename Cache, typename Uid, typename Queue>
bool StageBuiltElseQueue(Cache& cache, const Uid& uid, Queue&& queue)
{
  const auto found = cache.shader_map.find(uid);
  if (found == cache.shader_map.end())
  {
    queue(uid);
    return false;
  }
  return !found->second.pending;
}

// Puts a newly built pipeline into its slot (unless the slot already has one) and records its
// backend cache blob.
template <typename Disk, typename Map, typename Uid>
const AbstractPipeline* StorePipeline(Map& map, Common::LinearDiskCache<Disk, u8>& disk_cache,
                                      const Uid& uid, std::unique_ptr<AbstractPipeline> pipeline)
{
  auto& slot = map[uid];
  slot.second = false;
  if (slot.first || !pipeline)
    return slot.first.get();

  slot.first = std::move(pipeline);
  if (g_ActiveConfig.bShaderCache)
  {
    const AbstractPipeline::CacheData blob = slot.first->GetCacheData();
    if (!blob.empty())
    {
      Disk key;
      ToDisk(uid, &key);
      disk_cache.Append(key, blob.data(), static_cast<u32>(blob.size()));
    }
  }
  return slot.first.get();
}

// Host-capability adjustments for ubershader pipelines. These differ from the specialised ones
// in PipelineUtils because ubershaders handle most state at run time.
GXUberPipelineUid AdjustUberUid(const GXUberPipelineUid& in)
{
  // Bytewise, so the padding that the map comparisons look at is carried over.
  GXUberPipelineUid out;
  std::memcpy(static_cast<void*>(&out), static_cast<const void*>(&in), sizeof(out));
  BlendingState& blend = out.blending_state;
  auto* ps = out.ps_uid.GetUidData();

  // The vertex layout is read in the shader.
  if (g_backend_info.bSupportsDynamicVertexLoader)
    out.vertex_format = nullptr;

  // Framebuffer fetch can do logic ops in the shader; otherwise approximate them by blending.
  const bool approximate_logic_op = blend.logic_op_enable && !g_backend_info.bSupportsLogicOp &&
                                    !g_backend_info.bSupportsFramebufferFetch;
  if (approximate_logic_op)
  {
    if (!blend.LogicOpApproximationIsExact())
    {
      WARN_LOG_FMT(VIDEO,
                   "Approximating logic op with blending, this will produce incorrect rendering.");
    }
    blend.ApproximateLogicOpWithBlending();
  }

  if (g_backend_info.bSupportsFramebufferFetch)
  {
    // The shader blends; the pipeline only masks writes.
    blend.hex = 0;
    blend.color_update = in.blending_state.color_update.Value();
    blend.alpha_update = in.blending_state.alpha_update.Value();
    ps->no_dual_src = true;
  }
  else
  {
    const bool dual_source_unusable =
        !g_backend_info.bSupportsDualSourceBlend ||
        (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING) &&
         !blend.RequiresDualSrc());
    if (dual_source_unusable)
    {
      blend.use_dual_src = false;
      ps->no_dual_src = true;
    }
  }

  // Lines and points become triangles in the vertex shader.
  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    const PrimitiveType triangles = g_backend_info.bSupportsPrimitiveRestart ?
                                        PrimitiveType::TriangleStrip :
                                        PrimitiveType::Triangles;
    out.rasterization_state.primitive = triangles;
    out.gs_uid.GetUidData()->primitive_type = static_cast<u32>(triangles);
  }

  return out;
}

// A fullscreen-style utility pipeline: triangles, no culling, depth or blending.
AbstractPipelineConfig UtilityPipeline(const AbstractShader* vs, const AbstractShader* gs,
                                       const AbstractShader* ps, const FramebufferState& target)
{
  AbstractPipelineConfig config;
  config.vertex_format = nullptr;
  config.vertex_shader = vs;
  config.geometry_shader = gs;
  config.pixel_shader = ps;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = target;
  config.usage = AbstractPipelineUsage::Utility;
  return config;
}

// Fills a shader module cache from its disk file.
template <typename Key, typename Cache>
class ShaderBlobLoader final : public Common::LinearDiskCacheReader<Key, u8>
{
public:
  ShaderBlobLoader(Cache& cache, ShaderStage stage) : m_cache(cache), m_stage(stage) {}

  void Read(const Key& key, const u8* value, u32 value_size) override
  {
    std::unique_ptr<AbstractShader> shader =
        g_gfx->CreateShaderFromBinary(m_stage, value, value_size);
    if (!shader)
      return;
    auto& slot = m_cache.shader_map[key];
    slot.shader = std::move(shader);
    slot.pending = false;
    CountNewShader(m_stage);
  }

private:
  Cache& m_cache;
  ShaderStage m_stage;
};

// Fills a pipeline map from the backend's pipeline cache file. One blob the backend refuses
// marks the whole file stale; the rest of it is then skipped.
template <typename Uid, typename Disk, typename Map, typename MakeConfig>
class PipelineBlobLoader final : public Common::LinearDiskCacheReader<Disk, u8>
{
public:
  PipelineBlobLoader(Map& map, MakeConfig make_config)
      : m_map(map), m_make_config(std::move(make_config))
  {
  }

  bool Stale() const { return m_stale; }

  void Read(const Disk& key, const u8* value, u32 value_size) override
  {
    Uid uid;
    FromDisk(key, &uid);
    if (m_stale || m_map.contains(uid))
      return;

    const std::optional<AbstractPipelineConfig> config = m_make_config(uid);
    if (!config)
      return;

    std::unique_ptr<AbstractPipeline> pipeline = g_gfx->CreatePipeline(*config, value, value_size);
    if (!pipeline)
    {
      m_stale = true;
      return;
    }
    auto& slot = m_map[uid];
    slot.first = std::move(pipeline);
    slot.second = false;
  }

private:
  Map& m_map;
  MakeConfig m_make_config;
  bool m_stale = false;
};
}  // namespace

// Compiles one shader stage on a worker and files it on the main thread.
template <typename Uid>
class ShaderCache::StageJob final : public AsyncShaderCompiler::WorkItem
{
public:
  StageJob(ShaderCache* owner, const Uid& uid) : m_owner(owner), m_uid(uid) {}

  bool Compile() override
  {
    if constexpr (std::is_same_v<Uid, VertexShaderUid>)
      m_result = m_owner->CompileVertexShader(m_uid);
    else if constexpr (std::is_same_v<Uid, PixelShaderUid>)
      m_result = m_owner->CompilePixelShader(m_uid);
    else if constexpr (std::is_same_v<Uid, UberShader::VertexShaderUid>)
      m_result = m_owner->CompileVertexUberShader(m_uid);
    else
      m_result = m_owner->CompilePixelUberShader(m_uid);
    return true;
  }

  void Retrieve() override
  {
    if constexpr (std::is_same_v<Uid, VertexShaderUid>)
      m_owner->InsertVertexShader(m_uid, std::move(m_result));
    else if constexpr (std::is_same_v<Uid, PixelShaderUid>)
      m_owner->InsertPixelShader(m_uid, std::move(m_result));
    else if constexpr (std::is_same_v<Uid, UberShader::VertexShaderUid>)
      m_owner->InsertVertexUberShader(m_uid, std::move(m_result));
    else
      m_owner->InsertPixelUberShader(m_uid, std::move(m_result));
  }

private:
  ShaderCache* m_owner;
  std::unique_ptr<AbstractShader> m_result;
  Uid m_uid;
};

// Builds one GX pipeline on a worker. The stages are checked when the job is made: if one is
// not built yet the job does nothing and, when retrieved, puts a fresh copy of itself back in
// the queue so the check runs again next time.
template <typename Uid>
class ShaderCache::PipelineJob final : public AsyncShaderCompiler::WorkItem
{
public:
  PipelineJob(ShaderCache* owner, Uid uid, u32 priority)
      : m_owner(owner), m_uid(std::move(uid)), m_priority(priority)
  {
    m_stages_ready = m_owner->PipelineStagesReady(m_uid, m_priority);
    if (m_stages_ready)
      m_config = m_owner->GetGXPipelineConfig(m_uid);
  }

  bool Compile() override
  {
    if (m_config)
      m_result = g_gfx->CreatePipeline(*m_config);
    return true;
  }

  void Retrieve() override
  {
    if (!m_stages_ready)
    {
      AsyncShaderCompiler& compiler = *m_owner->m_async_shader_compiler;
      compiler.QueueWorkItem(
          AsyncShaderCompiler::CreateWorkItem<PipelineJob>(m_owner, m_uid, m_priority),
          m_priority);
      return;
    }

    if constexpr (std::is_same_v<Uid, GXPipelineUid>)
      m_owner->InsertGXPipeline(m_uid, std::move(m_result));
    else
      m_owner->InsertGXUberPipeline(m_uid, std::move(m_result));
  }

private:
  ShaderCache* m_owner;
  std::unique_ptr<AbstractPipeline> m_result;
  Uid m_uid;
  u32 m_priority;
  std::optional<AbstractPipelineConfig> m_config;
  bool m_stages_ready = false;
};

ShaderCache::ShaderCache() : m_api_type{APIType::Nothing}
{
}

ShaderCache::~ShaderCache()
{
  ClearCaches();
}

bool ShaderCache::Initialize()
{
  m_api_type = g_backend_info.api_type;
  m_host_config.bits = ShaderHostConfig::GetCurrent().bits;
  if (!BuildUtilityShaders())
    return false;

  m_async_shader_compiler = g_gfx->CreateAsyncShaderCompiler();
  m_frame_end_hook = GetVideoEvents().after_frame_event.Register(
      [this](Core::System&) { RetrieveAsyncShaders(); });
  return true;
}

void ShaderCache::FinishPrecompile()
{
  QueueUnbuiltPipelines();
  if (g_ActiveConfig.bWaitForShadersBeforeStarting)
    WaitForAsyncCompiler();
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderCompilerThreads());
}

void ShaderCache::InitializeShaderCache()
{
  // Precompiling runs with its own (usually larger) thread count.
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());

  if (g_ActiveConfig.bShaderCache && m_api_type != APIType::Nothing)
  {
    LoadCaches();
    OpenPipelineUIDList();
  }
  if (g_ActiveConfig.UsingUberShaders())
    QueueUberShaderPipelines();

  FinishPrecompile();
}

void ShaderCache::Reload()
{
  WaitForAsyncCompiler();
  ClosePipelineUIDList();
  ClearCaches();

  if (!BuildUtilityShaders())
    PanicAlertFmt("Failed to compile shared pipelines after reload.");
  if (g_ActiveConfig.bShaderCache)
    LoadCaches();

  // The pipeline maps keep their keys through ClearCaches, so rebuilding the pipelines brings
  // back every stage they use, ubershaders included.
  m_async_shader_compiler->ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());
  FinishPrecompile();
}

void ShaderCache::RetrieveAsyncShaders()
{
  m_async_shader_compiler->RetrieveWorkItems();
}

void ShaderCache::Shutdown()
{
  // Unfinished compiles are dropped rather than waited for.
  if (m_async_shader_compiler)
  {
    m_async_shader_compiler->StopWorkerThreads();
    m_async_shader_compiler->ClearAllWork();
  }
  ClosePipelineUIDList();
}

const AbstractPipeline* ShaderCache::GetPipelineForUid(const GXPipelineUid& uid)
{
  const auto found = m_gx_pipeline_cache.find(uid);
  const bool known = found != m_gx_pipeline_cache.end();
  if (known && !found->second.second)
    return found->second.first.get();

  // Unknown, or still compiling in the background: build it now.
  std::unique_ptr<AbstractPipeline> pipeline;
  if (const std::optional<AbstractPipelineConfig> config = GetGXPipelineConfig(uid))
    pipeline = g_gfx->CreatePipeline(*config);
  if (!known && g_ActiveConfig.bShaderCache)
    AppendGXPipelineUID(uid);
  return InsertGXPipeline(uid, std::move(pipeline));
}

std::optional<const AbstractPipeline*> ShaderCache::GetPipelineForUidAsync(const GXPipelineUid& uid)
{
  const auto found = m_gx_pipeline_cache.find(uid);
  if (found == m_gx_pipeline_cache.end())
  {
    AppendGXPipelineUID(uid);
    QueuePipelineCompile(uid, COMPILE_PRIORITY_ONDEMAND_PIPELINE);
    return std::nullopt;
  }
  if (found->second.second)
    return std::nullopt;
  return found->second.first.get();
}

const AbstractPipeline* ShaderCache::GetUberPipelineForUid(const GXUberPipelineUid& uid)
{
  const auto found = m_gx_uber_pipeline_cache.find(uid);
  if (found != m_gx_uber_pipeline_cache.end() && !found->second.second)
    return found->second.first.get();

  std::unique_ptr<AbstractPipeline> pipeline;
  if (const std::optional<AbstractPipelineConfig> config = GetGXPipelineConfig(uid))
    pipeline = g_gfx->CreatePipeline(*config);
  return InsertGXUberPipeline(uid, std::move(pipeline));
}

void ShaderCache::WaitForAsyncCompiler()
{
  // Drawn by the compiler while it waits, and presented straight away.
  const auto draw_progress = [](size_t done, size_t total) {
    const ImGuiIO& io = ImGui::GetIO();
    const float ui_scale = io.DisplayFramebufferScale.x;
    const ImVec2 middle(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    constexpr ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::SetNextWindowSize(ImVec2(400.0f * ui_scale, 50.0f * ui_scale), ImGuiCond_Always);
    ImGui::SetNextWindowPos(middle, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(Common::GetStringT("Compiling Shaders").c_str(), nullptr, window_flags))
    {
      ImGui::Text("Compiling shaders: %zu/%zu", done, total);
      const float fraction =
          static_cast<float>(done) / static_cast<float>(std::max(total, size_t{1}));
      ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), "");
    }
    ImGui::End();
    g_presenter->Present();
  };

  AsyncShaderCompiler& compiler = *m_async_shader_compiler;
  bool keep_waiting = true;
  while (keep_waiting && (compiler.HasPendingWork() || compiler.HasCompletedWork()))
  {
    keep_waiting = compiler.WaitUntilCompletion(draw_progress);
    compiler.RetrieveWorkItems();
  }

  // One more frame so the progress window disappears.
  g_presenter->Present();
}

template <ShaderStage stage, typename K, typename T>
void ShaderCache::LoadShaderCache(T& cache, APIType api_type, const char* type, bool include_gameid)
{
  const std::string path = GetDiskShaderCacheFileName(api_type, type, include_gameid, true);
  ShaderBlobLoader<K, T> loader(cache, stage);
  const u32 loaded = cache.disk_cache.OpenAndRead(path, loader);
  INFO_LOG_FMT(VIDEO, "Loaded {} cached shaders from {}", loaded, path);
}

template <typename T>
void ShaderCache::ClearShaderCache(T& cache)
{
  cache.disk_cache.Sync();
  cache.disk_cache.Close();
  cache.shader_map.clear();
}

template <typename KeyType, typename DiskKeyType, typename T>
void ShaderCache::LoadPipelineCache(T& cache, Common::LinearDiskCache<DiskKeyType, u8>& disk_cache,
                                    APIType api_type, const char* type, bool include_gameid)
{
  const std::string path = GetDiskShaderCacheFileName(api_type, type, include_gameid, true);
  const auto make_config = [this](const KeyType& uid) { return GetGXPipelineConfig(uid); };
  PipelineBlobLoader<KeyType, DiskKeyType, T, decltype(make_config)> loader(cache, make_config);
  const u32 loaded = disk_cache.OpenAndRead(path, loader);
  INFO_LOG_FMT(VIDEO, "Loaded {} cached pipelines from {}", loaded, path);

  if (!loader.Stale())
    return;

  // A rejected blob usually means a driver or hardware change, so none of the file is worth
  // keeping. Start it afresh; the pipelines will be written to it again as they are rebuilt.
  WARN_LOG_FMT(VIDEO, "Failed to load one or more pipelines from cache '{}'. Discarding.", path);
  disk_cache.Close();
  File::Delete(path);
  disk_cache.OpenAndRead(path, loader);
}

template <typename T, typename Y>
void ShaderCache::ClearPipelineCache(T& cache, Y& disk_cache)
{
  disk_cache.Sync();
  disk_cache.Close();

  // The keys stay, so the pipelines can be rebuilt later.
  for (auto& [uid, slot] : cache)
  {
    slot.first.reset();
    slot.second = false;
  }
}

void ShaderCache::LoadCaches()
{
  if (g_backend_info.bSupportsShaderBinaries)
  {
    // Shared between games: the ubershaders and the (few) geometry shaders.
    LoadShaderCache<ShaderStage::Vertex, UberShader::VertexShaderUid>(m_uber_vs_cache, m_api_type,
                                                                      "uber-vs", false);
    LoadShaderCache<ShaderStage::Pixel, UberShader::PixelShaderUid>(m_uber_ps_cache, m_api_type,
                                                                    "uber-ps", false);
    if (m_host_config.backend_geometry_shaders)
    {
      LoadShaderCache<ShaderStage::Geometry, GeometryShaderUid>(m_gs_cache, m_api_type, "gs",
                                                                false);
    }

    // Per game.
    LoadShaderCache<ShaderStage::Vertex, VertexShaderUid>(m_vs_cache, m_api_type, "specialized-vs",
                                                          true);
    LoadShaderCache<ShaderStage::Pixel, PixelShaderUid>(m_ps_cache, m_api_type, "specialized-ps",
                                                        true);
  }

  if (!g_backend_info.bSupportsPipelineCacheData)
    return;
  LoadPipelineCache<GXPipelineUid, SerializedGXPipelineUid>(
      m_gx_pipeline_cache, m_gx_pipeline_disk_cache, m_api_type, "specialized-pipeline", true);
  LoadPipelineCache<GXUberPipelineUid, SerializedGXUberPipelineUid>(
      m_gx_uber_pipeline_cache, m_gx_uber_pipeline_disk_cache, m_api_type, "uber-pipeline", false);
}

void ShaderCache::ClearCaches()
{
  // Each pipeline map goes before the stages it uses.
  ClearPipelineCache(m_gx_pipeline_cache, m_gx_pipeline_disk_cache);
  ClearShaderCache(m_vs_cache);
  ClearShaderCache(m_gs_cache);
  ClearShaderCache(m_ps_cache);
  ClearPipelineCache(m_gx_uber_pipeline_cache, m_gx_uber_pipeline_disk_cache);
  ClearShaderCache(m_uber_vs_cache);
  ClearShaderCache(m_uber_ps_cache);

  for (std::unique_ptr<AbstractShader>* utility :
       {&m_screen_quad_vs, &m_texture_copy_vs, &m_efb_copy_vs, &m_texcoord_gs, &m_color_gs,
        &m_texture_copy_ps, &m_color_ps})
  {
    utility->reset();
  }

  m_efb_copy_to_vram_pipelines.clear();
  m_efb_copy_to_ram_pipelines.clear();
  m_rgba8_copy.reset();
  m_rgba8_stereo_copy.reset();
  for (std::unique_ptr<AbstractPipeline>& palette : m_palette_conversion_pipelines)
    palette.reset();
  m_texture_reinterpret_pipelines.clear();
  m_texture_decoding_shaders.clear();

  SETSTAT(g_stats.num_pixel_shaders_created, 0);
  SETSTAT(g_stats.num_pixel_shaders_alive, 0);
  SETSTAT(g_stats.num_vertex_shaders_created, 0);
  SETSTAT(g_stats.num_vertex_shaders_alive, 0);
}

void ShaderCache::QueueUnbuiltPipelines()
{
  for (const auto& [uid, slot] : m_gx_pipeline_cache)
  {
    if (!slot.first)
      QueuePipelineCompile(uid, COMPILE_PRIORITY_SHADERCACHE_PIPELINE);
  }
  for (const auto& [uid, slot] : m_gx_uber_pipeline_cache)
  {
    if (!slot.first)
      QueueUberPipelineCompile(uid, COMPILE_PRIORITY_UBERSHADER_PIPELINE);
  }
}

std::unique_ptr<AbstractShader> ShaderCache::CompileVertexShader(const VertexShaderUid& uid) const
{
  const ShaderCode code = GenerateVertexShaderCode(m_api_type, m_host_config, uid.GetUidData(), {});
  return g_gfx->CreateShaderFromSource(ShaderStage::Vertex, code.GetBuffer());
}

std::unique_ptr<AbstractShader>
ShaderCache::CompileVertexUberShader(const UberShader::VertexShaderUid& uid) const
{
  const ShaderCode code = UberShader::GenVertexShader(m_api_type, m_host_config, uid.GetUidData());
  return g_gfx->CreateShaderFromSource(ShaderStage::Vertex, code.GetBuffer(), nullptr,
                                       fmt::to_string(*uid.GetUidData()));
}

std::unique_ptr<AbstractShader> ShaderCache::CompilePixelShader(const PixelShaderUid& uid) const
{
  const ShaderCode code = GeneratePixelShaderCode(m_api_type, m_host_config, uid.GetUidData(), {});
  return g_gfx->CreateShaderFromSource(ShaderStage::Pixel, code.GetBuffer());
}

std::unique_ptr<AbstractShader>
ShaderCache::CompilePixelUberShader(const UberShader::PixelShaderUid& uid) const
{
  const ShaderCode code = UberShader::GenPixelShader(m_api_type, m_host_config, uid.GetUidData());
  return g_gfx->CreateShaderFromSource(ShaderStage::Pixel, code.GetBuffer(), nullptr,
                                       fmt::to_string(*uid.GetUidData()));
}

template <typename Uid>
const AbstractShader* ShaderCache::StoreShader(ShaderModuleCache<Uid>& cache, const Uid& uid,
                                               std::unique_ptr<AbstractShader> shader,
                                               ShaderStage stage)
{
  auto& slot = cache.shader_map[uid];
  slot.pending = false;
  // The first successful build wins; a later duplicate (or a failure) is discarded.
  if (!shader || slot.shader)
    return slot.shader.get();

  if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
  {
    const AbstractShader::BinaryData binary = shader->GetBinary();
    if (!binary.empty())
      cache.disk_cache.Append(uid, binary.data(), static_cast<u32>(binary.size()));
  }
  CountNewShader(stage);
  slot.shader = std::move(shader);
  return slot.shader.get();
}

const AbstractShader* ShaderCache::InsertVertexShader(const VertexShaderUid& uid,
                                                      std::unique_ptr<AbstractShader> shader)
{
  return StoreShader(m_vs_cache, uid, std::move(shader), ShaderStage::Vertex);
}

const AbstractShader* ShaderCache::InsertVertexUberShader(const UberShader::VertexShaderUid& uid,
                                                          std::unique_ptr<AbstractShader> shader)
{
  return StoreShader(m_uber_vs_cache, uid, std::move(shader), ShaderStage::Vertex);
}

const AbstractShader* ShaderCache::InsertPixelShader(const PixelShaderUid& uid,
                                                     std::unique_ptr<AbstractShader> shader)
{
  return StoreShader(m_ps_cache, uid, std::move(shader), ShaderStage::Pixel);
}

const AbstractShader* ShaderCache::InsertPixelUberShader(const UberShader::PixelShaderUid& uid,
                                                         std::unique_ptr<AbstractShader> shader)
{
  return StoreShader(m_uber_ps_cache, uid, std::move(shader), ShaderStage::Pixel);
}

const AbstractShader* ShaderCache::CreateGeometryShader(const GeometryShaderUid& uid)
{
  const ShaderCode code = GenerateGeometryShaderCode(m_api_type, m_host_config, uid.GetUidData());
  std::unique_ptr<AbstractShader> shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Geometry, code.GetBuffer(), nullptr,
                                    fmt::format("Geometry shader: {}", *uid.GetUidData()));
  return StoreShader(m_gs_cache, uid, std::move(shader), ShaderStage::Geometry);
}

bool ShaderCache::NeedsGeometryShader(const GeometryShaderUid& uid) const
{
  return m_host_config.backend_geometry_shaders && !uid.GetUidData()->IsPassthrough();
}

bool ShaderCache::UseGeometryShaderForEFBCopies() const
{
  // Only stereo needs one, to write both layers.
  return m_host_config.backend_geometry_shaders && m_host_config.stereo;
}

std::optional<const AbstractShader*> ShaderCache::GeometryStageFor(const GeometryShaderUid& uid)
{
  if (!NeedsGeometryShader(uid))
    return nullptr;
  const AbstractShader* gs = ReadyOrBuild(m_gs_cache, uid, [&] { return CreateGeometryShader(uid); });
  if (!gs)
    return std::nullopt;
  return gs;
}

AbstractPipelineConfig ShaderCache::GetGXPipelineConfig(
    const NativeVertexFormat* vertex_format, const AbstractShader* vertex_shader,
    const AbstractShader* geometry_shader, const AbstractShader* pixel_shader,
    const RasterizationState& rasterization_state, const DepthState& depth_state,
    const BlendingState& blending_state, AbstractPipelineUsage usage)
{
  AbstractPipelineConfig config = {};
  config.vertex_format = vertex_format;
  config.vertex_shader = vertex_shader;
  config.geometry_shader = geometry_shader;
  config.pixel_shader = pixel_shader;
  config.rasterization_state = rasterization_state;
  config.depth_state = depth_state;
  config.blending_state = blending_state;
  config.framebuffer_state = g_framebuffer_manager->GetEFBFramebufferState();
  config.usage = usage;
  return config;
}

std::optional<AbstractPipelineConfig> ShaderCache::GetGXPipelineConfig(const GXPipelineUid& in)
{
  const GXPipelineUid uid = ApplyDriverBugs(in);

  const AbstractShader* vs = ReadyOrBuild(m_vs_cache, uid.vs_uid, [&] {
    return InsertVertexShader(uid.vs_uid, CompileVertexShader(uid.vs_uid));
  });

  PixelShaderUid ps_uid = uid.ps_uid;
  ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  const AbstractShader* ps = ReadyOrBuild(
      m_ps_cache, ps_uid, [&] { return InsertPixelShader(ps_uid, CompilePixelShader(ps_uid)); });

  if (!vs || !ps)
    return std::nullopt;
  const std::optional<const AbstractShader*> gs = GeometryStageFor(uid.gs_uid);
  if (!gs)
    return std::nullopt;

  return GetGXPipelineConfig(uid.vertex_format, vs, *gs, ps, uid.rasterization_state,
                             uid.depth_state, uid.blending_state, AbstractPipelineUsage::GX);
}

std::optional<AbstractPipelineConfig>
ShaderCache::GetGXPipelineConfig(const GXUberPipelineUid& in)
{
  const GXUberPipelineUid uid = AdjustUberUid(in);

  const AbstractShader* vs = ReadyOrBuild(m_uber_vs_cache, uid.vs_uid, [&] {
    return InsertVertexUberShader(uid.vs_uid, CompileVertexUberShader(uid.vs_uid));
  });

  UberShader::PixelShaderUid ps_uid = uid.ps_uid;
  UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  const AbstractShader* ps = ReadyOrBuild(m_uber_ps_cache, ps_uid, [&] {
    return InsertPixelUberShader(ps_uid, CompilePixelUberShader(ps_uid));
  });

  if (!vs || !ps)
    return std::nullopt;
  const std::optional<const AbstractShader*> gs = GeometryStageFor(uid.gs_uid);
  if (!gs)
    return std::nullopt;

  return GetGXPipelineConfig(uid.vertex_format, vs, *gs, ps, uid.rasterization_state,
                             uid.depth_state, uid.blending_state, AbstractPipelineUsage::GXUber);
}

const AbstractPipeline* ShaderCache::InsertGXPipeline(const GXPipelineUid& uid,
                                                      std::unique_ptr<AbstractPipeline> pipeline)
{
  return StorePipeline(m_gx_pipeline_cache, m_gx_pipeline_disk_cache, uid, std::move(pipeline));
}

const AbstractPipeline* ShaderCache::InsertGXUberPipeline(const GXUberPipelineUid& uid,
                                                          std::unique_ptr<AbstractPipeline> pipeline)
{
  return StorePipeline(m_gx_uber_pipeline_cache, m_gx_uber_pipeline_disk_cache, uid,
                       std::move(pipeline));
}

// The UID list file: a u32 magic and a u32 version, then SerializedGXPipelineUid records. It is
// kept open while the game runs and new UIDs are appended as they are first seen.
void ShaderCache::OpenPipelineUIDList()
{
  static constexpr u32 UID_LIST_MAGIC = 0x44495550;  // "PUID" read as little-endian bytes
  static constexpr u32 UID_LIST_VERSION = GX_PIPELINE_UID_VERSION;
  constexpr u64 HEADER_BYTES = sizeof(u32) * 2;
  constexpr size_t RECORD_BYTES = sizeof(SerializedGXPipelineUid);

  const std::string path =
      File::GetUserPath(D_CACHE_IDX) + SConfig::GetInstance().GetGameID() + ".uidcache";
  File::IOFile& file = m_uid_list_file;

  // Reads the records of an existing file and leaves it positioned for appending. Anything
  // unexpected (other version, partial record, short read) rejects the whole file.
  const auto read_existing = [&]() -> bool {
    u32 magic;
    u32 version;
    if (!file.ReadBytes(&magic, sizeof(magic)) || !file.ReadBytes(&version, sizeof(version)))
      return false;
    if (magic != UID_LIST_MAGIC || version != UID_LIST_VERSION)
      return false;

    const u64 size = file.GetSize();
    const size_t records = static_cast<size_t>(size - HEADER_BYTES) / RECORD_BYTES;
    const size_t used_bytes = records * RECORD_BYTES + HEADER_BYTES;
    if (size != used_bytes)
      return false;

    for (size_t n = 0; n < records; ++n)
    {
      SerializedGXPipelineUid record;
      if (!file.ReadBytes(&record, sizeof(record)))
        return false;
      AddSerializedGXPipelineUID(record);
    }
    return file.Seek(static_cast<s64>(used_bytes), File::SeekOrigin::Begin);
  };

  if (file.Open(path, "rb+") && !read_existing())
    file.Close();

  // Missing or rejected: start a new file, seeded with what is already known so a partly
  // written file that had to be dropped does not lose its earlier entries.
  if (!file.IsOpen() && file.Open(path, "wb"))
  {
    file.WriteBytes(&UID_LIST_MAGIC, sizeof(UID_LIST_MAGIC));
    file.WriteBytes(&UID_LIST_VERSION, sizeof(UID_LIST_VERSION));
    for (const auto& [uid, slot] : m_gx_pipeline_cache)
      AppendGXPipelineUID(uid);
  }

  INFO_LOG_FMT(VIDEO, "Read {} pipeline UIDs from {}", m_gx_pipeline_cache.size(), path);
}

void ShaderCache::ClosePipelineUIDList()
{
  m_uid_list_file.Close();
}

void ShaderCache::AddSerializedGXPipelineUID(const SerializedGXPipelineUid& uid)
{
  GXPipelineUid real_uid;
  FromDisk(uid, &real_uid);
  // An empty slot: compiled later by QueueUnbuiltPipelines.
  m_gx_pipeline_cache.try_emplace(real_uid);
}

void ShaderCache::AppendGXPipelineUID(const GXPipelineUid& uid)
{
  if (!m_uid_list_file.IsOpen())
    return;

  SerializedGXPipelineUid record;
  ToDisk(uid, &record);
  if (m_uid_list_file.WriteBytes(&record, sizeof(record)))
    return;
  WARN_LOG_FMT(VIDEO, "Writing pipeline UID to cache failed, closing file.");
  m_uid_list_file.Close();
}

void ShaderCache::QueueVertexShaderCompile(const VertexShaderUid& uid, u32 priority)
{
  m_vs_cache.shader_map[uid].pending = true;
  m_async_shader_compiler->QueueWorkItem(
      AsyncShaderCompiler::CreateWorkItem<StageJob<VertexShaderUid>>(this, uid), priority);
}

void ShaderCache::QueueVertexUberShaderCompile(const UberShader::VertexShaderUid& uid, u32 priority)
{
  m_uber_vs_cache.shader_map[uid].pending = true;
  m_async_shader_compiler->QueueWorkItem(
      AsyncShaderCompiler::CreateWorkItem<StageJob<UberShader::VertexShaderUid>>(this, uid),
      priority);
}

void ShaderCache::QueuePixelShaderCompile(const PixelShaderUid& uid, u32 priority)
{
  m_ps_cache.shader_map[uid].pending = true;
  m_async_shader_compiler->QueueWorkItem(
      AsyncShaderCompiler::CreateWorkItem<StageJob<PixelShaderUid>>(this, uid), priority);
}

void ShaderCache::QueuePixelUberShaderCompile(const UberShader::PixelShaderUid& uid, u32 priority)
{
  m_uber_ps_cache.shader_map[uid].pending = true;
  m_async_shader_compiler->QueueWorkItem(
      AsyncShaderCompiler::CreateWorkItem<StageJob<UberShader::PixelShaderUid>>(this, uid),
      priority);
}

bool ShaderCache::PipelineStagesReady(const GXPipelineUid& uid, u32 priority)
{
  const GXPipelineUid adjusted = ApplyDriverBugs(uid);
  const bool vs_ready = StageBuiltElseQueue(m_vs_cache, adjusted.vs_uid, [&](const auto& vs) {
    QueueVertexShaderCompile(vs, priority);
  });

  PixelShaderUid ps_uid = adjusted.ps_uid;
  ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  const bool ps_ready = StageBuiltElseQueue(
      m_ps_cache, ps_uid, [&](const auto& ps) { QueuePixelShaderCompile(ps, priority); });

  return vs_ready && ps_ready;
}

bool ShaderCache::PipelineStagesReady(const GXUberPipelineUid& uid, u32 priority)
{
  const GXUberPipelineUid adjusted = AdjustUberUid(uid);
  const bool vs_ready = StageBuiltElseQueue(m_uber_vs_cache, adjusted.vs_uid, [&](const auto& vs) {
    QueueVertexUberShaderCompile(vs, priority);
  });

  UberShader::PixelShaderUid ps_uid = adjusted.ps_uid;
  UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
  const bool ps_ready = StageBuiltElseQueue(
      m_uber_ps_cache, ps_uid, [&](const auto& ps) { QueuePixelUberShaderCompile(ps, priority); });

  return vs_ready && ps_ready;
}

void ShaderCache::QueuePipelineCompile(const GXPipelineUid& uid, u32 priority)
{
  m_async_shader_compiler->QueueWorkItem(
      AsyncShaderCompiler::CreateWorkItem<PipelineJob<GXPipelineUid>>(this, uid, priority),
      priority);
  m_gx_pipeline_cache[uid].second = true;
}

void ShaderCache::QueueUberPipelineCompile(const GXUberPipelineUid& uid, u32 priority)
{
  m_async_shader_compiler->QueueWorkItem(
      AsyncShaderCompiler::CreateWorkItem<PipelineJob<GXUberPipelineUid>>(this, uid, priority),
      priority);
  m_gx_uber_pipeline_cache[uid].second = true;
}

// Seeds the ubershader pipeline map with every combination a game could ask for, so they can be
// built before the game starts.
void ShaderCache::QueueUberShaderPipelines()
{
  // Ubershader pipelines all share one vertex format with every attribute enabled; this
  // position-only declaration is expanded to it.
  PortableVertexDeclaration minimal_decl = {};
  minimal_decl.position.components = 4;
  minimal_decl.position.type = ComponentFormat::Float;
  minimal_decl.position.enable = true;
  minimal_decl.stride = sizeof(float) * 4;
  NativeVertexFormat* const uber_format = VertexLoaderManager::GetUberVertexFormat(minimal_decl);

  const auto add = [&](const UberShader::VertexShaderUid& vs, const GeometryShaderUid& gs,
                       const UberShader::PixelShaderUid& ps, const BlendingState& blend) {
    GXUberPipelineUid uid;
    uid.vertex_format = uber_format;
    uid.vs_uid = vs;
    uid.gs_uid = gs;
    uid.ps_uid = ps;
    const auto primitive = static_cast<PrimitiveType>(gs.GetUidData()->primitive_type);
    uid.rasterization_state = RenderState::GetCullBackFaceRasterizationState(primitive);
    uid.depth_state = RenderState::GetNoDepthTestingDepthState();
    uid.blending_state = blend;
    // Integer output only exists for logic ops, so give the pipeline one.
    if (ps.GetUidData()->uint_output)
    {
      uid.blending_state.logic_op_enable = true;
      uid.blending_state.logic_mode = LogicOp::And;
    }
    m_gx_uber_pipeline_cache.try_emplace(uid);
  };

  // With a dynamic vertex loader the vertex layout no longer forces variants, but drivers still
  // specialise internally on parts of the blend state (which parts differs between vendors and
  // APIs). Building the write-mask and dual-source variants up front avoids most of those
  // hidden recompiles.
  const auto add_blend_variants = [&](const UberShader::VertexShaderUid& vs,
                                      const GeometryShaderUid& gs,
                                      const UberShader::PixelShaderUid& ps) {
    BlendingState blend = RenderState::GetNoBlendingBlendState();
    add(vs, gs, ps, blend);
    if (!g_backend_info.bSupportsDynamicVertexLoader)
      return;

    blend.alpha_update = false;
    add(vs, gs, ps, blend);
    blend.alpha_update = true;
    blend.color_update = false;
    add(vs, gs, ps, blend);
    blend.color_update = true;

    const auto* ps_data = ps.GetUidData();
    if (ps_data->no_dual_src || ps_data->uint_output)
      return;
    blend.blend_enable = true;
    blend.use_dual_src = true;
    blend.src_factor = SrcBlendFactor::SrcAlpha;
    blend.dst_factor = DstBlendFactor::InvSrcAlpha;
    add(vs, gs, ps, blend);
  };

  UberShader::EnumerateVertexShaderUids([&](const UberShader::VertexShaderUid& vs) {
    const u32 texgens = vs.GetUidData()->num_texgens;
    UberShader::EnumeratePixelShaderUids([&](const UberShader::PixelShaderUid& ps) {
      // The texgen counts of the stages always agree in real draws.
      if (ps.GetUidData()->num_texgens != texgens)
        return;
      UberShader::PixelShaderUid trimmed_ps = ps;
      UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &trimmed_ps);

      EnumerateGeometryShaderUids([&](const GeometryShaderUid& gs) {
        const auto* gs_data = gs.GetUidData();
        if (gs_data->numTexGens != texgens)
          return;
        if (!gs_data->IsPassthrough() && !m_host_config.backend_geometry_shaders)
          return;
        add_blend_variants(vs, gs, trimmed_ps);
      });
    });
  });
}

const AbstractPipeline*
ShaderCache::GetEFBCopyToVRAMPipeline(const TextureConversionShaderGen::TCShaderUid& uid)
{
  if (const auto found = m_efb_copy_to_vram_pipelines.find(uid);
      found != m_efb_copy_to_vram_pipelines.end())
  {
    return found->second.get();
  }

  const ShaderCode code = TextureConversionShaderGen::GeneratePixelShader(m_api_type, uid.GetUidData());
  const std::unique_ptr<AbstractShader> ps = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, code.GetBuffer(), nullptr,
      fmt::format("EFB copy to VRAM pixel shader: {}", *uid.GetUidData()));

  // A failed build is remembered as null so it is not retried.
  std::unique_ptr<AbstractPipeline> pipeline;
  if (ps)
  {
    const AbstractShader* gs = UseGeometryShaderForEFBCopies() ? m_texcoord_gs.get() : nullptr;
    pipeline = g_gfx->CreatePipeline(UtilityPipeline(m_efb_copy_vs.get(), gs, ps.get(),
                                                     RenderState::GetRGBA8FramebufferState()));
  }
  return m_efb_copy_to_vram_pipelines.emplace(uid, std::move(pipeline)).first->second.get();
}

const AbstractPipeline* ShaderCache::GetEFBCopyToRAMPipeline(const EFBCopyParams& uid)
{
  if (const auto found = m_efb_copy_to_ram_pipelines.find(uid);
      found != m_efb_copy_to_ram_pipelines.end())
  {
    return found->second.get();
  }

  const std::string code = TextureConversionShaderTiled::GenerateEncodingShader(uid, m_api_type);
  const std::unique_ptr<AbstractShader> ps = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, code, nullptr, fmt::format("EFB copy to RAM pixel shader: {}", uid));

  std::unique_ptr<AbstractPipeline> pipeline;
  if (ps)
  {
    pipeline = g_gfx->CreatePipeline(
        UtilityPipeline(m_screen_quad_vs.get(), nullptr, ps.get(),
                        RenderState::GetColorFramebufferState(AbstractTextureFormat::BGRA8)));
  }
  return m_efb_copy_to_ram_pipelines.emplace(uid, std::move(pipeline)).first->second.get();
}

bool ShaderCache::BuildUtilityShaders()
{
  const auto build = [](ShaderStage stage, std::string_view source, std::string_view name) {
    return g_gfx->CreateShaderFromSource(stage, source, nullptr, name);
  };

  m_screen_quad_vs = build(ShaderStage::Vertex,
                           FramebufferShaderGen::GenerateScreenQuadVertexShader(),
                           "Screen quad vertex shader");
  m_texture_copy_vs = build(ShaderStage::Vertex,
                            FramebufferShaderGen::GenerateTextureCopyVertexShader(),
                            "Texture copy vertex shader");
  m_efb_copy_vs = build(ShaderStage::Vertex,
                        TextureConversionShaderGen::GenerateVertexShader(m_api_type).GetBuffer(),
                        "EFB copy vertex shader");
  if (!m_screen_quad_vs || !m_texture_copy_vs || !m_efb_copy_vs)
    return false;

  const bool stereo_copies = UseGeometryShaderForEFBCopies();
  if (stereo_copies)
  {
    m_texcoord_gs = build(ShaderStage::Geometry,
                          FramebufferShaderGen::GeneratePassthroughGeometryShader(1, 0),
                          "Texcoord passthrough geometry shader");
    m_color_gs = build(ShaderStage::Geometry,
                       FramebufferShaderGen::GeneratePassthroughGeometryShader(0, 1),
                       "Color passthrough geometry shader");
    if (!m_texcoord_gs || !m_color_gs)
      return false;
  }

  m_texture_copy_ps = build(ShaderStage::Pixel,
                            FramebufferShaderGen::GenerateTextureCopyPixelShader(),
                            "Texture copy pixel shader");
  m_color_ps = build(ShaderStage::Pixel, FramebufferShaderGen::GenerateColorPixelShader(),
                     "Color pixel shader");
  if (!m_texture_copy_ps || !m_color_ps)
    return false;

  const FramebufferState rgba8 = RenderState::GetRGBA8FramebufferState();
  m_rgba8_copy = g_gfx->CreatePipeline(
      UtilityPipeline(m_texture_copy_vs.get(), nullptr, m_texture_copy_ps.get(), rgba8));
  if (!m_rgba8_copy)
    return false;

  if (stereo_copies)
  {
    m_rgba8_stereo_copy = g_gfx->CreatePipeline(UtilityPipeline(
        m_texture_copy_vs.get(), m_texcoord_gs.get(), m_texture_copy_ps.get(), rgba8));
    if (!m_rgba8_stereo_copy)
      return false;
  }

  if (!m_host_config.backend_palette_conversion)
    return true;

  for (size_t index = 0; index < NUM_PALETTE_CONVERSION_SHADERS; ++index)
  {
    const auto format = static_cast<TLUTFormat>(index);
    const std::unique_ptr<AbstractShader> ps =
        build(ShaderStage::Pixel,
              TextureConversionShaderTiled::GeneratePaletteConversionShader(format, m_api_type),
              fmt::format("Palette conversion pixel shader: {}", format));
    if (!ps)
      return false;
    m_palette_conversion_pipelines[index] =
        g_gfx->CreatePipeline(UtilityPipeline(m_screen_quad_vs.get(), nullptr, ps.get(), rgba8));
    if (!m_palette_conversion_pipelines[index])
      return false;
  }
  return true;
}

const AbstractPipeline* ShaderCache::GetPaletteConversionPipeline(TLUTFormat format)
{
  const auto index = static_cast<size_t>(format);
  ASSERT(index < NUM_PALETTE_CONVERSION_SHADERS);
  return m_palette_conversion_pipelines[index].get();
}

const AbstractPipeline* ShaderCache::GetTextureReinterpretPipeline(TextureFormat from_format,
                                                                   TextureFormat to_format)
{
  // The slot is made first, so a pair that cannot be reinterpreted stays null.
  const auto [slot, is_new] =
      m_texture_reinterpret_pipelines.emplace(std::make_pair(from_format, to_format), nullptr);
  if (!is_new)
    return slot->second.get();

  const std::string source =
      FramebufferShaderGen::GenerateTextureReinterpretShader(from_format, to_format);
  if (source.empty())
    return nullptr;

  const std::unique_ptr<AbstractShader> ps = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, source, nullptr,
      fmt::format("Texture reinterpret pixel shader: {} to {}", from_format, to_format));
  if (!ps)
    return nullptr;

  slot->second = g_gfx->CreatePipeline(UtilityPipeline(m_screen_quad_vs.get(), nullptr, ps.get(),
                                                       RenderState::GetRGBA8FramebufferState()));
  return slot->second.get();
}

const AbstractShader*
ShaderCache::GetTextureDecodingShader(TextureFormat format,
                                      std::optional<TLUTFormat> palette_format)
{
  const std::pair<u32, u32> key(static_cast<u32>(format),
                                static_cast<u32>(palette_format.value_or(TLUTFormat::IA8)));
  const auto [slot, is_new] = m_texture_decoding_shaders.emplace(key, nullptr);
  if (!is_new)
    return slot->second.get();

  // The decoders are written in GLSL whatever the backend.
  const std::string source =
      TextureConversionShaderTiled::GenerateDecodingShader(format, palette_format, APIType::OpenGL);
  if (source.empty())
    return nullptr;

  std::string name;
  if (palette_format)
    name = fmt::format("Texture decoding compute shader: {}, {}", format, *palette_format);
  else
    name = fmt::format("Texture decoding compute shader: {}", format);

  std::unique_ptr<AbstractShader> cs =
      g_gfx->CreateShaderFromSource(ShaderStage::Compute, source, nullptr, name);
  if (!cs)
    return nullptr;
  slot->second = std::move(cs);
  return slot->second.get();
}
}  // namespace VideoCommon
