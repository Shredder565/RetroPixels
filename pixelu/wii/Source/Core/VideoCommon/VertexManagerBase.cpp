// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexManagerBase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/SmallVector.h"

#include "Core/DolphinAnalytics.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/CustomShaderCache.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

std::unique_ptr<VertexManagerBase> g_vertex_manager;

using OpcodeDecoder::Primitive;

namespace
{
// The host primitive a GX primitive is drawn as. Triangles become strips when primitive
// restart separates them.
PrimitiveType HostPrimitive(Primitive primitive, bool restart)
{
  if (primitive >= Primitive::GX_DRAW_POINTS)
    return PrimitiveType::Points;
  if (primitive >= Primitive::GX_DRAW_LINES)
    return PrimitiveType::Lines;
  return restart ? PrimitiveType::TriangleStrip : PrimitiveType::Triangles;
}

// Projection aspect ratio over viewport aspect ratio. Neither PAL nor NTSC (BT.601) has square
// pixels, so the heuristic compares this with a generous margin.
float ProjectionViewportRatio(const Projection::Raw& projection, const Viewport& viewport)
{
  const float projection_ar = projection[2] / projection[0];
  const float viewport_ar = viewport.wd / viewport.ht;
  return std::abs(projection_ar / viewport_ar);
}

bool NearRatio(float ratio, float target, const VideoConfig& config)
{
  return std::abs(ratio - target) < config.widescreen_heuristic_aspect_ratio_slop;
}

// Index buffer space needed per GX vertex, as "this many vertices fit in `free_indices`":
// free_indices / divisor * multiplier + extra (in u32 arithmetic).
struct IndexBudget
{
  u32 divisor;
  u32 multiplier;
  u32 extra;

  u32 Fit(u32 free_indices) const { return free_indices / divisor * multiplier + extra; }
};

std::optional<IndexBudget> BudgetFor(Primitive primitive, bool restart, bool vs_expand)
{
  switch (primitive)
  {
  case Primitive::GX_DRAW_QUADS:
  case Primitive::GX_DRAW_QUADS_2:
    return restart ? IndexBudget{5, 4, 0} : IndexBudget{6, 4, 0};
  case Primitive::GX_DRAW_TRIANGLES:
    return restart ? IndexBudget{4, 3, 0} : IndexBudget{1, 1, 0};
  case Primitive::GX_DRAW_TRIANGLE_STRIP:
    // With restart one index is kept back for the restart (wrapping when none are free).
    return restart ? IndexBudget{1, 1, 0xffffffffu} : IndexBudget{3, 1, 2};
  case Primitive::GX_DRAW_TRIANGLE_FAN:
    return restart ? IndexBudget{6, 4, 1} : IndexBudget{3, 1, 2};
  case Primitive::GX_DRAW_LINES:
    if (!vs_expand)
      return IndexBudget{1, 1, 0};
    return IndexBudget{restart ? 5u : 6u, 2, 0};
  case Primitive::GX_DRAW_LINE_STRIP:
    if (!vs_expand)
      return IndexBudget{2, 1, 1};
    return IndexBudget{restart ? 5u : 6u, 1, 1};
  case Primitive::GX_DRAW_POINTS:
    if (!vs_expand)
      return IndexBudget{1, 1, 0};
    return IndexBudget{restart ? 5u : 6u, 1, 0};
  }
  return std::nullopt;
}

PerfQueryGroup DepthQueryGroup()
{
  return bpmem.zcontrol.early_ztest ? PQG_ZCOMP_ZCOMPLOC : PQG_ZCOMP;
}
}  // namespace

VertexManagerBase::VertexManagerBase()
    : m_cpu_vertex_buffer(MAXVBUFFERSIZE), m_cpu_index_buffer(MAXIBUFFERSIZE)
{
}

VertexManagerBase::~VertexManagerBase() = default;

bool VertexManagerBase::Initialize()
{
  auto& events = GetVideoEvents();
  m_frame_end_event = events.after_frame_event.Register([this](Core::System&) { OnEndFrame(); });
  m_after_present_event = events.after_present_event.Register(
      [this](const PresentInfo& info) { m_ticks_elapsed = info.emulated_timestamp; });
  m_index_generator.Init();
  m_custom_shader_cache = std::make_unique<CustomShaderCache>();
  m_cpu_cull.Init();
  return true;
}

u32 VertexManagerBase::GetRemainingSize() const
{
  return static_cast<u32>(m_end_buffer_pointer - m_cur_buffer_pointer);
}

void VertexManagerBase::AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices)
{
  m_index_generator.AddIndices(primitive, num_vertices);
}

bool VertexManagerBase::AreAllVerticesCulled(VertexLoaderBase* loader,
                                             OpcodeDecoder::Primitive primitive, const u8* src,
                                             u32 count)
{
  return m_cpu_cull.AreAllVerticesCulled(loader, primitive, src, count);
}

DataReader VertexManagerBase::PrepareForAdditionalData(OpcodeDecoder::Primitive primitive,
                                                       u32 count, u32 stride, bool cullall)
{
  // EFB pokes share these buffers, so they go out first.
  g_framebuffer_manager->FlushEFBPokes();

  // The JIT loader may store up to 4 bytes past the last vertex.
  const u32 vertex_bytes = count * stride + 4;

  // A batch holds one kind of host primitive.
  const PrimitiveType wanted = HostPrimitive(primitive, g_backend_info.bSupportsPrimitiveRestart);
  if (wanted != m_current_primitive_type) [[unlikely]]
  {
    Flush();
    m_current_primitive_type = wanted;
    // Line and point culling depends on the primitive.
    SetRasterizationStateChanged();
  }

  u32 room_buffer = GetRemainingIndices(primitive);
  u32 room_generator = m_index_generator.GetRemainingIndices(primitive);
  if (!m_is_flushed &&
      (count > room_generator || count > room_buffer || vertex_bytes > GetRemainingSize()))
      [[unlikely]]
  {
    Flush();
  }

  m_cull_all = cullall;

  if (m_is_flushed) [[unlikely]]
  {
    if (cullall)
    {
      // Never drawn, so it can stay in CPU memory.
      m_base_buffer_pointer = m_cpu_vertex_buffer.data();
      m_cur_buffer_pointer = m_base_buffer_pointer;
      m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
      m_index_generator.Start(m_cpu_index_buffer.data());
    }
    else
    {
      ResetBuffer(stride);
    }
    room_generator = m_index_generator.GetRemainingIndices(primitive);
    room_buffer = GetRemainingIndices(primitive);
    m_is_flushed = false;
  }

  // A fresh batch can still be too small for one huge draw (vertex shader line/point expansion
  // of a very long draw); that would need splitting the draw or 32-bit indices.
  ASSERT_MSG(VIDEO, count <= room_generator,
             "VertexManager: Too few remaining index values ({} > {}). "
             "32-bit indices or primitive breaking needed.",
             count, room_generator);
  ASSERT_MSG(VIDEO, count <= room_buffer,
             "VertexManager: Buffer not large enough for all indices! ({} > {}) "
             "Increase MAXIBUFFERSIZE or we need primitive breaking after all.",
             count, room_buffer);
  ASSERT_MSG(VIDEO, vertex_bytes <= GetRemainingSize(),
             "VertexManager: Buffer not large enough for all vertices! ({} > {}) "
             "Increase MAXVBUFFERSIZE or we need primitive breaking after all.",
             vertex_bytes, GetRemainingSize());

  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

DataReader VertexManagerBase::DisableCullAll(u32 stride)
{
  if (m_cull_all)
  {
    m_cull_all = false;
    ResetBuffer(stride);
  }
  return DataReader(m_cur_buffer_pointer, m_end_buffer_pointer);
}

void VertexManagerBase::FlushData(u32 count, u32 stride)
{
  m_cur_buffer_pointer += count * stride;
}

u32 VertexManagerBase::GetRemainingIndices(OpcodeDecoder::Primitive primitive) const
{
  const u32 free_indices = MAXIBUFFERSIZE - m_index_generator.GetIndexLen();
  const bool vs_expand =
      primitive >= Primitive::GX_DRAW_LINES && g_Config.UseVSForLinePointExpand();
  const std::optional<IndexBudget> budget =
      BudgetFor(primitive, g_backend_info.bSupportsPrimitiveRestart, vs_expand);
  return budget ? budget->Fit(free_indices) : 0;
}

auto VertexManagerBase::ResetFlushAspectRatioCount() -> FlushStatistics
{
  FlushStatistics collected = m_flush_statistics;
  m_flush_statistics = {};
  return collected;
}

void VertexManagerBase::ResetBuffer(u32)
{
  m_base_buffer_pointer = m_cpu_vertex_buffer.data();
  m_cur_buffer_pointer = m_base_buffer_pointer;
  m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
  m_index_generator.Start(m_cpu_index_buffer.data());
}

void VertexManagerBase::CommitBuffer(u32, u32, u32, u32* out_base_vertex, u32* out_base_index)
{
  *out_base_vertex = 0;
  *out_base_index = 0;
}

void VertexManagerBase::DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex)
{
  // Pending bounding box changes must reach the GPU before a draw that updates it.
  if (g_bounding_box->IsEnabled() && g_ActiveConfig.bBBoxEnable && g_backend_info.bSupportsBBox)
    g_bounding_box->Flush();

  g_gfx->DrawIndexed(base_index, num_indices, base_vertex);
}

void VertexManagerBase::UploadUniforms()
{
}

void VertexManagerBase::InvalidateConstants()
{
  auto& system = Core::System::GetInstance();
  system.GetVertexShaderManager().dirty = true;
  system.GetGeometryShaderManager().dirty = true;
  system.GetPixelShaderManager().dirty = true;
}

void VertexManagerBase::UploadUtilityUniforms(const void*, u32)
{
}

void VertexManagerBase::UploadUtilityVertices(const void* vertices, u32 vertex_stride,
                                              u32 num_vertices, const u16* indices, u32 num_indices,
                                              u32* out_base_vertex, u32* out_base_index)
{
  // GX vertices must have been drawn before a utility draw reuses the buffers.
  ASSERT(m_is_flushed);

  ResetBuffer(std::max(vertex_stride, 1u));
  if (vertices != nullptr)
  {
    const u32 bytes = vertex_stride * num_vertices;
    ASSERT((m_cur_buffer_pointer + bytes) <= m_end_buffer_pointer);
    std::memcpy(m_cur_buffer_pointer, vertices, bytes);
    m_cur_buffer_pointer += bytes;
  }
  if (indices != nullptr)
    m_index_generator.AddExternalIndices(indices, num_indices, num_vertices);

  CommitBuffer(num_vertices, vertex_stride, num_indices, out_base_vertex, out_base_index);
}

u32 VertexManagerBase::GetTexelBufferElementSize(TexelBufferFormat buffer_format)
{
  // 1, 2, 4 and 8 bytes.
  return 1u << static_cast<u32>(buffer_format);
}

bool VertexManagerBase::UploadTexelBuffer(const void*, u32, TexelBufferFormat, u32*)
{
  return false;
}

bool VertexManagerBase::UploadTexelBuffer(const void*, u32, TexelBufferFormat, u32*, const void*,
                                          u32, TexelBufferFormat, u32*)
{
  return false;
}

BitSet32 VertexManagerBase::UsedTextures() const
{
  BitSet32 used;
  const u32 stages = bpmem.genMode.numtevstages + 1u;

  // Texture maps sampled directly by TEV stages.
  for (u32 stage = 0; stage < stages; stage++)
  {
    const auto& order = bpmem.tevorders[stage / 2];
    if (order.getEnable(stage & 1))
      used[order.getTexMap(stage & 1)] = true;
  }

  // Texture maps sampled by the indirect stages that active TEV stages use.
  const u32 indirect_stages = bpmem.genMode.numindstages;
  if (indirect_stages != 0)
  {
    for (u32 stage = 0; stage < stages; stage++)
    {
      const auto& ind = bpmem.tevind[stage];
      if (ind.IsActive() && ind.bt < indirect_stages)
        used[bpmem.tevindref.getTexMap(static_cast<int>(ind.bt.Value()))] = true;
    }
  }
  return used;
}

void VertexManagerBase::UpdateFlushStatistics()
{
  // Projections seen per flush, for the anamorphic widescreen heuristic (GameCube only).
  const auto& projection = xfmem.projection.rawProjection;
  const auto& viewport = xfmem.viewport;
  FlushStatistics::ProjectionCounts& counts =
      xfmem.projection.type == ProjectionType::Perspective ? m_flush_statistics.perspective :
                                                             m_flush_statistics.orthographic;

  const float ratio = ProjectionViewportRatio(projection, viewport);
  counts.average_ratio.Push(ratio);

  const size_t indices = m_index_generator.GetIndexLen();
  if (NearRatio(ratio, g_ActiveConfig.widescreen_heuristic_widescreen_ratio, g_ActiveConfig))
  {
    counts.anamorphic_flush_count++;
    counts.anamorphic_vertex_count += indices;
  }
  else if (NearRatio(ratio, g_ActiveConfig.widescreen_heuristic_standard_ratio, g_ActiveConfig))
  {
    counts.normal_flush_count++;
    counts.normal_vertex_count += indices;
  }
  else
  {
    counts.other_flush_count++;
    counts.other_vertex_count += indices;
  }
}

void VertexManagerBase::Flush()
{
  if (m_is_flushed)
    return;
  m_is_flushed = true;

  // The first draw of a frame starts it.
  if (m_draw_counter == 0)
    GetVideoEvents().before_frame_event.Trigger();

  const bool texgens_differ = xfmem.numTexGen.numTexGens != bpmem.genMode.numtexgens;
  const bool colors_differ = xfmem.numChan.numColorChans != bpmem.genMode.numcolchans;
  if (texgens_differ || colors_differ)
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Mismatched configuration between XF and BP stages - {}/{} texgens, {}/{} colors. "
        "Skipping draw. Please report on the issue tracker.",
        xfmem.numTexGen.numTexGens, bpmem.genMode.numtexgens.Value(), xfmem.numChan.numColorChans,
        bpmem.genMode.numcolchans.Value());
    // Reported so the games that do this can be found.
    if (texgens_differ)
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUTexGensBetweenXFAndBP);
    if (colors_differ)
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUColorsBetweenXFAndBP);
    return;
  }

  auto& system = Core::System::GetInstance();
  if (!system.IsWii())
    UpdateFlushStatistics();

  auto& pixel_shader_manager = system.GetPixelShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& xf_state_manager = system.GetXFStateManager();

  if (g_ActiveConfig.bGraphicMods)
  {
    const double seconds = static_cast<double>(m_ticks_elapsed) /
                           system.GetSystemTimers().GetTicksPerSecond();
    pixel_shader_manager.constants.time_ms = static_cast<u32>(seconds * 1000);
  }

  NativeVertexFormat* const format = VertexLoaderManager::GetCurrentVertexFormat();
  CalculateNormals(format);

  // Load the textures this draw samples. With graphics mods, also note each texture name (and
  // the first unit it is on) for the mods' draw actions.
  const BitSet32 used_textures = UsedTextures();
  std::vector<std::string> texture_names;
  Common::SmallVector<u32, 8> texture_units;
  std::array<SamplerState, 8> samplers;
  if (!m_cull_all)
  {
    const bool mods = g_ActiveConfig.bGraphicMods;
    for (const int bit : used_textures)
    {
      const u32 unit = static_cast<u32>(bit);
      const auto entry = g_texture_cache->Load(unit);
      if (!entry)
        continue;
      if (mods && !Common::Contains(texture_names, entry->texture_info_name))
      {
        texture_names.push_back(entry->texture_info_name);
        texture_units.push_back(unit);
      }
      const float scale =
          static_cast<float>(entry->GetWidth()) / static_cast<float>(entry->native_width);
      samplers[unit] = TextureCacheBase::GetSamplerState(unit, scale, entry->is_custom_tex,
                                                         entry->has_arbitrary_mips);
    }
  }
  vertex_shader_manager.SetConstants(texture_names, xf_state_manager);

  // Without z-freeze this batch defines the depth slope (this needs the vertex shader constants
  // set above). With z-freeze the saved slope is applied once.
  if (!bpmem.genMode.zfreeze)
  {
    CalculateZSlope(format);
  }
  else if (m_zslope.dirty && !m_cull_all)
  {
    pixel_shader_manager.SetZSlope(m_zslope.dfdx, m_zslope.dfdy, m_zslope.f0);
    m_zslope.dirty = false;
  }

  if (!m_cull_all)
  {
    DrawBatch(used_textures, samplers, texture_names, texture_units, pixel_shader_manager,
              geometry_shader_manager);
  }
}

void VertexManagerBase::DrawBatch(const BitSet32& used_textures,
                                  const std::array<SamplerState, 8>& samplers,
                                  const std::vector<std::string>& texture_names,
                                  const Common::SmallVector<u32, 8>& texture_units,
                                  PixelShaderManager& pixel_shader_manager,
                                  GeometryShaderManager& geometry_shader_manager)
{
  // Let graphics mods see the draw: they may skip it or supply custom pixel shaders.
  CustomPixelShaderContents custom_shaders;
  std::optional<CustomPixelShader> offered_shader;
  std::vector<std::string> custom_shader_textures;
  std::span<u8> custom_uniforms;
  bool skip = false;
  for (const std::string& name : texture_names)
  {
    GraphicsModActionData::DrawStarted draw_started{texture_units, &skip, &offered_shader,
                                                    &custom_uniforms};
    for (const auto& action : g_graphics_mod_manager->GetDrawStartedActions(name))
    {
      action->OnDrawStarted(&draw_started);
      if (offered_shader)
      {
        custom_shaders.shaders.push_back(*offered_shader);
        custom_shader_textures.push_back(name);
      }
      offered_shader = std::nullopt;
    }
  }

  // From CommitBuffer() on no utility vertices may be uploaded, or the batch's buffers would be
  // replaced.
  if (m_index_generator.GetIndexLen() == 0)
    return;

  // Binding may apply palettes and decode textures on the GPU, which use utility draws without
  // vertices and compute shaders, so this is still safe here.
  g_texture_cache->BindTextures(used_textures, samplers);

  if (!skip)
  {
    UpdatePipelineConfig();
    UpdatePipelineObject();
    if (m_current_pipeline_object != nullptr)
    {
      const AbstractPipeline* pipeline = m_current_pipeline_object;
      if (!custom_shaders.shaders.empty())
      {
        if (const AbstractPipeline* custom = GetCustomPipeline(custom_shaders, pipeline))
          pipeline = custom;
      }
      RenderDrawCall(pixel_shader_manager, geometry_shader_manager, custom_uniforms,
                     m_current_primitive_type, pipeline);
    }
  }

  // A skipped draw still counts for the emulated state.
  OnDraw();
  g_framebuffer_manager->FlagPeekCacheAsOutOfDate();
}

void VertexManagerBase::DoState(PointerWrap& p)
{
  // Loading a state drops whatever is batched first.
  if (p.IsReadMode())
    Flush();

  p.Do(m_zslope);
  p.Do(VertexLoaderManager::normal_cache);
  p.Do(VertexLoaderManager::tangent_cache);
  p.Do(VertexLoaderManager::binormal_cache);
}

void VertexManagerBase::CalculateZSlope(NativeVertexFormat* format)
{
  const float offset_x = xfmem.viewport.xOrig - static_cast<float>(bpmem.scissorOffset.x * 2);
  const float offset_y = xfmem.viewport.yOrig - static_cast<float>(bpmem.scissorOffset.y * 2);

  if (m_current_primitive_type != PrimitiveType::Triangles &&
      m_current_primitive_type != PrimitiveType::TriangleStrip)
  {
    return;
  }

  u32 matrix = g_main_cp_state.matrix_index_a.PosNormalMtxIdx;
  const PortableVertexDeclaration decl = format->GetVertexDeclaration();

  // The last triangle needs three vertices in this batch.
  if ((m_cur_buffer_pointer - m_base_buffer_pointer) < (decl.stride * 3))
    return;

  // Transform the last three vertices (kept by the loaders, newest last in the cache) to
  // screen space.
  auto& vertex_shader_manager = Core::System::GetInstance().GetVertexShaderManager();
  struct ScreenPoint
  {
    float x, y, z;
  };
  std::array<ScreenPoint, 3> pt;
  for (size_t i = 0; i < 3; i++)
  {
    const size_t row = 2 - i;
    if (decl.posmtx.enable)
      matrix = VertexLoaderManager::position_matrix_index_cache[row];
    if (decl.position.components == 2)
      VertexLoaderManager::position_cache[row][2] = 0;

    std::array<float, 4> clip;
    vertex_shader_manager.TransformToClipSpace(&VertexLoaderManager::position_cache[row][0],
                                               clip.data(), matrix);
    const float inv_w = 1.0f / clip[3];
    pt[i].x = clip[0] * inv_w * xfmem.viewport.wd + offset_x;
    pt[i].y = clip[1] * inv_w * xfmem.viewport.ht + offset_y;
    pt[i].z = clip[2] * inv_w * xfmem.viewport.zRange + xfmem.viewport.farZ;
  }

  // Plane through the three points: z = f0 + dfdx * x + dfdy * y.
  const float ex_ca = pt[2].x - pt[0].x;
  const float ex_ab = pt[0].x - pt[1].x;
  const float ey_ab = pt[0].y - pt[1].y;
  const float ey_ca = pt[2].y - pt[0].y;
  const float ez_ca = pt[2].z - pt[0].z;
  const float ez_ba = pt[1].z - pt[0].z;
  const float nx = ez_ca * -ey_ab - ez_ba * ey_ca;
  const float ny = ex_ca * ez_ba + ex_ab * ez_ca;
  const float nz = -ex_ab * ey_ca - ex_ca * -ey_ab;

  // Degenerate triangles have no plane.
  if (nz == 0)
    return;

  m_zslope.dfdx = -nx / nz;
  m_zslope.dfdy = -ny / nz;
  m_zslope.f0 = pt[0].z - (pt[0].x * m_zslope.dfdx + pt[0].y * m_zslope.dfdy);
  m_zslope.dirty = true;
}

void VertexManagerBase::CalculateNormals(NativeVertexFormat* format)
{
  // Formats without tangent/binormal (or without a normal) take the cached vectors from the
  // last vertex that had them, as vertex shader constants. Formats that have them leave the
  // caches to the loaders.
  const PortableVertexDeclaration decl = format->GetVertexDeclaration();
  if (decl.normals[1].enable)
    return;

  auto& vertex_shader_manager = Core::System::GetInstance().GetVertexShaderManager();
  auto& constants = vertex_shader_manager.constants;
  const auto publish = [&vertex_shader_manager](auto& constant, std::array<float, 4>& cache) {
    cache[3] = 0;
    if (constant != cache)
    {
      constant = cache;
      vertex_shader_manager.dirty = true;
    }
  };

  VertexLoaderManager::tangent_cache[3] = 0;
  VertexLoaderManager::binormal_cache[3] = 0;
  publish(constants.cached_tangent, VertexLoaderManager::tangent_cache);
  publish(constants.cached_binormal, VertexLoaderManager::binormal_cache);

  if (decl.normals[0].enable)
    return;
  publish(constants.cached_normal, VertexLoaderManager::normal_cache);
}

void VertexManagerBase::UpdatePipelineConfig()
{
  auto& config = m_current_pipeline_config;
  auto& uber = m_current_uber_pipeline_config;

  NativeVertexFormat* const format = VertexLoaderManager::GetCurrentVertexFormat();
  if (format != config.vertex_format)
  {
    config.vertex_format = format;
    uber.vertex_format = VertexLoaderManager::GetUberVertexFormat(format->GetVertexDeclaration());
    m_pipeline_config_changed = true;
  }

  const VertexShaderUid vs = GetVertexShaderUid();
  if (vs != config.vs_uid)
  {
    config.vs_uid = vs;
    uber.vs_uid = UberShader::GetVertexShaderUid();
    m_pipeline_config_changed = true;
  }

  const PixelShaderUid ps = GetPixelShaderUid();
  if (ps != config.ps_uid)
  {
    config.ps_uid = ps;
    uber.ps_uid = UberShader::GetPixelShaderUid();
    m_pipeline_config_changed = true;
  }

  const GeometryShaderUid gs = GetGeometryShaderUid(GetCurrentPrimitiveType());
  if (gs != config.gs_uid)
  {
    config.gs_uid = gs;
    uber.gs_uid = gs;
    m_pipeline_config_changed = true;
  }

  // The fixed-function states are only regenerated when their registers were written.
  if (m_rasterization_state_changed)
  {
    m_rasterization_state_changed = false;
    RasterizationState state = {};
    state.Generate(bpmem, m_current_primitive_type);
    if (state != config.rasterization_state)
    {
      config.rasterization_state = state;
      uber.rasterization_state = state;
      m_pipeline_config_changed = true;
    }
  }
  if (m_depth_state_changed)
  {
    m_depth_state_changed = false;
    DepthState state = {};
    state.Generate(bpmem);
    if (state != config.depth_state)
    {
      config.depth_state = state;
      uber.depth_state = state;
      m_pipeline_config_changed = true;
    }
  }
  if (m_blending_state_changed)
  {
    m_blending_state_changed = false;
    BlendingState state = {};
    state.Generate(bpmem);
    if (state != config.blending_state)
    {
      config.blending_state = state;
      uber.blending_state = state;
      m_pipeline_config_changed = true;
    }
  }
}

void VertexManagerBase::UpdatePipelineObject()
{
  if (!m_pipeline_config_changed)
    return;
  m_current_pipeline_object = nullptr;
  m_pipeline_config_changed = false;

  const ShaderCompilationMode mode = g_ActiveConfig.iShaderCompilationMode;
  switch (mode)
  {
  case ShaderCompilationMode::Synchronous:
    // Compile the specialised shader now, whatever it costs.
    m_current_pipeline_object = g_shader_cache->GetPipelineForUid(m_current_pipeline_config);
    break;

  case ShaderCompilationMode::SynchronousUberShaders:
    m_current_pipeline_object =
        g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
    break;

  case ShaderCompilationMode::AsynchronousUberShaders:
  case ShaderCompilationMode::AsynchronousSkipRendering:
  {
    // Use the specialised shader once it has been compiled in the background.
    const std::optional<const AbstractPipeline*> ready =
        g_shader_cache->GetPipelineForUidAsync(m_current_pipeline_config);
    if (ready)
    {
      m_current_pipeline_object = *ready;
      break;
    }
    if (mode == ShaderCompilationMode::AsynchronousUberShaders)
    {
      // Meanwhile draw with the ubershader.
      m_current_pipeline_object =
          g_shader_cache->GetUberPipelineForUid(m_current_uber_pipeline_config);
    }
    else
    {
      // Skip the draw, and look again next time even if no register changes.
      m_pipeline_config_changed = true;
    }
    break;
  }
  }
}

void VertexManagerBase::OnConfigChange()
{
  // The vertex shader expansion setting picks the line/point index layouts.
  m_index_generator.Init();
}

void VertexManagerBase::OnDraw()
{
  m_draw_counter++;

  // An EFB copy that came too soon after the previous one is executed once enough draws have
  // followed it.
  const u32 draws_since_copy = m_draw_counter - m_last_efb_copy_draw_counter;
  if (m_unflushed_efb_copy && draws_since_copy > MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }

  if (m_scheduled_command_buffer_kicks.empty() || !m_allow_background_execution)
    return;

  // Kick a command buffer at the draws OnEndFrame scheduled (kept sorted).
  if (std::ranges::binary_search(m_scheduled_command_buffer_kicks, m_draw_counter))
  {
    g_gfx->Flush();
    m_unflushed_efb_copy = false;
    m_last_efb_copy_draw_counter = m_draw_counter;
  }
}

void VertexManagerBase::OnCPUEFBAccess()
{
  // Several accesses with no draw in between count once.
  if (!m_cpu_accesses_this_frame.empty() && m_cpu_accesses_this_frame.back() == m_draw_counter)
    return;
  m_cpu_accesses_this_frame.push_back(m_draw_counter);
}

void VertexManagerBase::OnEFBCopyToRAM()
{
  // Without deferred copies the copy is a CPU access to plan for next frame.
  if (!g_ActiveConfig.bDeferEFBCopies)
  {
    OnCPUEFBAccess();
    return;
  }

  // Execute now only if enough draws separate this copy from the previous one.
  const u32 draws_since_copy = m_draw_counter - m_last_efb_copy_draw_counter;
  m_last_efb_copy_draw_counter = m_draw_counter;
  if (draws_since_copy < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
  {
    m_unflushed_efb_copy = true;
    return;
  }
  m_unflushed_efb_copy = false;
  g_gfx->Flush();
}

void VertexManagerBase::OnEndFrame()
{
  m_draw_counter = 0;
  m_last_efb_copy_draw_counter = 0;
  m_scheduled_command_buffer_kicks.clear();

  // With no CPU reads the whole frame stays in one command buffer, for the most CPU/GPU overlap.
  if (m_cpu_accesses_this_frame.empty())
    return;

  // Plan next frame's kicks to shorten readback waits: half way to each access, or every
  // `interval` draws for long stretches. Stretches shorter than the minimum are merged into
  // the next one.
  if (g_ActiveConfig.iCommandBufferExecuteInterval > 0)
  {
    const u32 interval = static_cast<u32>(g_ActiveConfig.iCommandBufferExecuteInterval);
    u32 stretch_start = 0;
    for (const u32 access : m_cpu_accesses_this_frame)
    {
      const u32 draws = access - stretch_start;
      if (draws < MINIMUM_DRAW_CALLS_PER_COMMAND_BUFFER_FOR_READBACK)
        continue;

      if (draws <= interval)
      {
        m_scheduled_command_buffer_kicks.push_back(stretch_start + draws / 2);
      }
      else
      {
        for (u32 at = interval; at < draws; at += interval)
          m_scheduled_command_buffer_kicks.push_back(stretch_start + at);
      }
      stretch_start = access;
    }
  }
  m_cpu_accesses_this_frame.clear();

  // Lets hybrid ubershaders switch to a specialised shader that finished compiling, even when
  // a single pipeline configuration is used all the time.
  InvalidatePipelineObject();
}

void VertexManagerBase::NotifyCustomShaderCacheOfHostChange(const ShaderHostConfig& host_config)
{
  m_custom_shader_cache->SetHostConfig(host_config);
  m_custom_shader_cache->Reload();
}

void VertexManagerBase::RenderDrawCall(PixelShaderManager& pixel_shader_manager,
                                       GeometryShaderManager& geometry_shader_manager,
                                       std::span<u8> custom_pixel_shader_uniforms,
                                       PrimitiveType primitive_type,
                                       const AbstractPipeline* current_pipeline)
{
  // Uniforms go last, after anything else that could have replaced them.
  geometry_shader_manager.SetConstants(primitive_type);
  pixel_shader_manager.SetConstants();
  const bool new_custom_block =
      !custom_pixel_shader_uniforms.empty() &&
      pixel_shader_manager.custom_constants.data() != custom_pixel_shader_uniforms.data();
  if (new_custom_block)
    pixel_shader_manager.custom_constants_dirty = true;
  pixel_shader_manager.custom_constants = custom_pixel_shader_uniforms;
  UploadUniforms();

  g_gfx->SetPipeline(current_pipeline);

  u32 base_vertex;
  u32 base_index;
  CommitBuffer(m_index_generator.GetNumVerts(),
               VertexLoaderManager::GetCurrentVertexFormat()->GetVertexStride(),
               m_index_generator.GetIndexLen(), &base_vertex, &base_index);

  // Vertex shader line/point expansion numbers host vertices 4 per GX vertex; except on D3D
  // (whose shader divides first) the base vertex has to be scaled to match.
  const bool expanded =
      primitive_type == PrimitiveType::Points || primitive_type == PrimitiveType::Lines;
  if (g_backend_info.api_type != APIType::D3D && g_ActiveConfig.UseVSForLinePointExpand() &&
      expanded)
  {
    base_vertex <<= 2;
  }

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->EnableQuery(DepthQueryGroup());

  DrawCurrentBatch(base_index, m_index_generator.GetIndexLen(), base_vertex);
  INCSTAT(g_stats.this_frame.num_draw_calls);

  if (PerfQueryBase::ShouldEmulate())
    g_perf_query->DisableQuery(DepthQueryGroup());
}

const AbstractPipeline*
VertexManagerBase::GetCustomPipeline(const CustomPixelShaderContents& custom_pixel_shader_contents,
                                     const AbstractPipeline* current_pipeline) const
{
  if (current_pipeline == nullptr || custom_pixel_shader_contents.shaders.empty())
    return nullptr;

  CustomShaderInstance instance;
  instance.pixel_contents = custom_pixel_shader_contents;
  const auto& base_config = current_pipeline->m_config;

  // An empty result means the pipeline is still being compiled.
  const auto specialised = [&] {
    return m_custom_shader_cache->GetPipelineAsync(m_current_pipeline_config, instance,
                                                   base_config);
  };
  const auto uber = [&] {
    return m_custom_shader_cache->GetPipelineAsync(m_current_uber_pipeline_config, instance,
                                                   base_config);
  };

  std::optional<const AbstractPipeline*> found;
  switch (g_ActiveConfig.iShaderCompilationMode)
  {
  case ShaderCompilationMode::Synchronous:
  case ShaderCompilationMode::AsynchronousSkipRendering:
    found = specialised();
    break;
  case ShaderCompilationMode::SynchronousUberShaders:
    // D3D has trouble compiling big custom ubershaders, so it gets specialised ones.
    found = g_backend_info.api_type == APIType::D3D ? specialised() : uber();
    break;
  case ShaderCompilationMode::AsynchronousUberShaders:
    found = specialised();
    if (!found)
      found = uber();
    break;
  }
  if (found)
    return *found;
  return nullptr;
}
