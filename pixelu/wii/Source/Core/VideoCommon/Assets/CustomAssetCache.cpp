// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/CustomAssetCache.h"

#include <algorithm>
#include <utility>

#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"

#include "UICommon/UICommon.h"

#include "VideoCommon/Assets/CustomAsset.h"

namespace VideoCommon
{
// ---------------------------------------------------------------------------------------------
// RecencyQueue
// ---------------------------------------------------------------------------------------------

std::list<CustomAsset*>::iterator& CustomAssetCache::RecencyQueue::Slot(std::size_t handle)
{
  if (handle >= m_position.size())
    m_position.resize(handle + 1, m_order.end());
  return m_position[handle];
}

void CustomAssetCache::RecencyQueue::Promote(std::size_t handle, CustomAsset* asset)
{
  auto& slot = Slot(handle);
  if (slot != m_order.end())
    m_order.erase(slot);
  slot = m_order.insert(m_order.begin(), asset);
}

void CustomAssetCache::RecencyQueue::Append(std::size_t handle, CustomAsset* asset)
{
  auto& slot = Slot(handle);
  if (slot == m_order.end())
    slot = m_order.insert(m_order.end(), asset);
}

void CustomAssetCache::RecencyQueue::Erase(std::size_t handle)
{
  if (handle >= m_position.size())
    return;
  auto& slot = m_position[handle];
  if (slot == m_order.end())
    return;
  m_order.erase(slot);
  slot = m_order.end();
}

CustomAsset* CustomAssetCache::RecencyQueue::PopBack()
{
  CustomAsset* const asset = m_order.back();
  m_position[asset->GetHandle()] = m_order.end();
  m_order.pop_back();
  return asset;
}

// ---------------------------------------------------------------------------------------------
// CustomAssetCache
// ---------------------------------------------------------------------------------------------

void CustomAssetCache::Initialize()
{
  // Budget: at most half of physical memory, and never touch the last 2 GiB.
  constexpr std::size_t reserve_floor = std::size_t{2} << 30;
  const std::size_t physical = Common::MemPhysical();
  const std::size_t reserved = std::max(physical / 2, std::min(physical, reserve_floor));
  m_byte_budget = physical - reserved;

  if (m_byte_budget == 0)
    ERROR_LOG_FMT(VIDEO, "Not enough system memory for custom resources.");

  m_loader.Initialize();
}

void CustomAssetCache::Shutdown()
{
  Reset();
  m_loader.Shutdown();
}

void CustomAssetCache::Reset()
{
  m_loader.Reset(true);

  m_loaded = {};
  m_wanted = {};
  m_assets.clear();
  m_handles.clear();
  m_dirty.clear();
  m_bytes_loaded = 0;
}

void CustomAssetCache::MarkAssetDirty(const CustomAssetLibrary::AssetID& asset_id)
{
  std::lock_guard guard(m_dirty_mutex);
  m_dirty.insert(asset_id);
}

void CustomAssetCache::MarkAssetPending(CustomAsset* asset)
{
  m_wanted.Promote(asset->GetHandle(), asset);
}

void CustomAssetCache::MarkAssetActive(CustomAsset* asset)
{
  m_loaded.Promote(asset->GetHandle(), asset);
}

void CustomAssetCache::Update()
{
  TakeDirtyAssets();
  TakeLoadResults();

  if (m_bytes_loaded > m_byte_budget)
    EvictToBudget();

  if (m_wanted.Empty() || m_bytes_loaded > m_byte_budget)
    return;

  m_loader.ScheduleAssetsToLoad(m_wanted.Items(), m_byte_budget - m_bytes_loaded);
}

void CustomAssetCache::TakeDirtyAssets()
{
  // Never stall the video thread on this: if the watcher holds the lock, try next frame.
  std::set<CustomAssetLibrary::AssetID> dirty;
  {
    std::unique_lock lock(m_dirty_mutex, std::try_to_lock);
    if (lock.owns_lock())
      dirty.swap(m_dirty);
  }

  const CustomAsset::TimeType requested = CustomAsset::ClockType::now();
  for (const CustomAssetLibrary::AssetID& id : dirty)
  {
    const auto found = m_handles.find(id);
    if (found == m_handles.end())
      continue;

    AssetData& data = m_assets[found->second];
    data.load_status = AssetData::LoadStatus::PendingReload;
    data.load_request_time = requested;
    // A new version on disk deserves a fresh attempt.
    data.has_load_error = false;
    m_wanted.Append(found->second, data.asset.get());

    DEBUG_LOG_FMT(VIDEO, "Dirty asset pending reload: {}", data.asset->GetAssetId());
  }
}

void CustomAssetCache::TakeLoadResults()
{
  const CustomAssetLoader::LoadResults results = m_loader.TakeLoadResults();

  // The loader reports net bytes. While an asset reloads, its old and new data briefly both
  // exist; only the loader's accounting is tracked here.
  m_bytes_loaded += static_cast<u64>(results.change_in_memory);

  for (const auto& [handle, succeeded] : results.asset_handles)
  {
    AssetData& data = m_assets[handle];

    // A reload was requested after this load began, so the result is already stale.
    if (data.load_request_time > data.asset->GetLastLoadedTime())
      continue;

    m_wanted.Erase(handle);
    data.load_request_time = {};

    if (succeeded)
    {
      m_loaded.Append(handle, data.asset.get());
      data.load_status = AssetData::LoadStatus::LoadFinished;
    }
    else
    {
      data.has_load_error = true;
    }

    for (AssetListener* listener : data.listeners)
    {
      if (succeeded)
        listener->NotifyAssetLoadSuccess();
      else
        listener->NotifyAssetLoadFailed();
    }
  }
}

void CustomAssetCache::EvictToBudget()
{
  // Evict down to 80% of the budget so the next few loads do not trigger eviction again.
  const u64 target = m_byte_budget * 8 / 10;
  if (m_bytes_loaded > target)
    INFO_LOG_FMT(VIDEO, "Memory usage over threshold: {}", UICommon::FormatSize(m_bytes_loaded));

  while (m_bytes_loaded > target && !m_loaded.Empty())
  {
    CustomAsset* const victim = m_loaded.PopBack();
    AssetData& data = m_assets[victim->GetHandle()];

    for (AssetListener* listener : data.listeners)
      listener->AssetUnloaded();

    const std::size_t freed = data.asset->Unload();
    m_bytes_loaded -= freed;
    data.load_status = AssetData::LoadStatus::Unloaded;
    data.load_request_time = {};

    INFO_LOG_FMT(VIDEO, "Unloading asset: {} ({})", data.asset->GetAssetId(),
                 UICommon::FormatSize(freed));
  }
}

}  // namespace VideoCommon
