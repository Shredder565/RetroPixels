// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/DiscExtractor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
namespace
{
// Large exports are copied in pieces of this size.
constexpr u64 EXPORT_CHUNK_SIZE = 0x08000000;

// NKit images change a few header bytes; exports put back what a normal dump has (zeros).
constexpr size_t NKIT_WII_FLAGS_OFFSET = 0x60;  // the "no hashes" and "no encryption" flags
constexpr size_t NKIT_WII_FLAGS_SIZE = 2;
constexpr size_t NKIT_DATA_OFFSET = 0x200;  // NKit's own information block
constexpr size_t NKIT_DATA_SIZE = 0x1c;

// Reads `size` bytes at `offset`, clears [clear_offset, clear_offset + clear_size) if the volume
// is an NKit image and writes the result to a new file.
bool ExportHeaderBytes(const Volume& volume, const Partition& partition, u64 offset, size_t size,
                       size_t clear_offset, size_t clear_size, const std::string& export_filename)
{
  File::IOFile file(export_filename, "wb");
  if (!file)
    return false;

  std::vector<u8> bytes(size);
  if (!volume.Read(offset, bytes.size(), bytes.data(), partition))
    return false;
  if (volume.IsNKit())
    std::fill_n(bytes.begin() + clear_offset, clear_size, u8{0});
  return file.WriteBytes(bytes.data(), bytes.size());
}

// Exports a (size, offset >> 2) pair's data from a Wii partition header.
bool ExportPartitionHeaderArea(const Volume& volume, const Partition& partition, u32 size_field,
                               u32 offset_field, const std::string& export_filename)
{
  if (volume.GetVolumeType() != Platform::WiiDisc)
    return false;

  const std::optional<u32> size =
      volume.ReadSwapped<u32>(partition.offset + size_field, PARTITION_NONE);
  const std::optional<u64> offset =
      volume.ReadSwappedAndShifted(partition.offset + offset_field, PARTITION_NONE);
  if (!size || !offset)
    return false;
  return ExportData(volume, PARTITION_NONE, partition.offset + *offset, *size, export_filename);
}

std::string WithTrailingSlash(std::string path)
{
  if (path.empty() || path.back() != '/')
    path += '/';
  return path;
}
}  // namespace

u64 ReadFile(const Volume& volume, const Partition& partition, const FileInfo* file_info,
             u8* buffer, u64 max_buffer_size, u64 offset_in_file)
{
  if (!file_info || file_info->IsDirectory())
    return 0;
  const u64 file_size = file_info->GetSize();
  if (offset_in_file >= file_size)
    return 0;

  const u64 length = std::min(max_buffer_size, file_size - offset_in_file);
  DEBUG_LOG_FMT(DISCIO, "Reading {:x} bytes at {:x} from file {}. Offset: {:x} Size: {:x}", length,
                offset_in_file, file_info->GetPath(), file_info->GetOffset(), file_size);

  if (!volume.Read(file_info->GetOffset() + offset_in_file, length, buffer, partition))
    return 0;
  return length;
}

u64 ReadFile(const Volume& volume, const Partition& partition, std::string_view path, u8* buffer,
             u64 max_buffer_size, u64 offset_in_file)
{
  const FileSystem* file_system = volume.GetFileSystem(partition);
  if (!file_system)
    return 0;
  const std::unique_ptr<FileInfo> file_info = file_system->FindFileInfo(path);
  return ReadFile(volume, partition, file_info.get(), buffer, max_buffer_size, offset_in_file);
}

bool ExportData(const Volume& volume, const Partition& partition, u64 offset, u64 size,
                const std::string& export_filename)
{
  File::IOFile file(export_filename, "wb");
  if (!file)
    return false;

  std::vector<u8> chunk;
  while (size > 0)
  {
    const size_t length = static_cast<size_t>(std::min(size, EXPORT_CHUNK_SIZE));
    chunk.resize(length);
    if (!volume.Read(offset, length, chunk.data(), partition))
      return false;
    if (!file.WriteBytes(chunk.data(), length))
      return false;
    offset += length;
    size -= length;
  }
  return true;
}

bool ExportFile(const Volume& volume, const Partition& partition, const FileInfo* file_info,
                const std::string& export_filename)
{
  if (!file_info || file_info->IsDirectory())
    return false;
  return ExportData(volume, partition, file_info->GetOffset(), file_info->GetSize(),
                    export_filename);
}

bool ExportFile(const Volume& volume, const Partition& partition, std::string_view path,
                const std::string& export_filename)
{
  const FileSystem* file_system = volume.GetFileSystem(partition);
  if (!file_system)
    return false;
  const std::unique_ptr<FileInfo> file_info = file_system->FindFileInfo(path);
  return ExportFile(volume, partition, file_info.get(), export_filename);
}

void ExportDirectory(const Volume& volume, const Partition& partition, const FileInfo& directory,
                     bool recursive, const std::string& filesystem_path,
                     const std::string& export_folder,
                     const std::function<bool(const std::string& path)>& update_progress)
{
  std::string destination = WithTrailingSlash(export_folder);
  if (directory.IsDirectory() && !directory.IsRoot())
    destination += directory.GetName() + '/';
  File::CreateFullPath(destination);

  for (const FileInfo& entry : directory)
  {
    const bool is_directory = entry.IsDirectory();
    const std::string name = is_directory ? entry.GetName() + '/' : entry.GetName();
    const std::string disc_path = filesystem_path + name;
    const std::string host_path = destination + name;

    if (update_progress(disc_path))
      return;
    DEBUG_LOG_FMT(DISCIO, "{}", host_path);

    if (is_directory)
    {
      if (recursive)
      {
        ExportDirectory(volume, partition, entry, true, disc_path, destination,
                        update_progress);
      }
    }
    else if (File::Exists(host_path))
    {
      NOTICE_LOG_FMT(DISCIO, "{} already exists", host_path);
    }
    else if (!ExportFile(volume, partition, &entry, host_path))
    {
      ERROR_LOG_FMT(DISCIO, "Could not export {}", host_path);
    }
  }
}

bool ExportWiiUnencryptedHeader(const Volume& volume, const std::string& export_filename)
{
  if (volume.GetVolumeType() != Platform::WiiDisc)
    return false;
  return ExportHeaderBytes(volume, PARTITION_NONE, WII_NONPARTITION_DISCHEADER_ADDRESS,
                           WII_NONPARTITION_DISCHEADER_SIZE, NKIT_WII_FLAGS_OFFSET,
                           NKIT_WII_FLAGS_SIZE, export_filename);
}

bool ExportWiiRegionData(const Volume& volume, const std::string& export_filename)
{
  if (volume.GetVolumeType() != Platform::WiiDisc)
    return false;
  return ExportData(volume, PARTITION_NONE, WII_REGION_DATA_ADDRESS, WII_REGION_DATA_SIZE,
                    export_filename);
}

bool ExportTicket(const Volume& volume, const Partition& partition,
                  const std::string& export_filename)
{
  if (volume.GetVolumeType() != Platform::WiiDisc)
    return false;
  return ExportData(volume, PARTITION_NONE, partition.offset + WII_PARTITION_TICKET_ADDRESS,
                    WII_PARTITION_TICKET_SIZE, export_filename);
}

bool ExportTMD(const Volume& volume, const Partition& partition, const std::string& export_filename)
{
  return ExportPartitionHeaderArea(volume, partition, WII_PARTITION_TMD_SIZE_ADDRESS,
                                   WII_PARTITION_TMD_OFFSET_ADDRESS, export_filename);
}

bool ExportCertificateChain(const Volume& volume, const Partition& partition,
                            const std::string& export_filename)
{
  return ExportPartitionHeaderArea(volume, partition, WII_PARTITION_CERT_CHAIN_SIZE_ADDRESS,
                                   WII_PARTITION_CERT_CHAIN_OFFSET_ADDRESS, export_filename);
}

bool ExportH3Hashes(const Volume& volume, const Partition& partition,
                    const std::string& export_filename)
{
  if (volume.GetVolumeType() != Platform::WiiDisc)
    return false;

  // The partition header gives the H3 table's position relative to the partition start.
  const u64 start = partition.offset;
  const std::optional<u64> h3_offset =
      volume.ReadSwappedAndShifted(start + WII_PARTITION_H3_OFFSET_ADDRESS, PARTITION_NONE);
  return h3_offset.has_value() && ExportData(volume, PARTITION_NONE, start + *h3_offset,
                                             WII_PARTITION_H3_SIZE, export_filename);
}

bool ExportHeader(const Volume& volume, const Partition& partition,
                  const std::string& export_filename)
{
  if (!IsDisc(volume.GetVolumeType()))
    return false;
  return ExportHeaderBytes(volume, partition, DISCHEADER_ADDRESS, DISCHEADER_SIZE,
                           NKIT_DATA_OFFSET, NKIT_DATA_SIZE, export_filename);
}

bool ExportBI2Data(const Volume& volume, const Partition& partition,
                   const std::string& export_filename)
{
  if (!IsDisc(volume.GetVolumeType()))
    return false;
  return ExportData(volume, partition, BI2_ADDRESS, BI2_SIZE, export_filename);
}

bool ExportApploader(const Volume& volume, const Partition& partition,
                     const std::string& export_filename)
{
  if (!IsDisc(volume.GetVolumeType()))
    return false;
  const std::optional<u64> size = GetApploaderSize(volume, partition);
  if (!size)
    return false;
  return ExportData(volume, partition, APPLOADER_ADDRESS, *size, export_filename);
}

bool ExportDOL(const Volume& volume, const Partition& partition, const std::string& export_filename)
{
  if (!IsDisc(volume.GetVolumeType()))
    return false;
  const std::optional<u64> offset = GetBootDOLOffset(volume, partition);
  if (!offset)
    return false;
  const std::optional<u32> size = GetBootDOLSize(volume, partition, *offset);
  if (!size)
    return false;
  return ExportData(volume, partition, *offset, *size, export_filename);
}

bool ExportFST(const Volume& volume, const Partition& partition, const std::string& export_filename)
{
  if (!IsDisc(volume.GetVolumeType()))
    return false;
  const std::optional<u64> offset = GetFSTOffset(volume, partition);
  const std::optional<u64> size = GetFSTSize(volume, partition);
  if (!offset || !size)
    return false;
  return ExportData(volume, partition, *offset, *size, export_filename);
}

bool ExportSystemData(const Volume& volume, const Partition& partition,
                      const std::string& export_folder)
{
  // Every part is attempted even if an earlier one fails.
  bool ok = true;

  const std::string sys = export_folder + "/sys/";
  File::CreateFullPath(sys);
  ok &= ExportHeader(volume, partition, sys + "boot.bin");
  ok &= ExportBI2Data(volume, partition, sys + "bi2.bin");
  ok &= ExportApploader(volume, partition, sys + "apploader.img");
  ok &= ExportDOL(volume, partition, sys + "main.dol");
  ok &= ExportFST(volume, partition, sys + "fst.bin");

  if (volume.GetVolumeType() == Platform::WiiDisc)
  {
    const std::string disc = export_folder + "/disc/";
    File::CreateFullPath(disc);
    ok &= ExportWiiUnencryptedHeader(volume, disc + "header.bin");
    ok &= ExportWiiRegionData(volume, disc + "region.bin");

    ok &= ExportTicket(volume, partition, export_folder + "/ticket.bin");
    ok &= ExportTMD(volume, partition, export_folder + "/tmd.bin");
    ok &= ExportCertificateChain(volume, partition, export_folder + "/cert.bin");
    if (volume.HasWiiHashes())
      ok &= ExportH3Hashes(volume, partition, export_folder + "/h3.bin");
  }

  return ok;
}

}  // namespace DiscIO
