// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/BitSet.h"

class PointerWrap;

// Dirty tracking for transform-unit (XF) state. XF and CP writes mark what they touched; the
// vertex shader constant code reads the marks when it next uploads and then clears them, so
// normally the marks cover what changed since the previous draw.
//
// Ranges are {first, last} pairs, {-1, -1} when nothing is pending.
class XFStateManager
{
public:
  void Init();
  void DoState(PointerWrap& p);

  // Records an XF memory write covering addresses [start, end).
  void InvalidateXFRange(int start, int end);

  void SetTexMatrixChangedA(u32 value);
  bool DidTexMatrixAChange() const { return m_texmtx_dirty[0]; }
  void ResetTexMatrixAChange();

  void SetTexMatrixChangedB(u32 value);
  bool DidTexMatrixBChange() const { return m_texmtx_dirty[1]; }
  void ResetTexMatrixBChange();

  bool DidPosNormalChange() const { return m_posnrm_dirty; }
  void ResetPosNormalChange();

  void SetProjectionChanged();
  bool DidProjectionChange() const { return m_projection_dirty; }
  void ResetProjection();

  void SetViewportChanged();
  bool DidViewportChange() const { return m_viewport_dirty; }
  void ResetViewportChange();

  void SetTexMatrixInfoChanged(int index);
  bool DidTexMatrixInfoChange() const { return m_texinfo_dirty; }
  void ResetTexMatrixInfoChange();

  void SetLightingConfigChanged();
  bool DidLightingConfigChange() const { return m_lighting_dirty; }
  void ResetLightingConfigChange();

  const std::array<int, 2>& GetLightsChanged() const { return m_light_span; }
  void ResetLightsChanged();

  void SetMaterialColorChanged(int index);
  const BitSet32& GetMaterialChanges() const { return m_material_dirty; }
  void ResetMaterialChanges();

  const std::array<int, 2>& GetPerVertexTransformMatrixChanges() const { return m_posmtx_span; }
  void ResetPerVertexTransformMatrixChanges();

  const std::array<int, 2>& GetPerVertexNormalMatrixChanges() const { return m_nrmmtx_span; }
  void ResetPerVertexNormalMatrixChanges();

  const std::array<int, 2>& GetPostTransformMatrixChanges() const { return m_postmtx_span; }
  void ResetPostTransformMatrixChanges();

private:
  using Span = std::array<int, 2>;

  // Flags.
  std::array<bool, 2> m_texmtx_dirty{};  // [0] texture matrices 0-3, [1] 4-7
  bool m_posnrm_dirty = false;
  bool m_projection_dirty = false;
  bool m_viewport_dirty = false;
  bool m_texinfo_dirty = false;
  bool m_lighting_dirty = false;
  BitSet32 m_material_dirty;

  // Address spans, each relative to its block (see InvalidateXFRange for the details).
  Span m_posmtx_span{};
  Span m_nrmmtx_span{};
  Span m_postmtx_span{};
  Span m_light_span{};
};
