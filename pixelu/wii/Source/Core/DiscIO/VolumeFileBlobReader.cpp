// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/VolumeFileBlobReader.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
std::unique_ptr<VolumeFileBlobReader> VolumeFileBlobReader::Create(const Volume& volume,
                                                                   const Partition& partition,
                                                                   std::string_view file_path)
{
  const FileSystem* file_system = volume.GetFileSystem(partition);
  if (!file_system)
    return nullptr;

  const std::unique_ptr<FileInfo> info = file_system->FindFileInfo(file_path);
  if (!info || info->IsDirectory())
    return nullptr;

  return std::unique_ptr<VolumeFileBlobReader>(new VolumeFileBlobReader(
      volume, partition, std::string(file_path), info->GetOffset(), info->GetSize()));
}

VolumeFileBlobReader::VolumeFileBlobReader(const Volume& volume, const Partition& partition,
                                           std::string file_path, u64 file_offset, u64 file_size)
    : m_volume(volume), m_partition(partition), m_file_path(std::move(file_path)),
      m_file_offset(file_offset), m_file_size(file_size)
{
}

VolumeFileBlobReader::~VolumeFileBlobReader() = default;

std::unique_ptr<BlobReader> VolumeFileBlobReader::CopyReader() const
{
  return std::unique_ptr<BlobReader>(
      new VolumeFileBlobReader(m_volume, m_partition, m_file_path, m_file_offset, m_file_size));
}

u64 VolumeFileBlobReader::GetDataSize() const
{
  return m_file_size;
}

u64 VolumeFileBlobReader::GetRawSize() const
{
  return m_file_size;
}

// Block layout and compression are those of the volume's underlying blob.
std::optional<int> VolumeFileBlobReader::GetCompressionLevel() const
{
  return m_volume.GetBlobReader().GetCompressionLevel();
}

std::string VolumeFileBlobReader::GetCompressionMethod() const
{
  return m_volume.GetBlobReader().GetCompressionMethod();
}

u64 VolumeFileBlobReader::GetBlockSize() const
{
  return m_volume.GetBlobReader().GetBlockSize();
}

bool VolumeFileBlobReader::HasFastRandomAccessInBlock() const
{
  return m_volume.GetBlobReader().HasFastRandomAccessInBlock();
}

bool VolumeFileBlobReader::Read(u64 offset, u64 length, u8* out_ptr)
{
  if (offset + length > m_file_size)
    return false;
  return m_volume.Read(m_file_offset + offset, length, out_ptr, m_partition);
}

}  // namespace DiscIO
