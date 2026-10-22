// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKGfx.h"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKPipeline.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VKVertexFormat.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
namespace
{
StateTracker* Tracker()
{
  return StateTracker::GetInstance();
}

VkCommandBuffer Cmd()
{
  return g_command_buffer_mgr->GetCurrentCommandBuffer();
}

// Packed A8R8G8B8 to normalised floats.
VkClearValue ColorClearValue(u32 argb)
{
  const auto channel = [argb](u32 shift) {
    return static_cast<float>((argb >> shift) & 0xFF) / 255.0f;
  };
  VkClearValue value = {};
  value.color.float32[0] = channel(16);
  value.color.float32[1] = channel(8);
  value.color.float32[2] = channel(0);
  value.color.float32[3] = channel(24);
  return value;
}

// 24-bit EFB depth to [0, 1], flipped when the device has no reversed depth range.
VkClearValue DepthClearValue(u32 z)
{
  VkClearValue value = {};
  value.depthStencil.depth = static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
  if (!g_backend_info.bSupportsReversedDepthRange)
    value.depthStencil.depth = 1.0f - value.depthStencil.depth;
  return value;
}

VkClearAttachment ClearOf(VkImageAspectFlags aspect, const VkClearValue& value)
{
  VkClearAttachment clear;
  clear.aspectMask = aspect;
  // Every color clear names attachment 0, extra outputs included.
  clear.colorAttachment = 0;
  clear.clearValue = value;
  return clear;
}
}  // namespace

VKGfx::VKGfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_swap_chain(std::move(swap_chain)), m_backbuffer_scale(backbuffer_scale)
{
  UpdateActiveConfig();
  m_sampler_states.fill(RenderState::GetPointSamplerState());

  // Start-up work (uploads, dummy textures) is already recorded: send it before the first frame.
  ExecuteCommandBuffer(true, false);
}

VKGfx::~VKGfx() = default;

// Object factories: thin forwards to each object type's own Create.

std::unique_ptr<AbstractTexture> VKGfx::CreateTexture(const TextureConfig& cfg,
                                                      std::string_view label)
{
  return VKTexture::Create(cfg, label);
}

std::unique_ptr<AbstractStagingTexture> VKGfx::CreateStagingTexture(StagingTextureType kind,
                                                                    const TextureConfig& cfg)
{
  return VKStagingTexture::Create(kind, cfg);
}

std::unique_ptr<AbstractFramebuffer>
VKGfx::CreateFramebuffer(AbstractTexture* color, AbstractTexture* depth,
                         std::vector<AbstractTexture*> extra_colors)
{
  auto* const vk_color = static_cast<VKTexture*>(color);
  auto* const vk_depth = static_cast<VKTexture*>(depth);
  return VKFramebuffer::Create(vk_color, vk_depth, std::move(extra_colors));
}

std::unique_ptr<AbstractShader> VKGfx::CreateShaderFromSource(ShaderStage stage,
                                                              std::string_view text,
                                                              VideoCommon::ShaderIncluder* includer,
                                                              std::string_view label)
{
  return VKShader::CreateFromSource(stage, text, includer, label);
}

std::unique_ptr<AbstractShader> VKGfx::CreateShaderFromBinary(ShaderStage stage, const void* code,
                                                              size_t code_size,
                                                              std::string_view label)
{
  return VKShader::CreateFromBinary(stage, code, code_size, label);
}

std::unique_ptr<NativeVertexFormat>
VKGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& decl)
{
  return std::make_unique<VertexFormat>(decl);
}

// Pipeline binaries are not used; the VkPipelineCache covers that.
std::unique_ptr<AbstractPipeline> VKGfx::CreatePipeline(const AbstractPipelineConfig& cfg,
                                                        const void*, size_t)
{
  return VKPipeline::Create(cfg);
}

bool VKGfx::IsHeadless() const
{
  return !m_swap_chain;
}

void VKGfx::SetPipeline(const AbstractPipeline* pipeline)
{
  Tracker()->SetPipeline(static_cast<const VKPipeline*>(pipeline));
}

void VKGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable,
                        bool alpha_enable, bool z_enable, u32 color, u32 z)
{
  const VkRect2D area = {
      {target_rc.left, target_rc.top},
      {static_cast<u32>(target_rc.GetWidth()), static_cast<u32>(target_rc.GetHeight())}};
  const VkClearValue color_value = ColorClearValue(color);
  const VkClearValue depth_value = DepthClearValue(z);

  // Three ways to clear, fastest first:
  //  - a clearing render pass, when nothing has been drawn yet and everything is cleared;
  //  - vkCmdClearAttachments inside the pass, for full colour and/or depth;
  //  - the generic draw-based clear for what remains (colour without alpha, or alpha only).
  const bool full_color = color_enable && alpha_enable;
  bool by_render_pass = !Tracker()->InRenderPass() && full_color && z_enable;
  bool by_clear_command = full_color || z_enable;
  // NVIDIA hangs on partial MSAA clears done with load ops or vkCmdClearAttachments.
  if (g_ActiveConfig.iMultisamples > 1 &&
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_MSAA_CLEAR))
  {
    by_render_pass = false;
    by_clear_command = false;
  }
  // Some drivers clear more than the render area with a clear load op.
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLEAR_LOADOP_RENDERPASS))
    by_render_pass = false;

  auto* framebuffer = static_cast<VKFramebuffer*>(m_current_framebuffer);
  if (by_render_pass)
  {
    framebuffer->SetAndClear(area, color_value, depth_value);
    return;
  }

  if (by_clear_command)
  {
    std::vector<VkClearAttachment> clears;
    const bool clear_color = full_color;
    if (clear_color)
    {
      clears.push_back(ClearOf(VK_IMAGE_ASPECT_COLOR_BIT, color_value));
      color_enable = false;
      alpha_enable = false;
    }
    if (z_enable)
    {
      clears.push_back(ClearOf(VK_IMAGE_ASPECT_DEPTH_BIT, depth_value));
      z_enable = false;
    }
    if (clear_color)
    {
      clears.insert(clears.end(), framebuffer->GetNumberOfAdditonalAttachments(),
                    ClearOf(VK_IMAGE_ASPECT_COLOR_BIT, color_value));
    }

    if (!clears.empty())
    {
      const VkClearRect rect = {area, 0, g_framebuffer_manager->GetEFBLayers()};
      // A clearing pass only covers its own area; leave it if the rectangle goes further.
      if (!Tracker()->IsWithinRenderArea(area.offset.x, area.offset.y, area.extent.width,
                                         area.extent.height))
      {
        Tracker()->EndClearRenderPass();
      }
      Tracker()->BeginRenderPass();
      vkCmdClearAttachments(Cmd(), static_cast<u32>(clears.size()), clears.data(), 1, &rect);
    }
  }

  if (color_enable || alpha_enable || z_enable)
    AbstractGfx::ClearRegion(target_rc, color_enable, alpha_enable, z_enable, color, z);
}

void VKGfx::Flush()
{
  ExecuteCommandBuffer(true, false);
}

void VKGfx::WaitForGPUIdle()
{
  ExecuteCommandBuffer(false, true);
}

VkResult VKGfx::RecoverSwapChain(VkResult failure)
{
  // The last frame may still be presenting: finish everything before touching the chain.
  ExecuteCommandBuffer(false, true);

  if (failure == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
  {
    // Presents keep failing this way until the swap chain is recreated.
    INFO_LOG_FMT(VIDEO, "Lost exclusive fullscreen.");
    m_swap_chain->RecreateSwapChain();
  }
  else if (failure == VK_SUBOPTIMAL_KHR || failure == VK_ERROR_OUT_OF_DATE_KHR)
  {
    INFO_LOG_FMT(VIDEO, "Resizing swap chain due to suboptimal/out-of-date");
    m_swap_chain->ResizeSwapChain();
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "Unknown present error {:#010X} {}, please report.",
                  std::to_underlying(failure), VkResultToString(failure));
    m_swap_chain->RecreateSwapChain();
  }

  const VkResult res = m_swap_chain->AcquireNextImage();
  if (res == VK_ERROR_OUT_OF_DATE_KHR)
  {
    INFO_LOG_FMT(VIDEO, "Swapchain still out of date, will try again next frame...");
  }
  else if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
  {
    PanicAlertFmt("Failed to grab image from swap chain: {:#010X} {}", std::to_underlying(res),
                  VkResultToString(res));
  }
  return res;
}

bool VKGfx::BindBackbuffer(const ClearColor& /*clear_color*/)
{
  Tracker()->EndRenderPass();
  // The worker may still be presenting the previous image.
  g_command_buffer_mgr->WaitForWorkerThreadIdle();

  HandleSurfaceChange();
  HandleSurfaceResize();

  // Apply a pending exclusive fullscreen change; after a failure stop asking.
  const bool fullscreen_now = m_swap_chain->GetCurrentFullscreenState();
  if (fullscreen_now != m_swap_chain->GetNextFullscreenState() &&
      !m_swap_chain->SetFullscreenState(m_swap_chain->GetNextFullscreenState()))
  {
    m_swap_chain->SetNextFullscreenState(m_swap_chain->GetCurrentFullscreenState());
  }

  // A failed present counts like a failed acquire (and no image has been acquired yet).
  const bool present_failed = g_command_buffer_mgr->CheckLastPresentFail();
  VkResult res = present_failed ? g_command_buffer_mgr->GetLastPresentResult() :
                                  m_swap_chain->AcquireNextImage();

  // Some AMD drivers report a lost exclusive mode that was never acquired (whenever it is
  // application controlled). Carry on as if nothing happened.
  if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT &&
      !m_swap_chain->GetCurrentFullscreenState())
  {
    res = present_failed ? m_swap_chain->AcquireNextImage() : VK_SUCCESS;
  }

  if (res != VK_SUCCESS)
    RecoverSwapChain(res);

  if (!m_swap_chain->IsCurrentImageValid())
    return false;

  // The previous contents do not matter. Layout changes must happen outside a render pass.
  VKTexture* backbuffer = m_swap_chain->GetCurrentTexture();
  backbuffer->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  backbuffer->TransitionToLayout(Cmd(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  SetAndClearFramebuffer(m_swap_chain->GetCurrentFramebuffer(),
                         ClearColor{{0.0f, 0.0f, 0.0f, 1.0f}});
  return true;
}

void VKGfx::PresentBackbuffer()
{
  Tracker()->EndRenderPass();

  if (m_swap_chain->IsCurrentImageValid())
  {
    // The transition also orders every draw into the image before the present. The submission
    // waits for the acquire semaphore and may run on the worker thread.
    m_swap_chain->GetCurrentTexture()->TransitionToLayout(Cmd(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    g_command_buffer_mgr->SubmitCommandBuffer(true, false, true, m_swap_chain->GetSwapChain(),
                                              m_swap_chain->GetCurrentImageIndex());
  }
  else
  {
    g_command_buffer_mgr->SubmitCommandBuffer(true, false, true);
  }

  Tracker()->InvalidateCachedState();
}

void VKGfx::SetFullscreen(bool enable_fullscreen)
{
  if (m_swap_chain->IsFullscreenSupported())
    m_swap_chain->SetNextFullscreenState(enable_fullscreen);
}

bool VKGfx::IsFullscreen() const
{
  return m_swap_chain != nullptr && m_swap_chain->GetCurrentFullscreenState();
}

void VKGfx::ExecuteCommandBuffer(bool submit_off_thread, bool wait_for_completion)
{
  Tracker()->EndRenderPass();
  g_command_buffer_mgr->SubmitCommandBuffer(submit_off_thread, wait_for_completion);
  Tracker()->InvalidateCachedState();
}

void VKGfx::HandleSurfaceChange()
{
  if (!g_presenter->SurfaceChangedTestAndClear() || !m_swap_chain)
    return;

  ExecuteCommandBuffer(false, true);
  // Drop any present failure: the new surface starts fresh.
  g_command_buffer_mgr->CheckLastPresentFail();
  if (!m_swap_chain->RecreateSurface(g_presenter->GetNewSurfaceHandle()))
    PanicAlertFmt("Failed to recreate Vulkan surface. Cannot continue.");
  PublishBackbufferSize();
}

void VKGfx::HandleSurfaceResize()
{
  if (!g_presenter->SurfaceResizedTestAndClear())
    return;
  if (!m_swap_chain)
  {
    WARN_LOG_FMT(VIDEO, "Surface resize event received without active surface, ignoring");
    return;
  }

  // The swap chain is about to be destroyed: let the GPU finish with it.
  ExecuteCommandBuffer(false, true);
  g_command_buffer_mgr->CheckLastPresentFail();
  m_swap_chain->RecreateSwapChain();
  PublishBackbufferSize();
}

void VKGfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  if ((bits & CONFIG_CHANGE_BIT_HOST_CONFIG) != 0)
    g_object_cache->ReloadPipelineCache();

  // A vsync change needs a different present mode.
  if (m_swap_chain && (bits & CONFIG_CHANGE_BIT_VSYNC) != 0)
  {
    ExecuteCommandBuffer(false, true);
    m_swap_chain->SetVSync(g_ActiveConfig.bVSyncActive);
  }

  // Quad-buffered stereo changes the layer count, HDR the format.
  if (m_swap_chain && (bits & (CONFIG_CHANGE_BIT_STEREO_MODE | CONFIG_CHANGE_BIT_HDR)) != 0)
  {
    ExecuteCommandBuffer(false, true);
    m_swap_chain->RecreateSwapChain();
  }

  // Samplers depend on forced filtering and anisotropy.
  if ((bits & (CONFIG_CHANGE_BIT_ANISOTROPY | CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING)) != 0)
  {
    ExecuteCommandBuffer(false, true);
    ResetSamplerStates();
  }
}

void VKGfx::PublishBackbufferSize()
{
  g_presenter->SetBackbuffer(static_cast<int>(m_swap_chain->GetWidth()),
                             static_cast<int>(m_swap_chain->GetHeight()));
}

void VKGfx::BindFramebuffer(VKFramebuffer* fb)
{
  Tracker()->EndRenderPass();
  // Render targets must not stay bound as textures.
  fb->Unbind();
  fb->TransitionForRender();
  Tracker()->SetFramebuffer(fb);
  m_current_framebuffer = fb;
}

void VKGfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer != framebuffer)
    BindFramebuffer(static_cast<VKFramebuffer*>(framebuffer));
}

void VKGfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;
  BindFramebuffer(static_cast<VKFramebuffer*>(framebuffer));
  // Start with a discarding pass; later passes (after a flush) load as usual.
  Tracker()->BeginDiscardRenderPass();
}

void VKGfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                   float depth_value)
{
  auto* fb = static_cast<VKFramebuffer*>(framebuffer);
  BindFramebuffer(fb);

  VkClearValue color;
  std::memcpy(color.color.float32, color_value.data(), sizeof(color.color.float32));
  VkClearValue depth;
  depth.depthStencil.depth = depth_value;
  depth.depthStencil.stencil = 0;
  fb->SetAndClear(fb->GetRect(), color, depth);
}

void VKGfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  const auto* vk_texture = static_cast<const VKTexture*>(texture);
  if (vk_texture == nullptr)
  {
    // Clears slot 0 whatever the index, as it always has.
    Tracker()->SetTexture(0, VK_NULL_HANDLE);
    return;
  }

  // Sampled textures are expected in SHADER_READ_ONLY already, so render passes are not
  // interrupted; fix it up if not.
  if (vk_texture->GetLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
  {
    if (Tracker()->InRenderPass())
    {
      WARN_LOG_FMT(VIDEO, "Transitioning image in render pass in VKGfx::SetTexture()");
      Tracker()->EndRenderPass();
    }
    vk_texture->TransitionToLayout(Cmd(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  Tracker()->SetTexture(index, vk_texture->GetView());
}

void VKGfx::SetSamplerState(u32 index, const SamplerState& state)
{
  if (m_sampler_states[index] == state)
    return;

  VkSampler sampler = g_object_cache->GetSampler(state);
  if (sampler == VK_NULL_HANDLE)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create sampler");
    sampler = g_object_cache->GetPointSampler();
  }
  Tracker()->SetSampler(index, sampler);
  m_sampler_states[index] = state;
}

void VKGfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  auto* vk_texture = static_cast<VKTexture*>(texture);
  if (vk_texture == nullptr)
  {
    Tracker()->SetImageTexture(index, VK_NULL_HANDLE);
    return;
  }

  using Layout = VKTexture::ComputeImageLayout;
  const Layout layout = !read ? Layout::WriteOnly : (write ? Layout::ReadWrite : Layout::ReadOnly);
  Tracker()->EndRenderPass();
  Tracker()->SetImageTexture(index, vk_texture->GetView());
  vk_texture->TransitionToLayout(Cmd(), layout);
}

void VKGfx::UnbindTexture(const AbstractTexture* texture)
{
  Tracker()->UnbindTexture(static_cast<const VKTexture*>(texture)->GetView());
}

void VKGfx::ResetSamplerStates()
{
  // Back to point sampling everywhere; the next draws set what they need.
  const VkSampler point = g_object_cache->GetPointSampler();
  for (u32 slot = 0; slot < static_cast<u32>(m_sampler_states.size()); ++slot)
  {
    m_sampler_states[slot] = RenderState::GetPointSamplerState();
    Tracker()->SetSampler(slot, point);
  }
  // No slot refers to a cached sampler any more.
  g_object_cache->ClearSamplerCache();
}

void VKGfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  VkRect2D scissor = {{rc.left, rc.top},
                      {static_cast<u32>(rc.GetWidth()), static_cast<u32>(rc.GetHeight())}};
  // Vulkan needs a non-negative offset: cut the part left of / above the origin.
  if (scissor.offset.x < 0)
  {
    scissor.extent.width -= static_cast<u32>(-scissor.offset.x);
    scissor.offset.x = 0;
  }
  if (scissor.offset.y < 0)
  {
    scissor.extent.height -= static_cast<u32>(-scissor.offset.y);
    scissor.offset.y = 0;
  }
  Tracker()->SetScissor(scissor);
}

void VKGfx::SetViewport(float x, float y, float width, float height, float near_depth,
                        float far_depth)
{
  Tracker()->SetViewport({x, y, width, height, near_depth, far_depth});
}

void VKGfx::Draw(u32 base_vertex, u32 num_vertices)
{
  if (Tracker()->Bind())
    vkCmdDraw(Cmd(), num_vertices, 1, base_vertex, 0);
}

void VKGfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (Tracker()->Bind())
    vkCmdDrawIndexed(Cmd(), num_indices, 1, base_index, static_cast<s32>(base_vertex), 0);
}

void VKGfx::DispatchComputeShader(const AbstractShader* shader, u32 /*groupsize_x*/,
                                  u32 /*groupsize_y*/, u32 /*groupsize_z*/, u32 groups_x,
                                  u32 groups_y, u32 groups_z)
{
  // Group sizes are compiled into the shader.
  Tracker()->SetComputeShader(static_cast<const VKShader*>(shader));
  if (Tracker()->BindCompute())
    vkCmdDispatch(Cmd(), groups_x, groups_y, groups_z);
}

SurfaceInfo VKGfx::GetSurfaceInfo() const
{
  // Headless: width 1 and height 0, as reported before.
  if (!m_swap_chain)
    return {1u, 0u, m_backbuffer_scale, AbstractTextureFormat::Undefined};
  return {m_swap_chain->GetWidth(), m_swap_chain->GetHeight(), m_backbuffer_scale,
          m_swap_chain->GetTextureFormat()};
}

}  // namespace Vulkan
