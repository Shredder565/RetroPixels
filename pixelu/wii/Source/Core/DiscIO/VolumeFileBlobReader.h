// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
class FileInfo;

// Presents one file inside a disc's file system as a blob of its own (used, for example, to
// open a disc that is stored as a file on another disc). The volume must outlive the reader.
class VolumeFileBlobReader final : public BlobReader
{
public:
  // Null if the partition has no file system or the path doesn't name a file.
  static std::unique_ptr<VolumeFileBlobReader>
  Create(const Volume& volume, const Partition& partition, std::string_view file_path);
  ~VolumeFileBlobReader() override;

  BlobType GetBlobType() const override { return BlobType::PLAIN; }
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  // These describe the container of the disc the file is on.
  u64 GetBlockSize() const override;
  bool HasFastRandomAccessInBlock() const override;
  std::string GetCompressionMethod() const override;
  std::optional<int> GetCompressionLevel() const override;

  bool Read(u64 offset, u64 length, u8* out_ptr) override;

private:
  VolumeFileBlobReader(const Volume& volume, const Partition& partition, std::string file_path,
                       u64 file_offset, u64 file_size);

  const Volume& m_volume;
  const Partition m_partition;
  const std::string m_file_path;
  const u64 m_file_offset;  // in the partition's data
  const u64 m_file_size;
};

}  // namespace DiscIO
