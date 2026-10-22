// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace DiscIO
{
enum class ConversionResultCode
{
  Success,
  Canceled,
  ReadFailed,
  WriteFailed,
  InternalError,
};

template <typename T>
using ConversionResult = std::expected<T, ConversionResultCode>;

// Runs the expensive half of a conversion (compression) on a pool of worker threads and the
// cheap half (writing) on a single output thread, which receives the results in exactly the
// order the work was submitted, so the output file doesn't depend on thread timing.
//
// Each worker owns one ThreadState, constructed on that thread and passed to `set_up` once.
// After any step fails (or `output` returns an error such as Canceled), further submissions
// are ignored and results that haven't been written yet are dropped. Callers should check
// GetStatus() between submissions and must call Shutdown() (or let the destructor do it) to
// wait for everything to finish.
template <typename ThreadState, typename Input, typename Output>
class MultithreadedCompressor
{
public:
  using SetUpFn = std::function<ConversionResultCode(ThreadState*)>;
  using CompressFn = std::function<ConversionResult<Output>(ThreadState*, Input)>;
  using OutputFn = std::function<ConversionResultCode(Output)>;

  MultithreadedCompressor(SetUpFn set_up, CompressFn compress, OutputFn output)
      : m_set_up(std::move(set_up)), m_compress(std::move(compress)), m_output(std::move(output))
  {
    const size_t worker_count = std::max(1u, std::thread::hardware_concurrency());
    // Allow a little queueing on top of the work in progress so workers rarely sit idle.
    m_max_in_flight = worker_count * 2;

    m_workers.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i)
      m_workers.emplace_back([this] { WorkerMain(); });
    m_writer = std::thread([this] { WriterMain(); });
  }

  MultithreadedCompressor(const MultithreadedCompressor&) = delete;
  MultithreadedCompressor& operator=(const MultithreadedCompressor&) = delete;

  ~MultithreadedCompressor() { Shutdown(); }

  // Queues one piece of work. Blocks while too much work is outstanding.
  void CompressAndWrite(Input input)
  {
    std::unique_lock lock(m_mutex);
    if (m_status != ConversionResultCode::Success || m_stopping)
      return;

    m_space_cv.wait(lock, [this] { return m_next_seq - m_next_to_write < m_max_in_flight; });

    m_queue.push_back(Job{m_next_seq++, std::move(input)});
    m_work_cv.notify_one();
  }

  // Records a failure. Only the first failure is kept.
  void SetError(ConversionResultCode code)
  {
    ASSERT(code != ConversionResultCode::Success);
    std::lock_guard lock(m_mutex);
    SetErrorLocked(code);
  }

  ConversionResultCode GetStatus() const
  {
    std::lock_guard lock(m_mutex);
    return m_status;
  }

  // Waits until all submitted work has been compressed and written (or dropped), then stops
  // the threads. Safe to call more than once.
  void Shutdown()
  {
    {
      std::lock_guard lock(m_mutex);
      if (m_stopping)
        return;
      m_stopping = true;
    }
    m_work_cv.notify_all();
    m_result_cv.notify_all();

    for (std::thread& worker : m_workers)
      worker.join();
    m_writer.join();
  }

private:
  struct Job
  {
    u64 seq;
    Input input;
  };

  void SetErrorLocked(ConversionResultCode code)
  {
    if (m_status == ConversionResultCode::Success)
      m_status = code;
  }

  void WorkerMain()
  {
    ThreadState state;
    const ConversionResultCode set_up_result = m_set_up(&state);
    const bool usable = set_up_result == ConversionResultCode::Success;
    if (!usable)
      SetError(set_up_result);

    std::unique_lock lock(m_mutex);
    while (true)
    {
      m_work_cv.wait(lock, [this] { return !m_queue.empty() || m_stopping; });
      if (m_queue.empty())
        return;  // stopping, and nothing left to do

      Job job = std::move(m_queue.front());
      m_queue.pop_front();
      lock.unlock();

      std::optional<Output> result;
      std::optional<ConversionResultCode> failure;
      if (usable)
      {
        ConversionResult<Output> compressed = m_compress(&state, std::move(job.input));
        if (compressed)
          result.emplace(std::move(*compressed));
        else
          failure = compressed.error();
      }

      lock.lock();
      if (failure)
        SetErrorLocked(*failure);
      m_results.emplace(job.seq, std::move(result));
      m_result_cv.notify_all();
    }
  }

  void WriterMain()
  {
    std::unique_lock lock(m_mutex);
    while (true)
    {
      m_result_cv.wait(lock, [this] {
        return m_results.contains(m_next_to_write) ||
               (m_stopping && m_next_to_write == m_next_seq);
      });

      auto it = m_results.find(m_next_to_write);
      if (it == m_results.end())
        return;  // stopping, and everything submitted has been handled

      std::optional<Output> result = std::move(it->second);
      m_results.erase(it);
      const bool write = result.has_value() && m_status == ConversionResultCode::Success;
      lock.unlock();

      if (write)
      {
        const ConversionResultCode code = m_output(std::move(*result));
        if (code != ConversionResultCode::Success)
          SetError(code);
      }

      lock.lock();
      ++m_next_to_write;
      m_space_cv.notify_all();
    }
  }

  SetUpFn m_set_up;
  CompressFn m_compress;
  OutputFn m_output;

  mutable std::mutex m_mutex;
  std::condition_variable m_work_cv;    // a job was queued, or stopping
  std::condition_variable m_result_cv;  // a job finished, or stopping
  std::condition_variable m_space_cv;   // a result was written

  std::deque<Job> m_queue;
  std::map<u64, std::optional<Output>> m_results;  // nullopt: the job failed
  u64 m_next_seq = 0;
  u64 m_next_to_write = 0;
  size_t m_max_in_flight = 1;

  ConversionResultCode m_status = ConversionResultCode::Success;
  bool m_stopping = false;

  std::vector<std::thread> m_workers;
  std::thread m_writer;
};

}  // namespace DiscIO
