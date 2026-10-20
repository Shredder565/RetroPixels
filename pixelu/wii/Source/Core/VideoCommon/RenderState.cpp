// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/RenderState.h"

#include <algorithm>

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/TextureConfig.h"

namespace
{
// Factors that read the second (dual-source) colour output's alpha.
constexpr bool ReadsSecondSource(SrcBlendFactor f)
{
  return f == SrcBlendFactor::SrcAlpha || f == SrcBlendFactor::InvSrcAlpha;
}
constexpr bool ReadsSecondSource(DstBlendFactor f)
{
  return f == DstBlendFactor::SrcAlpha || f == DstBlendFactor::InvSrcAlpha;
}

// An EFB without alpha behaves as if destination alpha were 1. The host target may still have
// an alpha channel, so factors that read it are replaced by constants.
template <typename Factor>
constexpr Factor WithoutDestAlpha(Factor f)
{
  if (f == Factor::DstAlpha)
    return Factor::One;
  if (f == Factor::InvDstAlpha)
    return Factor::Zero;
  return f;
}

// Factors for the alpha channel. For alpha, a "colour" factor means the same as the matching
// alpha factor, and expressing it as alpha lets backends use the second source's alpha with
// dual-source blending. The source factor's colour operand is the destination colour and the
// destination factor's is the source colour.
constexpr SrcBlendFactor AlphaFactor(SrcBlendFactor f)
{
  if (f == SrcBlendFactor::DstClr)
    return SrcBlendFactor::DstAlpha;
  if (f == SrcBlendFactor::InvDstClr)
    return SrcBlendFactor::InvDstAlpha;
  return f;
}
constexpr DstBlendFactor AlphaFactor(DstBlendFactor f)
{
  if (f == DstBlendFactor::SrcClr)
    return DstBlendFactor::SrcAlpha;
  if (f == DstBlendFactor::InvSrcClr)
    return DstBlendFactor::InvSrcAlpha;
  return f;
}

// Blend equation used in place of a logic op: enabled, subtract, src factor, dst factor.
struct BlendForLogicOp
{
  bool enable;
  bool subtract;
  SrcBlendFactor src;
  DstBlendFactor dst;
};

constexpr BlendForLogicOp LogicOpBlend(LogicOp op)
{
  using S = SrcBlendFactor;
  using D = DstBlendFactor;
  switch (op)
  {
  case LogicOp::And:
    return {true, false, S::DstClr, D::Zero};
  case LogicOp::AndReverse:
    return {true, true, S::One, D::InvSrcClr};
  case LogicOp::AndInverted:
    return {true, true, S::DstClr, D::One};
  case LogicOp::NoOp:
    return {true, false, S::Zero, D::One};
  case LogicOp::Xor:
  case LogicOp::Nor:
  case LogicOp::Nand:
    return {true, false, S::InvDstClr, D::InvSrcClr};
  case LogicOp::Or:
  case LogicOp::OrInverted:
    return {true, false, S::InvDstClr, D::One};
  case LogicOp::Equiv:
  case LogicOp::Invert:
    return {true, false, S::InvDstClr, D::Zero};
  case LogicOp::OrReverse:
    return {true, false, S::One, D::InvDstAlpha};
  default:
    // Clear, Copy, CopyInverted and Set: the pixel shader produces the result, written as is.
    return {false, false, S::One, D::Zero};
  }
}

WrapMode ValidWrap(WrapMode mode)
{
  // The reserved wrap value behaves like clamp on hardware.
  return mode <= WrapMode::Mirror ? mode : WrapMode::Clamp;
}

u32 AnisotropyLog2(MaxAniso aniso)
{
  switch (aniso)
  {
  case MaxAniso::Two:
    return 1;
  case MaxAniso::Four:
    return 2;
  default:
    return 0;
  }
}
}  // namespace

void RasterizationState::Generate(const BPMemory& bp, PrimitiveType primitive_type)
{
  const bool is_triangles =
      primitive_type == PrimitiveType::Triangles || primitive_type == PrimitiveType::TriangleStrip;
  cull_mode = is_triangles ? bp.genMode.cull_mode.Value() : CullMode::None;
  primitive = primitive_type;
}

void DepthState::Generate(const BPMemory& bp)
{
  test_enable = bp.zmode.test_enable.Value();
  update_enable = bp.zmode.update_enable.Value();
  func = bp.zmode.func.Value();
}

bool BlendingState::RequiresDualSrc() const
{
  if (!blend_enable || !use_dual_src)
    return false;
  return ReadsSecondSource(src_factor.Value()) || ReadsSecondSource(src_factor_alpha.Value()) ||
         ReadsSecondSource(dst_factor.Value()) || ReadsSecondSource(dst_factor_alpha.Value());
}

void BlendingState::Generate(const BPMemory& bp)
{
  hex = 0;

  const bool efb_has_alpha = bp.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;
  // When the alpha test always fails nothing is written at all.
  const bool can_write = bp.alpha_test.TestResult() != AlphaTestResult::Fail;
  const bool write_color = bp.blendmode.color_update && can_write;
  const bool write_alpha = bp.blendmode.alpha_update && efb_has_alpha && can_write;
  // Destination alpha replaces the written alpha with a constant (set in the pixel shader), so
  // the alpha channel is then copied from the source without blending.
  const bool constant_alpha = bp.dstalpha.enable && write_alpha;

  color_update = write_color;
  alpha_update = write_alpha;
  use_dual_src = true;

  if (bp.blendmode.blend_enable)
  {
    blend_enable = true;
    if (bp.blendmode.subtract)
    {
      // Subtractive mode is always dst - src with unit factors.
      subtract = true;
      src_factor = SrcBlendFactor::One;
      dst_factor = DstBlendFactor::One;
      subtract_alpha = !constant_alpha;
      src_factor_alpha = SrcBlendFactor::One;
      dst_factor_alpha = constant_alpha ? DstBlendFactor::Zero : DstBlendFactor::One;
    }
    else
    {
      SrcBlendFactor src = bp.blendmode.src_factor;
      DstBlendFactor dst = bp.blendmode.dst_factor;
      if (!efb_has_alpha)
      {
        src = WithoutDestAlpha(src);
        dst = WithoutDestAlpha(dst);
      }
      src_factor = src;
      dst_factor = dst;
      src_factor_alpha = constant_alpha ? SrcBlendFactor::One : AlphaFactor(src);
      dst_factor_alpha = constant_alpha ? DstBlendFactor::Zero : AlphaFactor(dst);
    }
  }
  else if (bp.blendmode.logic_op_enable)
  {
    if (bp.blendmode.logic_mode == LogicOp::NoOp)
    {
      // A no-op leaves colour alone; only a destination alpha write is left (some games, Kirby's
      // Return to Dreamland among them, use exactly that).
      color_update = false;
      alpha_update = constant_alpha;
    }
    else
    {
      // Destination alpha is not applied together with logic ops.
      logic_op_enable = true;
      logic_mode = bp.blendmode.logic_mode;
    }
  }

  // Channels that are not written get the identity blend (keep destination). Besides being
  // tidy, this keeps dual-source factors out of unused channels, which some drivers (Intel on
  // D3D12) mishandle.
  if (!color_update)
  {
    src_factor = SrcBlendFactor::Zero;
    dst_factor = DstBlendFactor::One;
  }
  if (!alpha_update)
  {
    src_factor_alpha = SrcBlendFactor::Zero;
    dst_factor_alpha = DstBlendFactor::One;
  }
}

void BlendingState::ApproximateLogicOpWithBlending()
{
  const BlendForLogicOp blend = LogicOpBlend(logic_mode.Value());

  logic_op_enable = false;
  use_dual_src = false;
  if (!blend.enable)
    return;

  blend_enable = true;
  subtract = blend.subtract;
  src_factor = blend.src;
  src_factor_alpha = blend.src;
  dst_factor = blend.dst;
  dst_factor_alpha = blend.dst;
}

bool BlendingState::LogicOpApproximationIsExact()
{
  // Copy is exact through the shader output alone; the others need shader help as well.
  return logic_mode.Value() == LogicOp::Copy || LogicOpApproximationWantsShaderHelp();
}

bool BlendingState::LogicOpApproximationWantsShaderHelp()
{
  // Results that do not depend on the destination: the pixel shader outputs them directly
  // (0, all ones, the inverted source), or keeps the destination (no-op).
  switch (logic_mode.Value())
  {
  case LogicOp::Clear:
  case LogicOp::Set:
  case LogicOp::NoOp:
  case LogicOp::Invert:
  case LogicOp::CopyInverted:
    return true;
  default:
    return false;
  }
}

void SamplerState::Generate(const BPMemory& bp, u32 index)
{
  // Only the fields below are written; other bits of tm0/tm1 are left as they were.
  const auto unit = bp.tex.GetUnit(index);
  const TexMode0& mode0 = unit.texMode0;
  const TexMode1& mode1 = unit.texMode1;

  tm0.min_filter = mode0.min_filter;
  tm0.mipmap_filter =
      mode0.mipmap_filter == MipMode::Linear ? FilterMode::Linear : FilterMode::Near;
  tm0.mag_filter = mode0.mag_filter;

  if (mode0.mipmap_filter == MipMode::None)
  {
    // Host samplers cannot turn mipmapping off, so the LOD range is pinned to level 0.
    tm1.max_lod = 0;
    tm1.min_lod = 0;
    tm0.lod_bias = 0;
  }
  else
  {
    // Hardware checks the maximum first, so a minimum above it is clamped down.
    const u32 max_lod = mode1.max_lod.Value();
    tm1.max_lod = max_lod;
    tm1.min_lod = std::min(max_lod, mode1.min_lod.Value());
    // GX bias is in 1/32 steps, ours in 1/256.
    tm0.lod_bias = mode0.lod_bias * 8;
  }

  tm0.wrap_u = ValidWrap(mode0.wrap_s);
  tm0.wrap_v = ValidWrap(mode0.wrap_t);
  tm0.anisotropic_filtering = AnisotropyLog2(mode0.max_aniso);
  tm0.diag_lod = mode0.diag_lod;
  tm0.lod_clamp = mode0.lod_clamp;
}

namespace RenderState
{
namespace
{
template <typename State>
State AllBitsSet()
{
  State state;
  state.hex = UINT32_C(0xFFFFFFFF);
  return state;
}

RasterizationState Rasterization(CullMode cull, PrimitiveType primitive)
{
  RasterizationState state{};
  state.cull_mode = cull;
  state.primitive = primitive;
  return state;
}

DepthState Depth(bool test, bool write)
{
  DepthState state{};
  state.test_enable = test;
  state.update_enable = write;
  state.func = CompareMode::Always;
  return state;
}

// Source replaces destination, with the given channels written.
BlendingState Replace(bool write)
{
  BlendingState state{};
  state.src_factor = SrcBlendFactor::One;
  state.src_factor_alpha = SrcBlendFactor::One;
  state.dst_factor = DstBlendFactor::Zero;
  state.dst_factor_alpha = DstBlendFactor::Zero;
  state.color_update = write;
  state.alpha_update = write;
  return state;
}

// Clamped sampler with the full LOD range and one filter for everything.
SamplerState ClampSampler(FilterMode filter)
{
  SamplerState state{};
  state.tm0.min_filter = filter;
  state.tm0.mag_filter = filter;
  state.tm0.mipmap_filter = filter;
  state.tm0.wrap_u = WrapMode::Clamp;
  state.tm0.wrap_v = WrapMode::Clamp;
  state.tm0.diag_lod = LODType::Edge;
  state.tm1.max_lod = 255;
  return state;
}
}  // namespace

RasterizationState GetInvalidRasterizationState()
{
  return AllBitsSet<RasterizationState>();
}

RasterizationState GetNoCullRasterizationState(PrimitiveType primitive)
{
  return Rasterization(CullMode::None, primitive);
}

RasterizationState GetCullBackFaceRasterizationState(PrimitiveType primitive)
{
  return Rasterization(CullMode::Back, primitive);
}

DepthState GetInvalidDepthState()
{
  return AllBitsSet<DepthState>();
}

DepthState GetNoDepthTestingDepthState()
{
  return Depth(false, false);
}

DepthState GetAlwaysWriteDepthState()
{
  return Depth(true, true);
}

BlendingState GetInvalidBlendingState()
{
  return AllBitsSet<BlendingState>();
}

BlendingState GetNoBlendingBlendState()
{
  return Replace(true);
}

BlendingState GetNoColorWriteBlendState()
{
  return Replace(false);
}

SamplerState GetInvalidSamplerState()
{
  SamplerState state;
  state.tm0.hex = 0xFFFFFFFF;
  state.tm1.hex = 0xFFFFFFFF;
  return state;
}

SamplerState GetPointSamplerState()
{
  return ClampSampler(FilterMode::Near);
}

SamplerState GetLinearSamplerState()
{
  return ClampSampler(FilterMode::Linear);
}

FramebufferState GetColorFramebufferState(AbstractTextureFormat format)
{
  FramebufferState state{};
  state.color_texture_format = format;
  state.depth_texture_format = AbstractTextureFormat::Undefined;
  state.samples = 1;
  return state;
}

FramebufferState GetRGBA8FramebufferState()
{
  return GetColorFramebufferState(AbstractTextureFormat::RGBA8);
}
}  // namespace RenderState
