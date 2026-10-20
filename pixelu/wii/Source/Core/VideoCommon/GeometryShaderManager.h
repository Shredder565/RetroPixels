// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/ConstantManager.h"

class PointerWrap;
enum class PrimitiveType : u32;

// Keeps GeometryShaderConstants in sync with the BP/XF registers; backend independent.
class GeometryShaderManager
{
public:
  void Init();
  // Recomputes config-dependent constants (after loading a state or changing settings).
  void Dirty();
  void DoState(PointerWrap& p);

  void SetConstants(PrimitiveType prim);
  void SetViewportChanged();
  void SetProjectionChanged();
  void SetLinePtWidthChanged();
  void SetTexCoordChanged(u8 texmapid);

  GeometryShaderConstants constants{};
  bool dirty = false;

private:
  void SetVSExpand(VSExpand expand);

  bool m_projection_changed = false;
  bool m_viewport_changed = false;
};
