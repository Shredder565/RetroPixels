// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

#include "Common/Event.h"
#include "Common/Functional.h"

namespace Common
{
class Event;
}
namespace Core
{
class System;
}
namespace PowerPC
{
enum class CPUCore;
}

// The CPU thread's run state and the ways other threads control it:
// stepping, breaking, pausing with a lock held, queued jobs.
namespace CPU
{
// The JITs read this through GetStatePtr(), so the values must stay.
enum class State
{
  Running = 0,
  Stepping = 2,
  PowerDown = 3
};

class CPUManager
{
public:
  explicit CPUManager(Core::System& system);
  CPUManager(const CPUManager& other) = delete;
  CPUManager(CPUManager&& other) = delete;
  CPUManager& operator=(const CPUManager& other) = delete;
  CPUManager& operator=(CPUManager&& other) = delete;
  ~CPUManager();

  void Init(PowerPC::CPUCore cpu_core);
  void Shutdown();

  // The CPU thread's main loop; returns after Stop().
  void Run();

  // Enters State::PowerDown for good and waits until the CPU thread is idle.
  void Stop();

  // Does nothing.
  void Reset();

  // Executes one instruction while stepping. `event` is set once done (or
  // at once when not stepping).
  void StepOpcode(Common::Event* event = nullptr);

  // Enters or leaves stepping. Entering waits for the CPU thread to idle.
  void SetStepping(bool stepping);

  // Enters stepping without waiting for the CPU thread, for breakpoints hit
  // on emulator threads. Not for the host: nothing is synchronised.
  void Break();

  // Leaves stepping; CPU thread only.
  void Continue();

  // True only for State::Stepping (not for PowerDown).
  bool IsStepping() const;

  State GetState() const;

  // For JIT code to read the state. Never write through it.
  const State* GetStatePtr() const;

  // Stops the CPU thread and keeps it idle until RestoreStateAndUnlock(),
  // so the caller may touch emulator state. Not recursive; must not be
  // called from emulator threads (deadlock). Returns whether the CPU was
  // running; pass that to RestoreStateAndUnlock().
  bool PauseAndLock();
  void RestoreStateAndUnlock(bool unpause_on_unlock);

  // Queues a function for the CPU thread, which runs it when it is not in
  // the run loop (use together with PauseAndLock()).
  void AddCPUThreadJob(Common::MoveOnlyFunction<void()> function);

private:
  void FlushStepSyncEventLocked();
  void ExecutePendingJobs(std::unique_lock<std::mutex>& state_lock);
  void StartTimePlayedTimer();
  void RunAdjacentSystems(bool running);
  bool SetStateLocked(State s);

  void RunUntilStopped(std::unique_lock<std::mutex>& state_lock);
  bool StepRequestedOrResumed(std::unique_lock<std::mutex>& state_lock,
                              Common::Event& gdb_step_event);
  void ServiceStepping(std::unique_lock<std::mutex>& state_lock, Common::Event& gdb_step_event);
  void MarkCPUThreadIdle();
  void WaitForCPUThreadIdle(std::unique_lock<std::mutex>& state_lock);

  // Written with m_state_change_lock held; read without it.
  State m_state = State::PowerDown;

  // Serialises SetStepping and PauseAndLock. Lock order: this lock first,
  // then m_state_change_lock, never the other way round.
  std::mutex m_stepping_lock;
  // Set when PauseAndLock made the calling thread the CPU thread.
  bool m_have_fake_cpu_thread = false;

  // Guards the state and the requests below.
  std::mutex m_state_change_lock;
  // Signalled when the CPU thread becomes idle.
  std::condition_variable m_state_cpu_idle_cvar;
  // Signalled for the CPU thread: state change, step request, unlock.
  std::condition_variable m_state_cpu_cvar;
  bool m_state_cpu_thread_active = false;
  bool m_state_paused_and_locked = false;
  // A Break() arrived while paused; overrides unpausing on unlock.
  bool m_state_system_request_stepping = false;
  bool m_state_cpu_step_instruction = false;
  Common::Event* m_state_cpu_step_instruction_sync = nullptr;
  std::queue<Common::MoveOnlyFunction<void()>> m_pending_jobs;
  Common::Event m_time_played_finish_sync;

  Core::System& m_system;
};
}  // namespace CPU
