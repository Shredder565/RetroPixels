// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <d3d12.h>

#include "VideoBackends/D3D12/DescriptorAllocator.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Constants.h"

namespace DX12
{
class DXFramebuffer;
class DXTexture;
class DXShader;
class DXPipeline;
class SwapChain;

// The D3D12 renderer. Bindings are recorded here and only written into the
// command list, in one go, when a draw or dispatch needs them.
class Gfx final : public ::AbstractGfx
{
public:
  Gfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale);
  ~Gfx() override;

  static Gfx* GetInstance() { return static_cast<Gfx*>(g_gfx.get()); }

  bool IsHeadless() const override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;

  std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         VideoCommon::ShaderIncluder* shader_includer,
                         std::string_view name) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;

  void Flush() override;
  void WaitForGPUIdle() override;

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable, bool alpha_enable,
                   bool z_enable, u32 color, u32 z) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void SetSamplerState(u32 index, const SamplerState& state) override;
  void SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write) override;
  void UnbindTexture(const AbstractTexture* texture) override;
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth) override;
  void Draw(u32 base_vertex, u32 num_vertices) override;
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) override;
  void DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                             u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z) override;
  bool BindBackbuffer(const ClearColor& clear_color = {}) override;
  void PresentBackbuffer() override;

  SurfaceInfo GetSurfaceInfo() const override;

  // Resolves pending queries, submits the command list and marks every
  // binding for re-recording in the next one. Also used to free up ring and
  // descriptor space.
  void ExecuteCommandList(bool wait_for_completion);

  // Constant buffer slot 0 pixel, 1 vertex, 2 custom pixel, 3 geometry.
  void SetConstantBuffer(u32 index, D3D12_GPU_VIRTUAL_ADDRESS address);

  // A pixel-stage texture slot, by its CPU-side SRV descriptor.
  void SetTextureDescriptor(u32 index, D3D12_CPU_DESCRIPTOR_HANDLE handle);

  // The bounding-box UAV.
  void SetPixelShaderUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle);

  void SetVertexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, D3D12_CPU_DESCRIPTOR_HANDLE srv,
                       u32 stride, u32 size);
  void SetIndexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size, DXGI_FORMAT format);

  // Records every changed binding; false when nothing can be drawn yet.
  bool ApplyState();

protected:
  void OnConfigChanged(u32 bits) override;

private:
  // Bindings waiting to be recorded. The four constant buffer bits are
  // consecutive so a slot number can select its bit.
  enum PendingBit : u32
  {
    PENDING_FRAMEBUFFER = 1u << 0,
    PENDING_PIPELINE = 1u << 1,
    PENDING_TEXTURE_TABLE = 1u << 2,  // texture slots changed, table must be rebuilt
    PENDING_SAMPLER_TABLE = 1u << 3,  // sampler states changed, group must be found
    PENDING_VIEWPORT = 1u << 4,
    PENDING_SCISSOR = 1u << 5,
    PENDING_COMPUTE_IMAGE = 1u << 6,
    PENDING_PS_UAV_TABLE = 1u << 7,
    PENDING_CBV_PIXEL = 1u << 8,
    PENDING_CBV_VERTEX = 1u << 9,
    PENDING_CBV_CUSTOM = 1u << 10,
    PENDING_CBV_GEOMETRY = 1u << 11,
    PENDING_BIND_TEXTURES = 1u << 12,  // texture table built, root argument to set
    PENDING_BIND_SAMPLERS = 1u << 13,
    PENDING_BIND_PS_UAV = 1u << 14,
    PENDING_VERTEX_BUFFER = 1u << 15,
    PENDING_INDEX_BUFFER = 1u << 16,
    PENDING_TOPOLOGY = 1u << 17,
    PENDING_ROOT_SIGNATURE = 1u << 18,
    PENDING_COMPUTE_ROOT_SIGNATURE = 1u << 19,
    PENDING_DESCRIPTOR_HEAPS = 1u << 20,
    PENDING_VS_SRV_TABLE = 1u << 21,
    PENDING_BIND_VS_SRV = 1u << 22,

    PENDING_EVERYTHING = (1u << 23) - 1,
  };

  void CheckForSwapChainChanges();

  void BindFramebuffer(DXFramebuffer* fb);
  void SetRootSignatures();
  void SetDescriptorHeaps();
  void UpdateDescriptorTables();
  bool UpdateSRVDescriptorTable();
  bool UpdateUAVDescriptorTable();
  bool UpdateVSSRVDescriptorTable();
  bool UpdateComputeUAVDescriptorTable();
  bool UpdateSamplerDescriptorTable();

  float m_backbuffer_scale;

  std::unique_ptr<SwapChain> m_swap_chain;

  // What the next draw should see.
  struct Bindings
  {
    ID3D12RootSignature* root_signature = nullptr;
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4> constant_buffers = {};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> textures = {};
    D3D12_CPU_DESCRIPTOR_HANDLE vs_srv = {};
    D3D12_CPU_DESCRIPTOR_HANDLE ps_uav = {};
    SamplerStateSet samplers = {};
    const DXTexture* compute_image = nullptr;
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissor = {};

    // Shader-visible copies made for the current command list.
    D3D12_GPU_DESCRIPTOR_HANDLE vs_srv_table = {};
    D3D12_GPU_DESCRIPTOR_HANDLE texture_table = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table = {};
    D3D12_GPU_DESCRIPTOR_HANDLE ps_uav_table = {};
    D3D12_GPU_DESCRIPTOR_HANDLE compute_uav_table = {};

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer = {};
    D3D12_INDEX_BUFFER_VIEW index_buffer = {};
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool integer_rtv = false;
  };
  Bindings m_state;
  u32 m_dirty_bits = PENDING_EVERYTHING;
};
}  // namespace DX12
