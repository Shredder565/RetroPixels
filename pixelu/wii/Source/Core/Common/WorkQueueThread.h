// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "Common/Event.h"
#include "Common/SPSCQueue.h"
#include "Common/Thread.h"

namespace Common
{
namespace detail
{
// A worker thread that feeds queued items to a function, one at a time, in order.
//
// The item queue is single-producer/single-consumer. With IsSingleProducer == false, a recursive
// mutex serialises all public calls so any number of threads may use them; with true, the caller
// promises only one thread ever does and no locking happens.
//
// Control requests (clear the queue, quit) are sent to the worker through a second queue so
// that only the worker ever pops from the item queue.
template <typename T, bool IsSingleProducer>
class WorkQueueThreadBase final
{
public:
  using FunctionType = std::function<void(T)>;

  WorkQueueThreadBase() = default;
  WorkQueueThreadBase(std::string name, FunctionType function)
  {
    Reset(std::move(name), std::move(function));
  }
  ~WorkQueueThreadBase() { Shutdown(); }

  // Finishes the current worker (if any), then starts a new one. Items pushed before the first
  // Reset() are kept and processed by the new worker.
  void Reset(std::string name, FunctionType function)
  {
    const auto lock = Lock();
    Shutdown();
    m_thread = std::thread([this, name = std::move(name), function = std::move(function)] {
      WorkerMain(name, function);
    });
  }

  template <typename... Args>
  void EmplaceItem(Args&&... args)
  {
    const auto lock = Lock();
    m_items.Emplace(std::forward<Args>(args)...);
    m_wake.Set();
  }
  void Push(T&& item) { EmplaceItem(std::move(item)); }
  void Push(const T& item) { EmplaceItem(item); }

  // Drops every queued item that has not started yet; returns once the worker has done so.
  void Cancel()
  {
    const auto lock = Lock();
    if (m_items.Empty())
      return;  // nothing to drop, no need to bother the worker
    SendControl([this] { m_items.Clear(); });
  }

  // Processes what is queued, then ends the worker. No-op without a worker.
  void Shutdown()
  {
    const auto lock = Lock();
    WaitForCompletion();
    EndWorker();
  }

  // Ends the worker after its current item; anything still queued stays queued.
  void Stop()
  {
    const auto lock = Lock();
    EndWorker();
  }

  void StopAndCancel()
  {
    const auto lock = Lock();
    Stop();
    Cancel();
  }

  // Blocks until the queue is empty. No-op without a worker.
  void WaitForCompletion()
  {
    const auto lock = Lock();
    if (IsRunning())
      m_items.WaitForEmpty();
  }

  bool IsRunning() { return m_thread.joinable(); }

private:
  // An empty Control means "quit".
  using Control = std::function<void()>;

  struct NoLock
  {
  };
  using MutexType = std::conditional_t<IsSingleProducer, NoLock, std::recursive_mutex>;

  auto Lock()
  {
    if constexpr (IsSingleProducer)
      return NoLock{};
    else
      return std::lock_guard<std::recursive_mutex>(m_mutex);
  }

  // Runs `fn` on the worker (or right here if there is none) and waits for it.
  void SendControl(Control fn)
  {
    if (!IsRunning())
    {
      fn();
      return;
    }
    m_control.Emplace(std::move(fn));
    m_wake.Set();
    m_control.WaitForEmpty();
  }

  void EndWorker()
  {
    if (!IsRunning())
      return;
    m_control.Emplace(Control{});
    m_wake.Set();
    m_thread.join();
    m_control.Clear();  // the quit request itself
  }

  void WorkerMain(const std::string& name, const FunctionType& function)
  {
    SetCurrentThreadName(name.c_str());

    for (;;)
    {
      // Control requests come first.
      while (!m_control.Empty())
      {
        Control& request = m_control.Front();
        if (!request)
          return;
        request();
        m_control.Pop();  // only now does SendControl() return
      }

      if (m_items.Empty())
      {
        m_wake.Wait();
        continue;
      }

      function(std::move(m_items.Front()));
      m_items.Pop();
    }
  }

  std::thread m_thread;
  WaitableSPSCQueue<T> m_items;
  WaitableSPSCQueue<Control> m_control;
  Event m_wake;
  [[no_unique_address]] MutexType m_mutex;
};

// Runs pushed functions on a worker thread.
template <template <typename> typename WorkThread>
class AsyncWorkThreadBase
{
public:
  using FuncType = std::function<void()>;

  AsyncWorkThreadBase() = default;
  explicit AsyncWorkThreadBase(std::string thread_name) { Reset(std::move(thread_name)); }

  void Reset(std::string thread_name)
  {
    m_worker.Reset(std::move(thread_name), [](FuncType func) { func(); });
  }

  void Push(FuncType func) { m_worker.Push(std::move(func)); }

  // Runs `func` on the worker and waits for it to finish.
  auto PushBlocking(FuncType func)
  {
    std::packaged_task<void()> task(std::move(func));
    auto done = task.get_future();
    m_worker.EmplaceItem([&task] { task(); });
    return done.get();
  }

  void Cancel() { m_worker.Cancel(); }
  void Shutdown() { m_worker.Shutdown(); }
  void WaitForCompletion() { m_worker.WaitForCompletion(); }

private:
  WorkThread<FuncType> m_worker;
};
}  // namespace detail

// Any thread may use the public interface.
template <typename T>
using WorkQueueThread = detail::WorkQueueThreadBase<T, false>;

// Only one thread may use the public interface; no locking.
template <typename T>
using WorkQueueThreadSP = detail::WorkQueueThreadBase<T, true>;

using AsyncWorkThread = detail::AsyncWorkThreadBase<WorkQueueThread>;
using AsyncWorkThreadSP = detail::AsyncWorkThreadBase<WorkQueueThreadSP>;
}  // namespace Common
