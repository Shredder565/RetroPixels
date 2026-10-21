// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <deque>
#include <fstream>
#include <optional>

#include "Common/CommonTypes.h"
#include "Common/SPSCQueue.h"

// Measures the rate of a recurring event (presented frames, v-blanks): the time between
// consecutive Count() calls is averaged over a sliding window.
class PerformanceTracker
{
public:
  // `log_name`: file in the log folder that each interval is appended to when render-time
  // logging is on. `sample_window_duration`: fixed averaging window; by default the window is
  // the configured performance sample period.
  PerformanceTracker(std::optional<std::string> log_name = std::nullopt,
                     const std::optional<DT> sample_window_duration = std::nullopt);
  ~PerformanceTracker() = default;

  PerformanceTracker(const PerformanceTracker&) = delete;
  PerformanceTracker& operator=(const PerformanceTracker&) = delete;
  PerformanceTracker(PerformanceTracker&&) = delete;
  PerformanceTracker& operator=(PerformanceTracker&&) = delete;

  void Reset();

  // Consumes the intervals recorded by Count() and refreshes the averages. Always call it from
  // the same thread, and regularly, so the queue doesn't grow.
  void UpdateStats();
  // Plots the intervals of the window with ImPlot.
  void ImPlotPlotLines(const char* label) const;

  // Records one occurrence of the event. Any thread, one at a time.
  void Count();

  // Readers, safe from any thread.
  DT GetSampleWindow() const;
  double GetHzAvg() const;
  DT GetDtAvg() const;
  DT GetDtStd() const;
  DT GetLastRawDt() const;
  // The next Count() only restarts the measurement (used after pauses).
  void InvalidateLastTime();

private:
  void TakeInterval(DT interval);
  void AddNewest(DT interval);
  void DropOldest();
  void AppendToLog(DT interval);

  std::optional<std::string> m_log_name;
  std::ofstream m_log_file;

  // Time of the previous Count(), and whether it can be used as the start of an interval.
  TimePoint m_previous_count;
  std::atomic<bool> m_have_previous = false;

  // Intervals from Count() waiting for UpdateStats().
  Common::SPSCQueue<DT> m_incoming;
  std::atomic<DT> m_latest_interval = DT::zero();

  const std::optional<DT> m_fixed_window;

  // The intervals inside the window, newest first, and their sum.
  std::deque<DT> m_window;
  DT m_window_sum = DT::zero();

  // Published results: mean interval, smoothed rate and the intervals' standard deviation.
  std::atomic<DT> m_mean_interval = DT::zero();
  std::atomic<double> m_smoothed_hz = 0.0;
  std::atomic<DT> m_interval_stddev = DT::zero();
};
