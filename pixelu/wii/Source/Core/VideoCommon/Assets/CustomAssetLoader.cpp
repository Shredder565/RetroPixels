// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/CustomAssetLoader.h"

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "UICommon/UICommon.h"

namespace VideoCommon
{
namespace
{
constexpr std::size_t WORKER_COUNT = 2;
}

void CustomAssetLoader::Initialize()
{
  if (m_workers.size() == WORKER_COUNT)
    return;
  JoinWorkers();
  SpawnWorkers(WORKER_COUNT);
}

void CustomAssetLoader::Shutdown()
{
  Reset(false);
}

void CustomAssetLoader::SpawnWorkers(std::size_t count)
{
  m_workers.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    m_workers.emplace_back(&CustomAssetLoader::WorkerMain, this, static_cast<u32>(i));
}

void CustomAssetLoader::JoinWorkers()
{
  if (m_workers.empty())
    return;

  {
    std::lock_guard guard(m_queue_mutex);
    m_stopping.Set();
    m_queue_cv.notify_all();
  }
  for (std::thread& worker : m_workers)
    worker.join();
  m_workers.clear();
  m_stopping.Clear();
}

void CustomAssetLoader::WorkerMain(u32 index)
{
  Common::SetCurrentThreadName(fmt::format("Asset Loader {}", index).c_str());

  std::unique_lock queue_lock(m_queue_mutex);
  for (;;)
  {
    m_queue_cv.wait(queue_lock, [this] { return m_stopping.IsSet() || !m_queue.empty(); });
    if (m_stopping.IsSet())
      break;

    // Over budget: abandon this batch; the cache schedules again with a fresh budget.
    if (m_memory_delta > m_budget)
    {
      m_queue.clear();
      continue;
    }

    CustomAsset* const asset = m_queue.front();
    m_queue.pop_front();
    const std::size_t handle = asset->GetHandle();

    // Another worker already has this asset.
    if (!m_busy.insert(handle).second)
      continue;

    queue_lock.unlock();
    // Release the old copy before reading the new one.
    m_memory_delta -= static_cast<s64>(asset->Unload());
    const std::size_t bytes = asset->Load();
    m_memory_delta += static_cast<s64>(bytes);
    queue_lock.lock();

    INFO_LOG_FMT(VIDEO, "CustomAssetLoader thread {} loaded: {} ({})", index, asset->GetAssetId(),
                 UICommon::FormatSize(bytes));
    {
      std::lock_guard results_guard(m_results_mutex);
      m_finished.emplace_back(handle, bytes != 0);
      // The asset may have been queued again meanwhile; the cache decides about another load
      // once it has seen this result.
      std::erase(m_queue, asset);
    }
    m_busy.erase(handle);
  }
}

CustomAssetLoader::LoadResults CustomAssetLoader::TakeLoadResults()
{
  std::lock_guard guard(m_results_mutex);
  LoadResults results;
  results.asset_handles = std::move(m_finished);
  m_finished.clear();
  results.change_in_memory = m_memory_delta.exchange(0);
  return results;
}

void CustomAssetLoader::ScheduleAssetsToLoad(std::list<CustomAsset*> assets_to_load,
                                             u64 allowed_memory)
{
  if (assets_to_load.empty()) [[unlikely]]
    return;

  std::lock_guard guard(m_queue_mutex);
  m_budget = static_cast<s64>(allowed_memory);
  m_queue = std::move(assets_to_load);
  m_queue_cv.notify_all();
}

void CustomAssetLoader::Reset(bool restart_worker_threads)
{
  const std::size_t pool_size = m_workers.size();
  JoinWorkers();

  m_queue.clear();
  m_finished.clear();
  m_budget = 0;
  m_memory_delta = 0;

  if (restart_worker_threads)
    SpawnWorkers(pool_size);
}
}  // namespace VideoCommon
