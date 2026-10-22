// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/BitField.h"
#include "Common/CommonTypes.h"

struct BPMemory;

enum class AbstractTextureFormat : u32;

enum class CompareMode : u32;
enum class CullMode : u32;
enum class DstBlendFactor : u32;
enum class FilterMode : u32;
enum class LODType : u32;
enum class LogicOp : u32;
enum class PixelFormat : u32;
enum class SrcBlendFactor : u32;
enum class WrapMode : u32;

enum class PrimitiveType : u32
{
  Points,
  Lines,
  Triangles,
  TriangleStrip,
};

// The fixed-function states of a host pipeline, each packed into one 32-bit word so they can be
// compared, hashed and stored cheaply. Copies only transfer `hex`.

union RasterizationState
{
  // From the GX generation mode; culling is off for points and lines.
  void Generate(const BPMemory& bp, PrimitiveType primitive_type);

  RasterizationState() = default;
  RasterizationState(const RasterizationState&) = default;
  RasterizationState& operator=(const RasterizationState& rhs)
  {
    hex = rhs.hex;
    return *this;
  }
  RasterizationState(RasterizationState&&) = default;
  RasterizationState& operator=(RasterizationState&& rhs)
  {
    hex = rhs.hex;
    return *this;
  }

  bool operator==(const RasterizationState& rhs) const { return hex == rhs.hex; }
  bool operator<(const RasterizationState& rhs) const { return hex < rhs.hex; }

  BitField<0, 2, CullMode> cull_mode;
  BitField<3, 2, PrimitiveType> primitive;

  u32 hex = 0;
};

union FramebufferState
{
  FramebufferState() = default;
  FramebufferState(const FramebufferState&) = default;
  FramebufferState& operator=(const FramebufferState& rhs)
  {
    hex = rhs.hex;
    return *this;
  }
  FramebufferState(FramebufferState&&) = default;
  FramebufferState& operator=(FramebufferState&& rhs)
  {
    hex = rhs.hex;
    return *this;
  }

  bool operator==(const FramebufferState& rhs) const { return hex == rhs.hex; }

  BitField<0, 8, AbstractTextureFormat> color_texture_format;
  BitField<8, 8, AbstractTextureFormat> depth_texture_format;
  BitField<16, 8, u32> samples;
  BitField<24, 1, u32> per_sample_shading;

  // Extra colour targets, all in color_texture_format.
  BitField<25, 3, u32> additional_color_attachment_count;

  u32 hex = 0;
};

union DepthState
{
  // From the GX Z mode register.
  void Generate(const BPMemory& bp);

  DepthState() = default;
  DepthState(const DepthState&) = default;
  DepthState& operator=(const DepthState& rhs)
  {
    hex = rhs.hex;
    return *this;
  }
  DepthState(DepthState&&) = default;
  DepthState& operator=(DepthState&& rhs)
  {
    hex = rhs.hex;
    return *this;
  }

  bool operator==(const DepthState& rhs) const { return hex == rhs.hex; }
  bool operator<(const DepthState& rhs) const { return hex < rhs.hex; }

  BitField<0, 1, u32> test_enable;
  BitField<1, 1, u32> update_enable;
  BitField<2, 3, CompareMode> func;

  u32 hex = 0;
};

union BlendingState
{
  // From the GX blend mode, alpha test, destination alpha and EFB format.
  void Generate(const BPMemory& bp);

  // Replaces the logic op with a blend equation that comes close for most operations. The
  // result is not exact, and for some operations not close at all.
  void ApproximateLogicOpWithBlending();
  bool LogicOpApproximationIsExact();
  bool LogicOpApproximationWantsShaderHelp();

  BlendingState() = default;
  BlendingState(const BlendingState&) = default;
  BlendingState& operator=(const BlendingState& rhs)
  {
    hex = rhs.hex;
    return *this;
  }
  BlendingState(BlendingState&&) = default;
  BlendingState& operator=(BlendingState&& rhs)
  {
    hex = rhs.hex;
    return *this;
  }

  bool operator==(const BlendingState& rhs) const { return hex == rhs.hex; }
  bool operator<(const BlendingState& rhs) const { return hex < rhs.hex; }

  BitField<0, 1, u32> blend_enable;
  BitField<1, 1, u32> logic_op_enable;
  BitField<3, 1, u32> color_update;
  BitField<4, 1, u32> alpha_update;
  BitField<5, 1, u32> subtract;
  BitField<6, 1, u32> subtract_alpha;
  BitField<7, 1, u32> use_dual_src;
  BitField<8, 3, DstBlendFactor> dst_factor;
  BitField<11, 3, SrcBlendFactor> src_factor;
  BitField<14, 3, DstBlendFactor> dst_factor_alpha;
  BitField<17, 3, SrcBlendFactor> src_factor_alpha;
  BitField<20, 4, LogicOp> logic_mode;

  // True when blending is on with dual-source output and a factor reads the second source.
  bool RequiresDualSrc() const;

  u32 hex = 0;
};

struct SamplerState
{
  // From texture unit `index`'s TexMode0/TexMode1 registers.
  void Generate(const BPMemory& bp, u32 index);

  SamplerState() = default;
  SamplerState(const SamplerState&) = default;
  SamplerState& operator=(const SamplerState& rhs)
  {
    tm0.hex = rhs.tm0.hex;
    tm1.hex = rhs.tm1.hex;
    return *this;
  }
  SamplerState(SamplerState&&) = default;
  SamplerState& operator=(SamplerState&& rhs)
  {
    tm0.hex = rhs.tm0.hex;
    tm1.hex = rhs.tm1.hex;
    return *this;
  }

  bool operator==(const SamplerState& rhs) const { return Hex() == rhs.Hex(); }
  bool operator<(const SamplerState& rhs) const { return Hex() < rhs.Hex(); }

  constexpr u64 Hex() const { return tm0.hex | (static_cast<u64>(tm1.hex) << 32); }

  // Host form of TexMode0/TexMode1, with finer LOD units.
  union TM0
  {
    // A GX mip filter of "none" is expressed by min_lod = max_lod = 0 instead.
    BitField<0, 1, FilterMode> min_filter;
    BitField<1, 1, FilterMode> mag_filter;
    BitField<2, 1, FilterMode> mipmap_filter;
    // Always a valid mode (the reserved value 3 is mapped to clamp).
    BitField<3, 2, WrapMode> wrap_u;
    BitField<5, 2, WrapMode> wrap_v;
    BitField<7, 1, LODType> diag_lod;
    BitField<8, 16, s32> lod_bias;         // in 1/256 units
    BitField<24, 1, bool, u32> lod_clamp;  // stored, not used by the backends
    BitField<25, 4, u32> anisotropic_filtering;
    u32 hex = 0;
  };
  union TM1
  {
    // min_lod <= max_lod always holds.
    BitField<0, 8, u32> min_lod;  // in 1/16 units
    BitField<8, 8, u32> max_lod;  // in 1/16 units
    u32 hex = 0;
  };

  TM0 tm0;
  TM1 tm1;
};

template <>
struct std::hash<SamplerState>
{
  std::size_t operator()(const SamplerState& state) const noexcept
  {
    return std::hash<u64>{}(state.Hex());
  }
};

// Ready-made states for utility drawing. The "invalid" ones have every bit set, so they never
// match a real state.
namespace RenderState
{
RasterizationState GetInvalidRasterizationState();
RasterizationState GetNoCullRasterizationState(PrimitiveType primitive);
RasterizationState GetCullBackFaceRasterizationState(PrimitiveType primitive);
DepthState GetInvalidDepthState();
DepthState GetNoDepthTestingDepthState();
DepthState GetAlwaysWriteDepthState();
BlendingState GetInvalidBlendingState();
BlendingState GetNoBlendingBlendState();
BlendingState GetNoColorWriteBlendState();
SamplerState GetInvalidSamplerState();
SamplerState GetPointSamplerState();
SamplerState GetLinearSamplerState();
FramebufferState GetColorFramebufferState(AbstractTextureFormat format);
FramebufferState GetRGBA8FramebufferState();
}  // namespace RenderState
