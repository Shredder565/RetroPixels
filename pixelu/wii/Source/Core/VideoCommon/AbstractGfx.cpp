// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/AbstractGfx.h"

#include <type_traits>

#include "Common/Assert.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

std::unique_ptr<AbstractGfx> g_gfx;

namespace
{
// Uniform block of the clear pipelines: RGBA colour, then depth, padded to 32 bytes.
struct ClearUniforms
{
  float rgba[4];
  float depth;
  float pad[3];
};
static_assert(std::is_standard_layout_v<ClearUniforms> && sizeof(ClearUniforms) == 32);

float ByteToUnorm(u32 packed, u32 shift)
{
  return static_cast<float>((packed >> shift) & 0xFF) / 255.0f;
}
}  // namespace

AbstractGfx::AbstractGfx()
{
  m_on_config_changed = GetVideoEvents().config_changed_event.Register(
      [this](u32 changed_bits) { OnConfigChanged(changed_bits); });
}

bool AbstractGfx::IsHeadless() const
{
  return true;
}

void AbstractGfx::BeginUtilityDrawing()
{
  // Anything still queued for the emulated GPU has to go out before the backend state changes.
  g_vertex_manager->Flush();
}

void AbstractGfx::EndUtilityDrawing()
{
  // Back to the EFB; the next GX draw binds its own pipeline.
  g_framebuffer_manager->BindEFBFramebuffer();

  const ViewportAndScissor& vs = m_saved_viewport;
  SetViewport(vs.viewport_x, vs.viewport_y, vs.viewport_width, vs.viewport_height,
              vs.viewport_near_depth, vs.viewport_far_depth);
  SetScissorRect(vs.scissor_rect);
}

void AbstractGfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  m_current_framebuffer = framebuffer;
}

void AbstractGfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  m_current_framebuffer = framebuffer;
}

void AbstractGfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor&,
                                         float)
{
  m_current_framebuffer = framebuffer;
}

void AbstractGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable,
                              bool alphaEnable, bool zEnable, u32 color, u32 z)
{
  // Generic path: draw one triangle covering the region with a clear pipeline.
  BeginUtilityDrawing();

  // The packed colour is ARGB8; the shader wants RGBA.
  ClearUniforms block{};
  block.rgba[0] = ByteToUnorm(color, 16);
  block.rgba[1] = ByteToUnorm(color, 8);
  block.rgba[2] = ByteToUnorm(color, 0);
  block.rgba[3] = ByteToUnorm(color, 24);
  block.depth = static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
  if (!g_backend_info.bSupportsReversedDepthRange)
    block.depth = 1.0f - block.depth;
  g_vertex_manager->UploadUtilityUniforms(&block, sizeof(block));

  g_gfx->SetPipeline(g_framebuffer_manager->GetClearPipeline(colorEnable, alphaEnable, zEnable));
  g_gfx->SetViewportAndScissor(target_rc);
  g_gfx->Draw(0, 3);
  EndUtilityDrawing();
}

void AbstractGfx::StoreViewportAndScissor(const ViewportAndScissor& viewport_and_scissor)
{
  m_saved_viewport = viewport_and_scissor;
}

void AbstractGfx::SetViewportAndScissor(const MathUtil::Rectangle<int>& rect, float min_depth,
                                        float max_depth)
{
  const float x = static_cast<float>(rect.left);
  const float y = static_cast<float>(rect.top);
  const float w = static_cast<float>(rect.GetWidth());
  const float h = static_cast<float>(rect.GetHeight());
  SetViewport(x, y, w, h, min_depth, max_depth);
  SetScissorRect(rect);
}

void AbstractGfx::ScaleTexture(AbstractFramebuffer* dst_framebuffer,
                               const MathUtil::Rectangle<int>& dst_rect,
                               const AbstractTexture* src_texture,
                               const MathUtil::Rectangle<int>& src_rect)
{
  ASSERT(dst_framebuffer->GetColorFormat() == AbstractTextureFormat::RGBA8);

  BeginUtilityDrawing();

  // The copy shader takes the source rectangle as offset and size in normalised coordinates.
  const u32 src_w = src_texture->GetWidth();
  const u32 src_h = src_texture->GetHeight();
  const MathUtil::Rectangle<int> src = ConvertFramebufferRectangle(src_rect, src_w, src_h);
  const float inv_w = 1.0f / static_cast<float>(src_w);
  const float inv_h = 1.0f / static_cast<float>(src_h);
  const std::array<float, 4> src_uv = {
      static_cast<float>(src.left) * inv_w, static_cast<float>(src.top) * inv_h,
      static_cast<float>(src.GetWidth()) * inv_w, static_cast<float>(src.GetHeight()) * inv_h};
  g_vertex_manager->UploadUtilityUniforms(&src_uv, sizeof(src_uv));

  // A copy that covers the whole target does not need the old contents.
  const bool covers_target = static_cast<u32>(dst_rect.GetWidth()) == dst_framebuffer->GetWidth() &&
                             static_cast<u32>(dst_rect.GetHeight()) == dst_framebuffer->GetHeight();
  if (covers_target)
    SetAndDiscardFramebuffer(dst_framebuffer);
  else
    SetFramebuffer(dst_framebuffer);

  SetViewportAndScissor(ConvertFramebufferRectangle(dst_rect, dst_framebuffer));
  const bool layered = dst_framebuffer->GetLayers() > 1;
  SetPipeline(layered ? g_shader_cache->GetRGBA8StereoCopyPipeline() :
                        g_shader_cache->GetRGBA8CopyPipeline());
  SetTexture(0, src_texture);
  SetSamplerState(0, RenderState::GetLinearSamplerState());
  Draw(0, 3);
  EndUtilityDrawing();

  if (AbstractTexture* color = dst_framebuffer->GetColorAttachment())
    color->FinishedRendering();
}

MathUtil::Rectangle<int>
AbstractGfx::ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                                         const AbstractFramebuffer* framebuffer) const
{
  return ConvertFramebufferRectangle(rect, framebuffer->GetWidth(), framebuffer->GetHeight());
}

MathUtil::Rectangle<int>
AbstractGfx::ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                                         [[maybe_unused]] u32 fb_width, u32 fb_height) const
{
  if (!g_backend_info.bUsesLowerLeftOrigin)
    return rect;

  // Mirror vertically; computed in unsigned arithmetic and wrapped back to int, as the
  // rectangle may extend past the framebuffer.
  MathUtil::Rectangle<int> flipped = rect;
  flipped.top = static_cast<int>(fb_height - static_cast<u32>(rect.bottom));
  flipped.bottom = static_cast<int>(fb_height - static_cast<u32>(rect.top));
  return flipped;
}

std::unique_ptr<VideoCommon::AsyncShaderCompiler> AbstractGfx::CreateAsyncShaderCompiler()
{
  return std::make_unique<VideoCommon::AsyncShaderCompiler>();
}

void AbstractGfx::OnConfigChanged(u32 changed_bits)
{
  // Shaders or sample counts are about to be rebuilt: let the GPU finish with the old objects
  // and drop the bound pipeline before they go away.
  constexpr u32 rebuild_bits = CONFIG_CHANGE_BIT_HOST_CONFIG | CONFIG_CHANGE_BIT_MULTISAMPLES;
  if ((changed_bits & rebuild_bits) == 0)
    return;
  WaitForGPUIdle();
  SetPipeline(nullptr);
}

bool AbstractGfx::UseGeometryShaderForUI() const
{
  // OpenGL draws each eye separately with glDrawBuffer instead of using a layered back buffer.
  return g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer &&
         !g_backend_info.bUsesExplictQuadBuffering;
}
