// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <deque>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "VideoCommon/PerformanceTracker.h"

namespace Core
{
class System;
}

// Frame rate, v-blank rate and emulation speed, and the ImGui overlay that shows them.
class PerformanceMetrics
{
public:
  PerformanceMetrics();
  ~PerformanceMetrics() = default;

  PerformanceMetrics(const PerformanceMetrics&) = delete;
  PerformanceMetrics& operator=(const PerformanceMetrics&) = delete;
  PerformanceMetrics(PerformanceMetrics&&) = delete;
  PerformanceMetrics& operator=(PerformanceMetrics&&) = delete;

  void Reset();

  void CountFrame();
  void CountVBlank();

  // CPU thread. Time spent sleeping by the throttle, a change of the emulated CPU clock (the
  // recorded tick counts are rescaled around `ticks`), and a speed measurement point.
  void CountThrottleSleep(DT sleep);
  void AdjustClockSpeed(s64 ticks, u32 new_ppc_clock, u32 old_ppc_clock);
  void CountPerformanceMarker(s64 ticks, u32 ticks_per_second);

  // Any thread.
  double GetFPS() const;
  double GetVPS() const;
  // Emulated time over real time, and over real time minus the throttle's sleeps.
  double GetSpeed() const;
  double GetMaxSpeed() const;
  void SetLatestFramePresentationOffset(DT offset);
  void SetLatestFrameBufferSize(u32 width, u32 height);

  // Emits the overlay windows (graphs, speed, FPS, VPS, XFB size) as configured.
  void DrawImGuiStats(const float backbuffer_scale);

private:
  struct FrameBufferSize
  {
    FrameBufferSize() : width(0), height(0) {}
    FrameBufferSize(u32 w, u32 h) : width(w), height(h) {}
    u32 width;
    u32 height;
  };

  struct OverlayLayout;
  void DrawGraphs(OverlayLayout& layout);

  PerformanceTracker m_fps_counter{"render_times.txt"};
  PerformanceTracker m_vps_counter{"vblank_times.txt"};

  // Smoothed top of the graph's time axis, in ms.
  double m_graph_ceiling_ms = 0.0;

  std::atomic<double> m_speed{};
  std::atomic<double> m_max_speed{};

  std::atomic<DT> m_frame_presentation_offset{};
  std::atomic<FrameBufferSize> m_frame_buffer_size{};

  // A speed measurement point: real time, real time without the throttle's sleeps, and the
  // emulated time in CPU ticks.
  struct Marker
  {
    TimePoint wall;
    TimePoint busy;
    s64 ticks;
  };
  std::deque<Marker> m_markers;
  DT m_slept{};

  Common::EventHook m_state_hook;
};
