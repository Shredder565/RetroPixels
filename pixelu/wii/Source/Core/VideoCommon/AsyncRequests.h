// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <concepts>
#include <functional>
#include <future>

#include "Common/Functional.h"
#include "Common/SPSCQueue.h"

struct EfbPokeData;
class PointerWrap;

// Hands work from the CPU thread to the video thread. In passthrough mode (single core, or
// while the video thread is not running) requests simply run on the calling thread.
class AsyncRequests
{
public:
  AsyncRequests();

  // Video thread: run everything that has been queued so far.
  void PullEvents();

  // CPU thread: block until the video thread has drained the queue.
  void WaitForEmptyQueue();

  // CPU thread: run `callback` on the video thread without waiting for it.
  template <std::invocable<> F>
  void PushEvent(F&& callback)
  {
    if (!m_passthrough)
    {
      QueueEvent(Event{std::forward<F>(callback)});
      return;
    }
    std::invoke(std::forward<F>(callback));
  }

  // CPU thread: run `callback` on the video thread and wait for its result.
  template <std::invocable<> F>
  auto PushBlockingEvent(F&& callback) -> std::invoke_result_t<F>
  {
    if (m_passthrough)
      return std::invoke(std::forward<F>(callback));

    // The queued wrapper refers to the task on this stack frame, which stays alive because we
    // wait on the result before returning.
    std::packaged_task job{std::forward<F>(callback)};
    auto result = job.get_future();
    QueueEvent(Event{[&job] { job(); }});
    return result.get();
  }

  // Only changed while the video thread is not running.
  void SetPassthrough(bool enable);

  static AsyncRequests* GetInstance() { return &s_singleton; }

private:
  using Event = Common::MoveOnlyFunction<void()>;

  void QueueEvent(Event&& event);

  static AsyncRequests s_singleton;

  Common::WaitableSPSCQueue<Event> m_queue;

  bool m_passthrough = true;
};
