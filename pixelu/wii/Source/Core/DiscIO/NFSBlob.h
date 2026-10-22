// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

// NFS: how Wii games sold on the Wii U eShop are stored. The disc is cut into 32 KiB blocks;
// only the blocks listed in the header's ranges are stored, each AES-128-CBC encrypted with
// the title key from ../code/htk.bin and an IV holding the block number. A 0x200-byte header
// and the stored blocks form one stream, split into hif_000000.nfs, hif_000001.nfs, ... of
// 0xFA00000 bytes each.

namespace DiscIO
{
static constexpr u32 NFS_MAGIC = 0x53474745;  // "EGGS" read as a little endian u32

// Big endian on disk
struct NFSLBARange
{
  u32 start_block;
  u32 num_blocks;
};

// Big endian on disk
struct NFSHeader
{
  u32 magic;  // "EGGS"
  u32 version;
  u32 unknown_1;
  u32 unknown_2;
  u32 lba_range_count;
  std::array<NFSLBARange, 61> lba_ranges;
  u32 end_magic;  // "SGGE"
};
static_assert(sizeof(NFSHeader) == 0x200);

class NFSFileReader final : public BlobReader
{
public:
  // `first_file` is hif_000000.nfs, found at `directory_path`.
  static std::unique_ptr<NFSFileReader> Create(File::DirectIOFile first_file,
                                               const std::string& directory_path);

  BlobType GetBlobType() const override { return BlobType::NFS; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override { return DataSizeType::LowerBound; }

  u64 GetBlockSize() const override { return BLOCK_SIZE; }
  bool HasFastRandomAccessInBlock() const override { return false; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr) override;

private:
  using Key = std::array<u8, Common::AES::Context::KEY_SIZE>;
  static constexpr u32 BLOCK_SIZE = 0x8000;
  static constexpr u32 MAX_FILE_SIZE = 0xFA00000;
  static constexpr u64 NOT_STORED = std::numeric_limits<u64>::max();

  struct Range
  {
    u32 first;          // first disc block
    u32 count;          // number of blocks
    u64 stored_before;  // stored blocks in all earlier ranges
  };

  NFSFileReader(std::vector<Range> ranges, std::vector<File::DirectIOFile> files, const Key& key,
                u64 raw_size, u64 data_size);

  u64 StoredIndexOf(u64 disc_block) const;
  bool LoadBlock(u64 disc_block);

  std::vector<Range> m_ranges;
  std::vector<File::DirectIOFile> m_files;
  Key m_key;
  std::unique_ptr<Common::AES::Context> m_aes;
  u64 m_raw_size;
  u64 m_data_size;

  u64 m_loaded_block = NOT_STORED;
  std::array<u8, BLOCK_SIZE> m_encrypted;
  std::array<u8, BLOCK_SIZE> m_decrypted;
};

}  // namespace DiscIO
