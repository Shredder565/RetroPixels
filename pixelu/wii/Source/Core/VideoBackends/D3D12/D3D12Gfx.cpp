// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/D3D12Gfx.h"

#include "Common/Logging/Log.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12BoundingBox.h"
#include "VideoBackends/D3D12/D3D12PerfQuery.h"
#include "VideoBackends/D3D12/D3D12SwapChain.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DX12Pipeline.h"
#include "VideoBackends/D3D12/DX12Shader.h"
#include "VideoBackends/D3D12/DX12Texture.h"
#include "VideoBackends/D3D12/DX12VertexFormat.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
namespace
{
// Whether the pipeline's vertex shader fetches vertices itself from the
// vertex buffer SRV (the uber shaders with a dynamic vertex loader, and
// every GX shader when lines and points are expanded in the vertex shader).
bool ReadsVertexSRV(const AbstractPipeline* pipeline)
{
  const AbstractPipelineUsage usage = static_cast<const DXPipeline*>(pipeline)->GetUsage();
  if (usage == AbstractPipelineUsage::GXUber && g_backend_info.bSupportsDynamicVertexLoader)
    return true;
  return usage != AbstractPipelineUsage::Utility && g_ActiveConfig.UseVSForLinePointExpand();
}

// Copies one CPU descriptor into a fresh slot of the current list's heap.
bool StageDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_GPU_DESCRIPTOR_HANDLE* table)
{
  DescriptorHandle slot;
  if (!g_dx_context->GetDescriptorAllocator()->Allocate(1, &slot))
    return false;

  g_dx_context->GetDevice()->CopyDescriptorsSimple(1, slot.cpu_handle, source,
                                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  *table = slot.gpu_handle;
  return true;
}

DXTexture* AsDX(AbstractTexture* texture)
{
  return static_cast<DXTexture*>(texture);
}
}  // namespace

Gfx::Gfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_backbuffer_scale(backbuffer_scale), m_swap_chain(std::move(swap_chain))
{
  m_state.root_signature = g_dx_context->GetGXRootSignature();

  // Unbound slots read the null SRV and sample with point filtering.
  const UINT64 null_srv = g_dx_context->GetNullSRVDescriptor().cpu_handle.ptr;
  for (u32 slot = 0; slot < VideoCommon::MAX_PIXEL_SHADER_SAMPLERS; ++slot)
  {
    m_state.textures[slot].ptr = null_srv;
    m_state.samplers.states[slot] = RenderState::GetPointSamplerState();
  }
}

Gfx::~Gfx() = default;

bool Gfx::IsHeadless() const
{
  return m_swap_chain == nullptr;
}

// Shaders, pipelines and vertex formats.

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromSource(ShaderStage stage,
                                                            std::string_view source,
                                                            VideoCommon::ShaderIncluder* includer,
                                                            std::string_view label)
{
  return DXShader::CreateFromSource(stage, source, includer, label);
}

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                            size_t size, std::string_view label)
{
  return DXShader::CreateFromBytecode(stage, DXShader::CreateByteCode(data, size), label);
}

std::unique_ptr<AbstractPipeline> Gfx::CreatePipeline(const AbstractPipelineConfig& pipeline_config,
                                                      const void* blob, size_t blob_size)
{
  return DXPipeline::Create(pipeline_config, blob, blob_size);
}

std::unique_ptr<NativeVertexFormat>
Gfx::CreateNativeVertexFormat(const PortableVertexDeclaration& declaration)
{
  return std::make_unique<DXVertexFormat>(declaration);
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
  return DXFramebuffer::Create(AsDX(color), AsDX(depth), std::move(extra_colors));
}

// Submission without (Flush) and with (WaitForGPUIdle) waiting.
void Gfx::WaitForGPUIdle()
{
  ExecuteCommandList(true);
}

void Gfx::Flush()
{
  ExecuteCommandList(false);
}

void Gfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable,
                      bool alpha_enable, bool z_enable, u32 color, u32 z)
{
  // Whole-colour and depth clears go straight to the views; a partial colour
  // clear needs the shader path in the base class.
  const bool clear_color_here = color_enable && alpha_enable;
  if (clear_color_here || z_enable)
  {
    D3D12_RECT area;
    area.left = target_rc.left;
    area.top = target_rc.top;
    area.right = target_rc.right;
    area.bottom = target_rc.bottom;
    const auto* const fb = static_cast<const DXFramebuffer*>(m_current_framebuffer);

    if (clear_color_here)
    {
      fb->TransitionRenderTargets();

      // Packed as ARGB8.
      const auto channel = [color](u32 shift) {
        return static_cast<float>((color >> shift) & 0xFF) / 255.0f;
      };
      const ClearColor rgba = {{channel(16), channel(8), channel(0), channel(24)}};
      fb->ClearRenderTargets(rgba, &area);
      color_enable = false;
      alpha_enable = false;
    }

    if (z_enable)
    {
      AsDX(m_current_framebuffer->GetDepthAttachment())
          ->TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);

      // 24-bit depth, stored reversed since D3D cannot flip the depth range.
      const float depth = 1.0f - static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
      fb->ClearDepth(depth, &area);
      z_enable = false;
    }
  }

  if (color_enable || alpha_enable || z_enable)
    ::AbstractGfx::ClearRegion(target_rc, color_enable, alpha_enable, z_enable, color, z);
}

void Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  if (m_current_pipeline == pipeline)
    return;

  m_current_pipeline = pipeline;
  m_dirty_bits |= PENDING_PIPELINE;
  if (pipeline == nullptr)
    return;

  const auto* const dx = static_cast<const DXPipeline*>(pipeline);
  if (dx->GetRootSignature() != m_state.root_signature)
  {
    // Root arguments do not survive a root signature change.
    m_state.root_signature = dx->GetRootSignature();
    m_dirty_bits |= PENDING_ROOT_SIGNATURE | PENDING_CBV_PIXEL | PENDING_CBV_VERTEX |
                    PENDING_CBV_GEOMETRY | PENDING_BIND_TEXTURES | PENDING_BIND_SAMPLERS |
                    PENDING_BIND_PS_UAV | PENDING_BIND_VS_SRV | PENDING_CBV_CUSTOM;
  }
  if (dx->UseIntegerRTV() != m_state.integer_rtv)
  {
    m_state.integer_rtv = dx->UseIntegerRTV();
    m_dirty_bits |= PENDING_FRAMEBUFFER;
  }
  if (dx->GetPrimitiveTopology() != m_state.topology)
  {
    m_state.topology = dx->GetPrimitiveTopology();
    m_dirty_bits |= PENDING_TOPOLOGY;
  }
}

void Gfx::BindFramebuffer(DXFramebuffer* fb)
{
  fb->TransitionRenderTargets();
  if (fb->HasDepthBuffer())
    AsDX(fb->GetDepthAttachment())->TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);

  const D3D12_CPU_DESCRIPTOR_HANDLE* const colour =
      m_state.integer_rtv ? fb->GetIntRTVDescriptorArray() : fb->GetRTVDescriptorArray();
  g_dx_context->GetCommandList()->OMSetRenderTargets(fb->GetRTVDescriptorCount(), colour, FALSE,
                                                     fb->GetDSVDescriptorArray());
  m_current_framebuffer = fb;
  m_dirty_bits &= ~PENDING_FRAMEBUFFER;
}

void Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer != framebuffer)
  {
    m_current_framebuffer = framebuffer;
    m_dirty_bits |= PENDING_FRAMEBUFFER;
  }
}

void Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  SetFramebuffer(framebuffer);

  // Discarding needs the attachments in their output states.
  DXFramebuffer* const fb = static_cast<DXFramebuffer*>(framebuffer);
  fb->TransitionRenderTargets();
  if (fb->HasDepthBuffer())
    AsDX(fb->GetDepthAttachment())->TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
  fb->Unbind();
}

void Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                 float depth_value)
{
  DXFramebuffer* const fb = static_cast<DXFramebuffer*>(framebuffer);
  BindFramebuffer(fb);
  fb->ClearRenderTargets(color_value, nullptr);
  fb->ClearDepth(depth_value, nullptr);
}

void Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  D3D12_RECT& scissor = m_state.scissor;
  const bool same = scissor.left == rc.left && scissor.top == rc.top && scissor.right == rc.right &&
                    scissor.bottom == rc.bottom;
  if (same)
    return;

  scissor.left = rc.left;
  scissor.top = rc.top;
  scissor.right = rc.right;
  scissor.bottom = rc.bottom;
  m_dirty_bits |= PENDING_SCISSOR;
}

void Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  // The texture must not be null; empty slots are set through UnbindTexture().
  const DXTexture* const dx = static_cast<const DXTexture*>(texture);
  const UINT64 srv = dx->GetSRVDescriptor().cpu_handle.ptr;
  if (m_state.textures[index].ptr == srv)
    return;

  m_state.textures[index].ptr = srv;
  dx->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_dirty_bits |= PENDING_TEXTURE_TABLE;
}

void Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  SamplerState& slot = m_state.samplers.states[index];
  if (slot != state)
  {
    slot = state;
    m_dirty_bits |= PENDING_SAMPLER_TABLE;
  }
}

void Gfx::SetComputeImageTexture(u32 /*index*/, AbstractTexture* texture, bool /*read*/,
                                 bool /*write*/)
{
  const DXTexture* const dx = static_cast<const DXTexture*>(texture);
  if (m_state.compute_image == dx)
    return;

  m_state.compute_image = dx;
  if (dx != nullptr)
    dx->TransitionToState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  m_dirty_bits |= PENDING_COMPUTE_IMAGE;
}

void Gfx::UnbindTexture(const AbstractTexture* texture)
{
  const UINT64 srv = static_cast<const DXTexture*>(texture)->GetSRVDescriptor().cpu_handle.ptr;
  const UINT64 null_srv = g_dx_context->GetNullSRVDescriptor().cpu_handle.ptr;
  for (D3D12_CPU_DESCRIPTOR_HANDLE& slot : m_state.textures)
  {
    if (slot.ptr == srv)
    {
      slot.ptr = null_srv;
      m_dirty_bits |= PENDING_TEXTURE_TABLE;
    }
  }

  if (m_state.compute_image == texture)
  {
    m_state.compute_image = nullptr;
    m_dirty_bits |= PENDING_COMPUTE_IMAGE;
  }
}

void Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                      float far_depth)
{
  D3D12_VIEWPORT& vp = m_state.viewport;
  const bool same = vp.TopLeftX == x && vp.TopLeftY == y && vp.Width == width &&
                    vp.Height == height && vp.MinDepth == near_depth && vp.MaxDepth == far_depth;
  if (same)
    return;

  vp.TopLeftX = x;
  vp.TopLeftY = y;
  vp.Width = width;
  vp.Height = height;
  vp.MinDepth = near_depth;
  vp.MaxDepth = far_depth;
  m_dirty_bits |= PENDING_VIEWPORT;
}

void Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  if (ApplyState())
    g_dx_context->GetCommandList()->DrawInstanced(num_vertices, 1, base_vertex, 0);
}

void Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (!ApplyState())
    return;

  ID3D12GraphicsCommandList* const list = g_dx_context->GetCommandList();

  // SV_VertexID does not include the base vertex in D3D12, so shaders that
  // fetch vertices themselves get it as a root constant.
  if (ReadsVertexSRV(m_current_pipeline))
    list->SetGraphicsRoot32BitConstant(ROOT_PARAMETER_BASE_VERTEX_CONSTANT, base_vertex, 0);
  list->DrawIndexedInstanced(num_indices, 1, base_index, static_cast<INT>(base_vertex), 0);
}

void Gfx::DispatchComputeShader(const AbstractShader* shader, u32 /*groupsize_x*/,
                                u32 /*groupsize_y*/, u32 /*groupsize_z*/, u32 groups_x,
                                u32 groups_y, u32 groups_z)
{
  SetRootSignatures();
  SetDescriptorHeaps();
  UpdateDescriptorTables();

  if ((m_dirty_bits & PENDING_COMPUTE_IMAGE) != 0 && !UpdateComputeUAVDescriptorTable())
  {
    // Out of descriptors: start a new list and build everything again.
    ExecuteCommandList(false);
    SetRootSignatures();
    SetDescriptorHeaps();
    UpdateDescriptorTables();
    UpdateComputeUAVDescriptorTable();
  }

  // Dispatches are rare, so the compute arguments are not tracked; they are
  // taken from the graphics bindings each time.
  ID3D12GraphicsCommandList* const list = g_dx_context->GetCommandList();
  list->SetPipelineState(static_cast<const DXShader*>(shader)->GetComputePipeline());
  list->SetComputeRootConstantBufferView(CS_ROOT_PARAMETER_CBV, m_state.constant_buffers[0]);
  list->SetComputeRootDescriptorTable(CS_ROOT_PARAMETER_SRV, m_state.texture_table);
  list->SetComputeRootDescriptorTable(CS_ROOT_PARAMETER_SAMPLERS, m_state.sampler_table);
  list->SetComputeRootDescriptorTable(CS_ROOT_PARAMETER_UAV, m_state.compute_uav_table);
  list->Dispatch(groups_x, groups_y, groups_z);

  // The compute pipeline replaced the graphics one.
  m_dirty_bits |= PENDING_PIPELINE;
}

bool Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  CheckForSwapChainChanges();
  SetAndClearFramebuffer(m_swap_chain->GetCurrentFramebuffer(), clear_color);
  return true;
}

void Gfx::CheckForSwapChainChanges()
{
  const bool new_surface = g_presenter->SurfaceChangedTestAndClear();
  const bool new_size =
      g_presenter->SurfaceResizedTestAndClear() || m_swap_chain->CheckForFullscreenChange();
  if (!new_surface && !new_size)
    return;

  // Earlier frames may still be using the buffers.
  WaitForGPUIdle();
  if (new_surface)
    m_swap_chain->ChangeSurface(g_presenter->GetNewSurfaceHandle());
  else
    m_swap_chain->ResizeSwapChain();

  g_presenter->SetBackbuffer(static_cast<int>(m_swap_chain->GetWidth()),
                             static_cast<int>(m_swap_chain->GetHeight()));
}

void Gfx::PresentBackbuffer()
{
  m_current_framebuffer = nullptr;

  m_swap_chain->GetCurrentTexture()->TransitionToState(D3D12_RESOURCE_STATE_PRESENT);
  ExecuteCommandList(false);
  m_swap_chain->Present();
}

SurfaceInfo Gfx::GetSurfaceInfo() const
{
  if (!m_swap_chain)
    return {0, 0, m_backbuffer_scale, AbstractTextureFormat::Undefined};

  return {static_cast<u32>(m_swap_chain->GetWidth()), static_cast<u32>(m_swap_chain->GetHeight()),
          m_backbuffer_scale, m_swap_chain->GetFormat()};
}

void Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  // Stereo changes the buffers' layer count and HDR their format, so the
  // swap chain is rebuilt once the GPU is idle.
  if (m_swap_chain && (bits & CONFIG_CHANGE_BIT_STEREO_MODE) != 0)
  {
    ExecuteCommandList(true);
    m_swap_chain->SetStereo(SwapChain::WantsStereo());
  }
  if (m_swap_chain && (bits & CONFIG_CHANGE_BIT_HDR) != 0)
  {
    ExecuteCommandList(true);
    m_swap_chain->SetHDR(SwapChain::WantsHDR());
  }

  // Cached sampler descriptors bake in filtering and anisotropy overrides.
  if ((bits & (CONFIG_CHANGE_BIT_ANISOTROPY | CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING)) != 0)
  {
    ExecuteCommandList(true);
    g_dx_context->GetSamplerHeapManager().Clear();
    g_dx_context->ResetSamplerAllocators();
  }

  // The GX root signature depends on bounding box, per-pixel lighting and
  // line/point expansion.
  if ((bits & CONFIG_CHANGE_BIT_HOST_CONFIG) != 0)
    g_dx_context->RecreateGXRootSignature();
}

void Gfx::ExecuteCommandList(bool wait_for_completion)
{
  PerfQuery::GetInstance()->ResolveQueries();
  g_dx_context->ExecuteCommandList(wait_for_completion);
  m_dirty_bits = PENDING_EVERYTHING;
}

void Gfx::SetConstantBuffer(u32 index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
  if (m_state.constant_buffers[index] != address)
  {
    m_state.constant_buffers[index] = address;
    m_dirty_bits |= PENDING_CBV_PIXEL << index;
  }
}

void Gfx::SetTextureDescriptor(u32 index, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
  if (m_state.textures[index].ptr != handle.ptr)
  {
    m_state.textures[index].ptr = handle.ptr;
    m_dirty_bits |= PENDING_TEXTURE_TABLE;
  }
}

void Gfx::SetPixelShaderUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
  if (m_state.ps_uav.ptr != handle.ptr)
  {
    m_state.ps_uav = handle;
    m_dirty_bits |= PENDING_PS_UAV_TABLE;
  }
}

void Gfx::SetVertexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, D3D12_CPU_DESCRIPTOR_HANDLE srv,
                          u32 stride, u32 size)
{
  D3D12_VERTEX_BUFFER_VIEW& view = m_state.vertex_buffer;
  if (view.BufferLocation != address || view.StrideInBytes != stride || view.SizeInBytes != size)
  {
    view.BufferLocation = address;
    view.StrideInBytes = stride;
    view.SizeInBytes = size;
    m_dirty_bits |= PENDING_VERTEX_BUFFER;
  }
  if (m_state.vs_srv.ptr != srv.ptr)
  {
    m_state.vs_srv = srv;
    m_dirty_bits |= PENDING_VS_SRV_TABLE;
  }
}

void Gfx::SetIndexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size, DXGI_FORMAT format)
{
  D3D12_INDEX_BUFFER_VIEW& view = m_state.index_buffer;
  if (view.BufferLocation == address && view.SizeInBytes == size && view.Format == format)
    return;

  view.BufferLocation = address;
  view.SizeInBytes = size;
  view.Format = format;
  m_dirty_bits |= PENDING_INDEX_BUFFER;
}

bool Gfx::ApplyState()
{
  if (m_current_framebuffer == nullptr || m_current_pipeline == nullptr)
    return false;

  // Building the tables may itself submit the list when descriptors run out,
  // so it comes before anything else is recorded.
  SetRootSignatures();
  SetDescriptorHeaps();
  UpdateDescriptorTables();

  // Everything recorded below is cleared up front, including the arguments a
  // utility pipeline's root signature has no slot for. Table rebuilds,
  // compute bindings and the root signatures stay pending.
  constexpr u32 RECORDED_HERE =
      PENDING_FRAMEBUFFER | PENDING_PIPELINE | PENDING_VIEWPORT | PENDING_SCISSOR |
      PENDING_PS_UAV_TABLE | PENDING_CBV_PIXEL | PENDING_CBV_VERTEX | PENDING_CBV_GEOMETRY |
      PENDING_BIND_TEXTURES | PENDING_BIND_SAMPLERS | PENDING_BIND_PS_UAV | PENDING_VERTEX_BUFFER |
      PENDING_INDEX_BUFFER | PENDING_TOPOLOGY | PENDING_BIND_VS_SRV | PENDING_CBV_CUSTOM;
  const u32 todo = m_dirty_bits;
  m_dirty_bits &= ~RECORDED_HERE;
  const auto pending = [todo](u32 bit) { return (todo & bit) != 0; };

  ID3D12GraphicsCommandList* const list = g_dx_context->GetCommandList();
  const auto* const pipeline = static_cast<const DXPipeline*>(m_current_pipeline);

  if (pending(PENDING_PIPELINE))
    list->SetPipelineState(pipeline->GetPipeline());
  if (pending(PENDING_FRAMEBUFFER))
    BindFramebuffer(static_cast<DXFramebuffer*>(m_current_framebuffer));
  if (pending(PENDING_VIEWPORT))
    list->RSSetViewports(1, &m_state.viewport);
  if (pending(PENDING_SCISSOR))
    list->RSSetScissorRects(1, &m_state.scissor);
  if (pending(PENDING_VERTEX_BUFFER))
    list->IASetVertexBuffers(0, 1, &m_state.vertex_buffer);
  if (pending(PENDING_INDEX_BUFFER))
    list->IASetIndexBuffer(&m_state.index_buffer);
  if (pending(PENDING_TOPOLOGY))
    list->IASetPrimitiveTopology(m_state.topology);
  if (pending(PENDING_BIND_TEXTURES))
    list->SetGraphicsRootDescriptorTable(ROOT_PARAMETER_PS_SRV, m_state.texture_table);
  if (pending(PENDING_BIND_SAMPLERS))
    list->SetGraphicsRootDescriptorTable(ROOT_PARAMETER_PS_SAMPLERS, m_state.sampler_table);

  // The utility root signature only has the pixel constants, textures and
  // samplers.
  if (pipeline->GetUsage() != AbstractPipelineUsage::Utility)
  {
    if (pending(PENDING_CBV_VERTEX))
    {
      const D3D12_GPU_VIRTUAL_ADDRESS vertex_cb = m_state.constant_buffers[1];
      list->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_VS_CBV, vertex_cb);
      list->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_VS_CBV2, vertex_cb);

      // Per-pixel lighting reads the vertex constants in the pixel shader too,
      // from whichever slot bounding box leaves free.
      if (g_ActiveConfig.bEnablePixelLighting)
      {
        list->SetGraphicsRootConstantBufferView(
            g_ActiveConfig.bBBoxEnable ? ROOT_PARAMETER_PS_CBV2 : ROOT_PARAMETER_PS_UAV_OR_CBV2,
            vertex_cb);
      }
    }

    if (pending(PENDING_CBV_CUSTOM))
    {
      list->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_VS_CUS_CBV,
                                              m_state.constant_buffers[2]);
      list->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_PS_CUS_CBV,
                                              m_state.constant_buffers[2]);
    }

    if (pending(PENDING_BIND_VS_SRV) && ReadsVertexSRV(pipeline))
      list->SetGraphicsRootDescriptorTable(ROOT_PARAMETER_VS_SRV, m_state.vs_srv_table);

    if (pending(PENDING_CBV_GEOMETRY))
      list->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_GS_CBV, m_state.constant_buffers[3]);

    if (pending(PENDING_BIND_PS_UAV) && g_ActiveConfig.bBBoxEnable)
      list->SetGraphicsRootDescriptorTable(ROOT_PARAMETER_PS_UAV_OR_CBV2, m_state.ps_uav_table);
  }

  if (pending(PENDING_CBV_PIXEL))
    list->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_PS_CBV, m_state.constant_buffers[0]);

  return true;
}

void Gfx::SetRootSignatures()
{
  const u32 todo = m_dirty_bits;
  m_dirty_bits &= ~(PENDING_ROOT_SIGNATURE | PENDING_COMPUTE_ROOT_SIGNATURE);

  ID3D12GraphicsCommandList* const list = g_dx_context->GetCommandList();
  if ((todo & PENDING_ROOT_SIGNATURE) != 0)
    list->SetGraphicsRootSignature(m_state.root_signature);
  if ((todo & PENDING_COMPUTE_ROOT_SIGNATURE) != 0)
    list->SetComputeRootSignature(g_dx_context->GetComputeRootSignature());
}

void Gfx::SetDescriptorHeaps()
{
  if ((m_dirty_bits & PENDING_DESCRIPTOR_HEAPS) == 0)
    return;

  g_dx_context->GetCommandList()->SetDescriptorHeaps(g_dx_context->GetGPUDescriptorHeapCount(),
                                                     g_dx_context->GetGPUDescriptorHeaps());
  m_dirty_bits &= ~PENDING_DESCRIPTOR_HEAPS;
}

void Gfx::UpdateDescriptorTables()
{
  // Each table is tried in turn even after one has failed.
  const bool textures_failed =
      (m_dirty_bits & PENDING_TEXTURE_TABLE) != 0 && !UpdateSRVDescriptorTable();
  const bool samplers_failed =
      (m_dirty_bits & PENDING_SAMPLER_TABLE) != 0 && !UpdateSamplerDescriptorTable();
  const bool uav_failed = (m_dirty_bits & PENDING_PS_UAV_TABLE) != 0 && !UpdateUAVDescriptorTable();
  const bool vs_srv_failed =
      (m_dirty_bits & PENDING_VS_SRV_TABLE) != 0 && !UpdateVSSRVDescriptorTable();
  if (!textures_failed && !samplers_failed && !uav_failed && !vs_srv_failed)
    return;

  WARN_LOG_FMT(VIDEO, "Executing command list while waiting for temporary {}",
               textures_failed ? "descriptors" : "samplers");

  // A new list starts with empty heaps, so every table is built again.
  ExecuteCommandList(false);
  SetRootSignatures();
  SetDescriptorHeaps();
  UpdateSRVDescriptorTable();
  UpdateSamplerDescriptorTable();
  UpdateUAVDescriptorTable();
  UpdateVSSRVDescriptorTable();
}

bool Gfx::UpdateSRVDescriptorTable()
{
  constexpr UINT SLOTS = VideoCommon::MAX_PIXEL_SHADER_SAMPLERS;
  DescriptorHandle table;
  if (!g_dx_context->GetDescriptorAllocator()->Allocate(SLOTS, &table))
    return false;

  // One contiguous destination range filled from single-descriptor sources.
  static constexpr std::array<UINT, SLOTS> SINGLE = {1, 1, 1, 1, 1, 1, 1, 1,
                                                     1, 1, 1, 1, 1, 1, 1, 1};
  const UINT range = SLOTS;
  g_dx_context->GetDevice()->CopyDescriptors(1, &table.cpu_handle, &range, SLOTS,
                                             m_state.textures.data(), SINGLE.data(),
                                             D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_state.texture_table = table.gpu_handle;
  m_dirty_bits = (m_dirty_bits & ~PENDING_TEXTURE_TABLE) | PENDING_BIND_TEXTURES;
  return true;
}

bool Gfx::UpdateSamplerDescriptorTable()
{
  if (!g_dx_context->GetSamplerAllocator()->GetGroupHandle(m_state.samplers,
                                                           &m_state.sampler_table))
  {
    // Full: start over in every list's sampler heap.
    g_dx_context->ResetSamplerAllocators();
    return false;
  }

  m_dirty_bits = (m_dirty_bits & ~PENDING_SAMPLER_TABLE) | PENDING_BIND_SAMPLERS;
  return true;
}

bool Gfx::UpdateUAVDescriptorTable()
{
  // Only bounding box uses the pixel-stage UAV.
  if (!g_ActiveConfig.bBBoxEnable)
    return true;

  if (!StageDescriptor(m_state.ps_uav, &m_state.ps_uav_table))
    return false;
  m_dirty_bits = (m_dirty_bits & ~PENDING_PS_UAV_TABLE) | PENDING_BIND_PS_UAV;
  return true;
}

bool Gfx::UpdateVSSRVDescriptorTable()
{
  if (!ReadsVertexSRV(m_current_pipeline))
    return true;

  if (!StageDescriptor(m_state.vs_srv, &m_state.vs_srv_table))
    return false;
  m_dirty_bits = (m_dirty_bits & ~PENDING_VS_SRV_TABLE) | PENDING_BIND_VS_SRV;
  return true;
}

bool Gfx::UpdateComputeUAVDescriptorTable()
{
  DescriptorHandle slot;
  if (!g_dx_context->GetDescriptorAllocator()->Allocate(1, &slot))
    return false;

  if (m_state.compute_image != nullptr)
  {
    g_dx_context->GetDevice()->CopyDescriptorsSimple(
        1, slot.cpu_handle, m_state.compute_image->GetUAVDescriptor().cpu_handle,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  else
  {
    // An all-zero description makes a null view.
    const D3D12_UNORDERED_ACCESS_VIEW_DESC null_view = {};
    g_dx_context->GetDevice()->CreateUnorderedAccessView(nullptr, nullptr, &null_view,
                                                         slot.cpu_handle);
  }

  m_dirty_bits &= ~PENDING_COMPUTE_IMAGE;
  m_state.compute_uav_table = slot.gpu_handle;
  return true;
}

}  // namespace DX12
