// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Timer.h"

#include <algorithm>
#include <ctime>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#else
#include <time.h>
#endif

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/TimeUtil.h"

namespace Common
{
namespace
{
// The scheduler granularity IncreaseResolution() asks for, and what SleepUntil() assumes.
constexpr int SCHEDULER_PERIOD_MS = 1;

template <typename Unit>
u64 SteadyNow()
{
  using namespace std::chrono;
  return static_cast<u64>(duration_cast<Unit>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

u64 Timer::NowUs()
{
  return SteadyNow<std::chrono::microseconds>();
}

u64 Timer::NowMs()
{
  return SteadyNow<std::chrono::milliseconds>();
}

void Timer::Stop()
{
  m_running = false;
  m_end_ms = NowMs();
}

// Starting with an offset makes the timer read as if it had been started `offset` ms earlier.
void Timer::StartWithOffset(u64 offset)
{
  m_running = true;
  m_end_ms = 0;
  m_start_ms = NowMs() - offset;
}

void Timer::Start()
{
  StartWithOffset(0);
}

u64 Timer::ElapsedMs() const
{
  // Unsigned subtraction also gives the right answer across one wrap-around; a timer that was
  // never started has start == end == 0.
  const u64 end = m_running ? NowMs() : m_end_ms;
  return end - m_start_ms;
}

u64 Timer::GetLocalTimeSinceJan1970()
{
  using namespace std::chrono;
  const std::time_t now = system_clock::to_time_t(system_clock::now());
  const std::optional<std::tm> local = LocalTime(now);
  if (!local)
    return static_cast<u64>(now);

  // Read the local calendar date and time back as if it were UTC.
  const year_month_day date{year{local->tm_year + 1900},
                            month{static_cast<unsigned>(local->tm_mon + 1)},
                            day{static_cast<unsigned>(local->tm_mday)}};
  const seconds since_epoch = sys_days{date}.time_since_epoch() + hours{local->tm_hour} +
                              minutes{local->tm_min} + seconds{local->tm_sec};
  return static_cast<u64>(since_epoch.count());
}

void Timer::IncreaseResolution()
{
#ifdef _WIN32
  // Opt out of EcoQoS: without this, Windows may slow the process down and coarsen its timers
  // while it is in the background or on efficiency cores. Best effort; never undone.
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask =
      PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;  // i.e. throttling off for the bits in ControlMask
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling,
                        sizeof(throttling));

  timeBeginPeriod(SCHEDULER_PERIOD_MS);
#endif
}

void Timer::RestoreResolution()
{
#ifdef _WIN32
  timeEndPeriod(SCHEDULER_PERIOD_MS);
#endif
}

PrecisionTimer::PrecisionTimer()
{
#ifdef _WIN32
  // High-resolution waitable timers exist since Windows 10 1803; fall back to a normal one.
  m_timer_handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
  if (!m_timer_handle)
  {
    ERROR_LOG_FMT(COMMON, "CREATE_WAITABLE_TIMER_HIGH_RESOLUTION: Error:{}", GetLastError());
    m_timer_handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (!m_timer_handle)
      ERROR_LOG_FMT(COMMON, "CreateWaitableTimerExW: Error:{}", GetLastError());
  }
#endif
}

PrecisionTimer::~PrecisionTimer()
{
#ifdef _WIN32
  if (m_timer_handle)
    CloseHandle(m_timer_handle);
#endif
}

void PrecisionTimer::SleepUntil(Clock::time_point target)
{
  using namespace std::chrono;

  // The last stretch (a scheduler period plus a little) is spun, since a sleep may overshoot by
  // about that much.
  constexpr auto spin_window = milliseconds{SCHEDULER_PERIOD_MS} + microseconds{20};

#ifdef _WIN32
  // Waitable timer due times are in 100 ns units. Long single waits turned out to be inaccurate,
  // so wait in slices of at most 95% of a scheduler period.
  using Ticks100ns = duration<LONGLONG, std::ratio<1, 10'000'000>>;
  constexpr Ticks100ns max_slice = duration_cast<Ticks100ns>(milliseconds{SCHEDULER_PERIOD_MS}) *
                                   95 / 100;
  for (;;)
  {
    const Ticks100ns remaining = duration_cast<Ticks100ns>(target - Clock::now() - spin_window);
    const LONGLONG ticks = std::min(remaining, max_slice).count();
    if (ticks <= 0)
      break;
    LARGE_INTEGER due{};
    due.QuadPart = -ticks;  // negative = relative to now
    SetWaitableTimerEx(m_timer_handle, &due, 0, nullptr, nullptr, nullptr, 0);
    WaitForSingleObject(m_timer_handle, INFINITE);
  }
#else
  std::this_thread::sleep_until(target - spin_window);
#endif

  while (Clock::now() < target)
  {
#ifdef _WIN32
    YieldProcessor();
#else
    std::this_thread::yield();
#endif
  }
}

auto SteadyAwakeClock::now() -> time_point
{
#ifdef _WIN32
  // Unbiased interrupt time excludes time spent asleep or hibernating; 100 ns units.
  ULONGLONG ticks = 0;
  if (!QueryUnbiasedInterruptTime(&ticks))
    ERROR_LOG_FMT(COMMON, "QueryUnbiasedInterruptTime");
  return time_point{duration{static_cast<rep>(ticks) * 100}};
#else
  // Linux's CLOCK_MONOTONIC does not advance during suspend.
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    ERROR_LOG_FMT(COMMON, "clock_gettime: {}", LastStrerrorString());
  return time_point{std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec}};
#endif
}
}  // namespace Common
