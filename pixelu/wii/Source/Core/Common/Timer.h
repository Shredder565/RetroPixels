// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <ratio>

#include "Common/CommonTypes.h"

namespace Common
{
// Millisecond stopwatch on the steady clock, plus some process-wide time helpers.
class Timer
{
public:
  // Steady-clock readings (arbitrary epoch).
  static u64 NowUs();
  static u64 NowMs();

  void Start();
  // Start() as if the timer had already been running for `offset` ms (resumes a stopped total).
  void StartWithOffset(u64 offset);
  void Stop();
  // Time between Start() and Stop() (or now, while running). 0 if never started.
  u64 ElapsedMs() const;

  // Seconds since 1970-01-01 00:00:00 in *local* time, i.e. a Unix timestamp shifted by the
  // local UTC offset (including daylight saving). Used to seed the emulated real-time clock.
  static u64 GetLocalTimeSinceJan1970();

  // Ask the OS for 1 ms scheduler granularity and no background power throttling (Windows).
  static void IncreaseResolution();
  static void RestoreResolution();

private:
  u64 m_start_ms{0};
  u64 m_end_ms{0};
  bool m_running{false};
};

// Sleeps until a steady-clock time point with sub-millisecond accuracy: it sleeps on a
// high-resolution waitable timer for most of the interval and spins for the rest.
class PrecisionTimer
{
public:
  PrecisionTimer();
  ~PrecisionTimer();

  PrecisionTimer(const PrecisionTimer&) = delete;
  PrecisionTimer& operator=(const PrecisionTimer&) = delete;

  void SleepUntil(Clock::time_point);

private:
#ifdef _WIN32
  void* m_timer_handle;  // a waitable timer HANDLE (kept as void* to avoid windows.h here)
#endif
};

// A steady clock that stands still while the machine is asleep or suspended, unlike
// std::chrono::steady_clock.
class SteadyAwakeClock
{
public:
  using rep = s64;
  using period = std::nano;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<SteadyAwakeClock>;

  static constexpr bool is_steady = true;

  static time_point now();
};
}  // namespace Common
