// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/AsyncShaderCompiler.h"

#include <chrono>
#include <thread>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "Core/Core.h"
#include "Core/System.h"

namespace VideoCommon
{
namespace
{
// Poll period while waiting for the queue to drain, and how many polls pass before progress
// is reported (about one second).
constexpr std::chrono::milliseconds POLL_PERIOD{1000 / 30};
constexpr u32 QUIET_POLLS = 1000 / (1000 / 30);
}  // namespace

AsyncShaderCompiler::AsyncShaderCompiler() = default;

AsyncShaderCompiler::~AsyncShaderCompiler()
{
  // Workers must have been stopped by the owner. Leftover items clean up after themselves.
  ASSERT(!HasWorkerThreads());
}

void AsyncShaderCompiler::QueueWorkItem(WorkItemPtr item, u32 priority)
{
  if (HasWorkerThreads())
  {
    std::lock_guard lock(m_queue_mutex);
    m_queue.emplace(priority, std::move(item));
    m_queue_cv.notify_one();
    return;
  }

  // No workers: compile right here. The item is retrieved whatever Compile returned.
  item->Compile();
  m_finished.push_back(std::move(item));
}

void AsyncShaderCompiler::RetrieveWorkItems()
{
  std::deque<WorkItemPtr> batch;
  {
    std::lock_guard lock(m_finished_mutex);
    batch.swap(m_finished);
  }

  for (; !batch.empty(); batch.pop_front())
    batch.front()->Retrieve();
}

bool AsyncShaderCompiler::HasPendingWork()
{
  std::lock_guard lock(m_queue_mutex);
  return m_compiling.load() != 0 || !m_queue.empty();
}

bool AsyncShaderCompiler::HasCompletedWork()
{
  std::lock_guard lock(m_finished_mutex);
  return !m_finished.empty();
}

void AsyncShaderCompiler::ClearAllWork()
{
  {
    std::lock_guard lock(m_queue_mutex);
    m_queue.clear();
  }
  std::lock_guard lock(m_finished_mutex);
  m_finished.clear();
}

bool AsyncShaderCompiler::WaitUntilCompletion(
    const std::function<void(size_t, size_t)>& progress_callback)
{
  if (!HasPendingWork())
    return true;

  // Short compiles finish without any progress display.
  for (u32 poll = 0; poll < QUIET_POLLS; ++poll)
  {
    std::this_thread::sleep_for(POLL_PERIOD);
    if (!HasPendingWork())
      return true;
  }

  // Total = everything known right now, plus one so the bar never shows complete early. The
  // two locks are only ever held together here.
  size_t total;
  {
    std::lock_guard queue_lock(m_queue_mutex);
    std::lock_guard finished_lock(m_finished_mutex);
    total = m_finished.size() + m_queue.size() + m_compiling.load() + 1;
  }

  auto& system = Core::System::GetInstance();
  while (Core::GetState(system) != Core::State::Stopping)
  {
    size_t queued;
    {
      std::lock_guard lock(m_queue_mutex);
      if (m_compiling.load() == 0 && m_queue.empty())
        return true;
      queued = m_queue.size();
    }
    progress_callback(total - queued, total);
    std::this_thread::sleep_for(POLL_PERIOD);
  }
  return false;
}

bool AsyncShaderCompiler::StartWorkerThreads(u32 num_worker_threads)
{
  for (u32 n = 0; n < num_worker_threads; ++n)
  {
    void* param = nullptr;
    if (!WorkerThreadInitMainThread(&param))
    {
      WARN_LOG_FMT(VIDEO, "Failed to initialize shader compiler worker thread.");
      break;
    }

    // The new thread reports through m_last_worker_ok and m_worker_ready.
    m_last_worker_ok.store(false);
    std::thread worker(&AsyncShaderCompiler::WorkerMain, this, param);
    m_worker_ready.Wait();
    if (!m_last_worker_ok.load())
    {
      WARN_LOG_FMT(VIDEO, "Failed to start shader compiler worker thread.");
      worker.join();
      break;
    }
    m_workers.push_back(std::move(worker));
  }

  // Asking for no workers is a success; otherwise at least one has to be running.
  return num_worker_threads == 0 || HasWorkerThreads();
}

bool AsyncShaderCompiler::ResizeWorkerThreads(u32 num_worker_threads)
{
  if (m_workers.size() == num_worker_threads)
    return true;

  StopWorkerThreads();
  return StartWorkerThreads(num_worker_threads);
}

bool AsyncShaderCompiler::HasWorkerThreads() const
{
  return !m_workers.empty();
}

void AsyncShaderCompiler::StopWorkerThreads()
{
  if (m_workers.empty())
    return;

  {
    std::lock_guard lock(m_queue_mutex);
    m_stop_requested.Set();
    m_queue_cv.notify_all();
  }
  for (std::thread& worker : m_workers)
    worker.join();
  m_workers.clear();
  m_stop_requested.Clear();
}

bool AsyncShaderCompiler::WorkerThreadInitMainThread(void**)
{
  return true;
}

bool AsyncShaderCompiler::WorkerThreadInitWorkerThread(void*)
{
  return true;
}

void AsyncShaderCompiler::WorkerThreadExit(void*)
{
}

void AsyncShaderCompiler::WorkerMain(void* param)
{
  Common::SetCurrentThreadName("AsyncShaderCompiler Worker");

  const bool ok = WorkerThreadInitWorkerThread(param);
  if (!ok)
    WARN_LOG_FMT(VIDEO, "Failed to initialize shader compiler worker.");
  m_last_worker_ok.store(ok);
  m_worker_ready.Set();
  if (!ok)
    return;

  ServeQueue();
  WorkerThreadExit(param);
}

void AsyncShaderCompiler::ServeQueue()
{
  std::unique_lock lock(m_queue_mutex);
  while (!m_stop_requested.IsSet())
  {
    // Sleep until notified, then work through the queue until it is empty or a stop is asked.
    m_queue_cv.wait(lock);

    while (!m_stop_requested.IsSet() && !m_queue.empty())
    {
      const auto first = m_queue.begin();
      WorkItemPtr item = std::move(first->second);
      m_queue.erase(first);
      ++m_compiling;
      lock.unlock();

      // Failed compiles are dropped without being retrieved.
      if (item->Compile())
      {
        std::lock_guard finished_lock(m_finished_mutex);
        m_finished.push_back(std::move(item));
      }

      lock.lock();
      --m_compiling;
    }
  }
}

}  // namespace VideoCommon
