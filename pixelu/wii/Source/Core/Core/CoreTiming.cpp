// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/CoreTiming.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/SPSCQueue.h"
#include "Common/ScopeGuard.h"

#include "Core/AchievementManager.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

namespace CoreTiming
{
namespace
{
// Longest stretch the CPU runs before the scheduler looks at the queue again.
constexpr int MAX_SLICE_LENGTH = 20000;

// The heap keeps the earliest event at the front, hence "greater".
constexpr std::ranges::greater HEAP_ORDER{};

void DiscardLostEvent(Core::System&, u64, s64)
{
}
}  // namespace

CoreTimingManager::CoreTimingManager(Core::System& system) : m_system(system)
{
}

// Overclocking keeps the emulated clock rate unchanged and instead lets one
// unit of downcount stand for more (or fewer) cycles, so code that waits a
// fixed number of cycles still behaves.
int CoreTimingManager::DowncountToCycles(int downcount) const
{
  return static_cast<int>(downcount * m_globals.last_OC_factor_inverted);
}

int CoreTimingManager::CyclesToDowncount(int cycles) const
{
  return static_cast<int>(cycles * m_last_oc_factor);
}

void CoreTimingManager::PushEvent(const Event& event)
{
  m_event_queue.push_back(event);
  std::ranges::push_heap(m_event_queue, HEAP_ORDER);
}

void CoreTimingManager::RebuildHeap()
{
  std::ranges::make_heap(m_event_queue, HEAP_ORDER);
}

EventType* CoreTimingManager::RegisterEvent(const std::string& name, TimedCallback callback)
{
  // Savestates refer to event types by name, so a name may only be used once.
  ASSERT_MSG(POWERPC, !m_event_types.contains(name),
             "Event type \"{}\" registered twice; register event types only during Init.", name);

  const auto [it, inserted] = m_event_types.try_emplace(name, std::move(callback), nullptr);
  EventType& type = it->second;
  type.name = &it->first;
  return &type;
}

void CoreTimingManager::UnregisterAllEvents()
{
  ASSERT_MSG(POWERPC, m_event_queue.empty(), "Event types unregistered while events are queued");
  m_event_types.clear();
}

void CoreTimingManager::Init()
{
  auto& ppc_state = m_system.GetPPCState();
  ppc_state.downcount = CyclesToDowncount(MAX_SLICE_LENGTH);
  m_globals.slice_length = MAX_SLICE_LENGTH;
  m_globals.global_timer = 0;
  m_idled_cycles = 0;

  // Between Init and the first Advance the CPU sits on the boundary of slice -1.
  m_is_global_timer_sane = true;

  ResetThrottle(0);

  m_event_fifo_id = 0;
  m_ev_lost = RegisterEvent("_lost_event", &DiscardLostEvent);

  m_registered_config_callback_id =
      CPUThreadConfigCallback::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();

  m_last_oc_factor = m_config_oc_factor;
  m_globals.last_OC_factor_inverted = m_config_oc_inv_factor;

  // Time spent paused must not be "caught up" by running flat out afterwards.
  m_core_state_changed_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    if (state == Core::State::Running)
      ResetThrottle(GetTicks());
  });

  m_throttled_after_presentation = false;
  m_frame_hook = m_system.GetVideoEvents().after_present_event.Register(
      [this](const PresentInfo&) { m_throttled_after_presentation.store(false); });
}

void CoreTimingManager::Shutdown()
{
  m_core_state_changed_hook.reset();

  std::lock_guard guard(m_ts_write_lock);
  MoveEvents();
  ClearPendingEvents();
  UnregisterAllEvents();
  CPUThreadConfigCallback::RemoveConfigChangedCallback(m_registered_config_callback_id);
  m_frame_hook.reset();
}

void CoreTimingManager::RefreshConfig()
{
  const float cpu_factor =
      Config::Get(Config::MAIN_OVERCLOCK_ENABLE) ? Config::Get(Config::MAIN_OVERCLOCK) : 1.0f;
  const float vi_factor =
      Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE) ? Config::Get(Config::MAIN_VI_OVERCLOCK) : 1.0f;
  m_config_oc_factor = cpu_factor * vi_factor;
  m_config_oc_inv_factor = 1.0f / m_config_oc_factor;
  m_config_sync_on_skip_idle = Config::Get(Config::MAIN_SYNC_ON_SKIP_IDLE);
  m_config_rush_frame_presentation = Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION);

  // Skipping more than half the audio buffer's worth of throttling would overfill it.
  m_max_throttle_skip_time =
      std::chrono::milliseconds{Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE)} / 2;

  // How far emulation may fall behind (or sleep ahead) before timings are relaxed.
  m_max_fallback = std::chrono::duration_cast<DT>(DT_ms(Config::Get(Config::MAIN_MAX_FALLBACK)));
  m_max_variance = std::chrono::duration_cast<DT>(DT_ms(Config::Get(Config::MAIN_TIMING_VARIANCE)));

  m_correct_time_drift = Config::Get(Config::MAIN_CORRECT_TIME_DRIFT);

  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    const float speed = Config::Get(Config::MAIN_EMULATION_SPEED);
    if (speed > 0.0f && speed < 1.0f)
    {
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
      OSD::AddMessage("Minimum speed is 100% in Hardcore Mode");
    }
  }

  UpdateSpeedLimit(GetTicks(), Config::Get(Config::MAIN_EMULATION_SPEED));

  m_use_precision_timer = Config::Get(Config::MAIN_PRECISION_FRAME_TIMING);
}

void CoreTimingManager::DoState(PointerWrap& p)
{
  std::lock_guard guard(m_ts_write_lock);

  p.Do(m_globals.slice_length);
  p.Do(m_globals.global_timer);
  p.Do(m_idled_cycles);
  p.Do(m_fake_dec_start_value);
  p.Do(m_fake_dec_start_ticks);
  p.Do(m_globals.fake_TB_start_value);
  p.Do(m_globals.fake_TB_start_ticks);
  p.Do(m_last_oc_factor);
  m_globals.last_OC_factor_inverted = 1.0f / m_last_oc_factor;
  p.Do(m_event_fifo_id);
  p.DoMarker("CoreTimingData");

  MoveEvents();

  // Event types are stored by name: registration order (and the set of
  // registered types) may differ between the saving and the loading run.
  p.DoEachElement(m_event_queue, [this](PointerWrap& pw, Event& event) {
    pw.Do(event.time);
    pw.Do(event.fifo_order);
    pw.Do(event.userdata);

    std::string type_name = pw.IsReadMode() ? std::string{} : *event.type->name;
    pw.Do(type_name);
    if (!pw.IsReadMode())
      return;

    const auto found = m_event_types.find(type_name);
    if (found == m_event_types.end())
    {
      WARN_LOG_FMT(POWERPC, "Savestate event of unregistered type \"{}\" dropped", type_name);
      event.type = m_ev_lost;
    }
    else
    {
      event.type = &found->second;
    }
  });
  p.DoMarker("CoreTimingEvents");

  if (!p.IsReadMode())
    return;

  // The stored order is just some heap layout; rebuild the heap from scratch.
  RebuildHeap();
  // Emulated time jumped, so the old throttle reference means nothing now.
  ResetThrottle(m_globals.global_timer);
}

u64 CoreTimingManager::GetTicks() const
{
  const u64 slice_start = static_cast<u64>(m_globals.global_timer);
  if (m_is_global_timer_sane)
    return slice_start;

  // Inside a slice: add what the CPU has already used of the slice budget.
  const int remaining = DowncountToCycles(m_system.GetPPCState().downcount);
  return slice_start + (m_globals.slice_length - remaining);
}

u64 CoreTimingManager::GetIdleTicks() const
{
  return static_cast<u64>(m_idled_cycles);
}

void CoreTimingManager::ClearPendingEvents()
{
  m_event_queue.clear();
}

void CoreTimingManager::ScheduleEvent(s64 cycles_into_future, EventType* event_type, u64 userdata,
                                      FromThread from)
{
  ASSERT_MSG(POWERPC, event_type != nullptr, "ScheduleEvent called with a null event type");

  bool on_cpu_thread;
  if (from == FromThread::ANY)
  {
    on_cpu_thread = Core::IsCPUThread();
  }
  else
  {
    on_cpu_thread = (from == FromThread::CPU);
    ASSERT_MSG(POWERPC, on_cpu_thread == Core::IsCPUThread(),
               "Event \"{}\" scheduled as {} from the other kind of thread", *event_type->name,
               on_cpu_thread ? "CPU" : "non-CPU");
  }

  if (!on_cpu_thread)
  {
    if (Core::WantsDeterminism())
    {
      ERROR_LOG_FMT(POWERPC,
                    "Event \"{}\" scheduled from a non-CPU thread during netplay or movie "
                    "playback/recording; this will probably desync.",
                    *event_type->name);
    }

    std::lock_guard guard(m_ts_write_lock);
    m_ts_queue.Push(Event{cycles_into_future, 0, userdata, event_type});
    return;
  }

  const s64 due_time = static_cast<s64>(GetTicks() + cycles_into_future);

  // Outside Advance(): make sure the current slice ends in time for this event.
  if (!m_is_global_timer_sane)
    ForceExceptionCheck(cycles_into_future);

  PushEvent(Event{due_time, m_event_fifo_id++, userdata, event_type});
}

void CoreTimingManager::RemoveEvent(EventType* event_type)
{
  const auto removed =
      std::erase_if(m_event_queue, [event_type](const Event& e) { return e.type == event_type; });
  if (removed > 0)
    RebuildHeap();
}

void CoreTimingManager::RemoveAllEvents(EventType* event_type)
{
  MoveEvents();
  RemoveEvent(event_type);
}

void CoreTimingManager::ForceExceptionCheck(s64 cycles)
{
  const s64 wanted = std::max<s64>(cycles, 0);
  auto& ppc_state = m_system.GetPPCState();
  const int remaining = DowncountToCycles(ppc_state.downcount);
  if (remaining <= wanted)
    return;

  // Shrink the slice so that it ends `wanted` cycles from now (less than the
  // remaining budget, so it fits in an int). The cycles already run stay
  // counted in slice_length - remaining.
  const int new_remaining = static_cast<int>(wanted);
  m_globals.slice_length -= remaining - new_remaining;
  ppc_state.downcount = CyclesToDowncount(new_remaining);
}

void CoreTimingManager::MoveEvents()
{
  Event event;
  while (m_ts_queue.Pop(event))
  {
    event.fifo_order = m_event_fifo_id++;
    event.time += m_globals.global_timer;
    PushEvent(event);
  }
}

void CoreTimingManager::Advance()
{
  CPUThreadConfigCallback::CheckForConfigChanges();

  MoveEvents();

  auto& power_pc = m_system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();

  // Close the slice that just ended.
  const int used = m_globals.slice_length - DowncountToCycles(ppc_state.downcount);
  m_globals.global_timer += used;

  // A new overclock setting takes effect from the next slice on.
  m_last_oc_factor = m_config_oc_factor;
  m_globals.last_OC_factor_inverted = m_config_oc_inv_factor;
  m_globals.slice_length = MAX_SLICE_LENGTH;

  m_is_global_timer_sane = true;
  while (!m_event_queue.empty() && m_event_queue.front().time <= m_globals.global_timer)
  {
    std::ranges::pop_heap(m_event_queue, HEAP_ORDER);
    const Event due = m_event_queue.back();
    m_event_queue.pop_back();
    due.type->callback(m_system, due.userdata, m_globals.global_timer - due.time);
  }
  m_is_global_timer_sane = false;

  if (!m_event_queue.empty())
  {
    const s64 until_next = m_event_queue.front().time - m_globals.global_timer;
    m_globals.slice_length = static_cast<int>(std::min<s64>(until_next, MAX_SLICE_LENGTH));
  }
  ppc_state.downcount = CyclesToDowncount(m_globals.slice_length);

  // Interrupts raised by the events above must be taken now, not one slice
  // later (Pokemon Box needs its first audio DMA interrupt on time).
  power_pc.CheckExternalExceptions();
}

TimePoint CoreTimingManager::CalculateTargetHostTimeInternal(s64 target_cycle)
{
  const s64 cycles = target_cycle - m_throttle_reference_cycle;
  const Clock::duration span = std::chrono::seconds{cycles};
  return m_throttle_reference_time + span / m_throttle_adj_clock_per_sec;
}

bool CoreTimingManager::IsSpeedUnlimited() const
{
  return m_throttle_adj_clock_per_sec == 0 || Core::GetIsThrottlerTempDisabled();
}

TimePoint CoreTimingManager::GetTargetHostTime(s64 target_cycle)
{
  return IsSpeedUnlimited() ? Clock::now() : CalculateTargetHostTimeInternal(target_cycle);
}

void CoreTimingManager::SleepUntil(TimePoint time_point)
{
  const bool precise = m_use_precision_timer.load(std::memory_order_relaxed);
  const bool cpu_thread = Core::IsCPUThread();

  auto sleep = [&](Common::PrecisionTimer& timer) {
    if (precise)
      timer.SleepUntil(time_point);
    else
      std::this_thread::sleep_until(time_point);
  };

  if (!cpu_thread)
  {
    sleep(m_precision_gpu_timer);
    return;
  }

  const TimePoint before = Clock::now();
  sleep(m_precision_cpu_timer);
  m_system.GetPerfMetrics().CountThrottleSleep(Clock::now() - before);
}

void CoreTimingManager::Throttle(const s64 target_cycle)
{
  const TimePoint now = Clock::now();

  // With Rush Frame Presentation only the first throttle after a present
  // sleeps, as long as emulation does not get too far ahead of real time.
  const bool throttled_before =
      m_throttled_after_presentation.exchange(true, std::memory_order_relaxed);
  if (throttled_before && m_config_rush_frame_presentation &&
      GetTargetHostTime(target_cycle) - now < m_max_throttle_skip_time)
  {
    return;
  }

  Common::ScopeGuard count_performance{[&] {
    m_system.GetPerfMetrics().CountPerformanceMarker(
        target_cycle, m_system.GetSystemTimers().GetTicksPerSecond());
  }};

  if (IsSpeedUnlimited())
  {
    ResetThrottle(target_cycle);
    m_throttle_disable_vi_int = false;
    return;
  }

  // Move the reference point forward in whole seconds so rounding errors
  // cannot accumulate.
  const s64 whole_seconds =
      (target_cycle - m_throttle_reference_cycle) / m_throttle_adj_clock_per_sec;
  m_throttle_reference_cycle += whole_seconds * m_throttle_adj_clock_per_sec;
  m_throttle_reference_time += std::chrono::seconds{whole_seconds};

  TimePoint target = CalculateTargetHostTimeInternal(target_cycle);

  // Too far behind: give up on the lost time instead of racing to catch up,
  // unless the user asked to correct time drift.
  const TimePoint earliest = now - m_max_fallback;
  if (!m_correct_time_drift && target < earliest)
  {
    const DT behind = earliest - target;
    DEBUG_LOG_FMT(CORE, "Emulation is too slow; dropping {} us of timing", DT_us(behind).count());
    m_throttle_reference_time += behind;
    target += behind;
  }

  UpdateVISkip(now, target);

  SleepUntil(target);
}

void CoreTimingManager::UpdateSpeedLimit(s64 cycle, double new_speed)
{
  m_emulation_speed = new_speed;

  const u32 new_rate =
      std::lround(m_system.GetSystemTimers().GetTicksPerSecond() * new_speed);

  // Keep host time continuous across the change by re-deriving the reference cycle.
  if (m_throttle_adj_clock_per_sec != 0)
  {
    const s64 elapsed = cycle - m_throttle_reference_cycle;
    m_throttle_reference_cycle = cycle - elapsed * new_rate / m_throttle_adj_clock_per_sec;
  }

  m_throttle_adj_clock_per_sec = new_rate;
}

void CoreTimingManager::ResetThrottle(s64 cycle)
{
  m_throttle_reference_cycle = cycle;
  m_throttle_reference_time = Clock::now();
}

void CoreTimingManager::UpdateVISkip(TimePoint current_time, TimePoint target_time)
{
  // Skip VI interrupts while the CPU lags by more than a fixed amount.
  const DT allowance = std::min(m_max_variance, m_max_fallback) / 2;
  m_throttle_disable_vi_int = target_time < current_time - allowance;
}

bool CoreTimingManager::GetVISkip() const
{
  return m_throttle_disable_vi_int && g_ActiveConfig.bVISkip && !Core::WantsDeterminism();
}

float CoreTimingManager::GetOverclock() const
{
  return m_config_oc_factor;
}

bool CoreTimingManager::UseSyncOnSkipIdle() const
{
  return m_config_sync_on_skip_idle;
}

void CoreTimingManager::LogPendingEvents() const
{
  std::vector<Event> sorted = m_event_queue;
  std::ranges::sort(sorted);
  for (const Event& event : sorted)
  {
    INFO_LOG_FMT(POWERPC, "Queued event {} due at {} (now {})", *event.type->name, event.time,
                 m_globals.global_timer);
  }
}

void CoreTimingManager::AdjustEventQueueTimes(u32 new_ppc_clock, u32 old_ppc_clock)
{
  const s64 now = m_globals.global_timer;

  UpdateSpeedLimit(now, m_emulation_speed);
  m_system.GetPerfMetrics().AdjustClockSpeed(now, new_ppc_clock, old_ppc_clock);

  // Scaling is monotonic, so the heap stays valid.
  for (Event& event : m_event_queue)
    event.time = now + (event.time - now) * new_ppc_clock / old_ppc_clock;
}

void CoreTimingManager::Idle()
{
  // With "sync on skip idle" the GPU must catch up first, or VI would drift
  // away from what the GPU has drawn.
  if (m_config_sync_on_skip_idle)
    m_system.GetFifo().FlushGpu();

  auto& ppc_state = m_system.GetPPCState();
  PowerPC::UpdatePerformanceMonitor(ppc_state.downcount, 0, 0, ppc_state);
  m_idled_cycles += DowncountToCycles(ppc_state.downcount);
  ppc_state.downcount = 0;
}

std::string CoreTimingManager::GetScheduledEventsSummary() const
{
  std::vector<Event> sorted = m_event_queue;
  std::ranges::sort(sorted);

  std::string summary = "Scheduled events\n";
  summary.reserve(1000);
  for (const Event& event : sorted)
    summary += fmt::format("{} : {} {:016x}\n", *event.type->name, event.time, event.userdata);
  return summary;
}

u32 CoreTimingManager::GetFakeDecStartValue() const
{
  return m_fake_dec_start_value;
}

void CoreTimingManager::SetFakeDecStartValue(u32 val)
{
  m_fake_dec_start_value = val;
}

u64 CoreTimingManager::GetFakeDecStartTicks() const
{
  return m_fake_dec_start_ticks;
}

void CoreTimingManager::SetFakeDecStartTicks(u64 val)
{
  m_fake_dec_start_ticks = val;
}

u64 CoreTimingManager::GetFakeTBStartValue() const
{
  return m_globals.fake_TB_start_value;
}

void CoreTimingManager::SetFakeTBStartValue(u64 val)
{
  m_globals.fake_TB_start_value = val;
}

u64 CoreTimingManager::GetFakeTBStartTicks() const
{
  return m_globals.fake_TB_start_ticks;
}

void CoreTimingManager::SetFakeTBStartTicks(u64 val)
{
  m_globals.fake_TB_start_ticks = val;
}

void GlobalAdvance()
{
  Core::System::GetInstance().GetCoreTiming().Advance();
}

void GlobalIdle()
{
  Core::System::GetInstance().GetCoreTiming().Idle();
}

}  // namespace CoreTiming
