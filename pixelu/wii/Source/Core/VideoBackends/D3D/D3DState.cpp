// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/D3DState.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
namespace D3D
{
std::unique_ptr<StateManager> stateman;

StateManager::StateManager() = default;
StateManager::~StateManager() = default;

void StateManager::ApplyOutputMerger()
{
  // Bound first: the runtime unbinds shader resources that alias a new render
  // target, which would undo texture bindings made before this.
  DXFramebuffer* const fb = m_wanted.framebuffer;
  const UINT color_count = fb->GetNumRTVs();
  ID3D11RenderTargetView* const* const colors =
      m_wanted.integer_rtv ? fb->GetIntegerRTVArray() : fb->GetRTVArray();

  if (g_backend_info.bSupportsBBox)
  {
    // The bounding-box UAV goes in the slot after the colour targets' range.
    D3D::context->OMSetRenderTargetsAndUnorderedAccessViews(
        color_count, colors, fb->GetDSV(), color_count + 1, 1, &m_wanted.om_uav, nullptr);
  }
  else
  {
    D3D::context->OMSetRenderTargets(color_count, colors, fb->GetDSV());
  }

  m_bound.framebuffer = fb;
  m_bound.om_uav = m_wanted.om_uav;
  m_bound.integer_rtv = m_wanted.integer_rtv;
}

void StateManager::ApplyConstants()
{
  if (m_wanted.ps_cbs != m_bound.ps_cbs)
  {
    D3D::context->PSSetConstantBuffers(0, 1, &m_wanted.ps_cbs[0]);
    D3D::context->PSSetConstantBuffers(1, 1, &m_wanted.ps_cbs[1]);
    m_bound.ps_cbs = m_wanted.ps_cbs;
  }

  // The vertex constants are visible to the vertex stage at both b0 and b1.
  if (m_wanted.vs_cb != m_bound.vs_cb)
  {
    D3D::context->VSSetConstantBuffers(0, 1, &m_wanted.vs_cb);
    D3D::context->VSSetConstantBuffers(1, 1, &m_wanted.vs_cb);
    m_bound.vs_cb = m_wanted.vs_cb;
  }

  if (m_wanted.custom_cb != m_bound.custom_cb)
  {
    D3D::context->PSSetConstantBuffers(2, 1, &m_wanted.custom_cb);
    D3D::context->VSSetConstantBuffers(2, 1, &m_wanted.custom_cb);
    m_bound.custom_cb = m_wanted.custom_cb;
  }

  if (m_wanted.gs_cb != m_bound.gs_cb)
  {
    D3D::context->GSSetConstantBuffers(0, 1, &m_wanted.gs_cb);
    m_bound.gs_cb = m_wanted.gs_cb;
  }
}

void StateManager::ApplyInputAssembler()
{
  if (m_wanted.vb != m_bound.vb || m_wanted.vb_stride != m_bound.vb_stride ||
      m_wanted.vb_offset != m_bound.vb_offset)
  {
    D3D::context->IASetVertexBuffers(0, 1, &m_wanted.vb, &m_wanted.vb_stride, &m_wanted.vb_offset);
    m_bound.vb = m_wanted.vb;
    m_bound.vb_stride = m_wanted.vb_stride;
    m_bound.vb_offset = m_wanted.vb_offset;
  }

  // Indices are always 16-bit.
  if (m_wanted.ib != m_bound.ib)
  {
    D3D::context->IASetIndexBuffer(m_wanted.ib, DXGI_FORMAT_R16_UINT, 0);
    m_bound.ib = m_wanted.ib;
  }

  if (m_wanted.topology != m_bound.topology)
  {
    D3D::context->IASetPrimitiveTopology(m_wanted.topology);
    m_bound.topology = m_wanted.topology;
  }

  if (m_wanted.layout != m_bound.layout)
  {
    D3D::context->IASetInputLayout(m_wanted.layout);
    m_bound.layout = m_wanted.layout;
  }
}

void StateManager::ApplyShaders()
{
  if (m_wanted.ps != m_bound.ps)
  {
    D3D::context->PSSetShader(m_wanted.ps, nullptr, 0);
    m_bound.ps = m_wanted.ps;
  }
  if (m_wanted.vs != m_bound.vs)
  {
    D3D::context->VSSetShader(m_wanted.vs, nullptr, 0);
    m_bound.vs = m_wanted.vs;
  }
  if (m_wanted.gs != m_bound.gs)
  {
    D3D::context->GSSetShader(m_wanted.gs, nullptr, 0);
    m_bound.gs = m_wanted.gs;
  }
}

void StateManager::Apply()
{
  const u64 dirty = m_dirty;
  if (dirty == 0)
    return;

  if (dirty & BIT_OUTPUT)
    ApplyOutputMerger();
  if (dirty & CONSTANT_BITS)
    ApplyConstants();
  if (dirty & INPUT_BITS)
    ApplyInputAssembler();
  if (dirty & SHADER_BITS)
    ApplyShaders();

  // Fixed-function state objects are rebound whenever they were touched,
  // even if they were set back to the bound object afterwards.
  if (dirty & BIT_BLEND)
  {
    D3D::context->OMSetBlendState(m_wanted.blend, nullptr, 0xFFFFFFFF);
    m_bound.blend = m_wanted.blend;
  }
  if (dirty & BIT_DEPTH)
  {
    D3D::context->OMSetDepthStencilState(m_wanted.depth, 0);
    m_bound.depth = m_wanted.depth;
  }
  if (dirty & BIT_RASTER)
  {
    D3D::context->RSSetState(m_wanted.raster);
    m_bound.raster = m_wanted.raster;
  }

  ApplyTextures();
  m_dirty = 0;
}

void StateManager::ApplyTextures()
{
  for (u32 slot = 0; slot < SLOTS; ++slot)
  {
    const u64 bit = TextureBit(slot);
    if ((m_dirty & bit) == 0)
      continue;
    m_dirty &= ~bit;
    if (m_wanted.srvs[slot] == m_bound.srvs[slot])
      continue;
    D3D::context->PSSetShaderResources(slot, 1, &m_wanted.srvs[slot]);
    m_bound.srvs[slot] = m_wanted.srvs[slot];
  }

  for (u32 slot = 0; slot < SLOTS; ++slot)
  {
    const u64 bit = SamplerBit(slot);
    if ((m_dirty & bit) == 0)
      continue;
    m_dirty &= ~bit;
    if (m_wanted.samplers[slot] == m_bound.samplers[slot])
      continue;
    D3D::context->PSSetSamplers(slot, 1, &m_wanted.samplers[slot]);
    m_bound.samplers[slot] = m_wanted.samplers[slot];
  }
}

u32 StateManager::UnsetTexture(ID3D11ShaderResourceView* srv)
{
  // Matches against what the context has bound, not what is pending.
  u32 cleared = 0;
  for (u32 slot = 0; slot < SLOTS; ++slot)
  {
    if (m_bound.srvs[slot] != srv)
      continue;
    SetTexture(slot, nullptr);
    cleared |= 1u << slot;
  }
  return cleared;
}

void StateManager::SetTextureByMask(u32 textureSlotMask, ID3D11ShaderResourceView* srv)
{
  for (u32 rest = textureSlotMask; rest != 0; rest &= rest - 1)
    SetTexture(static_cast<size_t>(std::countr_zero(rest)), srv);
}

void StateManager::SetComputeUAV(u32 index, ID3D11UnorderedAccessView* uav)
{
  if (m_compute_uavs[index] == uav)
    return;

  // All image slots are rebound together.
  m_compute_uavs[index] = uav;
  D3D::context->CSSetUnorderedAccessViews(0, COMPUTE_SLOTS, m_compute_uavs.data(), nullptr);
}

void StateManager::SetComputeShader(ID3D11ComputeShader* shader)
{
  if (m_compute_shader == shader)
    return;

  m_compute_shader = shader;
  D3D::context->CSSetShader(shader, nullptr, 0);
}

namespace
{
// Copies source into target for the first COMPUTE_SLOTS entries, issuing one
// bind call per run of consecutive changed slots.
template <typename T, size_t N, typename BindFn>
void SyncRuns(std::array<T*, N>& target, T* const* source, BindFn&& bind)
{
  u32 slot = 0;
  while (slot < N)
  {
    if (target[slot] == source[slot])
    {
      ++slot;
      continue;
    }

    const u32 first = slot;
    while (slot < N && target[slot] != source[slot])
    {
      target[slot] = source[slot];
      ++slot;
    }
    bind(first, slot - first, &target[first]);
  }
}
}  // namespace

void StateManager::SyncComputeBindings()
{
  // The compute stage takes the pixel stage's b0, textures and samplers.
  if (m_compute_cb != m_wanted.ps_cbs[0])
  {
    m_compute_cb = m_wanted.ps_cbs[0];
    D3D::context->CSSetConstantBuffers(0, 1, &m_compute_cb);
  }

  SyncRuns(m_compute_srvs, m_wanted.srvs.data(),
           [](u32 first, u32 count, ID3D11ShaderResourceView* const* views) {
             D3D::context->CSSetShaderResources(first, count, views);
           });
  SyncRuns(m_compute_samplers, m_wanted.samplers.data(),
           [](u32 first, u32 count, ID3D11SamplerState* const* samplers) {
             D3D::context->CSSetSamplers(first, count, samplers);
           });
}
}  // namespace D3D

namespace
{
// The SDK's default sampler (CD3D11_SAMPLER_DESC(CD3D11_DEFAULT())), field by field.
D3D11_SAMPLER_DESC DefaultSamplerDesc()
{
  D3D11_SAMPLER_DESC desc;
  desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.MipLODBias = 0.0f;
  desc.MaxAnisotropy = 1;
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  desc.BorderColor[0] = 1.0f;
  desc.BorderColor[1] = 1.0f;
  desc.BorderColor[2] = 1.0f;
  desc.BorderColor[3] = 1.0f;
  desc.MinLOD = -FLT_MAX;
  desc.MaxLOD = FLT_MAX;
  return desc;
}

// The SDK's default depth-stencil description: stencil faces keep everything
// and always pass.
D3D11_DEPTH_STENCIL_DESC DefaultDepthStencilDesc()
{
  D3D11_DEPTH_STENCILOP_DESC face;
  face.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  face.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  face.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  face.StencilFunc = D3D11_COMPARISON_ALWAYS;

  D3D11_DEPTH_STENCIL_DESC desc;
  desc.DepthEnable = TRUE;
  desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  desc.DepthFunc = D3D11_COMPARISON_LESS;
  desc.StencilEnable = FALSE;
  desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
  desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
  desc.FrontFace = face;
  desc.BackFace = face;
  return desc;
}

// Filter for (min, mag, mip) linear bits: index = min*4 + mag*2 + mip.
constexpr std::array<D3D11_FILTER, 8> FILTERS = {
    D3D11_FILTER_MIN_MAG_MIP_POINT,                // point point point
    D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR,         // point point linear
    D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,   // point linear point
    D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR,         // point linear linear
    D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT,         // linear point point
    D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,  // linear point linear
    D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,         // linear linear point
    D3D11_FILTER_MIN_MAG_MIP_LINEAR,               // linear linear linear
};

// Indexed by WrapMode. The invalid value 3 behaves like clamp on hardware.
constexpr std::array<D3D11_TEXTURE_ADDRESS_MODE, 4> ADDRESS_MODES = {
    D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR,
    D3D11_TEXTURE_ADDRESS_CLAMP};

constexpr std::array<D3D11_LOGIC_OP, 16> LOGIC_OPS = {
    D3D11_LOGIC_OP_CLEAR,         D3D11_LOGIC_OP_AND,          D3D11_LOGIC_OP_AND_REVERSE,
    D3D11_LOGIC_OP_COPY,          D3D11_LOGIC_OP_AND_INVERTED, D3D11_LOGIC_OP_NOOP,
    D3D11_LOGIC_OP_XOR,           D3D11_LOGIC_OP_OR,           D3D11_LOGIC_OP_NOR,
    D3D11_LOGIC_OP_EQUIV,         D3D11_LOGIC_OP_INVERT,       D3D11_LOGIC_OP_OR_REVERSE,
    D3D11_LOGIC_OP_COPY_INVERTED, D3D11_LOGIC_OP_OR_INVERTED,  D3D11_LOGIC_OP_NAND,
    D3D11_LOGIC_OP_SET,
};

// Cull mode by CullMode; the "all" setting is handled by the vertex path and
// culls back faces here.
constexpr std::array<D3D11_CULL_MODE, 4> CULL_MODES = {D3D11_CULL_NONE, D3D11_CULL_BACK,
                                                       D3D11_CULL_FRONT, D3D11_CULL_BACK};

// By CompareMode. The depth range is reversed, so less and greater trade places.
constexpr std::array<D3D11_COMPARISON_FUNC, 8> DEPTH_COMPARES = {
    D3D11_COMPARISON_NEVER,         D3D11_COMPARISON_GREATER, D3D11_COMPARISON_EQUAL,
    D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_LESS,    D3D11_COMPARISON_NOT_EQUAL,
    D3D11_COMPARISON_LESS_EQUAL,    D3D11_COMPARISON_ALWAYS,
};

constexpr std::array<D3D11_PRIMITIVE_TOPOLOGY, 4> TOPOLOGIES = {
    D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP};

UINT8 WriteMaskFor(const BlendingState& state)
{
  UINT8 mask = 0;
  if (state.color_update)
  {
    mask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
           D3D11_COLOR_WRITE_ENABLE_BLUE;
  }
  if (state.alpha_update)
    mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
  return mask;
}

// Blend factor by SrcBlendFactor/DstBlendFactor. Only entries 2 and 3 differ
// between source and destination; the alpha factors use the second colour
// output when dual-source blending is on.
D3D11_BLEND BlendFactorFor(u32 factor, bool is_source, bool dual_source)
{
  switch (factor)
  {
  case 0:
    return D3D11_BLEND_ZERO;
  case 1:
    return D3D11_BLEND_ONE;
  case 2:
    return is_source ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR;
  case 3:
    return is_source ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
  case 4:
    return dual_source ? D3D11_BLEND_SRC1_ALPHA : D3D11_BLEND_SRC_ALPHA;
  case 5:
    return dual_source ? D3D11_BLEND_INV_SRC1_ALPHA : D3D11_BLEND_INV_SRC_ALPHA;
  case 6:
    return D3D11_BLEND_DEST_ALPHA;
  default:
    return D3D11_BLEND_INV_DEST_ALPHA;
  }
}

D3D11_BLEND_OP BlendOpFor(bool subtract)
{
  return subtract ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_ADD;
}
}  // namespace

StateCache::~StateCache() = default;

ID3D11SamplerState* StateCache::Get(SamplerState state)
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (const auto found = m_sampler.find(state); found != m_sampler.end())
    return found->second.Get();

  D3D11_SAMPLER_DESC desc = DefaultSamplerDesc();
  const u32 linear_min = state.tm0.min_filter == FilterMode::Linear ? 1 : 0;
  const u32 linear_mag = state.tm0.mag_filter == FilterMode::Linear ? 1 : 0;
  const u32 linear_mip = state.tm0.mipmap_filter == FilterMode::Linear ? 1 : 0;
  desc.Filter = FILTERS[linear_min * 4 + linear_mag * 2 + linear_mip];
  desc.AddressU = ADDRESS_MODES[static_cast<u32>(state.tm0.wrap_u.Value())];
  desc.AddressV = ADDRESS_MODES[static_cast<u32>(state.tm0.wrap_v.Value())];
  // LODs are in 1/16 steps, the bias in 1/256 steps.
  desc.MaxLOD = static_cast<float>(state.tm1.max_lod.Value()) / 16.f;
  desc.MinLOD = static_cast<float>(state.tm1.min_lod.Value()) / 16.f;
  desc.MipLODBias = static_cast<float>(state.tm0.lod_bias.Value()) / 256.f;

  if (const u32 aniso_log2 = state.tm0.anisotropic_filtering; aniso_log2 != 0)
  {
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << aniso_log2;
  }

  ComPtr<ID3D11SamplerState> created;
  const HRESULT result = D3D::device->CreateSamplerState(&desc, &created);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Creating D3D sampler state failed: {}", DX11HRWrap(result));
  return m_sampler.emplace(state, std::move(created)).first->second.Get();
}

ID3D11BlendState* StateCache::Get(BlendingState state)
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (const auto found = m_blend.find(state.hex); found != m_blend.end())
    return found->second.Get();

  // Logic ops need the 11.1 blend description. If the driver refuses one,
  // the ordinary blend state below is used instead.
  if (state.logic_op_enable && g_backend_info.bSupportsLogicOp)
  {
    D3D11_BLEND_DESC1 desc1 = {};
    D3D11_RENDER_TARGET_BLEND_DESC1& target = desc1.RenderTarget[0];
    target.RenderTargetWriteMask = WriteMaskFor(state);
    target.LogicOpEnable = TRUE;
    target.LogicOp = LOGIC_OPS[static_cast<u32>(state.logic_mode.Value())];

    ComPtr<ID3D11BlendState1> created1;
    const HRESULT result = D3D::device1->CreateBlendState1(&desc1, &created1);
    if (SUCCEEDED(result))
      return m_blend.emplace(state.hex, std::move(created1)).first->second.Get();
    WARN_LOG_FMT(VIDEO, "Creating D3D blend state failed with an error: {}", DX11HRWrap(result));
  }

  const bool dual_source = state.use_dual_src;
  D3D11_BLEND_DESC desc = {};
  desc.AlphaToCoverageEnable = FALSE;
  desc.IndependentBlendEnable = FALSE;
  D3D11_RENDER_TARGET_BLEND_DESC& target = desc.RenderTarget[0];
  target.BlendEnable = state.blend_enable ? TRUE : FALSE;
  target.RenderTargetWriteMask = WriteMaskFor(state);
  target.SrcBlend = BlendFactorFor(static_cast<u32>(state.src_factor.Value()), true, dual_source);
  target.SrcBlendAlpha =
      BlendFactorFor(static_cast<u32>(state.src_factor_alpha.Value()), true, dual_source);
  target.DestBlend = BlendFactorFor(static_cast<u32>(state.dst_factor.Value()), false, dual_source);
  target.DestBlendAlpha =
      BlendFactorFor(static_cast<u32>(state.dst_factor_alpha.Value()), false, dual_source);
  target.BlendOp = BlendOpFor(state.subtract);
  target.BlendOpAlpha = BlendOpFor(state.subtract_alpha);

  ComPtr<ID3D11BlendState> created;
  const HRESULT result = D3D::device->CreateBlendState(&desc, &created);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Creating D3D blend state failed: {}", DX11HRWrap(result));
  return m_blend.emplace(state.hex, std::move(created)).first->second.Get();
}

ID3D11RasterizerState* StateCache::Get(RasterizationState state)
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (const auto found = m_raster.find(state.hex); found != m_raster.end())
    return found->second.Get();

  D3D11_RASTERIZER_DESC desc = {};
  desc.FillMode = D3D11_FILL_SOLID;
  desc.CullMode = CULL_MODES[static_cast<u32>(state.cull_mode.Value())];
  desc.ScissorEnable = TRUE;

  ComPtr<ID3D11RasterizerState> created;
  const HRESULT result = D3D::device->CreateRasterizerState(&desc, &created);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Creating D3D rasterizer state failed: {}",
             DX11HRWrap(result));
  return m_raster.emplace(state.hex, std::move(created)).first->second.Get();
}

ID3D11DepthStencilState* StateCache::Get(DepthState state)
{
  std::lock_guard<std::mutex> lock(m_lock);
  if (const auto found = m_depth.find(state.hex); found != m_depth.end())
    return found->second.Get();

  D3D11_DEPTH_STENCIL_DESC desc = DefaultDepthStencilDesc();
  desc.DepthFunc = D3D11_COMPARISON_GREATER;
  if (state.test_enable)
  {
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask =
        state.update_enable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = DEPTH_COMPARES[static_cast<u32>(state.func.Value())];
  }
  else
  {
    // No test means no writes either; the compare function is left at GREATER.
    desc.DepthEnable = FALSE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  }

  ComPtr<ID3D11DepthStencilState> created;
  const HRESULT result = D3D::device->CreateDepthStencilState(&desc, &created);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Creating D3D depth stencil state failed: {}",
             DX11HRWrap(result));
  return m_depth.emplace(state.hex, std::move(created)).first->second.Get();
}

D3D11_PRIMITIVE_TOPOLOGY StateCache::GetPrimitiveTopology(PrimitiveType primitive)
{
  return TOPOLOGIES[static_cast<u32>(primitive)];
}

}  // namespace DX11
