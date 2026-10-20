// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PixelShaderManager.h"

#include <array>
#include <cstddef>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace
{
// The fixed konstant fractions 1, 7/8, 3/4, ... 1/8 in 8-bit units (konst rows 0-7). Rows 8-11
// are the invalid selections, which read as zero, as does the alpha of rows 12-15 (the .rgb
// selections of the konstant registers).
constexpr std::array<s32, 8> FIXED_KONST = {255, 223, 191, 159, 128, 96, 64, 32};

// The fog range-adjust centre register is offset by this from the real screen centre.
constexpr s32 FOG_RANGE_CENTER_BIAS = 342;

// Assigns `value` to `field`, reporting whether it changed.
template <typename Field, typename Value>
bool Store(Field& field, const Value& value)
{
  const Field converted = static_cast<Field>(value);
  if (field == converted)
    return false;
  field = converted;
  return true;
}
}  // namespace

void PixelShaderManager::Init()
{
  constants = {};
  m_fog_range_adjusted_changed = true;
  m_viewport_changed = false;

  // Constants that are not zero for zeroed registers.
  for (int matrix = 0; matrix < 3; ++matrix)
    SetIndMatrixChanged(matrix);
  SetZTextureTypeChanged();
  for (u8 texmap = 0; texmap < 8; ++texmap)
    SetTexCoordChanged(texmap);

  for (std::size_t row = 0; row < FIXED_KONST.size(); ++row)
    constants.konst[row].fill(FIXED_KONST[row]);

  Dirty();
}

void PixelShaderManager::Dirty()
{
  m_fog_range_adjusted_changed = true;
  SetEfbScaleChanged(g_framebuffer_manager->EFBToScaledXf(1),
                     g_framebuffer_manager->EFBToScaledYf(1));
  SetFogParamChanged();
  dirty = true;
}

void PixelShaderManager::SetConstants()
{
  if (m_fog_range_adjusted_changed)
  {
    // Depends on both the fog range registers and the viewport, so it is resolved here.
    const auto& range = bpmem.fogRange;
    if (!g_ActiveConfig.bDisableFog && range.Base.Enabled == 1)
    {
      // Horizontal centre normalised to [-1, 1]; .w is the scaled viewport width.
      const int center = static_cast<int>(range.Base.Center.Value()) - FOG_RANGE_CENTER_BIAS;
      float normalized = static_cast<float>(center) / (2.0f * xfmem.viewport.wd);
      normalized = normalized * 2.0f - 1.0f;
      constants.fogf[2] = normalized;
      constants.fogf[3] = static_cast<float>(
          g_framebuffer_manager->EFBToScaledX(static_cast<int>(2.0f * xfmem.viewport.wd)));

      // Ten adjustment factors (two per K register), centre to edge, packed into three vec4s.
      constexpr float K_SCALE = 4.0f;
      for (std::size_t n = 0; n < 2 * std::size(range.K); ++n)
      {
        constants.fogrange[n / 4][n % 4] =
            range.K[n / 2].GetValue(static_cast<int>(n % 2)) * K_SCALE;
      }
    }
    else
    {
      constants.fogf[2] = 0;
      constants.fogf[3] = 1;
    }
    m_fog_range_adjusted_changed = false;
    dirty = true;
  }

  if (m_viewport_changed)
  {
    constants.zbias[1][0] = static_cast<s32>(xfmem.viewport.farZ);
    constants.zbias[1][1] = static_cast<s32>(xfmem.viewport.zRange);
    m_viewport_changed = false;
    dirty = true;
  }

  if (m_indirect_dirty)
  {
    // .z: tevind of each active TEV stage. .w: indirect reference of each indirect stage, with
    // bit 16 marking it as used (indexed by indirect stage, not TEV stage). A zero tevind is a
    // passthrough, so .z needs no extra flag.
    for (std::size_t ind = 0; ind < 4; ++ind)
      constants.pack1[ind][3] = 0;

    const u32 tev_stages = bpmem.genMode.numtevstages + 1;
    for (u32 stage = 0; stage < tev_stages; ++stage)
    {
      const auto& ind = bpmem.tevind[stage];
      constants.pack1[stage][2] = ind.hex;

      const u32 ind_stage = ind.bt;
      if (!ind.IsActive() || ind_stage >= bpmem.genMode.numindstages)
        continue;
      const u32 coord = bpmem.tevindref.getTexCoord(static_cast<int>(ind_stage));
      const u32 map = bpmem.tevindref.getTexMap(static_cast<int>(ind_stage));
      constants.pack1[ind_stage][3] = coord | (map << 8) | (1u << 16);
    }
    m_indirect_dirty = false;
    dirty = true;
  }

  // Note that this flag is never cleared: the value is recomputed on every call once set.
  if (m_dest_alpha_dirty)
  {
    // Only meaningful with alpha writes to an alpha-carrying EFB; otherwise force zero.
    const bool active = bpmem.blendmode.alpha_update && bpmem.dstalpha.enable &&
                        bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;
    if (Store(constants.dstalpha, active ? bpmem.dstalpha.hex : 0u))
      dirty = true;
  }
}

void PixelShaderManager::SetTevColor(int index, int component, s32 value)
{
  auto& color = constants.colors[index];
  color[component] = value;
  dirty = true;

  PRIM_LOG("tev color{}: {} {} {} {}", index, color[0], color[1], color[2], color[3]);
}

void PixelShaderManager::SetTevKonstColor(int index, int component, s32 value)
{
  auto& color = constants.kcolors[index];
  color[component] = value;
  dirty = true;

  // Ubershader konst table: rows 12-15 are the .rgb selections (alpha stays zero), rows 16-31
  // the splatted .rrrr/.gggg/.bbbb/.aaaa selections.
  if (component != 3)
    constants.konst[12 + index][component] = value;
  constants.konst[16 + index + 4 * component].fill(value);

  PRIM_LOG("tev konst color{}: {} {} {} {}", index, color[0], color[1], color[2], color[3]);
}

void PixelShaderManager::SetTevOrder(int index, u32 order)
{
  if (Store(constants.pack2[index][0], order))
    dirty = true;
}

void PixelShaderManager::SetTevKSel(int index, u32 ksel)
{
  if (Store(constants.pack2[index][1], ksel))
    dirty = true;
}

void PixelShaderManager::SetTevCombiner(int index, int alpha, u32 combiner)
{
  if (Store(constants.pack1[index][alpha], combiner))
    dirty = true;
}

void PixelShaderManager::SetTevIndirectChanged()
{
  m_indirect_dirty = true;
}

void PixelShaderManager::SetAlpha()
{
  constants.alpha[0] = bpmem.alpha_test.ref0;
  constants.alpha[1] = bpmem.alpha_test.ref1;
  constants.alpha[3] = static_cast<s32>(bpmem.dstalpha.alpha);
  dirty = true;
}

void PixelShaderManager::SetAlphaTestChanged()
{
  // Zero means "always passes"; otherwise bit 31 is set so that a test that can never pass with
  // all-zero fields still differs from zero.
  const bool always_passes = bpmem.alpha_test.TestResult() == AlphaTestResult::Pass;
  const u32 value = always_passes ? 0u : (bpmem.alpha_test.hex | (1u << 31));
  if (Store(constants.alphaTest, value))
    dirty = true;
}

void PixelShaderManager::SetDestAlphaChanged()
{
  m_dest_alpha_dirty = true;
}

void PixelShaderManager::SetTexDims(int texmapid, u32 width, u32 height)
{
  auto& dims = constants.texdims[texmapid];
  const bool changed = Store(dims[0], width) | Store(dims[1], height);
  if (changed)
    dirty = true;
}

void PixelShaderManager::SetSamplerState(int texmapid, u32 tm0, u32 tm1)
{
  auto& words = constants.pack2[texmapid];
  const bool changed = Store(words[2], tm0) | Store(words[3], tm1);
  if (changed)
    dirty = true;
}

void PixelShaderManager::SetZTextureBias()
{
  constants.zbias[1][3] = bpmem.ztex1.bias;
  dirty = true;
}

void PixelShaderManager::SetViewportChanged()
{
  m_viewport_changed = true;
  // The fog range adjustment uses the viewport width too.
  m_fog_range_adjusted_changed = true;
}

void PixelShaderManager::SetEfbScaleChanged(float scalex, float scaley)
{
  constants.efbscale[0] = 1.0f / scalex;
  constants.efbscale[1] = 1.0f / scaley;
  dirty = true;
}

void PixelShaderManager::SetZSlope(float dfdx, float dfdy, float f0)
{
  constants.zslope[0] = dfdx;
  constants.zslope[1] = dfdy;
  constants.zslope[2] = f0;
  dirty = true;
}

void PixelShaderManager::SetIndTexScaleChanged(bool high)
{
  const auto& scale = bpmem.texscale[high];
  constants.indtexscale[high] = {static_cast<s32>(scale.ss0.Value()),
                                 static_cast<s32>(scale.ts0.Value()),
                                 static_cast<s32>(scale.ss1.Value()),
                                 static_cast<s32>(scale.ts1.Value())};
  dirty = true;
}

void PixelShaderManager::SetIndMatrixChanged(int matrixidx)
{
  const auto& matrix = bpmem.indmtx[matrixidx];
  const u8 scale = matrix.GetScale();
  // Two rows per matrix: the static 2x3 matrix row in .xyz and the shift for the dynamic
  // matrices (17 - scale exponent) in .w.
  const s32 shift = 17 - scale;
  constants.indtexmtx[2 * matrixidx] = {matrix.col0.ma, matrix.col1.mc, matrix.col2.me, shift};
  constants.indtexmtx[2 * matrixidx + 1] = {matrix.col0.mb, matrix.col1.md, matrix.col2.mf, shift};
  dirty = true;

  PRIM_LOG("indmtx{}: scale={}, mat=({} {} {}; {} {} {})", matrixidx, scale, matrix.col0.ma,
           matrix.col1.mc, matrix.col2.me, matrix.col0.mb, matrix.col1.md, matrix.col2.mf);
}

void PixelShaderManager::SetZTextureTypeChanged()
{
  // Weights that assemble the depth value from the texel's channels, plus the bias scale.
  auto& weights = constants.zbias[0];
  const ZTexFormat format = bpmem.ztex2.type;
  if (format == ZTexFormat::U8)
    weights = {0, 0, 0, 1};
  else if (format == ZTexFormat::U16)
    weights = {1, 0, 0, 256};
  else if (format == ZTexFormat::U24)
    weights = {65536, 256, 1, 0};
  else
    PanicAlertFmt("Invalid ztex format {}", bpmem.ztex2.type);
  dirty = true;
}

void PixelShaderManager::SetZTextureOpChanged()
{
  constants.ztex_op = bpmem.ztex2.op;
  dirty = true;
}

void PixelShaderManager::SetTexCoordChanged(u8 texmapid)
{
  const TCoordInfo& coord = bpmem.texcoords[texmapid];
  constants.texdims[texmapid][2] = coord.s.scale_minus_1 + 1;
  constants.texdims[texmapid][3] = coord.t.scale_minus_1 + 1;
  dirty = true;
}

void PixelShaderManager::SetFogColorChanged()
{
  if (g_ActiveConfig.bDisableFog)
    return;

  const auto& color = bpmem.fog.color;
  constants.fogcolor[0] = color.r;
  constants.fogcolor[1] = color.g;
  constants.fogcolor[2] = color.b;
  dirty = true;
}

void PixelShaderManager::SetFogParamChanged()
{
  // With fog disabled the parameters are neutralised (the shift values must stay non-zero).
  const bool enabled = !g_ActiveConfig.bDisableFog;
  const auto& fog = bpmem.fog;
  constants.fogf[0] = enabled ? fog.GetA() : 0.f;
  constants.fogf[1] = enabled ? fog.GetC() : 0.f;
  constants.fogi[1] = enabled ? static_cast<s32>(fog.b_magnitude) : 1;
  constants.fogi[3] = enabled ? static_cast<s32>(fog.b_shift) : 1;
  constants.fogParam3 = enabled ? fog.c_proj_fsel.hex : 0u;
  dirty = true;
}

void PixelShaderManager::SetFogRangeAdjustChanged()
{
  if (g_ActiveConfig.bDisableFog)
    return;

  m_fog_range_adjusted_changed = true;
  if (Store(constants.fogRangeBase, bpmem.fogRange.Base.hex))
    dirty = true;
}

void PixelShaderManager::SetGenModeChanged()
{
  constants.genmode = bpmem.genMode.hex;
  // The number of TEV and indirect stages feeds the indirect constants.
  m_indirect_dirty = true;
  dirty = true;
}

void PixelShaderManager::SetZModeControl()
{
  const bool rgba6 =
      bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24 && !g_ActiveConfig.bForceTrueColor;
  const u32 late_ztest = bpmem.GetEmulatedZ() == EmulatedZ::Late ? 1u : 0u;
  const u32 rgba6_format = rgba6 ? 1u : 0u;
  const u32 dither = (rgba6 && bpmem.blendmode.dither) ? 1u : 0u;

  const bool changed = constants.late_ztest != late_ztest ||
                       constants.rgba6_format != rgba6_format || constants.dither != dither;
  if (changed)
  {
    constants.late_ztest = late_ztest;
    constants.rgba6_format = rgba6_format;
    constants.dither = dither;
    dirty = true;
  }
  m_dest_alpha_dirty = true;
}

void PixelShaderManager::SetBlendModeChanged()
{
  bool changed = Store(constants.dither, constants.rgba6_format && bpmem.blendmode.dither);

  BlendingState blend = {};
  blend.Generate(bpmem);
  changed |= Store(constants.blend_enable, blend.blend_enable.Value());
  changed |= Store(constants.blend_src_factor, blend.src_factor.Value());
  changed |= Store(constants.blend_src_factor_alpha, blend.src_factor_alpha.Value());
  changed |= Store(constants.blend_dst_factor, blend.dst_factor.Value());
  changed |= Store(constants.blend_dst_factor_alpha, blend.dst_factor_alpha.Value());
  changed |= Store(constants.blend_subtract, blend.subtract.Value());
  changed |= Store(constants.blend_subtract_alpha, blend.subtract_alpha.Value());
  changed |= Store(constants.logic_op_enable, blend.logic_op_enable.Value());
  changed |= Store(constants.logic_op_mode, blend.logic_mode.Value());
  if (changed)
    dirty = true;

  m_dest_alpha_dirty = true;
}

void PixelShaderManager::SetBoundingBoxActive(bool active)
{
  // Compares against the effective state but stores the requested one, as it always has.
  const bool effective = active && g_ActiveConfig.bBBoxEnable;
  if (effective == (constants.bounding_box != 0))
    return;
  constants.bounding_box = active;
  dirty = true;
}

void PixelShaderManager::DoState(PointerWrap& p)
{
  p.Do(m_fog_range_adjusted_changed);
  p.Do(m_viewport_changed);
  p.Do(m_indirect_dirty);
  p.Do(m_dest_alpha_dirty);
  p.Do(constants);

  // Config-dependent values are rebuilt from the loaded registers (which must be loaded first).
  if (p.IsReadMode())
    Dirty();
}
