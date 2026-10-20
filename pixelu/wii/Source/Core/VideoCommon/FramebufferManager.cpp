// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/FramebufferManager.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/System.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

std::unique_ptr<FramebufferManager> g_framebuffer_manager;

namespace
{
// Poke batches are drawn once this many vertices (six per poked pixel) would be exceeded.
constexpr size_t POKE_BATCH_VERTICES = 32768;

constexpr u32 LAST_EFB_ROW = EFB_HEIGHT - 1;

u32 EFBLayerCount()
{
  return g_ActiveConfig.stereo_mode != StereoMode::Off ? 2 : 1;
}

// Clear value for a fresh EFB: black, and the far plane in the backend's depth convention.
void ClearToDefaults(AbstractFramebuffer* framebuffer)
{
  const float far_depth = g_backend_info.bSupportsReversedDepthRange ? 1.0f : 0.0f;
  g_gfx->SetAndClearFramebuffer(framebuffer, {{0.0f, 0.0f, 0.0f, 0.0f}}, far_depth);
}

// Rectangle clamped to the EFB texture.
MathUtil::Rectangle<int> ClampToEFB(MathUtil::Rectangle<int> rect, u32 width, u32 height)
{
  rect.ClampUL(0, 0, static_cast<int>(width), static_cast<int>(height));
  return rect;
}

// A utility pipeline that draws a full-screen triangle into a single colour target.
AbstractPipelineConfig FullScreenUtilityConfig(const AbstractShader* vertex_shader,
                                               const AbstractShader* geometry_shader,
                                               const AbstractShader* pixel_shader,
                                               const FramebufferState& target)
{
  AbstractPipelineConfig config = {};
  config.vertex_shader = vertex_shader;
  config.geometry_shader = geometry_shader;
  config.pixel_shader = pixel_shader;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = target;
  config.usage = AbstractPipelineUsage::Utility;
  return config;
}

std::unique_ptr<AbstractShader> CompilePixelShader(const std::string& source, std::string_view name)
{
  return g_gfx->CreateShaderFromSource(ShaderStage::Pixel, source, nullptr, name);
}

std::unique_ptr<AbstractShader> CompileVertexShader(const std::string& source,
                                                    std::string_view name)
{
  return g_gfx->CreateShaderFromSource(ShaderStage::Vertex, source, nullptr, name);
}
}  // namespace

FramebufferManager::FramebufferManager() : m_last_pixel_format(PixelFormat::INVALID_FMT)
{
}

FramebufferManager::~FramebufferManager()
{
  DropClearPipelines();
  DropPokePipelines();
  DropConversionPipelines();
  DropCopyPipelines();
  DestroyPeekCaches();
  DestroyEFB();

  // What the teardown above does not cover goes last, in this order.
  m_frame_end_hook.reset();
  m_restore_pipeline.reset();
  m_color_resolve_pipeline.reset();
  m_color_resolve_target.reset();
}

bool FramebufferManager::Initialize(int efb_scale)
{
  if (!CreateEFB(efb_scale))
  {
    PanicAlertFmt("Failed to create EFB framebuffer");
    return false;
  }

  m_tile_size = static_cast<u32>(std::max(g_ActiveConfig.iEFBAccessTileSize, 0));

  // Each step names itself when it fails.
  const std::pair<bool (FramebufferManager::*)(), const char*> steps[] = {
      {&FramebufferManager::CreatePeekCaches, "Failed to create EFB readback framebuffer"},
      {&FramebufferManager::BuildCopyPipelines, "Failed to compile EFB readback pipelines"},
      {&FramebufferManager::BuildConversionPipelines,
       "Failed to compile EFB conversion pipelines"},
      {&FramebufferManager::BuildClearPipelines, "Failed to compile EFB clear pipelines"},
      {&FramebufferManager::BuildPokePipelines, "Failed to compile EFB poke pipelines"},
  };
  for (const auto& [step, failure] : steps)
  {
    if (!(this->*step)())
    {
      PanicAlertFmt("{}", failure);
      return false;
    }
  }

  m_frame_end_hook =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { EndOfFrame(); });
  return true;
}

void FramebufferManager::RecreateEFBFramebuffer(int efb_scale)
{
  FlushEFBPokes();
  InvalidatePeekCache(true);

  DestroyPeekCaches();
  DestroyEFB();
  const bool ok = CreateEFB(efb_scale) && CreatePeekCaches();
  if (!ok)
    PanicAlertFmt("Failed to recreate EFB framebuffer");
}

void FramebufferManager::RecompileShaders()
{
  DropPokePipelines();
  DropClearPipelines();
  DropConversionPipelines();
  DropCopyPipelines();

  const bool ok = BuildCopyPipelines() && BuildConversionPipelines() && BuildClearPipelines() &&
                  BuildPokePipelines();
  if (!ok)
    PanicAlertFmt("Failed to recompile EFB pipelines");
}

AbstractTextureFormat FramebufferManager::GetEFBColorFormat()
{
  // Whatever pixel format the game picks (RGB8, RGBA6 or RGB565), the host buffer is RGBA8;
  // the format matters only where alpha is or is not present.
  return AbstractTextureFormat::RGBA8;
}

AbstractTextureFormat FramebufferManager::GetEFBDepthFormat()
{
  // Drivers that cannot clear D32F get D24_S8 instead (copies still go to R32F).
  return DriverDetails::HasBug(DriverDetails::BUG_BROKEN_D32F_CLEAR) ?
             AbstractTextureFormat::D24_S8 :
             AbstractTextureFormat::D32F;
}

AbstractTextureFormat FramebufferManager::GetEFBDepthCopyFormat()
{
  return AbstractTextureFormat::R32F;
}

TextureConfig FramebufferManager::GetEFBColorTextureConfig(u32 width, u32 height)
{
  return TextureConfig(width, height, 1, EFBLayerCount(), g_ActiveConfig.iMultisamples,
                       GetEFBColorFormat(), AbstractTextureFlag_RenderTarget,
                       AbstractTextureType::Texture_2DArray);
}

TextureConfig FramebufferManager::GetEFBDepthTextureConfig(u32 width, u32 height)
{
  return TextureConfig(width, height, 1, EFBLayerCount(), g_ActiveConfig.iMultisamples,
                       GetEFBDepthFormat(), AbstractTextureFlag_RenderTarget,
                       AbstractTextureType::Texture_2DArray);
}

FramebufferState FramebufferManager::GetEFBFramebufferState() const
{
  FramebufferState state = {};
  state.color_texture_format = m_color->GetFormat();
  state.depth_texture_format = m_depth->GetFormat();
  state.per_sample_shading = IsEFBMultisampled() && g_ActiveConfig.bSSAA;
  state.samples = m_color->GetSamples();
  return state;
}

MathUtil::Rectangle<int>
FramebufferManager::ConvertEFBRectangle(const MathUtil::Rectangle<int>& rc) const
{
  MathUtil::Rectangle<int> scaled;
  scaled.left = EFBToScaledX(rc.left);
  scaled.right = EFBToScaledX(rc.right);
  scaled.top = EFBToScaledY(rc.top);
  scaled.bottom = EFBToScaledY(rc.bottom);
  return scaled;
}

unsigned int FramebufferManager::GetEFBScale() const
{
  return static_cast<unsigned int>(m_scale);
}

int FramebufferManager::EFBToScaledX(int x) const
{
  return x * static_cast<int>(m_scale);
}

int FramebufferManager::EFBToScaledY(int y) const
{
  return y * static_cast<int>(m_scale);
}

float FramebufferManager::EFBToScaledXf(float x) const
{
  return x * (static_cast<float>(GetEFBWidth()) / static_cast<float>(EFB_WIDTH));
}

float FramebufferManager::EFBToScaledYf(float y) const
{
  return y * (static_cast<float>(GetEFBHeight()) / static_cast<float>(EFB_HEIGHT));
}

std::tuple<u32, u32> FramebufferManager::ComputeEFBSize(int efb_scale)
{
  m_scale = efb_scale == EFB_SCALE_AUTO_INTEGRAL ?
                static_cast<float>(g_presenter->AutoIntegralScale()) :
                static_cast<float>(efb_scale);

  // Never wider than the backend's largest texture.
  const u32 max_size = g_backend_info.MaxTextureSize;
  if (static_cast<float>(max_size) < static_cast<float>(EFB_WIDTH) * m_scale)
    m_scale = static_cast<float>(max_size / EFB_WIDTH);

  const u32 factor = static_cast<u32>(static_cast<int>(m_scale));
  return {std::max(EFB_WIDTH * factor, 1u), std::max(EFB_HEIGHT * factor, 1u)};
}

bool FramebufferManager::CreateEFB(int efb_scale)
{
  const auto [width, height] = ComputeEFBSize(efb_scale);
  const TextureConfig color_cfg = GetEFBColorTextureConfig(width, height);
  const TextureConfig depth_cfg = GetEFBDepthTextureConfig(width, height);

  m_color = g_gfx->CreateTexture(color_cfg, "EFB color texture");
  m_depth = g_gfx->CreateTexture(depth_cfg, "EFB depth texture");
  m_color_spare = g_gfx->CreateTexture(color_cfg, "EFB convert color texture");
  if (!m_color || !m_depth || !m_color_spare)
    return false;

  // Both colour buffers share the depth buffer.
  m_framebuffer = g_gfx->CreateFramebuffer(m_color.get(), m_depth.get());
  m_framebuffer_spare = g_gfx->CreateFramebuffer(m_color_spare.get(), m_depth.get());
  if (!m_framebuffer || !m_framebuffer_spare)
    return false;

  if (g_ActiveConfig.MultisamplingEnabled())
  {
    // Backends without partial resolves resolve by drawing, so the target must be renderable.
    const bool draw_resolve = !g_backend_info.bSupportsPartialMultisampleResolve;
    const u32 flags = draw_resolve ? AbstractTextureFlag_RenderTarget : 0;
    m_color_resolved = g_gfx->CreateTexture(
        TextureConfig(color_cfg.width, color_cfg.height, 1, color_cfg.layers, 1, color_cfg.format,
                      flags, AbstractTextureType::Texture_2DArray),
        "EFB color resolve texture");
    if (!m_color_resolved)
      return false;

    if (draw_resolve)
    {
      m_color_resolve_target = g_gfx->CreateFramebuffer(m_color_resolved.get(), nullptr);
      if (!m_color_resolve_target)
        return false;
    }
  }

  // Depth reads go through an R32F copy for MSAA resolves and for non-R32F depth buffers.
  if (g_ActiveConfig.MultisamplingEnabled() || GetEFBDepthFormat() != GetEFBDepthCopyFormat())
  {
    m_depth_resolved = g_gfx->CreateTexture(
        TextureConfig(depth_cfg.width, depth_cfg.height, 1, depth_cfg.layers, 1,
                      GetEFBDepthCopyFormat(), AbstractTextureFlag_RenderTarget,
                      AbstractTextureType::Texture_2DArray),
        "EFB depth resolve texture");
    if (!m_depth_resolved)
      return false;

    m_depth_resolve_target = g_gfx->CreateFramebuffer(m_depth_resolved.get(), nullptr);
    if (!m_depth_resolve_target)
      return false;
  }

  ClearToDefaults(m_framebuffer.get());

  // The pixel shader constants include the EFB scale.
  Core::System::GetInstance().GetPixelShaderManager().Dirty();
  return true;
}

void FramebufferManager::DestroyEFB()
{
  // The colour resolve framebuffer is kept; CreateEFB replaces it when it needs one.
  m_framebuffer.reset();
  m_framebuffer_spare.reset();
  m_color.reset();
  m_color_spare.reset();
  m_depth.reset();
  m_color_resolved.reset();
  m_depth_resolve_target.reset();
  m_depth_resolved.reset();
}

void FramebufferManager::BindEFBFramebuffer()
{
  g_gfx->SetFramebuffer(m_framebuffer.get());
}

AbstractTexture* FramebufferManager::ResolveEFBColorTexture(const MathUtil::Rectangle<int>& region)
{
  if (!IsEFBMultisampled())
    return m_color.get();

  const MathUtil::Rectangle<int> area = ClampToEFB(region, GetEFBWidth(), GetEFBHeight());
  if (g_backend_info.bSupportsPartialMultisampleResolve)
  {
    const u32 layers = GetEFBLayers();
    for (u32 layer = 0; layer < layers; ++layer)
      m_color_resolved->ResolveFromTexture(m_color.get(), area, layer, 0);
  }
  else
  {
    // Resolve by drawing with the averaging shader.
    g_gfx->BeginUtilityDrawing();
    m_color->FinishedRendering();
    g_gfx->SetAndDiscardFramebuffer(m_color_resolve_target.get());
    g_gfx->SetPipeline(m_color_resolve_pipeline.get());
    g_gfx->SetTexture(0, m_color.get());
    g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
    g_gfx->SetViewportAndScissor(area);
    g_gfx->Draw(0, 3);
    m_color_resolved->FinishedRendering();
    g_gfx->EndUtilityDrawing();
  }

  m_color_resolved->FinishedRendering();
  return m_color_resolved.get();
}

AbstractTexture* FramebufferManager::ResolveEFBDepthTexture(const MathUtil::Rectangle<int>& region,
                                                            bool force_r32f)
{
  const bool multisampled = IsEFBMultisampled();
  const bool already_r32f_compatible = m_depth->GetFormat() == AbstractTextureFormat::D32F;
  if (!multisampled && (!force_r32f || already_r32f_compatible))
    return m_depth.get();

  // Draw into the R32F texture: a resolve with MSAA, else a plain format conversion copy.
  const MathUtil::Rectangle<int> area = ClampToEFB(region, GetEFBWidth(), GetEFBHeight());
  const AbstractPipeline* pipeline =
      multisampled ? m_depth_resolve_pipeline.get() : m_peek_depth.copy_pipeline.get();

  g_gfx->BeginUtilityDrawing();
  m_depth->FinishedRendering();
  g_gfx->SetAndDiscardFramebuffer(m_depth_resolve_target.get());
  g_gfx->SetPipeline(pipeline);
  g_gfx->SetTexture(0, m_depth.get());
  g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
  g_gfx->SetViewportAndScissor(area);
  g_gfx->Draw(0, 3);
  m_depth_resolved->FinishedRendering();
  g_gfx->EndUtilityDrawing();
  return m_depth_resolved.get();
}

bool FramebufferManager::ReinterpretPixelData(EFBReinterpretType convtype)
{
  const AbstractPipeline* pipeline = m_conversion_pipelines[static_cast<u32>(convtype)].get();
  if (pipeline == nullptr)
    return false;

  // Convert into the spare colour buffer. Not discarded: that would drop the shared depth
  // buffer too.
  g_gfx->BeginUtilityDrawing();
  m_color->FinishedRendering();
  g_gfx->SetFramebuffer(m_framebuffer_spare.get());
  g_gfx->SetViewportAndScissor(m_framebuffer->GetRect());
  g_gfx->SetPipeline(pipeline);
  g_gfx->SetTexture(0, m_color.get());
  g_gfx->Draw(0, 3);

  // The converted buffer becomes the EFB.
  m_color.swap(m_color_spare);
  m_framebuffer.swap(m_framebuffer_spare);
  g_gfx->EndUtilityDrawing();

  InvalidatePeekCache(true);
  return true;
}

bool FramebufferManager::BuildConversionPipelines()
{
  const AbstractShader* geometry_shader =
      IsEFBStereo() ? g_shader_cache->GetTexcoordGeometryShader() : nullptr;

  for (u32 index = 0; index < NUM_EFB_REINTERPRET_TYPES; ++index)
  {
    const auto type = static_cast<EFBReinterpretType>(index);
    const std::unique_ptr<AbstractShader> shader = CompilePixelShader(
        FramebufferShaderGen::GenerateFormatConversionShader(type, GetEFBSamples()),
        fmt::format("Framebuffer conversion pixel shader {}", type));
    if (!shader)
      return false;

    const AbstractPipelineConfig config =
        FullScreenUtilityConfig(g_shader_cache->GetScreenQuadVertexShader(), geometry_shader,
                                shader.get(), GetEFBFramebufferState());
    m_conversion_pipelines[index] = g_gfx->CreatePipeline(config);
    if (!m_conversion_pipelines[index])
      return false;
  }
  return true;
}

void FramebufferManager::DropConversionPipelines()
{
  for (std::unique_ptr<AbstractPipeline>& pipeline : m_conversion_pipelines)
    pipeline.reset();
}

bool FramebufferManager::DepthPeeksNeedIntermediate() const
{
  // Depth cannot always be copied straight into a staging texture: some backends cannot read
  // depth back at all, some only whole depth resources, and the formats must be compatible.
  if (!g_backend_info.bSupportsDepthReadback)
    return true;
  if (Tiled() && !g_backend_info.bSupportsPartialDepthCopies)
    return true;
  return !AbstractTexture::IsCompatibleDepthAndColorFormats(m_depth->GetFormat(),
                                                            GetEFBDepthCopyFormat());
}

u32 FramebufferManager::TileIndexAt(u32 x, u32 y) const
{
  if (!Tiled())
    return 0;
  return (y / m_tile_size) * m_tiles_per_row + x / m_tile_size;
}

MathUtil::Rectangle<int> FramebufferManager::TileRect(u32 tile) const
{
  if (!Tiled())
    return {0, 0, static_cast<int>(EFB_WIDTH), static_cast<int>(EFB_HEIGHT)};

  const u32 x0 = (tile % m_tiles_per_row) * m_tile_size;
  const u32 y0 = (tile / m_tiles_per_row) * m_tile_size;
  const u32 x1 = std::min(x0 + m_tile_size, EFB_WIDTH);
  const u32 y1 = std::min(y0 + m_tile_size, EFB_HEIGHT);
  return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1)};
}

template <typename T>
T FramebufferManager::ReadCached(bool depth, u32 x, u32 y)
{
  // Callers use upper-left coordinates; the cache follows the backend's origin.
  if (g_backend_info.bUsesLowerLeftOrigin)
    y = LAST_EFB_ROW - y;

  PeekCache& cache = Cache(depth);
  const u32 tile = TileIndexAt(x, y);
  if (!cache.tiles[tile].valid)
    FillTile(depth, tile, false);
  cache.tiles[tile].reads |= 1;

  if (cache.pending_readback)
  {
    cache.staging->Flush();
    cache.pending_readback = false;
  }

  T value;
  cache.staging->ReadTexel(x, y, &value);
  return value;
}

u32 FramebufferManager::PeekEFBColor(u32 x, u32 y)
{
  return ReadCached<u32>(false, x, y);
}

float FramebufferManager::PeekEFBDepth(u32 x, u32 y)
{
  return ReadCached<float>(true, x, y);
}

void FramebufferManager::SetEFBCacheTileSize(u32 size)
{
  if (size == m_tile_size)
    return;

  InvalidatePeekCache(true);
  m_tile_size = size;
  DestroyPeekCaches();
  if (!CreatePeekCaches())
    PanicAlertFmt("Failed to create EFB readback framebuffers");
}

void FramebufferManager::RefreshPeekCache()
{
  if (!m_peek_color.wants_refill && !m_peek_depth.wants_refill)
    return;

  // Re-read, without waiting, every tile that was read recently but has been invalidated, so
  // the next peeks do not stall.
  bool issued = false;
  const size_t count = m_peek_color.tiles.size();
  for (size_t i = 0; i < count; ++i)
  {
    const u32 tile = static_cast<u32>(i);
    for (const bool depth : {false, true})
    {
      const PeekTile& t = Cache(depth).tiles[i];
      if (t.reads != 0 && !t.valid)
      {
        FillTile(depth, tile, true);
        issued = true;
      }
    }
  }

  m_peek_depth.wants_refill = false;
  m_peek_color.wants_refill = false;
  if (issued)
    g_gfx->Flush();
}

void FramebufferManager::InvalidateCache(PeekCache& cache, bool forced)
{
  if (!forced && !cache.stale)
    return;

  if (cache.any_valid)
  {
    for (PeekTile& tile : cache.tiles)
      tile.valid = false;
    cache.wants_refill = true;
  }
  cache.any_valid = false;
  cache.stale = false;
}

void FramebufferManager::InvalidatePeekCache(bool forced)
{
  InvalidateCache(m_peek_color, forced);
  InvalidateCache(m_peek_depth, forced);
}

void FramebufferManager::FlagPeekCacheAsOutOfDate()
{
  for (PeekCache* cache : {&m_peek_color, &m_peek_depth})
  {
    if (cache->any_valid)
      cache->stale = true;
  }

  // With deferred invalidation the caches stay usable until the end of the frame.
  if (!g_ActiveConfig.bEFBAccessDeferInvalidation)
    InvalidatePeekCache(true);
}

void FramebufferManager::EndOfFrame()
{
  const size_t count = m_peek_color.tiles.size();
  for (size_t i = 0; i < count; ++i)
  {
    PeekTile& color = m_peek_color.tiles[i];
    PeekTile& depth = m_peek_depth.tiles[i];
    color.reads = static_cast<u8>(color.reads << 1);
    depth.reads = static_cast<u8>(depth.reads << 1);
  }
}

bool FramebufferManager::BuildCopyPipelines()
{
  // Copies of the EFB (or a resolved version) into the peek cache render targets.
  AbstractPipelineConfig config = FullScreenUtilityConfig(
      g_shader_cache->GetTextureCopyVertexShader(),
      IsEFBStereo() ? g_shader_cache->GetTexcoordGeometryShader() : nullptr,
      g_shader_cache->GetTextureCopyPixelShader(),
      RenderState::GetColorFramebufferState(GetEFBColorFormat()));
  m_peek_color.copy_pipeline = g_gfx->CreatePipeline(config);
  if (!m_peek_color.copy_pipeline)
    return false;

  config.framebuffer_state.color_texture_format = GetEFBDepthCopyFormat();
  m_peek_depth.copy_pipeline = g_gfx->CreatePipeline(config);
  if (!m_peek_depth.copy_pipeline)
    return false;

  if (IsEFBMultisampled())
  {
    // Resolve shaders, R32F target for depth and the colour format for colour.
    const std::unique_ptr<AbstractShader> depth_resolve = CompilePixelShader(
        FramebufferShaderGen::GenerateResolveDepthPixelShader(GetEFBSamples()),
        "Depth resolve pixel shader");
    if (!depth_resolve)
      return false;
    config.pixel_shader = depth_resolve.get();
    m_depth_resolve_pipeline = g_gfx->CreatePipeline(config);
    if (!m_depth_resolve_pipeline)
      return false;

    if (!g_backend_info.bSupportsPartialMultisampleResolve)
    {
      config.framebuffer_state.color_texture_format = GetEFBColorFormat();
      const std::unique_ptr<AbstractShader> color_resolve = CompilePixelShader(
          FramebufferShaderGen::GenerateResolveColorPixelShader(GetEFBSamples()),
          "Color resolve pixel shader");
      if (!color_resolve)
        return false;
      config.pixel_shader = color_resolve.get();
      m_color_resolve_pipeline = g_gfx->CreatePipeline(config);
      if (!m_color_resolve_pipeline)
        return false;
    }
  }

  // Savestate restore: writes colour and depth into the EFB.
  const std::unique_ptr<AbstractShader> restore = CompilePixelShader(
      FramebufferShaderGen::GenerateEFBRestorePixelShader(), "EFB restore pixel shader");
  if (!restore)
    return false;
  config.vertex_shader = g_shader_cache->GetScreenQuadVertexShader();
  config.pixel_shader = restore.get();
  config.depth_state = RenderState::GetAlwaysWriteDepthState();
  config.framebuffer_state = GetEFBFramebufferState();
  config.framebuffer_state.per_sample_shading = false;
  m_restore_pipeline = g_gfx->CreatePipeline(config);
  return m_restore_pipeline != nullptr;
}

void FramebufferManager::DropCopyPipelines()
{
  m_depth_resolve_pipeline.reset();
  m_peek_depth.copy_pipeline.reset();
  m_peek_color.copy_pipeline.reset();
}

bool FramebufferManager::CreatePeekCaches()
{
  const u32 cache_w = Tiled() ? m_tile_size : EFB_WIDTH;
  const u32 cache_h = Tiled() ? m_tile_size : EFB_HEIGHT;
  const bool scaled = GetEFBScale() != 1;

  // Render targets for downscaling (colour and depth) or depth conversion.
  const auto make_target = [&](PeekCache& cache, AbstractTextureFormat format, const char* name) {
    const TextureConfig cfg(cache_w, cache_h, 1, 1, 1, format, AbstractTextureFlag_RenderTarget,
                            AbstractTextureType::Texture_2DArray);
    cache.texture = g_gfx->CreateTexture(cfg, name);
    if (!cache.texture)
      return false;
    cache.framebuffer = g_gfx->CreateFramebuffer(cache.texture.get(), nullptr);
    return cache.framebuffer != nullptr;
  };
  if (scaled && !make_target(m_peek_color, GetEFBColorFormat(), "EFB color cache"))
    return false;
  if ((DepthPeeksNeedIntermediate() || scaled) &&
      !make_target(m_peek_depth, GetEFBDepthCopyFormat(), "EFB depth cache"))
  {
    return false;
  }

  // The staging textures always cover the whole EFB.
  const auto staging_config = [](AbstractTextureFormat format) {
    return TextureConfig(EFB_WIDTH, EFB_HEIGHT, 1, 1, 1, format, 0,
                         AbstractTextureType::Texture_2DArray);
  };
  m_peek_color.staging =
      g_gfx->CreateStagingTexture(StagingTextureType::Mutable, staging_config(GetEFBColorFormat()));
  m_peek_depth.staging = g_gfx->CreateStagingTexture(StagingTextureType::Mutable,
                                                     staging_config(GetEFBDepthCopyFormat()));
  if (!m_peek_color.staging || !m_peek_depth.staging)
    return false;

  u32 tile_count = 1;
  m_tiles_per_row = 1;
  if (Tiled())
  {
    const u32 across = (EFB_WIDTH + m_tile_size - 1) / m_tile_size;
    const u32 down = (EFB_HEIGHT + m_tile_size - 1) / m_tile_size;
    tile_count = across * down;
    m_tiles_per_row = across;
  }
  for (PeekCache* cache : {&m_peek_color, &m_peek_depth})
    cache->tiles.assign(tile_count, PeekTile{false, 0});
  return true;
}

void FramebufferManager::DestroyPeekCaches()
{
  for (PeekCache* cache : {&m_peek_color, &m_peek_depth})
  {
    cache->staging.reset();
    cache->framebuffer.reset();
    cache->texture.reset();
    cache->wants_refill = false;
    cache->any_valid = false;
  }
}

void FramebufferManager::FillTile(bool depth, u32 tile, bool async)
{
  FlushEFBPokes();
  g_vertex_manager->OnCPUEFBAccess();

  PeekCache& cache = Cache(depth);
  const bool via_intermediate = depth && DepthPeeksNeedIntermediate();
  const MathUtil::Rectangle<int> native = TileRect(tile);
  const MathUtil::Rectangle<int> scaled = ConvertEFBRectangle(native);
  AbstractTexture* source = depth ? ResolveEFBDepthTexture(scaled) : ResolveEFBColorTexture(scaled);
  const MathUtil::Rectangle<int> tile_area(0, 0, native.GetWidth(), native.GetHeight());

  if (GetEFBScale() != 1 || via_intermediate)
  {
    // Draw the tile at native size into the cache target (point sampling for depth, linear for
    // colour), then copy that. One sample per pixel, so higher scales than 2x skip samples.
    g_gfx->BeginUtilityDrawing();
    source->FinishedRendering();

    const float inv_w = 1.0f / static_cast<float>(m_framebuffer->GetWidth());
    const float inv_h = 1.0f / static_cast<float>(m_framebuffer->GetHeight());
    const std::array<float, 4> src_uv = {static_cast<float>(scaled.left) * inv_w,
                                         static_cast<float>(scaled.top) * inv_h,
                                         static_cast<float>(scaled.GetWidth()) * inv_w,
                                         static_cast<float>(scaled.GetHeight()) * inv_h};
    g_vertex_manager->UploadUtilityUniforms(&src_uv, sizeof(src_uv));

    // Tiles in the last row can be shorter than the tile size.
    g_gfx->SetAndDiscardFramebuffer(cache.framebuffer.get());
    g_gfx->SetViewportAndScissor(tile_area);
    g_gfx->SetPipeline(cache.copy_pipeline.get());
    g_gfx->SetTexture(0, source);
    g_gfx->SetSamplerState(0, depth ? RenderState::GetPointSamplerState() :
                                      RenderState::GetLinearSamplerState());
    g_gfx->Draw(0, 3);

    // The staging copy does the layout transition itself.
    cache.staging->CopyFromTexture(cache.texture.get(), tile_area, 0, 0, native);
    g_gfx->EndUtilityDrawing();
  }
  else
  {
    cache.staging->CopyFromTexture(source, native, 0, 0, native);
  }

  if (async)
  {
    cache.pending_readback = true;
  }
  else
  {
    cache.staging->Flush();
    cache.pending_readback = false;
  }
  cache.any_valid = true;
  cache.stale = false;
  cache.tiles[tile].valid = true;
}

void FramebufferManager::ClearEFB(const MathUtil::Rectangle<int>& rc, bool color_enable,
                                  bool alpha_enable, bool z_enable, u32 color, u32 z,
                                  PixelFormat pixel_format)
{
  FlushEFBPokes();
  FlagPeekCacheAsOutOfDate();

  MathUtil::Rectangle<int> target =
      g_gfx->ConvertFramebufferRectangle(ConvertEFBRectangle(rc), m_framebuffer.get());
  // Both bounds use the framebuffer width (the height is not clamped tighter than that).
  const int limit = static_cast<int>(m_framebuffer->GetWidth());
  target.ClampUL(0, 0, limit, limit);

  // Formats without alpha read alpha as opaque, so clearing alpha to 0xFF in the host buffer is
  // always right there; that also keeps the fast path on backends that cannot mask alpha.
  const bool no_alpha = pixel_format == PixelFormat::RGB565_Z16 ||
                        pixel_format == PixelFormat::RGB8_Z24 || pixel_format == PixelFormat::Z24;
  if (no_alpha)
  {
    alpha_enable = true;
    color &= 0x00FFFFFF;
  }

  g_gfx->ClearRegion(target, color_enable, alpha_enable, z_enable, color, z);
}

bool FramebufferManager::BuildClearPipelines()
{
  const std::unique_ptr<AbstractShader> vertex_shader =
      CompileVertexShader(FramebufferShaderGen::GenerateClearVertexShader(), "Clear vertex shader");
  if (!vertex_shader)
    return false;

  AbstractPipelineConfig config = FullScreenUtilityConfig(
      vertex_shader.get(), IsEFBStereo() ? g_shader_cache->GetColorGeometryShader() : nullptr,
      g_shader_cache->GetColorPixelShader(), GetEFBFramebufferState());
  config.depth_state = RenderState::GetAlwaysWriteDepthState();

  for (u32 index = 0; index < m_clear_pipelines.size(); ++index)
  {
    const bool write_color = (index & 4) != 0;
    const bool write_alpha = (index & 2) != 0;
    const bool write_depth = (index & 1) != 0;
    config.blending_state.color_update = write_color;
    config.blending_state.alpha_update = write_alpha;
    config.depth_state.test_enable = write_depth;
    config.depth_state.update_enable = write_depth;

    m_clear_pipelines[index] = g_gfx->CreatePipeline(config);
    if (!m_clear_pipelines[index])
      return false;
  }
  return true;
}

void FramebufferManager::DropClearPipelines()
{
  for (std::unique_ptr<AbstractPipeline>& pipeline : m_clear_pipelines)
    pipeline.reset();
}

AbstractPipeline* FramebufferManager::GetClearPipeline(bool colorEnable, bool alphaEnable,
                                                       bool zEnable) const
{
  const u32 index = (colorEnable ? 4u : 0u) | (alphaEnable ? 2u : 0u) | (zEnable ? 1u : 0u);
  return m_clear_pipelines[index].get();
}

template <typename T>
void FramebufferManager::UpdateCached(bool depth, u32 x, u32 y, const T& value)
{
  if (g_backend_info.bUsesLowerLeftOrigin)
    y = LAST_EFB_ROW - y;

  // A valid cached tile gets the poked value too, so it stays valid.
  PeekCache& cache = Cache(depth);
  if (cache.tiles[TileIndexAt(x, y)].valid)
    cache.staging->WriteTexel(x, y, &value);
}

void FramebufferManager::PokeEFBColor(u32 x, u32 y, u32 color)
{
  if (m_color_pokes.size() + 6 > POKE_BATCH_VERTICES)
    FlushEFBPokes();

  QueuePoke(m_color_pokes, x, y, 0.0f, color);
  UpdateCached(false, x, y, color);
}

void FramebufferManager::PokeEFBDepth(u32 x, u32 y, float depth)
{
  if (m_depth_pokes.size() + 6 > POKE_BATCH_VERTICES)
    FlushEFBPokes();

  QueuePoke(m_depth_pokes, x, y, depth, 0);
  UpdateCached(true, x, y, depth);
}

void FramebufferManager::QueuePoke(std::vector<PokeVertex>& queue, u32 x, u32 y, float z,
                                   u32 color)
{
  // Size of one native pixel in clip space.
  const float px_w = 1.0f / EFB_WIDTH * 2.0f;
  const float px_h = 1.0f / EFB_HEIGHT * 2.0f;
  const float fx = static_cast<float>(x);
  const float fy = static_cast<float>(y);

  if (g_backend_info.bSupportsLargePoints)
  {
    // One point at the pixel centre, as large as a scaled pixel.
    const float cx = (fx + 0.5f) * px_w - 1.0f;
    const float cy = 1.0f - (fy + 0.5f) * px_h;
    queue.push_back({{cx, cy, z, static_cast<float>(GetEFBScale())}, color});
    return;
  }

  // Two triangles covering the pixel.
  const float left = fx * px_w - 1.0f;
  const float top = 1.0f - fy * px_h;
  const float right = left + px_w;
  const float bottom = top - px_h;
  const std::array<std::pair<float, float>, 6> corners = {{
      {left, top},
      {right, top},
      {left, bottom},
      {left, bottom},
      {right, top},
      {right, bottom},
  }};
  for (const auto& [vx, vy] : corners)
    queue.push_back({{vx, vy, z, 1.0f}, color});
}

void FramebufferManager::FlushEFBPokes()
{
  if (!m_color_pokes.empty())
    DrawPokes(m_color_pokes, m_color_poke_pipeline.get());
  if (!m_depth_pokes.empty())
    DrawPokes(m_depth_pokes, m_depth_poke_pipeline.get());
}

void FramebufferManager::DrawPokes(std::vector<PokeVertex>& queue, const AbstractPipeline* pipeline)
{
  const u32 count = static_cast<u32>(queue.size());

  g_gfx->BeginUtilityDrawing();
  u32 first_vertex;
  u32 first_index;
  g_vertex_manager->UploadUtilityVertices(queue.data(), sizeof(PokeVertex), count, nullptr, 0,
                                          &first_vertex, &first_index);
  g_gfx->SetViewportAndScissor(m_framebuffer->GetRect());
  g_gfx->SetPipeline(pipeline);
  g_gfx->Draw(first_vertex, count);
  g_gfx->EndUtilityDrawing();

  queue.clear();
}

bool FramebufferManager::BuildPokePipelines()
{
  PortableVertexDeclaration layout = {};
  layout.stride = sizeof(PokeVertex);
  AttributeFormat& position = layout.position;
  position.enable = true;
  position.type = ComponentFormat::Float;
  position.components = 4;
  position.integer = false;
  position.offset = offsetof(PokeVertex, position);
  AttributeFormat& rgba = layout.colors[0];
  rgba.enable = true;
  rgba.type = ComponentFormat::UByte;
  rgba.components = 4;
  rgba.integer = false;
  rgba.offset = offsetof(PokeVertex, color);

  m_poke_format = g_gfx->CreateNativeVertexFormat(layout);
  if (!m_poke_format)
    return false;

  const std::unique_ptr<AbstractShader> vertex_shader = CompileVertexShader(
      FramebufferShaderGen::GenerateEFBPokeVertexShader(), "EFB poke vertex shader");
  if (!vertex_shader)
    return false;

  AbstractPipelineConfig config = FullScreenUtilityConfig(
      vertex_shader.get(), IsEFBStereo() ? g_shader_cache->GetColorGeometryShader() : nullptr,
      g_shader_cache->GetColorPixelShader(), GetEFBFramebufferState());
  config.vertex_format = m_poke_format.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(
      g_backend_info.bSupportsLargePoints ? PrimitiveType::Points : PrimitiveType::Triangles);
  m_color_poke_pipeline = g_gfx->CreatePipeline(config);
  if (!m_color_poke_pipeline)
    return false;

  // Depth pokes write depth only.
  config.depth_state = RenderState::GetAlwaysWriteDepthState();
  config.blending_state = RenderState::GetNoColorWriteBlendState();
  m_depth_poke_pipeline = g_gfx->CreatePipeline(config);
  return m_depth_poke_pipeline != nullptr;
}

void FramebufferManager::DropPokePipelines()
{
  m_depth_poke_pipeline.reset();
  m_color_poke_pipeline.reset();
  m_poke_format.reset();
}

void FramebufferManager::DoState(PointerWrap& p)
{
  FlushEFBPokes();
  p.Do(m_last_pixel_format);

  // Whether the EFB contents are in the state is recorded in the state itself.
  bool with_contents = Config::Get(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE);
  p.Do(with_contents);
  if (!with_contents)
    return;

  if (p.IsWriteMode() || p.IsMeasureMode())
    SaveEFB(p);
  else
    LoadEFB(p);
}

void FramebufferManager::SaveEFB(PointerWrap& p)
{
  // MSAA buffers are saved resolved; loading them back is close but not exact.
  AbstractTexture* color = ResolveEFBColorTexture(m_color->GetRect());
  AbstractTexture* depth = ResolveEFBDepthTexture(m_depth->GetRect(), true);

  // Stored as plain textures (not render targets).
  const auto plain_config = [](const AbstractTexture* tex, AbstractTextureFormat format) {
    return TextureConfig(tex->GetWidth(), tex->GetHeight(), tex->GetLevels(), tex->GetLayers(), 1,
                         format, 0, AbstractTextureType::Texture_2DArray);
  };
  g_texture_cache->SerializeTexture(color, plain_config(color, GetEFBColorFormat()), p);
  g_texture_cache->SerializeTexture(depth, plain_config(depth, GetEFBDepthCopyFormat()), p);
}

void FramebufferManager::LoadEFB(PointerWrap& p)
{
  InvalidatePeekCache(true);

  auto color = g_texture_cache->DeserializeTexture(p);
  auto depth = g_texture_cache->DeserializeTexture(p);

  // Unreadable data, or a state saved with a different stereo setting: start from a clear EFB.
  if (!color || !depth || color->texture->GetLayers() != m_color->GetLayers())
  {
    WARN_LOG_FMT(VIDEO, "Failed to deserialize EFB contents. Clearing instead.");
    ClearToDefaults(m_framebuffer.get());
    return;
  }

  // A different internal resolution is fine: colour is filtered to the new size (depth is
  // always point sampled).
  const bool resized = color->texture->GetWidth() != m_color->GetWidth() ||
                       color->texture->GetHeight() != m_color->GetHeight();

  g_gfx->BeginUtilityDrawing();
  g_gfx->SetAndDiscardFramebuffer(m_framebuffer.get());
  g_gfx->SetViewportAndScissor(m_framebuffer->GetRect());
  g_gfx->SetPipeline(m_restore_pipeline.get());
  g_gfx->SetTexture(0, color->texture.get());
  g_gfx->SetTexture(1, depth->texture.get());
  g_gfx->SetSamplerState(0, resized ? RenderState::GetLinearSamplerState() :
                                      RenderState::GetPointSamplerState());
  g_gfx->SetSamplerState(1, RenderState::GetPointSamplerState());
  g_gfx->Draw(0, 3);
  g_gfx->EndUtilityDrawing();
}
