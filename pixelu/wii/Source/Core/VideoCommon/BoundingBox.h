// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"

class PixelShaderManager;
class PointerWrap;

using BBoxType = s32;
constexpr u32 NUM_BBOX_VALUES = 4;

// Emulates the pixel engine's bounding box registers (left, right, top, bottom). Backends that
// can track the box on the GPU implement Read/Write; this class caches the four values on the
// CPU side, batching writes and reading back lazily.
class BoundingBox
{
public:
  explicit BoundingBox() = default;
  virtual ~BoundingBox() = default;

  bool IsEnabled() const { return m_active; }
  void Enable(PixelShaderManager& pixel_shader_manager);
  void Disable(PixelShaderManager& pixel_shader_manager);

  // Pushes pending CPU-side writes to the backend and marks the cache stale (a draw follows).
  void Flush();

  u16 Get(u32 index);
  void Set(u32 index, u16 value);

  void DoState(PointerWrap& p);

  // The three backend hooks may only be used when the backend reports bounding box support.
  virtual bool Initialize() = 0;

protected:
  virtual std::vector<BBoxType> Read(u32 index, u32 length) = 0;
  virtual void Write(u32 index, std::span<const BBoxType> values) = 0;

private:
  // True when emulation is switched off (by config or by the backend), in which case reads and
  // writes go to m_fallback instead.
  static bool UseFallback();
  void RefreshFromBackend();

  bool m_active = false;

  std::array<BBoxType, NUM_BBOX_VALUES> m_cache = {};
  std::array<bool, NUM_BBOX_VALUES> m_pending_write = {};
  bool m_cache_valid = true;

  // Without real emulation, values written by the game are simply echoed back. Games built on
  // the official SDK reset the box to its extents (1023, 0, 1023, 0) before drawing and treat
  // getting those back as "nothing drawn", which is far safer than returning junk (some titles
  // crash on junk values).
  std::array<u16, 4> m_fallback = {};
};

extern std::unique_ptr<BoundingBox> g_bounding_box;
