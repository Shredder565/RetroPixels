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
// WBFS: the file system USB loaders put on hard drives, here as an image holding one Wii disc.
// The drive is divided into "WBFS sectors" (clusters); a table maps each cluster of the disc
// to the drive cluster that holds it, and unused disc clusters aren't stored at all. Images
// over 4 GiB may continue in name.wbf1, name.wbf2, ... (up to ten files in total).
static constexpr u32 WBFS_MAGIC = 0x53464257;  // "WBFS" read as a little endian u32

class WbfsFileReader final : public BlobReader
{
public:
  ~WbfsFileReader() override;

  static std::unique_ptr<WbfsFileReader> Create(File::DirectIOFile file, const std::string& path);

  BlobType GetBlobType() const override { return BlobType::WBFS; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override { return m_raw_size; }
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override { return DataSizeType::UpperBound; }

  u64 GetBlockSize() const override { return m_cluster_size; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 nbytes, u8* out_ptr) override;

private:
  struct Part
  {
    File::DirectIOFile file;
    u64 start;  // where the file begins on the (virtual) drive
    u64 size;
  };

  explicit WbfsFileReader(std::vector<Part> parts);
  bool Parse();

  std::vector<Part> m_parts;
  u64 m_raw_size = 0;  // all parts together

  u32 m_cluster_shift = 0;
  u64 m_cluster_size = 0;
  std::vector<u16> m_cluster_map;  // disc cluster -> drive cluster (unused ones are 0)
};

}  // namespace DiscIO
