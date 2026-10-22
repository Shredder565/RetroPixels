// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/RenderState.h"

namespace DX11
{
class DXFramebuffer;

// Creates D3D11 state objects for renderer states, once per distinct state.
// The cache keeps ownership; callers hold plain pointers.
class StateCache
{
public:
  ~StateCache();

  ID3D11SamplerState* Get(SamplerState state);
  ID3D11BlendState* Get(BlendingState state);
  ID3D11RasterizerState* Get(RasterizationState state);
  ID3D11DepthStencilState* Get(DepthState state);

  static D3D11_PRIMITIVE_TOPOLOGY GetPrimitiveTopology(PrimitiveType primitive);

private:
  // Declaration order fixes the release order on shutdown.
  std::unordered_map<u32, ComPtr<ID3D11DepthStencilState>> m_depth;
  std::unordered_map<u32, ComPtr<ID3D11RasterizerState>> m_raster;
  std::unordered_map<u32, ComPtr<ID3D11BlendState>> m_blend;
  std::unordered_map<SamplerState, ComPtr<ID3D11SamplerState>> m_sampler;
  std::mutex m_lock;
};

namespace D3D
{
// Shadows the immediate context's pipeline bindings. Setters only record the
// wanted binding and mark it dirty when it differs from what the context has;
// Apply pushes the dirty ones before a draw.
class StateManager
{
public:
  StateManager();
  ~StateManager();

  void SetBlendState(ID3D11BlendState* state)
  {
    Track(m_bound.blend, m_wanted.blend, state, BIT_BLEND);
  }
  void SetDepthState(ID3D11DepthStencilState* state)
  {
    Track(m_bound.depth, m_wanted.depth, state, BIT_DEPTH);
  }
  void SetRasterizerState(ID3D11RasterizerState* state)
  {
    Track(m_bound.raster, m_wanted.raster, state, BIT_RASTER);
  }

  void SetTexture(size_t index, ID3D11ShaderResourceView* texture)
  {
    Track(m_bound.srvs[index], m_wanted.srvs[index], texture, TextureBit(index));
  }
  void SetSampler(size_t index, ID3D11SamplerState* sampler)
  {
    Track(m_bound.samplers[index], m_wanted.samplers[index], sampler, SamplerBit(index));
  }

  // Pixel stage b0 and b1 (b1 carries vertex constants for per-pixel lighting).
  void SetPixelConstants(ID3D11Buffer* buffer0, ID3D11Buffer* buffer1 = nullptr)
  {
    if (m_bound.ps_cbs[0] != buffer0 || m_bound.ps_cbs[1] != buffer1)
      m_dirty |= BIT_PS_CB;
    m_wanted.ps_cbs = {buffer0, buffer1};
  }
  void SetVertexConstants(ID3D11Buffer* buffer)
  {
    Track(m_bound.vs_cb, m_wanted.vs_cb, buffer, BIT_VS_CB);
  }
  // Custom shader constants, shared by the vertex and pixel stages.
  void SetCustomConstants(ID3D11Buffer* buffer)
  {
    Track(m_bound.custom_cb, m_wanted.custom_cb, buffer, BIT_CUSTOM_CB);
  }
  void SetGeometryConstants(ID3D11Buffer* buffer)
  {
    Track(m_bound.gs_cb, m_wanted.gs_cb, buffer, BIT_GS_CB);
  }

  void SetVertexBuffer(ID3D11Buffer* buffer, u32 stride, u32 offset)
  {
    if (m_bound.vb != buffer || m_bound.vb_stride != stride || m_bound.vb_offset != offset)
      m_dirty |= BIT_VB;
    m_wanted.vb = buffer;
    m_wanted.vb_stride = stride;
    m_wanted.vb_offset = offset;
  }
  void SetIndexBuffer(ID3D11Buffer* buffer) { Track(m_bound.ib, m_wanted.ib, buffer, BIT_IB); }
  void SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
  {
    Track(m_bound.topology, m_wanted.topology, topology, BIT_INPUT);
  }
  void SetInputLayout(ID3D11InputLayout* layout)
  {
    Track(m_bound.layout, m_wanted.layout, layout, BIT_INPUT);
  }

  void SetPixelShader(ID3D11PixelShader* shader) { Track(m_bound.ps, m_wanted.ps, shader, BIT_PS); }
  // Binds immediately, with class instances; the shadow is updated to match.
  void SetPixelShaderDynamic(ID3D11PixelShader* shader, ID3D11ClassInstance* const* classInstances,
                             u32 classInstancesCount)
  {
    D3D::context->PSSetShader(shader, classInstances, classInstancesCount);
    m_bound.ps = shader;
    m_wanted.ps = shader;
  }
  void SetVertexShader(ID3D11VertexShader* shader)
  {
    Track(m_bound.vs, m_wanted.vs, shader, BIT_VS);
  }
  void SetGeometryShader(ID3D11GeometryShader* shader)
  {
    Track(m_bound.gs, m_wanted.gs, shader, BIT_GS);
  }

  // The framebuffer, the bounding-box UAV and the integer-view choice are
  // bound together by one output-merger call.
  void SetFramebuffer(DXFramebuffer* fb)
  {
    Track(m_bound.framebuffer, m_wanted.framebuffer, fb, BIT_OUTPUT);
  }
  void SetOMUAV(ID3D11UnorderedAccessView* uav)
  {
    Track(m_bound.om_uav, m_wanted.om_uav, uav, BIT_OUTPUT);
  }
  void SetIntegerRTV(bool enable)
  {
    Track(m_bound.integer_rtv, m_wanted.integer_rtv, enable, BIT_OUTPUT);
  }

  // Clears every pixel-stage slot currently holding srv; returns the slots as a mask.
  u32 UnsetTexture(ID3D11ShaderResourceView* srv);
  // Puts srv into every slot of the mask.
  void SetTextureByMask(u32 textureSlotMask, ID3D11ShaderResourceView* srv);
  // Pushes dirty pixel-stage textures and samplers only.
  void ApplyTextures();

  // Pushes every dirty binding. Call right before drawing.
  void Apply();

  // Compute bindings are not shadowed per draw: they are copied from the
  // graphics bindings on demand, since compute is rare.
  void SetComputeUAV(u32 index, ID3D11UnorderedAccessView* uav);
  void SetComputeShader(ID3D11ComputeShader* shader);
  void SyncComputeBindings();

private:
  static constexpr u32 SLOTS = VideoCommon::MAX_PIXEL_SHADER_SAMPLERS;
  static constexpr u32 COMPUTE_SLOTS = VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS;

  // Dirty bits: one per texture slot, one per sampler slot, then one per group.
  static constexpr u64 Bit(u32 n) { return u64{1} << n; }
  static constexpr u64 TextureBit(size_t slot) { return Bit(static_cast<u32>(slot)); }
  static constexpr u64 SamplerBit(size_t slot) { return Bit(SLOTS + static_cast<u32>(slot)); }
  static constexpr u64 BIT_PS_CB = u64{1} << (2 * SLOTS + 0);
  static constexpr u64 BIT_VS_CB = u64{1} << (2 * SLOTS + 1);
  static constexpr u64 BIT_CUSTOM_CB = u64{1} << (2 * SLOTS + 2);
  static constexpr u64 BIT_GS_CB = u64{1} << (2 * SLOTS + 3);
  static constexpr u64 BIT_VB = u64{1} << (2 * SLOTS + 4);
  static constexpr u64 BIT_IB = u64{1} << (2 * SLOTS + 5);
  static constexpr u64 BIT_PS = u64{1} << (2 * SLOTS + 6);
  static constexpr u64 BIT_VS = u64{1} << (2 * SLOTS + 7);
  static constexpr u64 BIT_GS = u64{1} << (2 * SLOTS + 8);
  static constexpr u64 BIT_INPUT = u64{1} << (2 * SLOTS + 9);
  static constexpr u64 BIT_BLEND = u64{1} << (2 * SLOTS + 10);
  static constexpr u64 BIT_DEPTH = u64{1} << (2 * SLOTS + 11);
  static constexpr u64 BIT_RASTER = u64{1} << (2 * SLOTS + 12);
  static constexpr u64 BIT_OUTPUT = u64{1} << (2 * SLOTS + 13);
  static_assert(2 * SLOTS + 13 < 64);

  static constexpr u64 CONSTANT_BITS = BIT_PS_CB | BIT_VS_CB | BIT_CUSTOM_CB | BIT_GS_CB;
  static constexpr u64 INPUT_BITS = BIT_VB | BIT_IB | BIT_INPUT;
  static constexpr u64 SHADER_BITS = BIT_PS | BIT_VS | BIT_GS;

  template <typename T>
  void Track(const T& bound, T& wanted, T value, u64 bit)
  {
    if (bound != value)
      m_dirty |= bit;
    wanted = value;
  }

  void ApplyOutputMerger();
  void ApplyConstants();
  void ApplyInputAssembler();
  void ApplyShaders();

  struct Bindings
  {
    std::array<ID3D11ShaderResourceView*, SLOTS> srvs;
    std::array<ID3D11SamplerState*, SLOTS> samplers;
    std::array<ID3D11Buffer*, 2> ps_cbs;
    ID3D11Buffer* vs_cb;
    ID3D11Buffer* custom_cb;
    ID3D11Buffer* gs_cb;
    ID3D11Buffer* vb;
    ID3D11Buffer* ib;
    u32 vb_stride;
    u32 vb_offset;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11InputLayout* layout;
    ID3D11PixelShader* ps;
    ID3D11VertexShader* vs;
    ID3D11GeometryShader* gs;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    ID3D11RasterizerState* raster;
    DXFramebuffer* framebuffer;
    ID3D11UnorderedAccessView* om_uav;
    bool integer_rtv;
  };

  u64 m_dirty = 0;
  Bindings m_wanted = {};  // what the next Apply should bind
  Bindings m_bound = {};   // what the context has

  // Compute stage, synced from m_wanted when a dispatch needs it.
  ID3D11Buffer* m_compute_cb = nullptr;
  std::array<ID3D11ShaderResourceView*, COMPUTE_SLOTS> m_compute_srvs{};
  std::array<ID3D11SamplerState*, COMPUTE_SLOTS> m_compute_samplers{};
  std::array<ID3D11UnorderedAccessView*, COMPUTE_SLOTS> m_compute_uavs{};
  ID3D11ComputeShader* m_compute_shader = nullptr;
};

extern std::unique_ptr<StateManager> stateman;

}  // namespace D3D

}  // namespace DX11
