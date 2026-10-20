// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PipelineUtils.h"

#include <cstring>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
namespace
{
// Without hardware logic ops or framebuffer fetch, logic ops become blend equations. Some of
// them need the pixel shader to adjust its output.
void ApproximateLogicOp(pixel_shader_uid_data* ps, BlendingState& blend)
{
  if (!blend.LogicOpApproximationIsExact())
    WARN_LOG_FMT(VIDEO,
                 "Approximating logic op with blending, this will produce incorrect rendering.");
  if (blend.LogicOpApproximationWantsShaderHelp())
  {
    ps->emulate_logic_op_with_blend = true;
    ps->logic_op_mode = static_cast<u32>(blend.logic_mode.Value());
  }
  blend.ApproximateLogicOpWithBlending();
}

void DisableDualSource(pixel_shader_uid_data* ps, BlendingState& blend)
{
  ps->no_dual_src = true;
  blend.use_dual_src = false;
}

// With framebuffer fetch the pixel shader can do the blending and logic op itself. This is used
// when the fixed-function path cannot do what is asked, and for early depth testing on
// hardware where forced early-Z does not work.
void MoveBlendingToShader(pixel_shader_uid_data* ps, BlendingState& blend)
{
  const bool early_z_unusable = DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DISCARD_WITH_EARLY_Z) ||
                                !g_backend_info.bSupportsEarlyZ;
  bool in_shader = false;
  if (early_z_unusable && ps->ztest == EmulatedZ::ForcedEarly)
  {
    ps->ztest = EmulatedZ::EarlyWithFBFetch;
    ps->no_dual_src = true;
    in_shader = in_shader || static_cast<bool>(blend.blend_enable);
  }
  if (blend.logic_op_enable && !g_backend_info.bSupportsLogicOp)
    in_shader = true;
  if (blend.use_dual_src && !g_backend_info.bSupportsDualSourceBlend)
    in_shader = true;
  if (!in_shader)
    return;

  ps->no_dual_src = true;
  if (blend.logic_op_enable)
  {
    ps->logic_op_enable = true;
    ps->logic_op_mode = static_cast<u32>(blend.logic_mode.Value());
    blend.logic_op_enable = false;
  }
  if (blend.blend_enable)
  {
    ps->blend_enable = true;
    ps->blend_src_factor = blend.src_factor;
    ps->blend_src_factor_alpha = blend.src_factor_alpha;
    ps->blend_dst_factor = blend.dst_factor;
    ps->blend_dst_factor_alpha = blend.dst_factor_alpha;
    ps->blend_subtract = blend.subtract;
    ps->blend_subtract_alpha = blend.subtract_alpha;
    blend.blend_enable = false;
  }
}

// Lines and points drawn as triangles generated in the vertex shader: the shader needs the
// vertex layout (it reads vertices itself), and the pipeline draws triangles.
void ExpandLinesAndPointsInVertexShader(GXPipelineUid& uid)
{
  vertex_shader_uid_data* vs = uid.vs_uid.GetUidData();
  const PortableVertexDeclaration& decl = uid.vertex_format->GetVertexDeclaration();

  vs->position_has_3_elems = decl.position.components >= 3;
  for (u32 unit = 0; unit < 8; ++unit)
  {
    const AttributeFormat& tc = decl.texcoords[unit];
    if (!tc.enable)
      continue;
    ASSERT(tc.components <= 3);
    vs->texGenInfo[unit].texcoord_elem_count = static_cast<u8>(tc.components);
  }

  const bool points = uid.rasterization_state.primitive == PrimitiveType::Points;
  vs->vs_expand = points ? VSExpand::Point : VSExpand::Line;
  uid.vertex_format = nullptr;

  const PrimitiveType triangles = g_backend_info.bSupportsPrimitiveRestart ?
                                      PrimitiveType::TriangleStrip :
                                      PrimitiveType::Triangles;
  uid.rasterization_state.primitive = triangles;
  uid.gs_uid.GetUidData()->primitive_type = static_cast<u32>(triangles);
}
}  // namespace

GXPipelineUid ApplyDriverBugs(const GXPipelineUid& in)
{
  // Bytewise copy so the padding (which the UID comparisons see) comes along.
  GXPipelineUid out;
  std::memcpy(static_cast<void*>(&out), static_cast<const void*>(&in), sizeof(out));
  pixel_shader_uid_data* ps = out.ps_uid.GetUidData();
  BlendingState& blend = out.blending_state;

  // Forcing early depth only matters when depth is written.
  if (ps->ztest == EmulatedZ::ForcedEarly && !out.depth_state.update_enable)
    ps->ztest = EmulatedZ::Early;

  if (blend.logic_op_enable && !g_backend_info.bSupportsLogicOp &&
      !g_backend_info.bSupportsFramebufferFetch)
  {
    ApproximateLogicOp(ps, blend);
  }

  // Where dual-source blending is unsupported (but fetch can stand in) or unreliable, only use
  // it when the blend equation needs it.
  const bool avoid_dual_source =
      (!g_backend_info.bSupportsDualSourceBlend && g_backend_info.bSupportsFramebufferFetch) ||
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING);
  if (avoid_dual_source && !blend.RequiresDualSrc())
    DisableDualSource(ps, blend);

  if (g_backend_info.bSupportsFramebufferFetch)
    MoveBlendingToShader(ps, blend);

  if (!g_backend_info.bSupportsDualSourceBlend)
    DisableDualSource(ps, blend);

  if (ps->ztest == EmulatedZ::ForcedEarly && !g_backend_info.bSupportsEarlyZ)
  {
    ASSERT(!ps->zfreeze);
    // Alpha test together with early depth cannot be emulated without forced early-Z in the
    // shader. Writing depth for every fragment matters more to games than a correct alpha test,
    // so the alpha test is made to always pass instead.
    ps->ztest = EmulatedZ::EarlyWithZComplocHack;
  }

  const PrimitiveType prim = out.rasterization_state.primitive;
  if (g_ActiveConfig.UseVSForLinePointExpand() &&
      (prim == PrimitiveType::Points || prim == PrimitiveType::Lines))
  {
    ExpandLinesAndPointsInVertexShader(out);
  }

  return out;
}

std::size_t PipelineToHash(const GXPipelineUid& in)
{
  XXH3_state_t state;
  XXH3_INITSTATE(&state);
  XXH3_64bits_reset_withSeed(&state, XXH64_hash_t{1});
  UpdateHashWithPipeline(in, &state);
  return XXH3_64bits_digest(&state);
}

void UpdateHashWithPipeline(const GXPipelineUid& in, XXH3_state_t* hash_state)
{
  // Field order is part of the hash value.
  const auto feed = [hash_state](const void* data, size_t size) {
    XXH3_64bits_update(hash_state, data, size);
  };
  feed(&in.vertex_format->GetVertexDeclaration(), sizeof(PortableVertexDeclaration));
  feed(&in.blending_state, sizeof(BlendingState));
  feed(&in.depth_state, sizeof(DepthState));
  feed(&in.rasterization_state, sizeof(RasterizationState));
  feed(&in.gs_uid, sizeof(GeometryShaderUid));
  feed(&in.ps_uid, sizeof(PixelShaderUid));
  feed(&in.vs_uid, sizeof(VertexShaderUid));
}
}  // namespace VideoCommon
