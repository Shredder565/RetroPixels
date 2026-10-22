// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/D3DGfx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <strsafe.h>
#include <tuple>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

#include "Core/Core.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#include "VideoBackends/D3D/DXPipeline.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace DX11
{
namespace
{
D3D::StateManager& State()
{
  return *D3D::stateman;
}
}  // namespace

Gfx::Gfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_backbuffer_scale(backbuffer_scale), m_swap_chain(std::move(swap_chain))
{
}

Gfx::~Gfx() = default;

bool Gfx::IsHeadless() const
{
  return m_swap_chain == nullptr;
}

// Shaders and pipelines.

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromSource(ShaderStage stage,
                                                            std::string_view source,
                                                            VideoCommon::ShaderIncluder* includer,
                                                            std::string_view label)
{
  std::optional<AbstractShader::BinaryData> code =
      DXShader::CompileShader(D3D::feature_level, stage, source, includer);
  if (!code)
    return nullptr;
  return DXShader::CreateFromBytecode(stage, std::move(*code), label);
}

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                            size_t size, std::string_view label)
{
  return DXShader::CreateFromBytecode(stage, DXShader::CreateByteCode(data, size), label);
}

std::unique_ptr<AbstractPipeline> Gfx::CreatePipeline(const AbstractPipelineConfig& pipeline_config,
                                                      const void* /*cache_data*/,
                                                      size_t /*cache_data_length*/)
{
  // D3D11 has no pipeline cache blobs.
  return DXPipeline::Create(pipeline_config);
}

// Textures and render targets.

std::unique_ptr<AbstractTexture> Gfx::CreateTexture(const TextureConfig& texture_config,
                                                    std::string_view label)
{
  return DXTexture::Create(texture_config, label);
}

std::unique_ptr<AbstractStagingTexture>
Gfx::CreateStagingTexture(StagingTextureType kind, const TextureConfig& texture_config)
{
  return DXStagingTexture::Create(kind, texture_config);
}

std::unique_ptr<AbstractFramebuffer>
Gfx::CreateFramebuffer(AbstractTexture* color, AbstractTexture* depth,
                       std::vector<AbstractTexture*> extra_colors)
{
  return DXFramebuffer::Create(static_cast<DXTexture*>(color), static_cast<DXTexture*>(depth),
                               std::move(extra_colors));
}

void Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  // m_current_pipeline is not tracked by this backend; the state manager
  // already filters repeated bindings.
  const auto* const dx = static_cast<const DXPipeline*>(pipeline);
  if (m_current_pipeline == dx)
    return;

  D3D::StateManager& state = State();
  if (!dx)
  {
    // Drop references to objects owned by a pipeline that may be going away.
    state.SetInputLayout(nullptr);
    state.SetVertexShader(nullptr);
    state.SetGeometryShader(nullptr);
    state.SetPixelShader(nullptr);
    return;
  }

  state.SetRasterizerState(dx->GetRasterizerState());
  state.SetDepthState(dx->GetDepthState());
  state.SetBlendState(dx->GetBlendState());
  state.SetPrimitiveTopology(dx->GetPrimitiveTopology());
  state.SetInputLayout(dx->GetInputLayout());
  state.SetVertexShader(dx->GetVertexShader());
  state.SetGeometryShader(dx->GetGeometryShader());
  state.SetPixelShader(dx->GetPixelShader());
  state.SetIntegerRTV(dx->UseLogicOp());
}

void Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  // An empty rectangle is widened to one pixel.
  D3D11_RECT scissor;
  scissor.left = rc.left;
  scissor.top = rc.top;
  scissor.right = std::max(rc.right, rc.left + 1);
  scissor.bottom = std::max(rc.bottom, rc.top + 1);
  D3D::context->RSSetScissorRects(1, &scissor);
}

void Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                      float far_depth)
{
  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = x;
  viewport.TopLeftY = y;
  viewport.Width = width;
  viewport.Height = height;
  viewport.MinDepth = near_depth;
  viewport.MaxDepth = far_depth;
  D3D::context->RSSetViewports(1, &viewport);
}

void Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  State().Apply();
  D3D::context->Draw(num_vertices, base_vertex);
}

void Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  State().Apply();
  D3D::context->DrawIndexed(num_indices, base_index, static_cast<INT>(base_vertex));
}

void Gfx::DispatchComputeShader(const AbstractShader* shader, u32 /*groupsize_x*/,
                                u32 /*groupsize_y*/, u32 /*groupsize_z*/, u32 groups_x,
                                u32 groups_y, u32 groups_z)
{
  // Group sizes are compiled into the shader.
  State().SetComputeShader(static_cast<const DXShader*>(shader)->GetD3DComputeShader());
  State().SyncComputeBindings();
  D3D::context->Dispatch(groups_x, groups_y, groups_z);
}

// The swap chain's single buffer is always the one drawn to.
void Gfx::PresentBackbuffer()
{
  m_swap_chain->Present();
}

bool Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  CheckForSwapChainChanges();
  SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(), clear_color);
  return true;
}

void Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);
  if (!m_swap_chain)
    return;

  // Quad-buffered stereo and HDR both need a new swap chain.
  if (bits & CONFIG_CHANGE_BIT_STEREO_MODE)
    m_swap_chain->SetStereo(SwapChain::WantsStereo());
  if (bits & CONFIG_CHANGE_BIT_HDR)
    m_swap_chain->SetHDR(SwapChain::WantsHDR());
}

void Gfx::CheckForSwapChainChanges()
{
  // A fullscreen change counts as a resize; it is only checked when the
  // presenter reports no resize itself.
  const bool new_surface = g_presenter->SurfaceChangedTestAndClear();
  const bool resized =
      g_presenter->SurfaceResizedTestAndClear() || m_swap_chain->CheckForFullscreenChange();
  if (!new_surface && !resized)
    return;

  if (new_surface)
    m_swap_chain->ChangeSurface(g_presenter->GetNewSurfaceHandle());
  else
    m_swap_chain->ResizeSwapChain();

  g_presenter->SetBackbuffer(static_cast<int>(m_swap_chain->GetWidth()),
                             static_cast<int>(m_swap_chain->GetHeight()));
}

void Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  // A framebuffer's textures cannot stay bound for sampling while it is a target.
  auto* const fb = static_cast<DXFramebuffer*>(framebuffer);
  fb->Unbind();
  State().SetFramebuffer(fb);
  m_current_framebuffer = fb;
}

void Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  // Nothing to gain from discarding on D3D11.
  SetFramebuffer(framebuffer);
}

void Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                 float depth_value)
{
  SetFramebuffer(framebuffer);
  // The clear needs the views bound first.
  State().Apply();
  static_cast<DXFramebuffer*>(framebuffer)->Clear(color_value, depth_value);
}

void Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  ID3D11ShaderResourceView* const view =
      texture ? static_cast<const DXTexture*>(texture)->GetD3DSRV() : nullptr;
  State().SetTexture(index, view);
}

void Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  State().SetSampler(index, m_state_cache.Get(state));
}

void Gfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool /*read*/, bool /*write*/)
{
  ID3D11UnorderedAccessView* const view =
      texture ? static_cast<DXTexture*>(texture)->GetD3DUAV() : nullptr;
  State().SetComputeUAV(index, view);
}

void Gfx::UnbindTexture(const AbstractTexture* texture)
{
  if (State().UnsetTexture(static_cast<const DXTexture*>(texture)->GetD3DSRV()) != 0)
    State().ApplyTextures();
}

void Gfx::Flush()
{
  D3D::context->Flush();
}

void Gfx::WaitForGPUIdle()
{
  // D3D11 has no way to block until the GPU is idle; flushing is the closest.
  D3D::context->Flush();
}

void Gfx::SetFullscreen(bool enable_fullscreen)
{
  if (m_swap_chain)
    m_swap_chain->SetFullscreen(enable_fullscreen);
}

bool Gfx::IsFullscreen() const
{
  return m_swap_chain && m_swap_chain->GetFullscreen();
}

SurfaceInfo Gfx::GetSurfaceInfo() const
{
  if (!m_swap_chain)
    return {0, 0, m_backbuffer_scale, AbstractTextureFormat::Undefined};
  return {m_swap_chain->GetWidth(), m_swap_chain->GetHeight(), m_backbuffer_scale,
          m_swap_chain->GetFormat()};
}

}  // namespace DX11
