// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Flag.h"
#include "Common/MathUtil.h"

#include "VideoCommon/OnScreenUIKeyMap.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoCommon.h"

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <tuple>

class AbstractTexture;
struct SurfaceInfo;
enum class DolphinKey;

namespace VideoCommon
{
class OnScreenUI;
class PostProcessing;

// Shows emulated frames on the host window: picks up the XFB the guest scans out, works out
// where in the window it goes (aspect ratio, crop, integer sizes), draws it through the
// post-processor, adds the on-screen UI and presents. Also feeds frame dumping.
class Presenter
{
public:
  using ClearColor = std::array<float, 4>;

  Presenter();
  virtual ~Presenter();

  // A VI scan-out of the XFB at xfb_addr.
  void ViSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks,
              TimePoint presentation_time);
  // An XFB copy shown straight away ("immediate XFB").
  void ImmediateSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height);

  // Timing to use for immediate swaps, which have none of their own.
  void SetNextSwapEstimatedTime(u64 ticks, TimePoint host_time);

  void Present(PresentInfo* present_info = nullptr);
  void ClearLastXfbId() { m_last_xfb_id = std::numeric_limits<u64>::max(); }

  bool Initialize();

  void ConfigChanged(u32 changed_bits);

  // The window (or display, when fullscreen) surface.
  int GetBackbufferWidth() const { return m_backbuffer_width; }
  int GetBackbufferHeight() const { return m_backbuffer_height; }
  float GetBackbufferScale() const { return m_backbuffer_scale; }
  u32 AutoIntegralScale() const;
  AbstractTextureFormat GetBackbufferFormat() const { return m_backbuffer_format; }
  void SetSuggestedWindowSize(int width, int height);
  void SetBackbuffer(int backbuffer_width, int backbuffer_height);
  void SetBackbuffer(SurfaceInfo info);
  void OnBackbufferSet(bool size_changed, bool is_first_set);

  void UpdateDrawRectangle();

  // Pixels to remove from each side of rect for the custom crop (left/top/right/bottom hold
  // the amounts, not coordinates).
  MathUtil::Rectangle<int> GetCustomCrop(const MathUtil::Rectangle<int>& rect) const;

  // rect with the custom crop removed.
  MathUtil::Rectangle<int> AdjustForCustomCrop(const MathUtil::Rectangle<int>& rect) const;

  // input_aspect_ratio changed the way the custom crop changes the shape of the XFB.
  float AdjustAspectRatioForCustomCrop(float input_aspect_ratio) const;

  // The aspect ratio the XFB is shown at.
  float CalculateDrawAspectRatio(bool allow_stretch = true) const;

  // Clips target_rect to a fb_width x fb_height surface and shrinks source_rect by the matching
  // (scaled) amount.
  void AdjustRectanglesToFitBounds(MathUtil::Rectangle<int>* target_rect,
                                   MathUtil::Rectangle<int>* source_rect, int fb_width,
                                   int fb_height);

  void ReleaseXFBContentLock();

  // Draws source_rc of source_texture to target_rc of the bound (and cleared) backbuffer,
  // through the post-processor, handling the stereo layouts.
  virtual void RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                                 const AbstractTexture* source_texture,
                                 const MathUtil::Rectangle<int>& source_rc);

  VideoCommon::PostProcessing* GetPostProcessor() const { return m_post_processor.get(); }

  // Window system notifications, picked up by the backend at its next present.
  void ChangeSurface(void* new_surface_handle);
  void ResizeSurface();
  bool SurfaceResizedTestAndClear() { return m_surface_resized.TestAndClear(); }
  bool SurfaceChangedTestAndClear() { return m_surface_changed.TestAndClear(); }
  void* GetNewSurfaceHandle();

  // Input for the on-screen UI.
  void SetKeyMap(const DolphinKeyMap& key_map);
  void SetKey(u32 key, bool is_down, const char* chars);
  void SetMousePos(float x, float y);
  void SetMousePress(u32 button_mask);

  int FrameCount() const { return static_cast<int>(m_frame_count); }

  void DoState(PointerWrap& p);

  const MathUtil::Rectangle<int>& GetTargetRectangle() const { return m_target_rectangle; }

private:
  // Takes the XFB from the texture cache. Returns true when it is the same one as last time.
  bool FetchXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks);

  void ProcessFrameDumping(u64 ticks) const;

  void OnBackBufferSizeChanged();

  // A width x height XFB stretched to the draw aspect ratio, then cropped and rounded to whole
  // pixels.
  std::tuple<int, int> CalculateOutputDimensions(int width, int height,
                                                 bool allow_stretch = true) const;
  // Crops to an exact 4:3 / 16:9 / custom shape when "crop to aspect ratio" is on.
  std::tuple<float, float> ApplyStandardAspectCrop(float width, float height,
                                                   bool allow_stretch = true) const;
  // Grows one side of width x height to reach the draw aspect ratio.
  std::tuple<float, float> ScaleToDisplayAspectRatio(int width, int height,
                                                     bool allow_stretch = true) const;

  // The two eye rectangles for side-by-side or top-and-bottom stereo.
  std::tuple<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
  ConvertStereoRectangle(const MathUtil::Rectangle<int>& rc) const;

  // The present time to sleep until, with SmoothEarlyPresentation's running offset applied.
  TimePoint GetUpdatedPresentationTime(TimePoint intended_presentation_time);

  std::mutex m_swap_mutex;

  int m_backbuffer_width = 0;
  int m_backbuffer_height = 0;
  float m_backbuffer_scale = 1.0f;
  AbstractTextureFormat m_backbuffer_format = AbstractTextureFormat::Undefined;

  void* m_new_surface_handle = nullptr;
  Common::Flag m_surface_changed;
  Common::Flag m_surface_resized;

  // Where the XFB goes in the window; anything outside it is black bars.
  MathUtil::Rectangle<int> m_target_rectangle = {};

  u32 m_auto_resolution_scale = 1;

  RcTcacheEntry m_xfb_entry;
  // The XFB's area in its texture (at internal resolution).
  MathUtil::Rectangle<int> m_xfb_rect{0, 0, MAX_XFB_WIDTH, MAX_XFB_HEIGHT};

  // Identifies the shown XFB, to spot repeated frames.
  u64 m_last_xfb_id = std::numeric_limits<u64>::max();

  // The last size asked of the window system (0 until the first request).
  int m_last_window_request_width = 0;
  int m_last_window_request_height = 0;

  std::unique_ptr<VideoCommon::PostProcessing> m_post_processor;
  std::unique_ptr<VideoCommon::OnScreenUI> m_onscreen_ui;

  u64 m_frame_count = 0;
  u64 m_present_count = 0;

  // The last XFB requested by the guest (native sizes); saved in savestates.
  u64 m_last_xfb_ticks = 0;
  u32 m_last_xfb_addr = 0;
  u32 m_last_xfb_width = MAX_XFB_WIDTH;
  u32 m_last_xfb_stride = 0;
  u32 m_last_xfb_height = MAX_XFB_HEIGHT;

  Common::EventHook m_config_changed;
  Common::EventHook m_end_field_hook;

  // Running offset for SmoothEarlyPresentation.
  DT m_presentation_time_offset{};

  // Expected timing of the next VI swap.
  u64 m_next_swap_estimated_ticks = 0;
  TimePoint m_next_swap_estimated_time{Clock::now()};

  // Set by an immediate swap, cleared at each VI field end (see GFX_HACK_CAP_IMMEDIATE_XFB).
  std::atomic_bool m_immediate_swap_happened_this_field{};
};

}  // namespace VideoCommon

extern std::unique_ptr<VideoCommon::Presenter> g_presenter;
