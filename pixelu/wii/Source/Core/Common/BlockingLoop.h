// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#include "Common/Event.h"
#include "Common/Flag.h"

namespace Common
{
// Drives a worker loop that re-runs a payload whenever other threads ask for it, e.g. the GPU
// thread processing FIFO data the CPU thread keeps adding.
//
// Wakeup() guarantees at least one more complete payload run after it; it never blocks and is
// cheap when the worker is already busy. Wait() blocks until such a run has finished. While
// work keeps coming in, the worker spins instead of sleeping; it only goes to sleep after
// AllowSleep() (or a Wait()) and a run that found nothing new.
//
// Calling Wait() while other threads keep calling Wakeup() may block for a long time.
class BlockingLoop
{
public:
  enum class StopMode
  {
    NonBlock,        // just request the stop
    Block,           // and wait for the loop to end
    BlockAndGiveUp,  // and wait, but give up if nobody is running the loop
  };

  BlockingLoop() { m_stopped.Set(); }
  ~BlockingLoop() { Stop(StopMode::BlockAndGiveUp); }

  void Wakeup()
  {
    if (m_state.load() == WORK_PENDING)
      return;  // the worker will run the payload again anyway
    if (m_state.exchange(WORK_PENDING) == ASLEEP)
      m_wake_worker.Set();
  }

  // Blocks until a payload run that started after the last Wakeup() has finished (returns at
  // once if the loop is stopped).
  void Wait()
  {
    if (IsDone())
      return;
    // Only one waiter at a time, since the done event wakes a single thread.
    std::lock_guard guard(m_waiter_lock);
    while (!IsDone())
      m_run_finished.Wait();
    // Someone waited for us, so the work has probably dried up: let the worker sleep.
    m_may_sleep.Set();
  }

  // Like Wait(), but calls yield_func() each time rel_time passes without progress.
  template <class Rep, class Period, typename Functor>
  void WaitYield(const std::chrono::duration<Rep, Period>& rel_time, Functor yield_func)
  {
    if (IsDone())
      return;
    std::lock_guard guard(m_waiter_lock);
    while (!IsDone())
    {
      if (!m_run_finished.WaitFor(rel_time))
        yield_func();
    }
    m_may_sleep.Set();
  }

  // Puts the loop in the running state before the worker enters Run(), so that Wait() blocks
  // until the worker's first payload run. May be called from any thread.
  void Prepare()
  {
    std::lock_guard guard(m_prepare_lock);
    if (!m_stopped.TestAndClear())
      return;  // already prepared or running
    m_state.store(RUN_AGAIN);  // run the payload once even without a Wakeup()
    m_shutdown.Clear();
    m_may_sleep.Set();
  }

  // The worker's loop. Runs `payload` as often as Wakeup() requires; with timeout > 0 (in
  // milliseconds) it also runs at least that often while asleep.
  template <class F>
  void Run(F payload, int64_t timeout = 0)
  {
    Prepare();

    while (!m_shutdown.IsSet())
    {
      payload();
      SettleAfterRun(timeout);
    }

    m_state.store(IDLE);
    m_stopped.Set();
    m_run_finished.Set();  // release any remaining waiter
  }

  // Ends the loop. Don't use a blocking mode from inside the payload.
  void Stop(StopMode mode = StopMode::Block)
  {
    if (m_stopped.IsSet())
      return;

    m_shutdown.Set();
    Wakeup();  // in case the worker sleeps

    if (mode == StopMode::Block)
    {
      Wait();
    }
    else if (mode == StopMode::BlockAndGiveUp)
    {
      // If nothing happens for 100 ms, assume no thread is running the loop.
      WaitYield(std::chrono::milliseconds(100), [this] { m_stopped.Set(); });
    }
  }

  bool IsRunning() const { return !m_stopped.IsSet() && !m_shutdown.IsSet(); }
  bool IsDone() const { return m_stopped.IsSet() || m_state.load() <= IDLE; }

  // Lets the worker fall back from spinning to sleeping once it runs out of work. Call it
  // regularly from the producer side.
  void AllowSleep() { m_may_sleep.Set(); }

private:
  // Worker states, ordered so that a Wakeup() only ever raises the value.
  enum : int
  {
    ASLEEP = 0,        // waiting for m_wake_worker
    IDLE = 1,          // all requested work done; spinning until allowed to sleep
    RUN_AGAIN = 2,     // one more run needed (a request may have come in during the last one)
    WORK_PENDING = 3,  // Wakeup() was called
  };

  // Decides what the worker does after a payload run. Wakeup() may change the state at any
  // time, but only to WORK_PENDING, so each step below re-checks after decrementing.
  void SettleAfterRun(int64_t timeout)
  {
    int state = m_state.load();

    if (state == WORK_PENDING)
    {
      // Requests during that run may not have been seen; run once more.
      m_state.fetch_sub(1);
      return;
    }

    if (state == RUN_AGAIN)
    {
      if (m_state.fetch_sub(1) != RUN_AGAIN)
        return;  // a new request arrived just now
      m_run_finished.Set();
      state = IDLE;
    }

    if (state == IDLE)
    {
      if (!m_may_sleep.TestAndClear())
        return;  // keep spinning: more work is probably on its way
      if (m_state.fetch_sub(1) != IDLE)
        return;  // a new request arrived just now
    }

    // Asleep until woken (or until the timeout passes).
    if (timeout > 0)
      m_wake_worker.WaitFor(std::chrono::milliseconds(timeout));
    else
      m_wake_worker.Wait();
  }

  std::mutex m_waiter_lock;
  std::mutex m_prepare_lock;

  Flag m_stopped;    // set while no loop is running; Wait() never blocks then
  Flag m_shutdown;   // asks Run() to return
  Flag m_may_sleep;  // the worker may go to sleep when idle

  Event m_wake_worker;
  Event m_run_finished;

  std::atomic<int> m_state{IDLE};
};
}  // namespace Common
