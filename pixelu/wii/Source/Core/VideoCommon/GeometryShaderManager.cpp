// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GeometryShaderManager.h"

#include <array>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace
{
// Texture coordinate offset (in 1/16 texel steps scaled by the shader) for each lineoff/pointoff
// register value. The top three settings all map to the smallest step.
constexpr std::array<s32, 8> TEXCOORD_OFFSET_FOR_SETTING = {0, 16, 8, 4, 2, 1, 1, 1};

// Replaces bit `bit` of a packed per-texmap flag word.
s32 WithBit(s32 word, u32 bit, u32 value)
{
  const u32 mask = 1u << bit;
  const u32 bits = (static_cast<u32>(word) & ~mask) | (value << bit);
  return static_cast<s32>(bits);
}
}  // namespace

void GeometryShaderManager::Init()
{
  constants = {};
  // Viewport and projection derive non-zero values from zeroed registers.
  m_viewport_changed = true;
  m_projection_changed = true;
  dirty = true;
}

void GeometryShaderManager::Dirty()
{
  m_projection_changed = true;
  // The line and point sizes depend on the EFB scale setting.
  SetLinePtWidthChanged();
  dirty = true;
}

void GeometryShaderManager::SetVSExpand(VSExpand expand)
{
  if (constants.vs_expand == expand)
    return;
  constants.vs_expand = expand;
  dirty = true;
}

void GeometryShaderManager::SetConstants(PrimitiveType prim)
{
  const VideoConfig& config = g_ActiveConfig;

  // Stereo eye separation; only recomputed while stereo is on, so the change stays pending
  // otherwise.
  if (m_projection_changed && config.stereo_mode != StereoMode::Off)
  {
    m_projection_changed = false;
    float left = 0.0f;
    float right = 0.0f;
    if (xfmem.projection.type == ProjectionType::Perspective)
    {
      const float separation = config.stereo_depth;
      left = config.bStereoSwapEyes ? separation : -separation;
      right = -left;
    }
    constants.stereoparams[0] = left;
    constants.stereoparams[1] = right;
    constants.stereoparams[2] = config.stereo_convergence;
    dirty = true;
  }

  if (config.UseVSForLinePointExpand())
  {
    VSExpand expand = VSExpand::None;
    if (prim == PrimitiveType::Points)
      expand = VSExpand::Point;
    else if (prim == PrimitiveType::Lines)
      expand = VSExpand::Line;
    SetVSExpand(expand);
  }

  if (m_viewport_changed)
  {
    m_viewport_changed = false;
    constants.lineptparams[0] = 2.0f * xfmem.viewport.wd;
    constants.lineptparams[1] = -2.0f * xfmem.viewport.ht;
    dirty = true;
  }
}

void GeometryShaderManager::SetViewportChanged()
{
  m_viewport_changed = true;
}

void GeometryShaderManager::SetProjectionChanged()
{
  m_projection_changed = true;
}

void GeometryShaderManager::SetLinePtWidthChanged()
{
  const auto& width = bpmem.lineptwidth;
  // Sizes are stored in sixths of a pixel.
  constants.lineptparams[2] = static_cast<float>(width.linesize.Value()) / 6.0f;
  constants.lineptparams[3] = static_cast<float>(width.pointsize.Value()) / 6.0f;
  constants.texoffset[2] = TEXCOORD_OFFSET_FOR_SETTING[width.lineoff];
  constants.texoffset[3] = TEXCOORD_OFFSET_FOR_SETTING[width.pointoff];
  dirty = true;
}

void GeometryShaderManager::SetTexCoordChanged(u8 texmapid)
{
  // .x and .y hold one bit per texmap: whether lines / points offset that coordinate.
  const TCoordInfo& coord = bpmem.texcoords[texmapid];
  constants.texoffset[0] = WithBit(constants.texoffset[0], texmapid, coord.s.line_offset);
  constants.texoffset[1] = WithBit(constants.texoffset[1], texmapid, coord.s.point_offset);
  dirty = true;
}

void GeometryShaderManager::DoState(PointerWrap& p)
{
  p.Do(m_projection_changed);
  p.Do(m_viewport_changed);
  p.Do(constants);

  // Loaded constants may come from different settings; rebuild what depends on them.
  if (p.IsReadMode())
    Dirty();
}
