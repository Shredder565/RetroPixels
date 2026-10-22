// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PerformanceTracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <utility>

#include <implot.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/MathUtil.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// The smoothing time constant of the rate is this fraction of the window.
constexpr double SMOOTHING_FRACTION = 0.25;
// Upper bound on the number of intervals kept.
constexpr std::size_t WINDOW_CAPACITY = std::size_t{1} << 12;
// Below this many intervals each one is plotted as a step (two points), above it as one point.
constexpr std::size_t STEP_PLOT_LIMIT = std::size_t{1} << 8;
static_assert(2 * STEP_PLOT_LIMIT <= WINDOW_CAPACITY);

// Plot scratch space: one point per interval, two in step mode, plus the live point.
std::array<float, WINDOW_CAPACITY + 1> s_plot_x;
std::array<float, WINDOW_CAPACITY + 1> s_plot_y;
}  // namespace

PerformanceTracker::PerformanceTracker(std::optional<std::string> log_name,
                                       const std::optional<DT> sample_window_duration)
    : m_log_name{std::move(log_name)}, m_fixed_window{sample_window_duration}
{
  Reset();
}

void PerformanceTracker::Reset()
{
  m_incoming.Clear();
  m_window.clear();
  m_window_sum = DT::zero();
  m_latest_interval = DT::zero();
  m_previous_count = Clock::now();
  m_smoothed_hz = 0.0;
  m_mean_interval = DT::zero();
  m_interval_stddev = DT::zero();
  m_have_previous = false;
}

void PerformanceTracker::Count()
{
  const TimePoint now = Clock::now();
  const DT interval = now - m_previous_count;
  m_previous_count = now;

  // The first call after a reset (or a pause) has nothing to measure against.
  if (!m_have_previous.exchange(true))
    return;

  m_latest_interval = interval;
  m_incoming.Push(interval);
}

void PerformanceTracker::UpdateStats()
{
  DT interval{};
  while (m_incoming.Pop(interval))
    TakeInterval(interval);

  MathUtil::RunningVariance<double> spread;
  for (const DT sample : m_window)
    spread.Push(DT_s(sample).count());
  m_interval_stddev = std::chrono::duration_cast<DT>(DT_s(spread.PopulationStandardDeviation()));
}

void PerformanceTracker::TakeInterval(DT interval)
{
  if (m_window.size() == WINDOW_CAPACITY)
    DropOldest();
  AddNewest(interval);

  // Keep the intervals that end inside the window (the newest one always stays).
  const DT window = GetSampleWindow();
  while (m_window_sum - m_window.back() >= window)
    DropOldest();

  const DT mean = m_window_sum / m_window.size();
  const double rate = DT_s(1.0) / mean;
  m_mean_interval = mean;

  // Exponential smoothing whose time constant follows the window (or the data, while the window
  // is not full yet).
  const DT_s time_constant = SMOOTHING_FRACTION * std::min(window, m_window_sum);
  const double weight = 1.0 - std::exp(-(DT_s(interval) / time_constant));

  const double previous = m_smoothed_hz.load();
  // Restart from the plain rate if the smoothed value went infinite or NaN.
  m_smoothed_hz = std::isfinite(previous) ? previous + weight * (rate - previous) : rate;

  AppendToLog(interval);
}

DT PerformanceTracker::GetSampleWindow() const
{
  if (m_fixed_window)
    return *m_fixed_window;
  const int period_us = std::max(1, g_ActiveConfig.iPerfSampleUSec);
  return std::chrono::duration_cast<DT>(DT_us{period_us});
}

double PerformanceTracker::GetHzAvg() const
{
  return m_smoothed_hz;
}

DT PerformanceTracker::GetDtAvg() const
{
  return m_mean_interval;
}

DT PerformanceTracker::GetDtStd() const
{
  return m_interval_stddev;
}

DT PerformanceTracker::GetLastRawDt() const
{
  return m_latest_interval;
}

void PerformanceTracker::InvalidateLastTime()
{
  m_have_previous = false;
}

void PerformanceTracker::ImPlotPlotLines(const char* label) const
{
  if (m_window.empty())
    return;

  // x runs right to left: 0 is now, each interval moves one interval's length further back.
  const bool steps = m_window.size() < STEP_PLOT_LIMIT;
  std::size_t count = 0;
  const auto plot = [steps, &count](DT height, DT width, float start_ms) {
    const float height_ms = static_cast<float>(DT_ms{height}.count());
    if (steps)
    {
      s_plot_x[count] = start_ms;
      s_plot_y[count] = height_ms;
      ++count;
    }
    s_plot_x[count] = static_cast<float>(start_ms + DT_ms{width}.count());
    s_plot_y[count] = height_ms;
    ++count;
  };

  // The frame in progress: at least as long as the time since the last Count().
  const DT since_last = Clock::now() - m_previous_count;
  plot(std::max(since_last, m_window.front()), DT{}, 0);

  for (const DT interval : m_window)
    plot(interval, interval, s_plot_x[count - 1]);

  ImPlot::PlotLine(label, s_plot_x.data(), s_plot_y.data(), static_cast<int>(count));
}

void PerformanceTracker::AddNewest(DT interval)
{
  m_window.push_front(interval);
  m_window_sum += interval;
}

void PerformanceTracker::DropOldest()
{
  m_window_sum -= m_window.back();
  m_window.pop_back();
}

void PerformanceTracker::AppendToLog(DT interval)
{
  if (!m_log_name || !g_ActiveConfig.bLogRenderTimeToFile)
    return;

  if (!m_log_file.is_open())
  {
    File::OpenFStream(m_log_file, File::GetUserPath(D_LOGS_IDX) + *m_log_name, std::ios_base::out);
  }

  m_log_file << std::fixed << std::setprecision(8) << DT_ms(interval).count() << std::endl;
}
