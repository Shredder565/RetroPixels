// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>

#include "Common/CommonTypes.h"
#include "VideoCommon/ConstantManager.h"

class PointerWrap;

// Keeps PixelShaderConstants in sync with the BP registers; backend independent.
// The Set*Changed() functions read bpmem/xfmem, so call them once the register write has landed.
class PixelShaderManager final
{
public:
  void Init();
  // Recomputes config-dependent constants (after loading a state or changing settings).
  void Dirty();
  void DoState(PointerWrap& p);

  // Applies the changes that were only flagged so far.
  void SetConstants();

  void SetTevColor(int index, int component, s32 value);
  void SetTevKonstColor(int index, int component, s32 value);
  void SetTevOrder(int index, u32 order);
  void SetTevKSel(int index, u32 ksel);
  void SetTevCombiner(int index, int alpha, u32 combiner);
  void SetAlpha();
  void SetAlphaTestChanged();
  void SetDestAlphaChanged();
  void SetTexDims(int texmapid, u32 width, u32 height);
  void SetSamplerState(int texmapid, u32 tm0, u32 tm1);
  void SetZTextureBias();
  void SetViewportChanged();
  void SetEfbScaleChanged(float scalex, float scaley);
  void SetZSlope(float dfdx, float dfdy, float f0);
  void SetIndMatrixChanged(int matrixidx);
  void SetTevIndirectChanged();
  void SetZTextureTypeChanged();
  void SetZTextureOpChanged();
  void SetIndTexScaleChanged(bool high);
  void SetTexCoordChanged(u8 texmapid);
  void SetFogColorChanged();
  void SetFogParamChanged();
  void SetFogRangeAdjustChanged();
  void SetGenModeChanged();
  void SetZModeControl();
  void SetBlendModeChanged();
  void SetBoundingBoxActive(bool active);

  PixelShaderConstants constants{};
  bool dirty = false;

  // Uniform data of graphics-mod pixel shaders.
  std::span<u8> custom_constants;
  bool custom_constants_dirty = false;

private:
  // Deferred updates, resolved in SetConstants(). The order is part of save states.
  bool m_fog_range_adjusted_changed = false;
  bool m_viewport_changed = false;
  bool m_indirect_dirty = false;
  bool m_dest_alpha_dirty = false;
};
