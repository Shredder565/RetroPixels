// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/DX12Pipeline.h"

#include <cstring>

#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DX12Shader.h"
#include "VideoBackends/D3D12/DX12Texture.h"
#include "VideoBackends/D3D12/DX12VertexFormat.h"

namespace DX12
{
namespace
{
struct TopologyPair
{
  D3D12_PRIMITIVE_TOPOLOGY topology;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
};

// Strips (and anything unexpected) draw as triangle strips.
TopologyPair TopologyFor(PrimitiveType primitive)
{
  switch (primitive)
  {
  case PrimitiveType::Points:
    return {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT};
  case PrimitiveType::Lines:
    return {D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE};
  case PrimitiveType::Triangles:
    return {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
  default:
    return {D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
  }
}

// Only fill, cull and multisampling are set; the remaining fields keep their
// zero values (no depth clip, clockwise front faces).
void FillRasterizer(D3D12_RASTERIZER_DESC& out, const RasterizationState& raster, u32 samples)
{
  // "Cull everything" is handled before drawing, so it falls back to front.
  constexpr D3D12_CULL_MODE CULL[4] = {D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK,
                                       D3D12_CULL_MODE_FRONT, D3D12_CULL_MODE_FRONT};
  out.FillMode = D3D12_FILL_MODE_SOLID;
  out.CullMode = CULL[static_cast<u32>(raster.cull_mode.Value())];
  out.MultisampleEnable = samples > 1 ? TRUE : FALSE;
}

// Stencil stays disabled.
void FillDepth(D3D12_DEPTH_STENCIL_DESC& out, const DepthState& depth)
{
  // Depth is stored reversed, so each ordered comparison is mirrored.
  constexpr D3D12_COMPARISON_FUNC COMPARE[8] = {
      D3D12_COMPARISON_FUNC_NEVER,      D3D12_COMPARISON_FUNC_GREATER,
      D3D12_COMPARISON_FUNC_EQUAL,      D3D12_COMPARISON_FUNC_GREATER_EQUAL,
      D3D12_COMPARISON_FUNC_LESS,       D3D12_COMPARISON_FUNC_NOT_EQUAL,
      D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
  out.DepthEnable = depth.test_enable ? TRUE : FALSE;
  out.DepthFunc = COMPARE[static_cast<u32>(depth.func.Value())];
  out.DepthWriteMask =
      depth.update_enable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
}

// GX blend factor codes 0-7. Code 2/3 means the other operand's colour, and
// the source alpha codes read the second output under dual-source blending.
D3D12_BLEND FactorFor(u32 code, bool for_source, bool dual_source)
{
  switch (code)
  {
  case 0:
    return D3D12_BLEND_ZERO;
  case 1:
    return D3D12_BLEND_ONE;
  case 2:
    return for_source ? D3D12_BLEND_DEST_COLOR : D3D12_BLEND_SRC_COLOR;
  case 3:
    return for_source ? D3D12_BLEND_INV_DEST_COLOR : D3D12_BLEND_INV_SRC_COLOR;
  case 4:
    return dual_source ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC_ALPHA;
  case 5:
    return dual_source ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC_ALPHA;
  case 6:
    return D3D12_BLEND_DEST_ALPHA;
  default:
    return D3D12_BLEND_INV_DEST_ALPHA;
  }
}

constexpr D3D12_LOGIC_OP LOGIC_OPS[16] = {D3D12_LOGIC_OP_CLEAR,         D3D12_LOGIC_OP_AND,
                                          D3D12_LOGIC_OP_AND_REVERSE,   D3D12_LOGIC_OP_COPY,
                                          D3D12_LOGIC_OP_AND_INVERTED,  D3D12_LOGIC_OP_NOOP,
                                          D3D12_LOGIC_OP_XOR,           D3D12_LOGIC_OP_OR,
                                          D3D12_LOGIC_OP_NOR,           D3D12_LOGIC_OP_EQUIV,
                                          D3D12_LOGIC_OP_INVERT,        D3D12_LOGIC_OP_OR_REVERSE,
                                          D3D12_LOGIC_OP_COPY_INVERTED, D3D12_LOGIC_OP_OR_INVERTED,
                                          D3D12_LOGIC_OP_NAND,          D3D12_LOGIC_OP_SET};

// The same blend for every target in use. Blending wins over a logic op;
// fields that do not apply stay zero.
void FillBlend(D3D12_BLEND_DESC& out, const BlendingState& blend, u32 targets)
{
  out.AlphaToCoverageEnable = FALSE;
  out.IndependentBlendEnable = FALSE;

  UINT8 write_mask = 0;
  if (blend.color_update)
  {
    write_mask |= D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                  D3D12_COLOR_WRITE_ENABLE_BLUE;
  }
  if (blend.alpha_update)
    write_mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;

  const bool dual = blend.use_dual_src;
  for (u32 t = 0; t < targets; ++t)
  {
    D3D12_RENDER_TARGET_BLEND_DESC& target = out.RenderTarget[t];
    target.RenderTargetWriteMask = write_mask;
    target.BlendEnable = blend.blend_enable ? TRUE : FALSE;
    if (blend.blend_enable)
    {
      target.BlendOp = blend.subtract ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
      target.BlendOpAlpha = blend.subtract_alpha ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
      target.SrcBlend = FactorFor(static_cast<u32>(blend.src_factor.Value()), true, dual);
      target.SrcBlendAlpha =
          FactorFor(static_cast<u32>(blend.src_factor_alpha.Value()), true, dual);
      target.DestBlend = FactorFor(static_cast<u32>(blend.dst_factor.Value()), false, dual);
      target.DestBlendAlpha =
          FactorFor(static_cast<u32>(blend.dst_factor_alpha.Value()), false, dual);
      continue;
    }

    target.LogicOpEnable = blend.logic_op_enable ? TRUE : FALSE;
    if (blend.logic_op_enable)
      target.LogicOp = LOGIC_OPS[static_cast<u32>(blend.logic_mode.Value())];
  }
}

D3D12_SHADER_BYTECODE CodeOf(const AbstractShader* shader)
{
  return static_cast<const DXShader*>(shader)->GetD3DByteCode();
}
}  // namespace

DXPipeline::DXPipeline(const AbstractPipelineConfig& config, ID3D12PipelineState* pipeline,
                       ID3D12RootSignature* root_signature, AbstractPipelineUsage usage,
                       D3D12_PRIMITIVE_TOPOLOGY primitive_topology, bool use_integer_rtv)
    : AbstractPipeline(config), m_pipeline(pipeline), m_root_signature(root_signature),
      m_usage(usage), m_primitive_topology(primitive_topology), m_use_integer_rtv(use_integer_rtv)
{
}

DXPipeline::~DXPipeline()
{
  m_pipeline->Release();
}

std::unique_ptr<DXPipeline> DXPipeline::Create(const AbstractPipelineConfig& config,
                                               const void* cache_data, size_t cache_data_size)
{
  DEBUG_ASSERT(config.vertex_shader && config.pixel_shader);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  if (config.usage == AbstractPipelineUsage::GX || config.usage == AbstractPipelineUsage::GXUber)
  {
    desc.pRootSignature = g_dx_context->GetGXRootSignature();
  }
  else if (config.usage == AbstractPipelineUsage::Utility)
  {
    desc.pRootSignature = g_dx_context->GetUtilityRootSignature();
  }
  else
  {
    PanicAlertFmt("Unknown pipeline layout.");
    return nullptr;
  }

  if (config.vertex_shader != nullptr)
    desc.VS = CodeOf(config.vertex_shader);
  if (config.geometry_shader != nullptr)
    desc.GS = CodeOf(config.geometry_shader);
  if (config.pixel_shader != nullptr)
    desc.PS = CodeOf(config.pixel_shader);

  const FramebufferState& fb = config.framebuffer_state;
  const u32 extra_targets = static_cast<u8>(fb.additional_color_attachment_count);
  FillBlend(desc.BlendState, config.blending_state, extra_targets + 1);
  desc.SampleMask = UINT32_MAX;
  FillRasterizer(desc.RasterizerState, config.rasterization_state, fb.samples);
  FillDepth(desc.DepthStencilState, config.depth_state);
  if (config.vertex_format != nullptr)
    static_cast<const DXVertexFormat*>(config.vertex_format)->GetInputLayoutDesc(&desc.InputLayout);

  // Index 0xFFFF restarts a strip.
  const bool strips = config.rasterization_state.primitive == PrimitiveType::TriangleStrip;
  desc.IBStripCutValue = strips ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF :
                                  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  const TopologyPair topology = TopologyFor(config.rasterization_state.primitive);
  desc.PrimitiveTopologyType = topology.type;

  if (fb.color_texture_format != AbstractTextureFormat::Undefined)
  {
    // All colour targets share one format; logic ops need the integer one.
    const DXGI_FORMAT color = D3DCommon::GetRTVFormatForAbstractFormat(
        fb.color_texture_format, config.blending_state.logic_op_enable);
    desc.NumRenderTargets = extra_targets + 1;
    for (u32 t = 0; t <= extra_targets; ++t)
      desc.RTVFormats[t] = color;
  }
  if (fb.depth_texture_format != AbstractTextureFormat::Undefined)
    desc.DSVFormat = D3DCommon::GetDSVFormatForAbstractFormat(fb.depth_texture_format);
  desc.SampleDesc.Count = fb.samples;
  desc.NodeMask = 1;
  desc.CachedPSO.pCachedBlob = cache_data;
  desc.CachedPSO.CachedBlobSizeInBytes = cache_data_size;

  ID3D12PipelineState* state;
  const HRESULT hr =
      g_dx_context->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
  if (FAILED(hr))
  {
    // A stale cache blob is a normal reason to fail; the caller retries without it.
    WARN_LOG_FMT(VIDEO, "CreateGraphicsPipelineState() {}failed: {}",
                 cache_data != nullptr ? "with cache data " : "", DX12HRWrap(hr));
    return nullptr;
  }

  const bool integer_output =
      config.blending_state.logic_op_enable && !config.blending_state.blend_enable;
  return std::make_unique<DXPipeline>(config, state, desc.pRootSignature, config.usage,
                                      topology.topology, integer_output);
}

AbstractPipeline::CacheData DXPipeline::GetCacheData() const
{
  ComPtr<ID3DBlob> blob;
  const HRESULT hr = m_pipeline->GetCachedBlob(&blob);
  if (FAILED(hr))
  {
    WARN_LOG_FMT(VIDEO, "ID3D12Pipeline::GetCachedBlob() failed: {}", DX12HRWrap(hr));
    return {};
  }

  // The size is queried again for the copy, as the blob has always been read.
  CacheData data(blob->GetBufferSize());
  std::memcpy(data.data(), blob->GetBufferPointer(), blob->GetBufferSize());
  return data;
}
}  // namespace DX12
