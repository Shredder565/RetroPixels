// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <functional>
#include <memory>

#include "Common/CommonTypes.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
class BlobReader;

// Rebuilds encrypted Wii partition data (hashes and AES) from a blob that can hand out the
// decrypted data, one 2 MiB group at a time, and remembers the last group it built.
class WiiEncryptionCache
{
public:
  using Key = std::array<u8, VolumeWii::AES_KEY_SIZE>;
  // Called with a group's freshly computed hash blocks before they're encrypted, so that the
  // caller can patch in hashes that differ on the original disc. `offset` is the group's
  // offset within the encrypted partition data.
  using HashExceptionCallback = std::function<void(
      VolumeWii::HashBlock hash_blocks[VolumeWii::BLOCKS_PER_GROUP], u64 offset)>;

  // `blob` is not owned and must outlive this object.
  explicit WiiEncryptionCache(BlobReader* blob);
  ~WiiEncryptionCache();

  WiiEncryptionCache(WiiEncryptionCache&&) = default;
  WiiEncryptionCache& operator=(WiiEncryptionCache&&) = default;
  WiiEncryptionCache(const WiiEncryptionCache&) = delete;
  WiiEncryptionCache& operator=(const WiiEncryptionCache&) = delete;

  // Builds the group at `offset` (a multiple of GROUP_TOTAL_SIZE within the encrypted
  // partition data). Returns null if reading from the blob failed; otherwise the result stays
  // valid until the next call or until this object is destroyed. Only the disc offset is used
  // to recognise a cached group.
  const std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>*
  EncryptGroup(u64 offset, u64 partition_data_offset, u64 partition_data_decrypted_size,
               const Key& key, const HashExceptionCallback& hash_exception_callback = {});

  // Any byte range of encrypted partition data, possibly covering partial groups.
  bool EncryptGroups(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset,
                     u64 partition_data_decrypted_size, const Key& key,
                     const HashExceptionCallback& hash_exception_callback = {});

private:
  using Group = std::array<u8, VolumeWii::GROUP_TOTAL_SIZE>;

  BlobReader* m_blob;
  std::unique_ptr<Group> m_group;  // allocated on first use
  bool m_group_valid = false;
  u64 m_group_disc_offset = 0;
};

}  // namespace DiscIO
