// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "Common/Flag.h"
#include "VideoCommon/Assets/CustomAsset.h"

namespace VideoCommon
{
// Loads assets on a small pool of worker threads. The cache hands it a list of assets in
// priority order together with a memory budget, then collects the results each frame.
class CustomAssetLoader
{
public:
  CustomAssetLoader() = default;
  ~CustomAssetLoader() = default;
  CustomAssetLoader(const CustomAssetLoader&) = delete;
  CustomAssetLoader(CustomAssetLoader&&) = delete;
  CustomAssetLoader& operator=(const CustomAssetLoader&) = delete;
  CustomAssetLoader& operator=(CustomAssetLoader&&) = delete;

  void Initialize();
  void Shutdown();

  // An asset handle and whether its load produced data.
  using AssetHandle = std::pair<std::size_t, bool>;
  struct LoadResults
  {
    std::vector<AssetHandle> asset_handles;
    // Bytes gained minus bytes released since the previous call.
    s64 change_in_memory;
  };

  // Hands over everything finished since the previous call.
  LoadResults TakeLoadResults();

  // Replaces the work queue with `assets_to_load` (highest priority first). Workers stop taking
  // new items once they have loaded more than `allowed_memory` bytes.
  void ScheduleAssetsToLoad(std::list<CustomAsset*> assets_to_load, u64 allowed_memory);

  // Drops queued work and pending results; the pool is restarted with the same size if asked.
  void Reset(bool restart_worker_threads = true);

private:
  void SpawnWorkers(std::size_t count);
  void JoinWorkers();
  void WorkerMain(u32 index);

  std::vector<std::thread> m_workers;
  Common::Flag m_stopping;

  // Work queue, guarded by m_queue_mutex together with m_budget and m_busy.
  std::mutex m_queue_mutex;
  std::condition_variable m_queue_cv;
  std::list<CustomAsset*> m_queue;
  s64 m_budget = 0;
  std::set<std::size_t> m_busy;

  // Finished work, guarded by m_results_mutex.
  std::mutex m_results_mutex;
  std::vector<AssetHandle> m_finished;

  std::atomic<s64> m_memory_delta = 0;
};
}  // namespace VideoCommon
