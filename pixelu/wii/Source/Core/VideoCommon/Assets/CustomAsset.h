// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace VideoCommon
{
// Base for every asset: one entry of a CustomAssetLibrary that can be loaded and unloaded.
// Subclasses implement LoadImpl/UnloadImpl for their own data type.
class CustomAsset
{
public:
  using ClockType = std::chrono::steady_clock;
  using TimeType = ClockType::time_point;

  CustomAsset(std::shared_ptr<CustomAssetLibrary> library, CustomAssetLibrary::AssetID asset_id,
              u64 session_id);
  virtual ~CustomAsset() = default;
  CustomAsset(const CustomAsset&) = delete;
  CustomAsset(CustomAsset&&) = delete;
  CustomAsset& operator=(const CustomAsset&) = delete;
  CustomAsset& operator=(CustomAsset&&) = delete;

  // Reads the asset; returns the bytes read, or 0 if the load produced nothing.
  std::size_t Load();

  // Drops the asset's data; returns the byte count it was holding.
  std::size_t Unload();

  // When the data currently held was read (the moment the read began).
  TimeType GetLastLoadedTime() const;

  const CustomAssetLibrary::AssetID& GetAssetId() const;

  // A small number naming this asset for the current session, cheaper to use as a key than
  // the asset id.
  std::size_t GetHandle() const;

protected:
  const std::shared_ptr<CustomAssetLibrary> m_owning_library;

private:
  virtual CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID& asset_id) = 0;
  virtual void UnloadImpl() = 0;

  CustomAssetLibrary::AssetID m_id;
  std::size_t m_session_handle;

  // Serializes Load and Unload.
  mutable std::mutex m_load_mutex;
  std::size_t m_resident_bytes = 0;
  std::atomic<TimeType> m_load_time = {};
};

// An asset whose loaded form is a shared UnderlyingType.
template <typename UnderlyingType>
class CustomLoadableAsset : public CustomAsset
{
public:
  using CustomAsset::CustomAsset;

  // Returns the data held right now, or null when nothing is loaded. A reload replaces the
  // pointer, so callers that care about reloads should call this again rather than keep it.
  [[nodiscard]] std::shared_ptr<UnderlyingType> GetData() const
  {
    std::lock_guard guard(m_data_mutex);
    return m_has_data ? m_data : nullptr;
  }

protected:
  // Lets `read` fill a new UnderlyingType and makes it the current data if any bytes were
  // read. The previous data stays current when the read produces nothing.
  template <typename ReadFn>
  CustomAssetLibrary::LoadInfo ReadAndPublish(ReadFn&& read)
  {
    auto fresh = std::make_shared<UnderlyingType>();
    const CustomAssetLibrary::LoadInfo info = read(fresh.get());
    if (info.bytes_loaded == 0)
      return {};

    std::lock_guard guard(m_data_mutex);
    m_data = std::move(fresh);
    m_has_data = true;
    return info;
  }

  // Guarded by m_data_mutex.
  bool m_has_data = false;
  mutable std::mutex m_data_mutex;
  std::shared_ptr<UnderlyingType> m_data;

private:
  void UnloadImpl() override
  {
    std::lock_guard guard(m_data_mutex);
    m_data.reset();
    m_has_data = false;
  }
};

// An asset paired with the load time its user last saw, so the user can tell whether the asset
// has been reloaded since.
template <typename AssetType>
struct CachedAsset
{
  std::shared_ptr<AssetType> m_asset;
  CustomAsset::TimeType m_cached_write_time;
};

}  // namespace VideoCommon
