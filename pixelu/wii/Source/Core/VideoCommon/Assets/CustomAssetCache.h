// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/Assets/AssetListener.h"
#include "VideoCommon/Assets/CustomAsset.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/CustomAssetLoader.h"

namespace VideoCommon
{
// Owns every custom asset (textures, shaders, materials, meshes) of the session.
//
// Assets that are wanted get queued for loading, most recently requested first, and loaded
// within a RAM budget derived from the machine's physical memory. When the loaded assets grow
// past the budget, the ones used least recently are unloaded again.
class CustomAssetCache
{
public:
  // Bookkeeping for one asset.
  struct AssetData
  {
    std::unique_ptr<CustomAsset> asset;

    // Told about load results and unloads. One entry per CreateAsset call.
    std::vector<AssetListener*> listeners;

    // Set when a reload is requested; a load that started earlier does not satisfy it.
    CustomAsset::TimeType load_request_time = {};
    bool has_load_error = false;

    enum class LoadStatus
    {
      PendingReload,
      LoadFinished,
      Unloaded,
    };
    LoadStatus load_status = LoadStatus::PendingReload;
  };

  // Returns the asset named asset_id, creating it as a T the first time, and registers
  // `listener` for it.
  template <typename T>
  T* CreateAsset(const CustomAssetLibrary::AssetID& asset_id,
                 std::shared_ptr<VideoCommon::CustomAssetLibrary> library, AssetListener* listener)
  {
    std::size_t handle;
    if (const auto found = m_handles.find(asset_id); found != m_handles.end())
    {
      handle = found->second;
    }
    else
    {
      // Handles are handed out in creation order, so they double as indices into m_assets.
      handle = m_assets.size();
      m_handles.emplace(asset_id, handle);
      AssetData& fresh = m_assets.emplace_back();
      fresh.asset = std::make_unique<T>(std::move(library), asset_id, handle);
    }

    AssetData& data = m_assets[handle];
    data.listeners.push_back(listener);
    data.load_status = AssetData::LoadStatus::PendingReload;
    return static_cast<T*>(data.asset.get());
  }

  // Null when no asset with that id was created.
  AssetData* GetAssetData(const CustomAssetLibrary::AssetID& asset_id)
  {
    const auto found = m_handles.find(asset_id);
    return found != m_handles.end() ? &m_assets[found->second] : nullptr;
  }

  void Initialize();
  void Shutdown();

  void Reset();

  // Queues a reload of the asset (for example because its file changed). Thread safe.
  void MarkAssetDirty(const CustomAssetLibrary::AssetID& asset_id);

  // Something is waiting for this asset: load it before other pending assets.
  void MarkAssetPending(CustomAsset* asset);

  // The asset was just used: keep it loaded longer than other assets.
  void MarkAssetActive(CustomAsset* asset);

  // Applies reload requests and load results, enforces the RAM budget and schedules loads.
  void Update();

private:
  // Assets ordered from most to least recently used, with constant time lookup by handle.
  class RecencyQueue
  {
  public:
    const std::list<CustomAsset*>& Items() const { return m_order; }
    bool Empty() const { return m_order.empty(); }
    std::size_t Count() const { return m_order.size(); }

    // Puts the asset at the front, moving it there if already queued.
    void Promote(std::size_t handle, CustomAsset* asset);
    // Puts the asset at the back unless it is already queued.
    void Append(std::size_t handle, CustomAsset* asset);
    void Erase(std::size_t handle);
    // Takes the asset at the back (the least recently used). The queue must not be empty.
    CustomAsset* PopBack();

  private:
    std::list<CustomAsset*>::iterator& Slot(std::size_t handle);

    std::list<CustomAsset*> m_order;
    // Indexed by handle; m_order.end() marks "not queued".
    std::vector<std::list<CustomAsset*>::iterator> m_position;
  };

  void TakeDirtyAssets();
  void TakeLoadResults();
  void EvictToBudget();

  // Loaded assets, most recently used first; eviction takes from the back.
  RecencyQueue m_loaded;
  // Assets waiting to be loaded, most recently requested first.
  RecencyQueue m_wanted;

  // Indexed by handle. A deque so AssetData references survive growth.
  std::deque<AssetData> m_assets;
  std::map<CustomAssetLibrary::AssetID, std::size_t> m_handles;

  u64 m_bytes_loaded = 0;
  u64 m_byte_budget = 0;

  std::mutex m_dirty_mutex;
  std::set<CustomAssetLibrary::AssetID> m_dirty;

  CustomAssetLoader m_loader;
};

}  // namespace VideoCommon
