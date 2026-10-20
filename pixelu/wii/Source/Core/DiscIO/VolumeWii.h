// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Lazy.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

namespace DiscIO
{
class BlobReader;
enum class BlobType;
enum class Country;
class FileSystem;
enum class Language;
enum class Region;
enum class Platform;

// A Wii disc image. The data of each partition is stored in 0x8000-byte blocks: 0x400 bytes of
// hashes followed by 0x7c00 bytes of data, both AES-128-CBC encrypted with the partition's title
// key on retail discs. 64 blocks form a group, which the H3 table covers with one hash.
class VolumeWii final : public VolumeDisc
{
public:
  static constexpr size_t AES_KEY_SIZE = Common::AES::Context::KEY_SIZE;

  static constexpr u32 BLOCKS_PER_GROUP = 0x40;

  static constexpr u64 BLOCK_HEADER_SIZE = 0x0400;
  static constexpr u64 BLOCK_DATA_SIZE = 0x7C00;
  static constexpr u64 BLOCK_TOTAL_SIZE = BLOCK_HEADER_SIZE + BLOCK_DATA_SIZE;

  static constexpr u64 GROUP_HEADER_SIZE = BLOCK_HEADER_SIZE * BLOCKS_PER_GROUP;
  static constexpr u64 GROUP_DATA_SIZE = BLOCK_DATA_SIZE * BLOCKS_PER_GROUP;
  static constexpr u64 GROUP_TOTAL_SIZE = GROUP_HEADER_SIZE + GROUP_DATA_SIZE;

  // The hash area at the start of each block:
  // H0: SHA-1 of each of the block's 31 data chunks of 0x400 bytes.
  // H1: SHA-1 of the H0 table of each block in this block's group of 8 (same in all 8).
  // H2: SHA-1 of the H1 table of each group of 8 in this group of 64 (same in all 64).
  struct HashBlock
  {
    std::array<Common::SHA1::Digest, 31> h0;
    std::array<u8, 20> padding_0;
    std::array<Common::SHA1::Digest, 8> h1;
    std::array<u8, 32> padding_1;
    std::array<Common::SHA1::Digest, 8> h2;
    std::array<u8, 32> padding_2;
  };
  static_assert(sizeof(HashBlock) == BLOCK_HEADER_SIZE);

  VolumeWii(std::unique_ptr<BlobReader> reader);
  ~VolumeWii() override;
  bool Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const override;
  bool HasWiiHashes() const override;
  bool HasWiiEncryption() const override;
  std::vector<Partition> GetPartitions() const override;
  Partition GetGamePartition() const override;
  std::optional<u32> GetPartitionType(const Partition& partition) const override;
  std::optional<u64> GetTitleID(const Partition& partition) const override;
  const IOS::ES::TicketReader& GetTicket(const Partition& partition) const override;
  const IOS::ES::TMDReader& GetTMD(const Partition& partition) const override;
  const std::vector<u8>& GetCertificateChain(const Partition& partition) const override;
  const FileSystem* GetFileSystem(const Partition& partition) const override;
  // Converts an offset in a partition's decrypted data to an offset in the raw image, given the
  // offset of the partition's data area relative to the partition.
  static u64 OffsetInHashedPartitionToRawOffset(u64 offset, const Partition& partition,
                                                u64 partition_data_offset);
  u64 PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const override;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const override;
  std::map<Language, std::string> GetLongNames() const override;
  std::vector<u32> GetBanner(u32* width, u32* height) const override;

  Platform GetVolumeType() const override;
  bool IsDatelDisc() const override;
  bool CheckH3TableIntegrity(const Partition& partition) const override;
  bool CheckBlockIntegrity(u64 block_index, const u8* encrypted_data,
                           const Partition& partition) const override;
  bool CheckBlockIntegrity(u64 block_index, const Partition& partition) const override;

  Region GetRegion() const override;
  BlobType GetBlobType() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override;
  u64 GetRawSize() const override;
  const BlobReader& GetBlobReader() const override;
  std::array<u8, 20> GetSyncHash() const override;

  // Computes the hash blocks of one group from its 64 data blocks. If read_function is given, it
  // is called for each block (in order) before that block is hashed, so reading can overlap
  // hashing; the result is false if read_function ever returned false.
  static bool HashGroup(const std::array<u8, BLOCK_DATA_SIZE> in[BLOCKS_PER_GROUP],
                        HashBlock out[BLOCKS_PER_GROUP],
                        const std::function<bool(size_t block)>& read_function = {});

  // Builds one encrypted group from decrypted partition data read through blob's
  // ReadWiiDecrypted. Blocks past partition_data_decrypted_size are zero. The callback may adjust
  // the hashes before encryption.
  static bool EncryptGroup(u64 offset, u64 partition_data_offset, u64 partition_data_decrypted_size,
                           const std::array<u8, AES_KEY_SIZE>& key, BlobReader* blob,
                           std::array<u8, GROUP_TOTAL_SIZE>* out,
                           const std::function<void(HashBlock hash_blocks[BLOCKS_PER_GROUP])>&
                               hash_exception_callback = {});

  static void DecryptBlockHashes(const u8* in, HashBlock* out, Common::AES::Context* aes_context);
  static void DecryptBlockData(const u8* in, u8* out, Common::AES::Context* aes_context);

protected:
  u32 GetOffsetShift() const override { return 2; }

private:
  // Everything about a partition is read on first use.
  struct PartitionInfo
  {
    u32 type = 0;
    Common::Lazy<IOS::ES::TicketReader> ticket;
    Common::Lazy<IOS::ES::TMDReader> tmd;
    Common::Lazy<std::vector<u8>> cert_chain;
    Common::Lazy<std::vector<u8>> h3_table;
    Common::Lazy<std::unique_ptr<Common::AES::Context>> key;
    Common::Lazy<u64> data_offset;  // relative to the partition
    Common::Lazy<std::unique_ptr<FileSystem>> file_system;
  };

  void AddPartition(const Partition& partition, u32 type);
  const PartitionInfo* FindPartition(const Partition& partition) const;
  bool ReadFromPartition(u64 offset, u64 length, u8* buffer, const Partition& partition,
                         const PartitionInfo& info) const;

  std::unique_ptr<BlobReader> m_reader;
  std::map<Partition, PartitionInfo> m_partitions;
  Partition m_game_partition = PARTITION_NONE;
  bool m_has_hashes = false;
  bool m_has_encryption = false;

  // The most recently decrypted block, identified by its raw offset.
  mutable u64 m_cached_block_offset = UINT64_MAX;
  mutable std::array<u8, BLOCK_DATA_SIZE> m_cached_block{};
};

}  // namespace DiscIO
