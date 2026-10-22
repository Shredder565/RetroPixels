// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PerformanceMetrics.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <imgui.h>
#include <implot.h>

#include "Common/HookableEvent.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr float WINDOW_OPACITY = 0.7f;

// Refresh rates whose frame times get a grid line on the graphs.
constexpr std::array<double, 14> GRID_RATES_HZ = {360.0, 240.0, 180.0, 120.0, 90.0, 59.94, 40.0,
                                                  29.97, 24.0,  20.0,  15.0,  10.0, 5.0,   2.0};

// Graph grid lines, in ms: 0, the frame time of each rate above, then 1 s and 2 s.
constexpr std::array<double, GRID_RATES_HZ.size() + 3> GRAPH_TICKS_MS = [] {
  std::array<double, GRID_RATES_HZ.size() + 3> ticks{};
  std::size_t next = 0;
  ticks[next++] = 0.0;
  for (const double hz : GRID_RATES_HZ)
    ticks[next++] = 1000.0 / hz;
  ticks[next++] = 1000.0;
  ticks[next] = 2000.0;
  return ticks;
}();
}  // namespace

// Where the next overlay window goes and how windows are placed this frame. Windows line up
// from the top-right corner, to the left of each other, or downwards when the graphs are off.
struct PerformanceMetrics::OverlayLayout
{
  float scale;
  float padding;
  ImVec2 display;
  ImGuiWindowFlags move_flag;
  ImGuiCond position_cond;
  ImGuiCond size_cond;
  bool vertical;
  float x;
  float y;

  // Moves the current window back inside the display (minus the padding) when it was dragged
  // out. Windows bigger than the display are left alone.
  void KeepCurrentWindowInside() const
  {
    const ImVec2 at = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float low_x = padding;
    const float high_x = display.x - padding - size.x;
    const float low_y = padding;
    const float high_y = display.y - padding - size.y;
    if (low_x > high_x || low_y > high_y)
      return;

    const float fixed_x = std::clamp(at.x, low_x, high_x);
    const float fixed_y = std::clamp(at.y, low_y, high_y);
    if (fixed_x != at.x || fixed_y != at.y)
      ImGui::SetWindowPos(ImVec2(fixed_x, fixed_y), ImGuiCond_Always);
  }

  // Anchors the next window's top-right corner at the current slot.
  void PlaceNextWindow() const
  {
    ImGui::SetNextWindowPos(ImVec2(x, y), position_cond, ImVec2(1.0f, 0.0f));
  }

  // Called inside a stat window: moves the slot for the next one past this window.
  void AdvancePastCurrentWindow()
  {
    if (vertical)
      y += ImGui::GetWindowHeight() + padding;
    else
      x -= ImGui::GetWindowWidth() + padding;
  }
};

PerformanceMetrics::PerformanceMetrics()
{
  // A state change (pause, unpause, ...) makes the next interval meaningless.
  m_state_hook = Core::AddOnStateChangedCallback([this](Core::State) {
    m_fps_counter.InvalidateLastTime();
    m_vps_counter.InvalidateLastTime();
  });
}

void PerformanceMetrics::Reset()
{
  m_fps_counter.Reset();
  m_vps_counter.Reset();

  m_markers.clear();
  m_slept = DT::zero();
  m_speed = 0;
  m_max_speed = 0;
  m_frame_presentation_offset = DT{};
}

void PerformanceMetrics::AdjustClockSpeed(s64 ticks, u32 new_ppc_clock, u32 old_ppc_clock)
{
  // Keep the emulated time between each marker and `ticks` the same at the new clock rate.
  for (Marker& marker : m_markers)
  {
    const s64 rescaled = (marker.ticks - ticks) * new_ppc_clock / old_ppc_clock;
    marker.ticks = ticks + rescaled;
  }
}

void PerformanceMetrics::CountPerformanceMarker(s64 core_ticks, u32 ticks_per_second)
{
  const TimePoint wall = Clock::now();
  const TimePoint busy = wall - m_slept;
  m_markers.push_back(Marker{wall, busy, core_ticks});

  // Measure over the configured sample period.
  const std::chrono::microseconds period{g_ActiveConfig.iPerfSampleUSec};
  while (wall - m_markers.front().wall > period)
    m_markers.pop_front();

  // One marker is not an interval yet.
  if (m_markers.size() < 2)
    return;

  const Marker& first = m_markers.front();
  const DT_s emulated = DT_s(core_ticks - first.ticks) / ticks_per_second;
  m_speed.store(emulated / (wall - first.wall), std::memory_order_relaxed);
  m_max_speed.store(emulated / (busy - first.busy), std::memory_order_relaxed);
}

// Event counters fed by the video and core threads.

void PerformanceMetrics::CountThrottleSleep(DT sleep)
{
  m_slept += sleep;
}

void PerformanceMetrics::CountVBlank()
{
  m_vps_counter.Count();
}

void PerformanceMetrics::CountFrame()
{
  m_fps_counter.Count();
}

// Values published by the presentation code for the stats window.

void PerformanceMetrics::SetLatestFrameBufferSize(u32 width, u32 height)
{
  m_frame_buffer_size.store(FrameBufferSize{width, height}, std::memory_order_relaxed);
}

void PerformanceMetrics::SetLatestFramePresentationOffset(DT offset)
{
  m_frame_presentation_offset.store(offset, std::memory_order_relaxed);
}

// Readers; safe from any thread.

double PerformanceMetrics::GetSpeed() const
{
  return m_speed.load(std::memory_order_relaxed);
}

double PerformanceMetrics::GetMaxSpeed() const
{
  return m_max_speed.load(std::memory_order_relaxed);
}

double PerformanceMetrics::GetVPS() const
{
  return m_vps_counter.GetHzAvg();
}

double PerformanceMetrics::GetFPS() const
{
  return m_fps_counter.GetHzAvg();
}

void PerformanceMetrics::DrawGraphs(OverlayLayout& layout)
{
  // 13 px keeps the axis numbers from running into each other.
  ImGui::PushFont(nullptr, 13.0f);
  ImGui::PushStyleColor(ImGuiCol_ResizeGrip, 0);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                                 layout.move_flag | ImGuiWindowFlags_NoFocusOnAppearing;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 4.f * layout.scale));

  // Default size: a quarter of the display, but never smaller than 200x144 (scaled) plus padding.
  const float width = std::max(200.f * layout.scale + 2.f * layout.padding, layout.display.x / 4.f);
  const float height =
      std::max(144.f * layout.scale + 2.f * layout.padding, layout.display.y / 4.f);

  layout.PlaceNextWindow();
  ImGui::SetNextWindowSize(ImVec2(width, height), layout.size_cond);
  ImGui::SetNextWindowBgAlpha(WINDOW_OPACITY);
  if (ImGui::Begin("PerformanceGraphs", nullptr, flags))
  {
    layout.KeepCurrentWindowInside();
    // The other windows always go below the graphs.
    layout.y += ImGui::GetWindowHeight() + layout.padding;

    // The time axis tops out at about mean + 2 sigma of a v-blank plus a frame, smoothed.
    const DT vblank_span = m_vps_counter.GetDtAvg() + 2 * m_vps_counter.GetDtStd();
    const DT frame_span = m_fps_counter.GetDtAvg() + 2 * m_fps_counter.GetDtStd();
    const double target_ms = DT_ms(vblank_span + frame_span).count();
    const double follow =
        std::max(0.0, 1.0 - std::exp(-4.0 * (DT_s(m_fps_counter.GetLastRawDt()) /
                                             DT_s(m_fps_counter.GetSampleWindow()))));
    if (std::isfinite(m_graph_ceiling_ms))
      m_graph_ceiling_ms += follow * (target_ms - m_graph_ceiling_ms);
    else
      m_graph_ceiling_ms = target_ms;

    const double span_ms =
        DT_ms(std::max(m_fps_counter.GetSampleWindow(), m_vps_counter.GetSampleWindow())).count();

    constexpr ImPlotFlags plot_flags =
        ImPlotFlags_NoFrame | ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot("PerformanceGraphs", ImVec2(-1.0, -1.0), plot_flags))
    {
      ImPlot::PushStyleColor(ImPlotCol_PlotBg, {0, 0, 0, 0});
      ImPlot::PushStyleColor(ImPlotCol_LegendBg, {0, 0, 0, 0.2f});
      ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0.f, 0.f));
      ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 1.5f * layout.scale);

      // Time runs right to left on x; y grows downwards.
      const ImPlotAxisFlags common_axis = ImPlotAxisFlags_Lock | ImPlotAxisFlags_Invert;
      ImPlot::SetupAxes(nullptr, nullptr,
                        common_axis | ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_NoHighlight,
                        common_axis | ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoHighlight);
      ImPlot::SetupAxisFormat(ImAxis_Y1, "%.1f");
      ImPlot::SetupAxisTicks(ImAxis_Y1, GRAPH_TICKS_MS.data(),
                             static_cast<int>(GRAPH_TICKS_MS.size()));
      ImPlot::SetupAxesLimits(0, span_ms, 0, m_graph_ceiling_ms, ImGuiCond_Always);
      ImPlot::SetupLegend(ImPlotLocation_SouthEast, ImPlotLegendFlags_None);

      m_vps_counter.ImPlotPlotLines("V-Blank (ms)");
      m_fps_counter.ImPlotPlotLines("Frame (ms)");
      ImPlot::EndPlot();

      ImPlot::PopStyleVar(2);
      ImPlot::PopStyleColor(2);
    }
    // The window padding is only restored when the window was drawn.
    ImGui::PopStyleVar();
  }
  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

void PerformanceMetrics::DrawImGuiStats(const float backbuffer_scale)
{
  m_vps_counter.UpdateStats();
  m_fps_counter.UpdateStats();

  const bool movable = Config::Get(Config::GFX_MOVABLE_PERFORMANCE_METRICS);

  const double fps = GetFPS();
  const double vps = GetVPS();
  const double speed = GetSpeed();

  // Text colour: cyan, or a red-yellow-green-cyan ramp over 80-100% speed when enabled.
  ImVec4 tint(0.0f, 1.0f, 1.0f, 1.0f);
  if (g_ActiveConfig.bShowSpeedColors)
  {
    tint.x = static_cast<float>(1.0 - (speed - 0.8) / 0.2);
    tint.y = static_cast<float>(speed / 0.8);
    tint.z = static_cast<float>((speed - 0.9) / 0.1);
  }

  // A new display size resets the windows to their default places and sizes.
  static ImVec2 previous_display(-1.0f, -1.0f);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const bool resized = display.x != previous_display.x || display.y != previous_display.y;
  previous_display = display;

  OverlayLayout layout{};
  layout.scale = backbuffer_scale;
  layout.padding = 8.f * backbuffer_scale;
  layout.display = display;
  layout.move_flag = movable ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoMove;
  layout.position_cond = (resized || !movable) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
  layout.size_cond = resized ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
  layout.vertical = !g_ActiveConfig.bShowGraphs;
  layout.x = display.x - layout.padding;
  layout.y = layout.padding;

  const ImGuiWindowFlags stat_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | layout.move_flag |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

  // One small auto-sized window per group of lines.
  const auto stat_window = [&](const char* name, const auto& lines) {
    layout.PlaceNextWindow();
    ImGui::SetNextWindowBgAlpha(WINDOW_OPACITY);
    if (ImGui::Begin(name, nullptr, stat_flags))
    {
      layout.AdvancePastCurrentWindow();
      layout.KeepCurrentWindowInside();
      lines();
    }
    ImGui::End();
  };

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.f * backbuffer_scale);

  if (g_ActiveConfig.bShowGraphs)
    DrawGraphs(layout);

  if (g_ActiveConfig.bShowSpeed)
  {
    stat_window("SpeedStats", [&] {
      ImGui::TextColored(tint, "Speed:%4.0lf%%", 100.0 * speed);
      ImGui::TextColored(tint, "Max:%6.0lf%%", 100.0 * GetMaxSpeed());
    });
  }

  if (g_ActiveConfig.bShowFPS || g_ActiveConfig.bShowFTimes)
  {
    stat_window("FPSStats", [&] {
      if (g_ActiveConfig.bShowFPS)
        ImGui::TextColored(tint, "FPS:%7.2lf", fps);
      if (g_ActiveConfig.bShowFTimes)
      {
        ImGui::TextColored(tint, "dt:%6.2lfms", DT_ms(m_fps_counter.GetDtAvg()).count());
        ImGui::TextColored(tint, " ±:%6.2lfms", DT_ms(m_fps_counter.GetDtStd()).count());
        const double offset_ms =
            DT_ms(m_frame_presentation_offset.load(std::memory_order_relaxed)).count();
        ImGui::TextColored(tint, "ofs:%5.1lfms", offset_ms);
      }
    });
  }

  if (g_ActiveConfig.bShowVPS || g_ActiveConfig.bShowVTimes)
  {
    stat_window("VPSStats", [&] {
      if (g_ActiveConfig.bShowVPS)
        ImGui::TextColored(tint, "VPS:%7.2lf", vps);
      if (g_ActiveConfig.bShowVTimes)
      {
        ImGui::TextColored(tint, "dt:%6.2lfms", DT_ms(m_vps_counter.GetDtAvg()).count());
        ImGui::TextColored(tint, " ±:%6.2lfms", DT_ms(m_vps_counter.GetDtStd()).count());
      }
    });
  }

  if (g_ActiveConfig.bShowInternalResolution)
  {
    stat_window("ResolutionStats", [&] {
      const FrameBufferSize xfb = m_frame_buffer_size.load(std::memory_order_relaxed);
      ImGui::TextColored(tint, "XFB res: %ux%u", xfb.width, xfb.height);
    });
  }

  ImGui::PopStyleVar(2);
}
