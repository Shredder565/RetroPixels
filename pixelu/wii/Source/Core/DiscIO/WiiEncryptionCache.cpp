// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/WiiEncryptionCache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
WiiEncryptionCache::WiiEncryptionCache(BlobReader* blob) : m_blob(blob)
{
}

WiiEncryptionCache::~WiiEncryptionCache() = default;

const std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>*
WiiEncryptionCache::EncryptGroup(u64 offset, u64 partition_data_offset,
                                 u64 partition_data_decrypted_size, const Key& key,
                                 const HashExceptionCallback& hash_exception_callback)
{
  ASSERT(offset % VolumeWii::GROUP_TOTAL_SIZE == 0);

  const u64 disc_offset = partition_data_offset + offset;
  if (m_group && m_group_valid && m_group_disc_offset == disc_offset)
    return m_group.get();

  if (!m_group)
    m_group = std::make_unique<Group>();

  // VolumeWii's callback doesn't take the offset; supply it here.
  std::function<void(VolumeWii::HashBlock*)> patch_hashes;
  if (hash_exception_callback)
  {
    patch_hashes = [&hash_exception_callback, offset](VolumeWii::HashBlock* hash_blocks) {
      hash_exception_callback(hash_blocks, offset);
    };
  }

  const u64 decrypted_offset = offset / VolumeWii::GROUP_TOTAL_SIZE * VolumeWii::GROUP_DATA_SIZE;
  m_group_valid = VolumeWii::EncryptGroup(decrypted_offset, partition_data_offset,
                                          partition_data_decrypted_size, key, m_blob,
                                          m_group.get(), patch_hashes);
  m_group_disc_offset = disc_offset;
  return m_group_valid ? m_group.get() : nullptr;
}

bool WiiEncryptionCache::EncryptGroups(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset,
                                       u64 partition_data_decrypted_size, const Key& key,
                                       const HashExceptionCallback& hash_exception_callback)
{
  constexpr u64 GROUP = VolumeWii::GROUP_TOTAL_SIZE;
  while (size > 0)
  {
    const u64 in_group = offset % GROUP;
    const Group* group = EncryptGroup(offset - in_group, partition_data_offset,
                                      partition_data_decrypted_size, key, hash_exception_callback);
    if (!group)
      return false;

    const u64 n = std::min(GROUP - in_group, size);
    std::memcpy(out_ptr, group->data() + in_group, n);
    offset += n;
    out_ptr += n;
    size -= n;
  }
  return true;
}

}  // namespace DiscIO
