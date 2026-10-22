// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// CoreTiming is the scheduler of the emulated machine. Everything that has to
// happen at a given point of emulated time (a VI half-line, an audio DMA
// block, a DVD transfer finishing, a decrementer underflow, ...) is an event
// placed on a single timeline counted in CPU core clock cycles.
//
// Usage: register an event type once (at Init, so savestates can find it by
// name), then schedule it as often as needed. A callback is told how many
// cycles late it runs; a periodic event re-arms itself with
// `ScheduleEvent(period - cycles_late, type)` so the period does not drift.
//
// The CPU core executes "slices": Advance() runs every event that is due,
// works out how long the CPU may run before the next event (at most
// 20000 cycles) and writes that budget into PowerPCState::downcount. The CPU
// counts the downcount down and calls Advance() again when it reaches zero.

#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Functional.h"
#include "Common/HookableEvent.h"
#include "Common/SPSCQueue.h"
#include "Common/Timer.h"
#include "Core/CPUThreadConfigCallback.h"

class PointerWrap;

namespace Core
{
class System;
}

namespace CoreTiming
{
// Timing values the JITs read straight from memory (by offsetof), so the
// member order here is part of the JIT interface.
struct Globals
{
  s64 global_timer = 0;
  int slice_length = 0;
  u64 fake_TB_start_value = 0;
  u64 fake_TB_start_ticks = 0;
  float last_OC_factor_inverted = 0.0f;
};

using TimedCallback =
    Common::MoveOnlyFunction<void(Core::System& system, u64 userdata, s64 cyclesLate)>;

struct EventType
{
  TimedCallback callback;
  // Points at the key of the registry entry; this name is what savestates store.
  const std::string* name;
};

struct Event
{
  s64 time;
  u64 fifo_order;
  u64 userdata;
  EventType* type;

  // Earlier time first; events due on the same cycle run in scheduling order.
  constexpr auto operator<=>(const Event& other) const
  {
    return std::tie(time, fifo_order) <=> std::tie(other.time, other.fifo_order);
  }
  constexpr bool operator==(const Event& other) const
  {
    return std::tie(time, fifo_order) == std::tie(other.time, other.fifo_order);
  }
};

enum class FromThread
{
  CPU,
  NON_CPU,
  // Decide at run time. Only for callers that really run on both kinds of thread.
  ANY
};

// Entry points for JIT-generated code, which has no CoreTimingManager pointer.
void GlobalAdvance();
void GlobalIdle();

class CoreTimingManager
{
public:
  explicit CoreTimingManager(Core::System& system);

  // Starts the timeline. Until the first Advance() the CPU is considered to be
  // at the end of slice -1, so the CPU loop must Advance() before running code.
  void Init();
  void Shutdown();

  // Current emulated time in cycles. CPU thread only.
  u64 GetTicks() const;
  u64 GetIdleTicks() const;
  TimePoint GetTargetHostTime(s64 target_cycle);

  void RefreshConfig();

  void DoState(PointerWrap& p);

  // Registers an event type under a unique name and returns its handle.
  EventType* RegisterEvent(const std::string& name, TimedCallback callback);
  void UnregisterAllEvents();

  // userdata is saved in savestates, so it must never hold a pointer. Events
  // scheduled from the CPU thread outside of Advance() shorten the current
  // slice when they fall due before it ends; events scheduled from other
  // threads are queued and merged at the next Advance().
  void ScheduleEvent(s64 cycles_into_future, EventType* event_type, u64 userdata = 0,
                     FromThread from = FromThread::CPU);

  // Removes every scheduled event of this type.
  void RemoveEvent(EventType* event_type);
  // Same, including events still waiting in the cross-thread queue.
  void RemoveAllEvents(EventType* event_type);

  // Ends the current slice, runs the events that are due and starts the next
  // slice (updating the PowerPC downcount and checking external exceptions).
  void Advance();
  void MoveEvents();

  // Skips the rest of the current slice (the CPU is idle until the next event).
  void Idle();

  // Drops every scheduled event. Only for shutdown and state loading.
  void ClearPendingEvents();

  void LogPendingEvents() const;

  std::string GetScheduledEventsSummary() const;

  // Rescales the time left until each event after a CPU clock change.
  void AdjustEventQueueTimes(u32 new_ppc_clock, u32 old_ppc_clock);

  u32 GetFakeDecStartValue() const;
  void SetFakeDecStartValue(u32 val);
  u64 GetFakeDecStartTicks() const;
  void SetFakeDecStartTicks(u64 val);
  u64 GetFakeTBStartValue() const;
  void SetFakeTBStartValue(u64 val);
  u64 GetFakeTBStartTicks() const;
  void SetFakeTBStartTicks(u64 val);

  // Makes the current slice end no later than `cycles` from now.
  void ForceExceptionCheck(s64 cycles);

  // Read and written directly by the JITs.
  Globals& GetGlobals() { return m_globals; }

  // Sleeps the CPU thread until host time catches up with `target_cycle`.
  void Throttle(const s64 target_cycle);

  // Callable from the CPU and GPU threads.
  void SleepUntil(TimePoint time_point);

  // Whether VideoInterface should skip the next VI interrupt (lag compensation).
  bool GetVISkip() const;

  float GetOverclock() const;

  bool UseSyncOnSkipIdle() const;

private:
  // Overclocking scales how many emulated cycles one unit of downcount is worth.
  int DowncountToCycles(int downcount) const;
  int CyclesToDowncount(int cycles) const;

  void PushEvent(const Event& event);
  void RebuildHeap();

  bool IsSpeedUnlimited() const;
  void UpdateSpeedLimit(s64 cycle, double new_speed);
  void ResetThrottle(s64 cycle);
  TimePoint CalculateTargetHostTimeInternal(s64 target_cycle);
  void UpdateVISkip(TimePoint current_time, TimePoint target_time);

  Globals m_globals;

  Core::System& m_system;

  // Registry of event types by name. Node-based, so EventType pointers stay valid.
  std::unordered_map<std::string, EventType> m_event_types;

  // Scheduled events as a binary min-heap ordered by (time, fifo_order).
  std::vector<Event> m_event_queue;
  u64 m_event_fifo_id = 0;
  std::mutex m_ts_write_lock;

  // Events scheduled from other threads; `time` holds cycles-into-future
  // until MoveEvents() turns it into an absolute time.
  Common::SPSCQueue<Event> m_ts_queue;

  float m_last_oc_factor = 0.0f;

  s64 m_idled_cycles = 0;
  u32 m_fake_dec_start_value = 0;
  u64 m_fake_dec_start_ticks = 0;

  // True while Advance() runs callbacks: global_timer is then exact and the
  // downcount must not be consulted.
  bool m_is_global_timer_sane = false;

  // Stand-in type for savestate events whose type is not registered.
  EventType* m_ev_lost = nullptr;

  CPUThreadConfigCallback::ConfigChangedCallbackID m_registered_config_callback_id;
  float m_config_oc_factor = 1.0f;
  float m_config_oc_inv_factor = 1.0f;
  bool m_config_sync_on_skip_idle = false;
  bool m_config_rush_frame_presentation = false;

  // Throttling: host time `m_throttle_reference_time` corresponds to emulated
  // cycle `m_throttle_reference_cycle`, at `m_throttle_adj_clock_per_sec`
  // cycles per host second (0 means unlimited speed).
  s64 m_throttle_reference_cycle = 0;
  TimePoint m_throttle_reference_time = Clock::now();
  u32 m_throttle_adj_clock_per_sec = 0;
  bool m_throttle_disable_vi_int = false;

  DT m_max_fallback = {};
  DT m_max_variance = {};
  bool m_correct_time_drift = false;
  double m_emulation_speed = 1.0;

  std::atomic_bool m_use_precision_timer = false;
  Common::PrecisionTimer m_precision_cpu_timer;
  Common::PrecisionTimer m_precision_gpu_timer;

  Common::EventHook m_core_state_changed_hook;
  Common::EventHook m_frame_hook;

  // Rush Frame Presentation: set once a frame has been throttled, cleared on present.
  std::atomic_bool m_throttled_after_presentation = false;
  DT m_max_throttle_skip_time{};
};

}  // namespace CoreTiming
