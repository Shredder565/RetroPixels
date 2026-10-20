// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Flag.h"

namespace VideoCommon
{
// Runs shader/pipeline compiles on a pool of worker threads. Compile() runs on a worker (or
// immediately when there are no workers); Retrieve() runs on the thread that calls
// RetrieveWorkItems, normally the video thread.
class AsyncShaderCompiler
{
public:
  class WorkItem
  {
  public:
    virtual ~WorkItem() = default;
    virtual bool Compile() = 0;
    virtual void Retrieve() = 0;
  };

  using WorkItemPtr = std::unique_ptr<WorkItem>;

  AsyncShaderCompiler();
  virtual ~AsyncShaderCompiler();

  template <typename T, typename... Params>
  static WorkItemPtr CreateWorkItem(Params&&... params)
  {
    return std::make_unique<T>(std::forward<Params>(params)...);
  }

  // Adds an item to the queue. Lower priority values are compiled first; items with the same
  // priority are compiled in the order they were queued.
  void QueueWorkItem(WorkItemPtr item, u32 priority);
  // Calls Retrieve on every finished item and drops it.
  void RetrieveWorkItems();
  // True while items are queued or being compiled.
  bool HasPendingWork();
  bool HasCompletedWork();

  // Drops every queued and every finished item.
  void ClearAllWork();

  // Blocks until the queue is empty. After a short grace period, progress_callback(done, total)
  // is called periodically. Returns false if emulation is stopping before the queue drains.
  bool WaitUntilCompletion(const std::function<void(size_t, size_t)>& progress_callback);

  // Thread control. These are separate from construction and destruction because the
  // per-thread hooks below are virtual.
  bool StartWorkerThreads(u32 num_worker_threads);
  bool ResizeWorkerThreads(u32 num_worker_threads);
  bool HasWorkerThreads() const;
  void StopWorkerThreads();

protected:
  // Backend hooks: prepare a thread on the starting thread, set it up on the worker itself,
  // and tear it down when the worker exits.
  virtual bool WorkerThreadInitMainThread(void** param);
  virtual bool WorkerThreadInitWorkerThread(void* param);
  virtual void WorkerThreadExit(void* param);

private:
  void WorkerMain(void* param);
  void ServeQueue();

  Common::Flag m_stop_requested;
  Common::Event m_worker_ready;

  std::vector<std::thread> m_workers;
  std::atomic_bool m_last_worker_ok{false};

  // Keyed by priority. A multimap rather than a priority_queue so items can be moved out.
  std::multimap<u32, WorkItemPtr> m_queue;
  std::mutex m_queue_mutex;
  std::condition_variable m_queue_cv;
  std::atomic_size_t m_compiling{0};

  std::deque<WorkItemPtr> m_finished;
  std::mutex m_finished_mutex;
};

}  // namespace VideoCommon
