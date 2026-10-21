// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/CustomAsset.h"

#include <utility>

namespace VideoCommon
{
CustomAsset::CustomAsset(std::shared_ptr<CustomAssetLibrary> library,
                         CustomAssetLibrary::AssetID asset_id, u64 session_id)
    : m_owning_library(std::move(library)), m_id(std::move(asset_id)), m_session_handle(session_id)
{
}

std::size_t CustomAsset::Load()
{
  std::lock_guard guard(m_load_mutex);

  // Take the timestamp before reading: a file changed while it is being read must still look
  // newer than what was loaded.
  const TimeType started = ClockType::now();

  const std::size_t bytes = LoadImpl(m_id).bytes_loaded;
  if (bytes == 0)
    return 0;

  m_resident_bytes = bytes;
  m_load_time = started;
  return bytes;
}

std::size_t CustomAsset::Unload()
{
  std::lock_guard guard(m_load_mutex);
  UnloadImpl();
  const std::size_t freed = m_resident_bytes;
  m_resident_bytes = 0;
  return freed;
}

CustomAsset::TimeType CustomAsset::GetLastLoadedTime() const
{
  return m_load_time.load();
}

const CustomAssetLibrary::AssetID& CustomAsset::GetAssetId() const
{
  return m_id;
}

std::size_t CustomAsset::GetHandle() const
{
  return m_session_handle;
}
}  // namespace VideoCommon
