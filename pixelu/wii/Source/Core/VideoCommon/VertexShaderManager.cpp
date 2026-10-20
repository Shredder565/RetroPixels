// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexShaderManager.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace
{
// Largest depth value of the 24-bit EFB depth buffer.
constexpr float MAX_DEPTH = 16777215.0f;

// Copies whole float4 rows [first_row, end_row) of an XF matrix block.
template <std::size_t N>
void CopyRows(std::array<float4, N>& dest, const float* xf_block, int first_row, int end_row)
{
  std::memcpy(dest[first_row].data(), xf_block + first_row * 4,
              static_cast<std::size_t>(end_row - first_row) * sizeof(float4));
}

// Copies a 3x4 matrix (three rows) from XF memory.
void CopyMatrix3x4(float4* dest, const float* src)
{
  std::memcpy(dest->data(), src, 3 * sizeof(float4));
}

// Light direction cleanup after normalising: NaN becomes 0, infinities become +-1.
float SanitizeDirection(float component)
{
  if (std::isnan(component))
    return 0.0f;
  if (std::isinf(component))
    return component > 0.0f ? 1.0f : -1.0f;
  return component;
}

void ConvertLight(const Light& src, VertexShaderConstants::Light& dst)
{
  // XF stores the colour as a, b, g, r bytes.
  for (int c = 0; c < 4; ++c)
    dst.color[c] = src.color[3 - c];

  for (int c = 0; c < 3; ++c)
  {
    dst.cosatt[c] = src.cosatt[c];
    dst.distatt[c] = src.distatt[c];
    dst.pos[c] = src.dpos[c];
  }
  // All-zero distance attenuation would divide by zero in the shader.
  constexpr float tiny = 0.00001f;
  if (std::fabs(src.distatt[0]) < tiny && std::fabs(src.distatt[1]) < tiny &&
      std::fabs(src.distatt[2]) < tiny)
  {
    dst.distatt[0] = tiny;
  }

  // Normalised in double precision. Whether hardware normalises at all is unverified.
  double length_sq = 0.0;
  for (int c = 0; c < 3; ++c)
    length_sq += double(src.ddir[c]) * double(src.ddir[c]);
  const double inv_length = 1.0 / std::sqrt(length_sq);
  for (int c = 0; c < 3; ++c)
    dst.dir[c] = SanitizeDirection(static_cast<float>(src.ddir[c] * inv_length));
}

// Material/ambient colours are RGBA8 words, unpacked to one channel per int.
void UnpackColor(int4& dest, u32 rgba)
{
  for (int c = 0; c < 4; ++c)
    dest[c] = static_cast<s32>((rgba >> (24 - 8 * c)) & 0xFF);
}

// row . (x, y, z, 1)
float TransformRow(const float* row, const float* v)
{
  return v[0] * row[0] + v[1] * row[1] + v[2] * row[2] + row[3];
}
}  // namespace

void VertexShaderManager::Init()
{
  m_projection_graphics_mod_change = false;
  constants = {};
  m_projection_matrix = Common::Matrix44::Identity().data;
  dirty = true;
}

Common::Matrix44 VertexShaderManager::LoadProjectionMatrix()
{
  const auto& raw = xfmem.projection.rawProjection;
  const ProjectionType type = xfmem.projection.type;
  auto& m = m_projection_matrix;

  if (type == ProjectionType::Perspective)
  {
    // Only x/y scale and the z row come from XF; the aspect hack and free-look FOV scale x/y.
    const Common::Vec2 fov = g_freelook_camera.IsActive() ?
                                 g_freelook_camera.GetFieldOfViewMultiplier() :
                                 Common::Vec2{1, 1};
    m.fill(0.0f);
    m[0] = raw[0] * g_ActiveConfig.fAspectRatioHackW * fov.x;
    m[2] = raw[1] * g_ActiveConfig.fAspectRatioHackW * fov.x;
    m[5] = raw[2] * g_ActiveConfig.fAspectRatioHackH * fov.y;
    m[6] = raw[3] * g_ActiveConfig.fAspectRatioHackH * fov.y;
    m[10] = raw[4];
    m[11] = raw[5];
    m[14] = -1.0f;
    g_stats.gproj = m;
  }
  else if (type == ProjectionType::Orthographic)
  {
    // Scale on the diagonal, offsets in the last column.
    m.fill(0.0f);
    m[0] = raw[0];
    m[3] = raw[1];
    m[5] = raw[2];
    m[7] = raw[3];
    m[10] = raw[4];
    m[11] = raw[5];
    m[15] = 1.0f;
    g_stats.g2proj = m;
    g_stats.proj = raw;
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "Unknown projection type: {}", xfmem.projection.type);
  }

  PRIM_LOG("Projection: {} {} {} {} {} {}", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);

  Common::Matrix44 result = Common::Matrix44::FromArray(m);
  if (g_freelook_camera.IsActive() && type == ProjectionType::Perspective)
    result *= g_freelook_camera.GetView();
  g_freelook_camera.GetController()->SetClean();
  return result;
}

void VertexShaderManager::SetProjectionMatrix(XFStateManager& xf_state_manager)
{
  if (!xf_state_manager.DidProjectionChange() && !g_freelook_camera.GetController()->IsDirty())
    return;
  xf_state_manager.ResetProjection();
  const Common::Matrix44 matrix = LoadProjectionMatrix();
  std::memcpy(constants.projection.data(), matrix.data.data(), sizeof(constants.projection));
}

bool VertexShaderManager::UseVertexDepthRange()
{
  // Not needed when the backend can take any depth range and clamp to the EFB maximum itself,
  // and impossible without depth clamping.
  if (g_backend_info.bSupportsUnrestrictedDepthRange || !g_backend_info.bSupportsDepthClamp)
    return false;

  // Z textures replace the depth and need the full range.
  if (bpmem.ztex2.op != ZTexOp::Disabled && !bpmem.zcontrol.early_ztest)
    return true;

  const float range = xfmem.viewport.zRange;
  const float far_z = xfmem.viewport.farZ;
  // Inverted ranges, where the backend cannot invert the viewport.
  if (!g_backend_info.bSupportsReversedDepthRange && (range < 0.0f || range > far_z))
    return true;

  // Ranges beyond the 24-bit depth buffer.
  return std::fabs(range) > MAX_DEPTH || std::fabs(far_z) > MAX_DEPTH;
}

void VertexShaderManager::SetConstants(std::span<const std::string> textures,
                                       XFStateManager& xf_state_manager)
{
  // The colour shown for missing vertex colours (RGBA8 setting).
  const u32 missing = g_ActiveConfig.iMissingColorValue;
  if (constants.missing_color_hex != missing)
  {
    constants.missing_color_hex = missing;
    for (int c = 0; c < 4; ++c)
    {
      const float channel = static_cast<float>((missing >> (24 - 8 * c)) & 0xFF);
      constants.missing_color_value[c] = channel / 255;
    }
    dirty = true;
  }

  // Position matrices: the changed span is in floats, uploaded in whole rows.
  if (const auto span = xf_state_manager.GetPerVertexTransformMatrixChanges(); span[0] >= 0)
  {
    CopyRows(constants.transformmatrices, xfmem.posMatrices, span[0] / 4, (span[1] + 3) / 4);
    xf_state_manager.ResetPerVertexTransformMatrixChanges();
    dirty = true;
  }

  // Normal matrices are 3x3 in XF but stored one float4 row per three floats here.
  if (const auto span = xf_state_manager.GetPerVertexNormalMatrixChanges(); span[0] >= 0)
  {
    for (int row = span[0] / 3; row < (span[1] + 2) / 3; ++row)
      std::memcpy(constants.normalmatrices[row].data(), &xfmem.normalMatrices[3 * row], 12);
    xf_state_manager.ResetPerVertexNormalMatrixChanges();
    dirty = true;
  }

  if (const auto span = xf_state_manager.GetPostTransformMatrixChanges(); span[0] >= 0)
  {
    CopyRows(constants.posttransformmatrices, xfmem.postMatrices, span[0] / 4,
             (span[1] + 3) / 4);
    xf_state_manager.ResetPostTransformMatrixChanges();
    dirty = true;
  }

  // Lights are 16 words each in XF memory.
  if (const auto span = xf_state_manager.GetLightsChanged(); span[0] >= 0)
  {
    for (int light = span[0] / 0x10; light < (span[1] + 15) / 0x10; ++light)
      ConvertLight(xfmem.lights[light], constants.lights[light]);
    xf_state_manager.ResetLightsChanged();
    dirty = true;
  }

  // Indices 0 and 1 are the ambient colours, 2 and 3 the material colours.
  for (const int index : xf_state_manager.GetMaterialChanges())
  {
    UnpackColor(constants.materials[index],
                index >= 2 ? xfmem.matColor[index - 2] : xfmem.ambColor[index]);
    dirty = true;
  }
  xf_state_manager.ResetMaterialChanges();

  const auto& index_a = g_main_cp_state.matrix_index_a;
  const auto& index_b = g_main_cp_state.matrix_index_b;

  // The current position matrix and its normal matrix (3x3, padded to float4 rows).
  if (xf_state_manager.DidPosNormalChange())
  {
    xf_state_manager.ResetPosNormalChange();
    const u32 pos_index = index_a.PosNormalMtxIdx;
    CopyMatrix3x4(constants.posnormalmatrix.data(), &xfmem.posMatrices[pos_index * 4]);
    const float* normal = &xfmem.normalMatrices[3 * (pos_index & 31)];
    for (int row = 0; row < 3; ++row)
      std::memcpy(constants.posnormalmatrix[3 + row].data(), normal + 3 * row, 3 * sizeof(float));
    dirty = true;
  }

  // Texture matrices 0-3 and 4-7, three rows each.
  if (xf_state_manager.DidTexMatrixAChange())
  {
    xf_state_manager.ResetTexMatrixAChange();
    const std::array<u32, 4> rows = {index_a.Tex0MtxIdx, index_a.Tex1MtxIdx, index_a.Tex2MtxIdx,
                                     index_a.Tex3MtxIdx};
    for (std::size_t n = 0; n < rows.size(); ++n)
      CopyMatrix3x4(&constants.texmatrices[3 * n], &xfmem.posMatrices[rows[n] * 4]);
    dirty = true;
  }
  if (xf_state_manager.DidTexMatrixBChange())
  {
    xf_state_manager.ResetTexMatrixBChange();
    const std::array<u32, 4> rows = {index_b.Tex4MtxIdx, index_b.Tex5MtxIdx, index_b.Tex6MtxIdx,
                                     index_b.Tex7MtxIdx};
    for (std::size_t n = 0; n < rows.size(); ++n)
      CopyMatrix3x4(&constants.texmatrices[12 + 3 * n], &xfmem.posMatrices[rows[n] * 4]);
    dirty = true;
  }

  if (xf_state_manager.DidViewportChange())
  {
    xf_state_manager.ResetViewportChange();
    const auto& viewport = xfmem.viewport;

    // Without antialiasing the console samples pixels at 7/12 rather than the host's 1/2; the
    // vertex shader shifts positions by the difference (see VertexShaderGen). Sample positions
    // from BP 0x01-0x04 would matter if antialiasing were ever emulated.
    const float center_shift = 7.0f / 12.0f - 0.5f;
    const bool rounding = g_ActiveConfig.UseVertexRounding();
    const float width = 2.f * viewport.wd;
    const float height = 2.f * viewport.ht;
    // (The height is scaled with the horizontal EFB scale, as it always has been.)
    const float scaled_width = rounding ? width : g_framebuffer_manager->EFBToScaledXf(width);
    const float scaled_height = rounding ? height : g_framebuffer_manager->EFBToScaledXf(height);
    auto& correction = constants.pixelcentercorrection;
    correction[0] = center_shift * (2.f / scaled_width);
    correction[1] = center_shift * (2.f / scaled_height);
    // .zw: depth scale and offset, identity unless the vertex shader maps depth itself.
    correction[2] = 1.0f;
    correction[3] = 0.0f;
    constants.viewport[0] = width;
    constants.viewport[1] = height;

    if (UseVertexDepthRange())
    {
      if (g_backend_info.bSupportsReversedDepthRange)
      {
        // Map into a reversed range so a game's own reversed-Z setup keeps its precision.
        correction[2] = std::fabs(viewport.zRange) / MAX_DEPTH;
        correction[3] = viewport.zRange < 0.0f ? viewport.farZ / MAX_DEPTH :
                                                 1.0f - viewport.farZ / MAX_DEPTH;
      }
      else
      {
        // Still drawable, with less precision (possible z-fighting).
        correction[2] = viewport.zRange / MAX_DEPTH;
        correction[3] = 1.0f - viewport.farZ / MAX_DEPTH;
      }
    }

    dirty = true;
    BPFunctions::SetScissorAndViewport(g_framebuffer_manager.get(), bpmem.scissorTL,
                                       bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
    g_stats.AddScissorRect();
  }

  // Graphics-mod actions for this projection type, globally and per bound texture.
  std::vector<GraphicsModAction*> mod_actions;
  if (g_ActiveConfig.bGraphicMods)
  {
    const ProjectionType type = xfmem.projection.type;
    const auto& global_actions = g_graphics_mod_manager->GetProjectionActions(type);
    mod_actions.insert(mod_actions.end(), global_actions.begin(), global_actions.end());
    for (const std::string& texture : textures)
    {
      for (GraphicsModAction* action :
           g_graphics_mod_manager->GetProjectionTextureActions(type, texture))
      {
        mod_actions.push_back(action);
      }
    }
  }

  // A mod that touched the previous projection forces one more reload to undo it.
  const bool reload_projection =
      xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty() ||
      !mod_actions.empty() || m_projection_graphics_mod_change;
  if (reload_projection)
  {
    xf_state_manager.ResetProjection();
    m_projection_graphics_mod_change = !mod_actions.empty();

    Common::Matrix44 matrix = LoadProjectionMatrix();
    GraphicsModActionData::Projection projection{&matrix};
    for (GraphicsModAction* action : mod_actions)
      action->OnProjection(&projection);

    std::memcpy(constants.projection.data(), matrix.data.data(), sizeof(constants.projection));
    dirty = true;
  }

  if (xf_state_manager.DidTexMatrixInfoChange())
  {
    xf_state_manager.ResetTexMatrixInfoChange();
    constants.xfmem_dualTexInfo = xfmem.dualTexTrans.enabled;
    for (std::size_t n = 0; n < std::size(xfmem.texMtxInfo); ++n)
      constants.xfmem_pack1[n][0] = xfmem.texMtxInfo[n].hex;
    for (std::size_t n = 0; n < std::size(xfmem.postMtxInfo); ++n)
      constants.xfmem_pack1[n][1] = xfmem.postMtxInfo[n].hex;
    dirty = true;
  }

  if (xf_state_manager.DidLightingConfigChange())
  {
    xf_state_manager.ResetLightingConfigChange();
    for (std::size_t chan = 0; chan < 2; ++chan)
    {
      constants.xfmem_pack1[chan][2] = xfmem.color[chan].hex;
      constants.xfmem_pack1[chan][3] = xfmem.alpha[chan].hex;
    }
    constants.xfmem_numColorChans = xfmem.numChan.numColorChans;
    dirty = true;
  }
}

void VertexShaderManager::TransformToClipSpace(const float* data, float* out, u32 mtxIdx)
{
  // Uses the projection stored by the last projection load (SetConstants() must have run).
  const float* world = &xfmem.posMatrices[(mtxIdx & 0x3f) * 4];
  const float view[3] = {TransformRow(world, data), TransformRow(world + 4, data),
                         TransformRow(world + 8, data)};
  const float* proj = m_projection_matrix.data();
  for (int row = 0; row < 4; ++row)
    out[row] = TransformRow(proj + 4 * row, view);
}

void VertexShaderManager::DoState(PointerWrap& p)
{
  p.DoArray(m_projection_matrix);
  g_freelook_camera.DoState(p);
  p.Do(constants);

  if (p.IsReadMode())
    dirty = true;
}
