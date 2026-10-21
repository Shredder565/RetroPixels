// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The core's control surface for front ends: starting and stopping
// emulation, pausing, frame stepping, running code on the CPU thread, and the
// queue of jobs the core hands back to the host thread.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/Functional.h"
#include "Common/HookableEvent.h"

struct BootParameters;
struct WindowSystemInfo;

namespace Core
{
class System;

bool GetIsThrottlerTempDisabled();
void SetIsThrottlerTempDisabled(bool disable);

// Called by the video interface at every emulated field.
void Callback_NewField(Core::System& system);

enum class State
{
  Uninitialized,
  Paused,
  Running,
  Stopping,
  Starting,
};

// The console type word games read from low memory (0x8000002C) or OSReport.
// Values as documented by YAGCD section 4.2.1.1.2 and printed by retail SDK
// OSInit/OSReportInfo builds.
enum class ConsoleType : u32
{
  // GameCube retail hardware.
  HW1 = 1,
  HW2 = 2,
  LatestProductionBoard = 3,
  Reserved = 4,

  // Wii retail and pre-production boards.
  PreProductionBoard0 = 0x10,
  PreProductionBoard1 = 0x11,
  PreProductionBoard2_1 = 0x12,
  PreProductionBoard2_2 = 0x20,
  RVL_Retail1 = 0x21,
  RVL_Retail2 = 0x22,
  RVL_Retail3 = 0x23,
  RVA1 = 0x100,  // Revolution arcade board

  // GameCube development systems. For FirstDevkit..ReservedDevkit the devkit
  // version is (value & 0x0fffffff) - 3.
  MacEmulator = 0x10000000,
  PcEmulator = 0x10000001,
  Arthur = 0x10000002,  // EPPC "Arthur" board
  Minnow = 0x10000003,  // EPPC "Minnow" board
  FirstDevkit = 0x10000004,
  SecondDevkit = 0x10000005,
  LatestDevkit = 0x10000006,
  ReservedDevkit = 0x10000007,

  // Wii development systems.
  RevolutionEmulator = 0x10000008,
  NDEV1_0 = 0x10000010,
  NDEV1_1 = 0x10000011,
  NDEV1_2 = 0x10000012,
  NDEV2_0 = 0x10000020,
  NDEV2_1 = 0x10000021,

  // TDEV-based emulation hardware; version as for the devkits.
  HW2TDEVSystem = 0x20000005,
  LatestTDEVSystem = 0x20000006,
  ReservedTDEVSystem = 0x20000007,
};

// Scoped access to emulator state that only the CPU thread may touch.
//
// On the CPU thread this does nothing. On any other thread it pauses the CPU
// thread and lets the current thread act as the CPU thread until the guard is
// destroyed. Functions that take a CPUThreadGuard reference must be called this
// way; ones that take a pointer accept nullptr and then fall back to something
// thread-safe.
class CPUThreadGuard final
{
public:
  explicit CPUThreadGuard(Core::System& system);
  ~CPUThreadGuard();

  CPUThreadGuard(const CPUThreadGuard&) = delete;
  CPUThreadGuard(CPUThreadGuard&&) = delete;
  CPUThreadGuard& operator=(const CPUThreadGuard&) = delete;
  CPUThreadGuard& operator=(CPUThreadGuard&&) = delete;

  Core::System& GetSystem() const { return m_system; }

private:
  Core::System& m_system;
  const bool m_was_cpu_thread;
  bool m_was_unpaused = false;
};

// Session control. Meant for the host (UI) thread, but any thread the core
// did not start may call them.
bool Init(Core::System& system, std::unique_ptr<BootParameters> boot, const WindowSystemInfo& wsi);
void Stop(Core::System& system);
void Shutdown(Core::System& system);

// Marks the calling thread as (no longer) the CPU or GPU thread.
void DeclareAsCPUThread();
void UndeclareAsCPUThread();
void DeclareAsGPUThread();
void UndeclareAsGPUThread();

// A log line naming the calling thread, for the shutdown sequence.
std::string StopMessage(bool main_thread, std::string_view message);

// Running or Paused.
bool IsRunning(Core::System& system);
// Starting, Running or Paused.
bool IsRunningOrStarting(Core::System& system);
// Uninitialized.
bool IsUninitialized(Core::System& system);

bool IsCPUThread();
bool IsGPUThread();

// True while something (a movie, NetPlay) needs bit-exact reproducible emulation.
bool WantsDeterminism();

// Pause or resume; safe from any thread, ignored unless emulation is running.
void SetState(Core::System& system, State state, bool report_state_change = true,
              bool override_achievement_restrictions = false);
State GetState(Core::System& system);

void SaveScreenShot();
void SaveScreenShot(std::string_view name);

// Shows an on-screen message while emulation runs.
void DisplayMessage(std::string message, int time_in_ms);

void FrameUpdateOnCPUThread();
void OnFrameEnd(Core::System& system);

// Runs `function` on the CPU thread: immediately when called from it,
// otherwise queued for it while it is briefly paused.
void RunOnCPUThread(Core::System& system, Common::MoveOnlyFunction<void()> function);

// State change notifications for the UI.
using StateChangedCallbackFunc = std::function<void(Core::State)>;
[[nodiscard]] Common::EventHook AddOnStateChangedCallback(StateChangedCallbackFunc callback);
void NotifyStateChanged(Core::State state);

void UpdateWantDeterminism(Core::System& system, bool initial = false);

// Hands `job` to the host thread, which runs it from HostDispatchJobs.
// Thread-safe. Jobs queued by one thread run in the order they were queued;
// jobs from different threads may interleave. A job should re-check the
// emulator's state when it runs. Unless run_during_stop is set, jobs still
// queued while emulation is stopping or stopped are dropped.
void QueueHostJob(std::function<void(Core::System&)> job, bool run_during_stop = false);

// Runs the queued host jobs; called by the host thread, which is told about
// new jobs through Host_Message(WMUserJobDispatch).
void HostDispatchJobs(Core::System& system);

void DoFrameStep(Core::System& system);

void UpdateInputGate(bool require_focus, bool require_full_focus = false);

void UpdateTitle(Core::System& system);
}  // namespace Core
