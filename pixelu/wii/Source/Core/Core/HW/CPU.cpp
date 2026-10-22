// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/CPU.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

#include "AudioCommon/AudioCommon.h"
#include "Common/Event.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/TimePlayed.h"
#include "VideoCommon/Fifo.h"

namespace CPU
{
CPUManager::CPUManager(Core::System& system) : m_system(system)
{
}

CPUManager::~CPUManager() = default;

State CPUManager::GetState() const { return m_state; }
const State* CPUManager::GetStatePtr() const { return &m_state; }
bool CPUManager::IsStepping() const { return GetState() == State::Stepping; }

// Nothing to reset: the state follows Init/Stop.
void CPUManager::Reset() {}

void CPUManager::Init(PowerPC::CPUCore cpu_core)
{
  m_system.GetPowerPC().Init(cpu_core);
  m_state = State::Stepping;
}

void CPUManager::Shutdown()
{
  Stop();
  m_system.GetPowerPC().Shutdown();
}

// --- Helpers (all with m_state_change_lock held) -----------------------------------

// Completes a pending single-step request.
void CPUManager::FlushStepSyncEventLocked()
{
  if (!m_state_cpu_step_instruction)
    return;
  m_state_cpu_step_instruction = false;
  if (Common::Event* sync = std::exchange(m_state_cpu_step_instruction_sync, nullptr))
    sync->Set();
}

// Runs the queued jobs without the lock held.
void CPUManager::ExecutePendingJobs(std::unique_lock<std::mutex>& state_lock)
{
  while (!m_pending_jobs.empty())
  {
    Common::MoveOnlyFunction<void()> job = std::move(m_pending_jobs.front());
    m_pending_jobs.pop();
    state_lock.unlock();
    job();
    state_lock.lock();
  }
}

void CPUManager::MarkCPUThreadIdle()
{
  m_state_cpu_thread_active = false;
  m_state_cpu_idle_cvar.notify_all();
}

void CPUManager::WaitForCPUThreadIdle(std::unique_lock<std::mutex>& state_lock)
{
  m_state_cpu_idle_cvar.wait(state_lock, [this] { return !m_state_cpu_thread_active; });
}

// Fifo and sound follow the run state. Core stops the sound stream itself at
// power down. These must not call back into Break or SetStepping.
void CPUManager::RunAdjacentSystems(bool running)
{
  m_system.GetFifo().EmulatorState(running);
  if (m_state != State::PowerDown)
    AudioCommon::SetSoundStreamRunning(m_system, running);
}

// PowerDown is final. Entering Stepping drops temporary breakpoints.
bool CPUManager::SetStateLocked(State s)
{
  if (m_state == State::PowerDown)
    return false;
  if (s == State::Stepping)
    m_system.GetPowerPC().GetBreakPoints().ClearTemporary();
  m_state = s;
  return true;
}

// --- Play time tracking --------------------------------------------------------------

void CPUManager::StartTimePlayedTimer()
{
  Common::SetCurrentThreadName("Play Time Tracker");

  // This clock stops while the host is suspended.
  Common::SteadyAwakeClock clock;
  auto last = clock.now();

  while (true)
  {
    TimePlayed time_played;
    auto now = clock.now();

    switch (m_state)
    {
    case State::Running:
      time_played.AddTime(SConfig::GetInstance().GetGameID(),
                          std::chrono::duration_cast<std::chrono::milliseconds>(now - last));
      break;
    case State::Stepping:
      // Paused: sleep until emulation resumes; the pause does not count.
      m_time_played_finish_sync.Wait();
      now = clock.now();
      break;
    default:
      break;
    }
    last = now;

    if (m_state == State::PowerDown)
      return;

    m_time_played_finish_sync.WaitFor(std::chrono::seconds(30));
  }
}

// --- CPU thread ----------------------------------------------------------------------

void CPUManager::RunUntilStopped(std::unique_lock<std::mutex>& state_lock)
{
  auto& power_pc = m_system.GetPowerPC();

  m_state_cpu_thread_active = true;
  state_lock.unlock();

  // When resuming on a breakpoint (or with memory checks, where any
  // instruction can break), execute one instruction with the interpreter so
  // the run loop does not stop on the same breakpoint again.
  // PowerPCManager::CheckBreakPoints is not used, as it would log the hit
  // a second time.
  if (power_pc.GetBreakPoints().IsAddressBreakPoint(power_pc.GetPPCState().pc) ||
      power_pc.GetMemChecks().HasAny())
  {
    m_state = State::Stepping;
    const PowerPC::CoreMode mode = power_pc.GetMode();
    power_pc.SetMode(PowerPC::CoreMode::Interpreter);
    power_pc.SingleStep();
    power_pc.SetMode(mode);
    m_state = State::Running;
  }

  power_pc.RunLoop();

  state_lock.lock();
  MarkCPUThreadIdle();
}

// Wait condition while stepping. Also keeps jobs, configuration changes and
// the GDB stub serviced; a GDB stub in control makes each wake-up a step.
bool CPUManager::StepRequestedOrResumed(std::unique_lock<std::mutex>& state_lock,
                                        Common::Event& gdb_step_event)
{
  ExecutePendingJobs(state_lock);
  CPUThreadConfigCallback::CheckForConfigChanges();

  state_lock.unlock();
  if (GDBStub::IsActive() && GDBStub::HasControl())
  {
    if (!GDBStub::JustConnected())
      GDBStub::SendSignal(GDBStub::Signal::Sigtrap);
    GDBStub::ProcessCommands(true);
    if (GDBStub::HasControl())
    {
      // Release a step request that was never serviced.
      if (m_state_cpu_step_instruction_sync != nullptr &&
          m_state_cpu_step_instruction_sync != &gdb_step_event)
      {
        m_state_cpu_step_instruction_sync->Set();
      }
      m_state_cpu_step_instruction = true;
      m_state_cpu_step_instruction_sync = &gdb_step_event;
    }
  }
  state_lock.lock();

  return m_state_cpu_step_instruction || !IsStepping();
}

void CPUManager::ServiceStepping(std::unique_lock<std::mutex>& state_lock,
                                 Common::Event& gdb_step_event)
{
  m_state_cpu_cvar.wait(state_lock, [&] {
    return StepRequestedOrResumed(state_lock, gdb_step_event);
  });

  if (!IsStepping())
  {
    // Resumed or powered down: release anyone waiting for a step.
    FlushStepSyncEventLocked();
    return;
  }
  if (m_state_paused_and_locked)
    return;

  m_state_cpu_thread_active = true;
  state_lock.unlock();
  m_system.GetPowerPC().SingleStep();
  state_lock.lock();
  MarkCPUThreadIdle();

  FlushStepSyncEventLocked();
  Host_UpdateDisasmDialog();
}

void CPUManager::Run()
{
  std::thread time_tracker;
  if (Config::Get(Config::MAIN_TIME_TRACKING))
    time_tracker = std::thread(&CPUManager::StartTimePlayedTimer, this);

  std::unique_lock state_lock(m_state_change_lock);
  while (m_state != State::PowerDown)
  {
    m_state_cpu_cvar.wait(state_lock, [this] { return !m_state_paused_and_locked; });
    ExecutePendingJobs(state_lock);
    CPUThreadConfigCallback::CheckForConfigChanges();

    // Must outlive the step it may be registered for (one loop iteration).
    Common::Event gdb_step_event;
    if (m_state == State::Running)
      RunUntilStopped(state_lock);
    else if (m_state == State::Stepping)
      ServiceStepping(state_lock, gdb_step_event);
  }

  if (time_tracker.joinable())
  {
    m_time_played_finish_sync.Set();
    time_tracker.join();
  }

  state_lock.unlock();
  Host_UpdateDisasmDialog();
}

// --- Control from other threads --------------------------------------------------------

void CPUManager::Stop()
{
  // PowerDown cannot be undone, so the stepping lock is not needed.
  std::unique_lock state_lock(m_state_change_lock);
  m_state = State::PowerDown;
  m_state_cpu_cvar.notify_one();
  WaitForCPUThreadIdle(state_lock);

  RunAdjacentSystems(false);
  FlushStepSyncEventLocked();
}

void CPUManager::StepOpcode(Common::Event* event)
{
  std::lock_guard state_lock(m_state_change_lock);
  if (!IsStepping())
  {
    if (event != nullptr)
      event->Set();
    return;
  }

  // A previous request may still be pending; release its waiter.
  if (m_state_cpu_step_instruction_sync != nullptr && m_state_cpu_step_instruction_sync != event)
    m_state_cpu_step_instruction_sync->Set();

  m_state_cpu_step_instruction = true;
  m_state_cpu_step_instruction_sync = event;
  m_state_cpu_cvar.notify_one();
}

void CPUManager::SetStepping(bool stepping)
{
  std::lock_guard stepping_lock(m_stepping_lock);
  std::unique_lock state_lock(m_state_change_lock);

  if (!stepping)
  {
    if (!SetStateLocked(State::Running))
      return;
    if (!Core::IsCPUThread())
      m_state_cpu_cvar.notify_one();
    m_time_played_finish_sync.Set();
    RunAdjacentSystems(true);
    return;
  }

  SetStateLocked(State::Stepping);
  // The CPU thread itself cannot wait for itself.
  if (!Core::IsCPUThread())
    WaitForCPUThreadIdle(state_lock);
  RunAdjacentSystems(false);
}

void CPUManager::Break()
{
  std::lock_guard state_lock(m_state_change_lock);
  if (!m_state_paused_and_locked)
  {
    // No waiting here: the CPU thread may be blocked on our caller.
    SetStateLocked(State::Stepping);
    RunAdjacentSystems(false);
    return;
  }
  // Another thread holds PauseAndLock: remember the request, so that its
  // unlock leaves the CPU stepping instead of resuming it.
  m_state_system_request_stepping = true;
}

void CPUManager::Continue()
{
  SetStepping(false);
  Core::NotifyStateChanged(Core::State::Running);
}

bool CPUManager::PauseAndLock()
{
  // Held until RestoreStateAndUnlock().
  m_stepping_lock.lock();

  bool was_running;
  {
    std::unique_lock state_lock(m_state_change_lock);
    m_state_paused_and_locked = true;
    was_running = GetState() == State::Running;
    SetStateLocked(State::Stepping);
    WaitForCPUThreadIdle(state_lock);
  }

  // The caller acts as the CPU thread until it unlocks.
  if (!Core::IsCPUThread())
  {
    Core::DeclareAsCPUThread();
    m_have_fake_cpu_thread = true;
  }
  return was_running;
}

void CPUManager::RestoreStateAndUnlock(const bool unpause_on_unlock)
{
  // Guarded by the stepping lock, which is still held.
  if (m_have_fake_cpu_thread)
  {
    m_have_fake_cpu_thread = false;
    Core::UndeclareAsCPUThread();
  }

  {
    std::lock_guard state_lock(m_state_change_lock);
    if (m_state_system_request_stepping)
      m_state_system_request_stepping = false;
    else if (unpause_on_unlock)
      SetStateLocked(State::Running);
    m_state_paused_and_locked = false;
    m_state_cpu_cvar.notify_one();

    RunAdjacentSystems(m_state == State::Running);
  }
  m_stepping_lock.unlock();
}

void CPUManager::AddCPUThreadJob(Common::MoveOnlyFunction<void()> function)
{
  std::lock_guard state_lock(m_state_change_lock);
  m_pending_jobs.push(std::move(function));
}

}  // namespace CPU
