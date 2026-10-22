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
// CISO ("compact ISO", used by some USB loaders): a 32 KiB header holding the block size and
// one used/unused flag per block, followed by the used blocks in order. Unused blocks read as
// zeroes.
static constexpr u32 CISO_MAGIC = 0x4F534943;  // "CISO" read as a little endian u32

static constexpr u32 CISO_HEADER_SIZE = 0x8000;
static constexpr u32 CISO_MAP_SIZE = CISO_HEADER_SIZE - sizeof(u32) - sizeof(char) * 4;

struct CISOHeader
{
  u32 magic;       // "CISO"
  u32 block_size;  // little endian
  u8 map[CISO_MAP_SIZE];  // 1 = block stored, 0 = block not stored
};

class CISOFileReader final : public BlobReader
{
public:
  static std::unique_ptr<CISOFileReader> Create(File::DirectIOFile file);

  BlobType GetBlobType() const override { return BlobType::CISO; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override { return DataSizeType::UpperBound; }

  u64 GetBlockSize() const override { return m_block_size; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr) override;

private:
  CISOFileReader(File::DirectIOFile file, const CISOHeader& header);

  // For each block on the disc: its position among the stored blocks, or NOT_STORED.
  static constexpr u32 NOT_STORED = 0xFFFFFFFF;

  File::DirectIOFile m_file;
  u64 m_file_size = 0;
  u32 m_block_size = 0;
  std::vector<u32> m_stored_index;
};

}  // namespace DiscIO
