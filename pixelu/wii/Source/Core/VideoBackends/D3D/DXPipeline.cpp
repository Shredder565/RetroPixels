// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/DXPipeline.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DVertexManager.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
DXPipeline::DXPipeline(const AbstractPipelineConfig& config, ID3D11InputLayout* input_layout,
                       ID3D11VertexShader* vertex_shader, ID3D11GeometryShader* geometry_shader,
                       ID3D11PixelShader* pixel_shader, ID3D11RasterizerState* rasterizer_state,
                       ID3D11DepthStencilState* depth_state, ID3D11BlendState* blend_state,
                       D3D11_PRIMITIVE_TOPOLOGY primitive_topology, bool use_logic_op)
    : AbstractPipeline(config), m_input_layout(input_layout), m_vertex_shader(vertex_shader),
      m_geometry_shader(geometry_shader), m_pixel_shader(pixel_shader),
      m_rasterizer_state(rasterizer_state), m_depth_state(depth_state), m_blend_state(blend_state),
      m_primitive_topology(primitive_topology), m_use_logic_op(use_logic_op)
{
}

DXPipeline::~DXPipeline() = default;

std::unique_ptr<DXPipeline> DXPipeline::Create(const AbstractPipelineConfig& config)
{
  // State objects come from the renderer's cache, in this order.
  StateCache& cache = static_cast<Gfx*>(g_gfx.get())->GetStateCache();
  ID3D11RasterizerState* const raster = cache.Get(config.rasterization_state);
  ID3D11DepthStencilState* const depth = cache.Get(config.depth_state);
  ID3D11BlendState* const blend = cache.Get(config.blending_state);
  const D3D11_PRIMITIVE_TOPOLOGY topology =
      StateCache::GetPrimitiveTopology(config.rasterization_state.primitive);
  if (raster == nullptr || depth == nullptr || blend == nullptr)
    return nullptr;

  const auto* const vs = static_cast<const DXShader*>(config.vertex_shader);
  const auto* const gs = static_cast<const DXShader*>(config.geometry_shader);
  const auto* const ps = static_cast<const DXShader*>(config.pixel_shader);
  ASSERT(vs != nullptr && ps != nullptr);

  // The input layout is validated against the vertex shader's signature.
  ID3D11InputLayout* layout = nullptr;
  if (config.vertex_format)
  {
    auto* const format =
        const_cast<D3DVertexFormat*>(static_cast<const D3DVertexFormat*>(config.vertex_format));
    const auto& code = vs->GetByteCode();
    layout = format->GetInputLayout(code.data(), code.size());
  }

  // Logic ops are drawn through the integer view, when the device has them.
  const bool logic_op = config.blending_state.logic_op_enable && g_backend_info.bSupportsLogicOp;

  return std::make_unique<DXPipeline>(
      config, layout, vs->GetD3DVertexShader(), gs ? gs->GetD3DGeometryShader() : nullptr,
      ps->GetD3DPixelShader(), raster, depth, blend, topology, logic_op);
}
}  // namespace DX11
