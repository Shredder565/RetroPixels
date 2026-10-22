// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
// GCZ: the disc split into equal blocks, each deflated separately (or stored raw when deflate
// doesn't help). Layout, all little endian:
//   CompressedBlobHeader
//   u64 block_offsets[num_blocks]  (relative to the end of the hash table; bit 63 = stored raw)
//   u32 block_adler32[num_blocks]  (of the bytes as stored in the file)
//   block data
static constexpr u32 GCZ_MAGIC = 0xB10BC001;

struct CompressedBlobHeader  // 32 bytes
{
  u32 magic_cookie;
  u32 sub_type;  // 0 = GameCube, 1 = Wii
  u64 compressed_data_size;
  u64 disc_size;
  u32 block_size;
  u32 num_blocks;
};

class CompressedBlobReader final : public SectorReader
{
public:
  static std::unique_ptr<CompressedBlobReader> Create(File::DirectIOFile file,
                                                      const std::string& filename);
  ~CompressedBlobReader() override;

  const CompressedBlobHeader& GetHeader() const { return m_header; }

  BlobType GetBlobType() const override { return BlobType::GCZ; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override { return m_file_size; }
  u64 GetDataSize() const override { return m_header.disc_size; }
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  u64 GetBlockSize() const override { return m_header.block_size; }
  bool HasFastRandomAccessInBlock() const override { return false; }
  std::string GetCompressionMethod() const override { return "Deflate"; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  // Bytes the block takes up in the file.
  u64 GetBlockCompressedSize(u64 block_num) const;
  bool GetBlock(u64 block_num, u8* out_ptr) override;

private:
  CompressedBlobReader(File::DirectIOFile file, std::string filename);
  bool Open();

  CompressedBlobHeader m_header = {};
  std::vector<u64> m_block_offsets;
  std::vector<u32> m_block_hashes;
  u64 m_data_start = 0;
  u64 m_file_size = 0;
  File::DirectIOFile m_file;
  std::string m_file_name;
  std::vector<u8> m_stored_block;  // raw bytes of the block being decoded
};

}  // namespace DiscIO
