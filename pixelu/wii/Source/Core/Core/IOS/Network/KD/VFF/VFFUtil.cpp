// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/KD/VFF/VFFUtil.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

// ff.h has to come before diskio.h.
// clang-format off
#include "ff.h"
#include "diskio.h"
// clang-format on

#include "Common/Align.h"
#include "Common/FatFsUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::NWC24
{
namespace
{
// Sector n of the volume is at byte n * 512 - 480 of the file: sector 1 starts right after the
// 32-byte header, and there is no sector 0.
constexpr u32 FIRST_SECTOR_OFFSET = SECTOR_SIZE - sizeof(VFFHeader);

constexpr u32 FAT12_MAX_CLUSTERS = 4085;
constexpr u32 FAT16_MAX_CLUSTERS = 65525;
// The root directory is 4 KiB: 128 entries of 32 bytes.
constexpr u16 ROOT_ENTRIES = 128;
constexpr u32 DIR_ENTRY_SIZE = 32;

// Write sizes handed to FatFs at once.
constexpr size_t WRITE_CHUNK = 32768;

// The VFF file as FatFs's block device.
class VFFDisk final : public Common::FatFsCallbacks
{
public:
  int DiskRead(u8, u8* buff, u32 sector, unsigned int count) override
  {
    if (!Seek(sector, "read"))
      return RES_ERROR;
    const size_t bytes = size_t{count} * SECTOR_SIZE;
    if (!file->Read(buff, bytes))
    {
      ERROR_LOG_FMT(IOS_WC24, "VFF read failed (sector={}, size={})", sector, bytes);
      return RES_ERROR;
    }
    return RES_OK;
  }

  int DiskWrite(u8, const u8* buff, u32 sector, unsigned int count) override
  {
    if (!Seek(sector, "write"))
      return RES_ERROR;
    const size_t bytes = size_t{count} * SECTOR_SIZE;
    if (!file->Write(buff, bytes))
    {
      ERROR_LOG_FMT(IOS_WC24, "VFF write failed (sector={}, size={})", sector, bytes);
      return RES_ERROR;
    }
    return RES_OK;
  }

  int DiskIOCtl(u8, u8 cmd, void* buff) override
  {
    if (cmd == GET_SECTOR_COUNT)
    {
      *static_cast<LBA_t*>(buff) = static_cast<LBA_t>(file->GetStatus()->size / SECTOR_SIZE);
      return RES_OK;
    }
    if (cmd != CTRL_SYNC)
      WARN_LOG_FMT(IOS_WC24, "Unexpected FAT ioctl {}", cmd);
    return RES_OK;
  }

  FS::FileHandle* file = nullptr;

private:
  bool Seek(u32 sector, const char* what) const
  {
    if (sector == 0)
    {
      ERROR_LOG_FMT(IOS_WC24, "Attempted to {} sector 0 of the VFF: Invalid VFF?", what);
      return false;
    }
    const u64 offset = u64{sector} * SECTOR_SIZE - FIRST_SECTOR_OFFSET;
    if (!file->Seek(static_cast<u32>(offset), FS::SeekMode::Set))
    {
      ERROR_LOG_FMT(IOS_WC24, "VFF seek failed (offset={})", offset);
      return false;
    }
    return true;
  }
};

// Fills in `volume` from the VFF header instead of letting FatFs look for a boot sector, which
// VFFs do not have. Layout: sector 1 holds the first FAT, then the second FAT, then the root
// directory, then the data clusters (one sector each).
bool SetUpVolume(FS::FileHandle& file, FATFS* volume)
{
  VFFHeader header;
  if (!file.Read(reinterpret_cast<u8*>(&header), sizeof(header)))
  {
    ERROR_LOG_FMT(IOS_WC24, "Failed to read VFF header.");
    return false;
  }

  const u16 endianness = Common::swap16(header.endianness);
  if (endianness == VF_LITTLE_ENDIAN)
  {
    // Not supported yet. (The channel would make a new big-endian one if it were deleted.)
    return false;
  }

  // Both values are 16 bits wide, exactly as in IOS.
  u16 cluster_bytes = 0;
  u16 clusters = 0;
  if (endianness == VF_BIG_ENDIAN)
  {
    cluster_bytes = static_cast<u16>(Common::swap16(header.cluster_size) * 16);
    clusters = static_cast<u16>(Common::swap32(header.volume_size) / cluster_bytes);
  }

  u32 fat_bytes = 0;
  if (clusters < FAT12_MAX_CLUSTERS)
  {
    volume->fs_type = FS_FAT12;
    fat_bytes = (clusters + 1u) / 2 * 3;
  }
  else if (clusters < FAT16_MAX_CLUSTERS)
  {
    volume->fs_type = FS_FAT16;
    fat_bytes = clusters * 2u;
  }
  else
  {
    ERROR_LOG_FMT(IOS_WC24, "VFF not FAT12 or 16! Cluster size: {}", cluster_bytes);
    return false;
  }
  // Each FAT is padded to a whole cluster.
  const u32 fat_sectors = Common::AlignUp(fat_bytes, u32{cluster_bytes}) / SECTOR_SIZE;

  const u32 fat_start = 1;
  const u32 root_start = fat_start + fat_sectors * 2;
  const u32 data_start = root_start + ROOT_ENTRIES / (SECTOR_SIZE / DIR_ENTRY_SIZE);

  volume->n_fats = 2;
  volume->csize = 1;
  volume->n_rootdir = ROOT_ENTRIES;
  volume->fsize = fat_sectors;
  // The space before the data area comes out of the volume's clusters.
  volume->n_fatent = clusters - data_start + 2;
  volume->volbase = 0;
  volume->fatbase = fat_start;
  volume->dirbase = root_start;
  volume->database = data_start;

  // Free space is unknown, there is no FSINFO, the current directory is the root and the
  // sector window is empty.
  volume->last_clst = 0xffffffff;
  volume->free_clst = 0xffffffff;
  volume->fsi_flag = 0x80;
  volume->id = 0;
  volume->cdir = 0;
  volume->wflag = 0;
  volume->winsect = std::numeric_limits<LBA_t>::max();
  return true;
}

// Opens the VFF at `path` as the FatFs volume and runs `use` on it. `open_error` and
// `mount_error` are what to answer when the file is missing or is not a usable VFF.
template <typename Use>
ErrorCode WithVFF(const std::string& path, const std::shared_ptr<FS::FileSystem>& fs,
                  ErrorCode open_error, ErrorCode mount_error, bool delete_on_failure, Use use)
{
  VFFDisk disk;
  ErrorCode result = WC24_OK;
  Common::RunInFatFsContext(disk, [&] {
    auto file = fs->OpenFile(PID_KD, PID_KD, path, FS::Mode::ReadWrite);
    if (!file)
    {
      ERROR_LOG_FMT(IOS_WC24, "Failed to open VFF at: {}", path);
      result = open_error;
      return;
    }
    disk.file = &*file;

    FATFS volume{};
    if (f_mount(&volume, "", 0) != FR_OK)
    {
      ERROR_LOG_FMT(IOS_WC24, "Failed to mount VFF at: {}", path);
      result = mount_error;
    }
    else
    {
      volume.fs_type = 0;
      volume.pdrv = 0;
      if (!SetUpVolume(*file, &volume))
      {
        ERROR_LOG_FMT(IOS_WC24, "Failed to mount VFF at: {}", path);
        result = mount_error;
      }
      else
      {
        result = use();
      }
      f_unmount("");
    }

    // A VFF that could not be written to is thrown away (while it is still open, as IOS does).
    if (delete_on_failure && result != WC24_OK)
      fs->Delete(PID_KD, PID_KD, path);
  });
  return result;
}

ErrorCode WriteVolumeFile(const std::string& name, std::span<const u8> data)
{
  FIL file{};
  if (f_open(&file, name.c_str(), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
  {
    ERROR_LOG_FMT(IOS_WC24, "Failed to open file {} in VFF", name);
    return WC24_ERR_FILE_OPEN;
  }

  // On a failed write the file is left open; the volume is unmounted (and the VFF deleted)
  // straight after anyway.
  for (size_t done = 0; done < data.size();)
  {
    const UINT chunk = static_cast<UINT>(std::min(data.size() - done, WRITE_CHUNK));
    UINT written = 0;
    const FRESULT status = f_write(&file, data.data() + done, chunk, &written);
    if (status != FR_OK)
    {
      ERROR_LOG_FMT(IOS_WC24, "Failed to write file {} to VFF: {}", name,
                    std::to_underlying(status));
      return WC24_ERR_FILE_WRITE;
    }
    if (written != chunk)
    {
      ERROR_LOG_FMT(IOS_WC24, "Failed to write bytes of file {} to VFF ({} != {})", name,
                    written, chunk);
      return WC24_ERR_FILE_WRITE;
    }
    done += chunk;
  }

  if (f_close(&file) != FR_OK)
  {
    ERROR_LOG_FMT(IOS_WC24, "Failed to close file {} in VFF", name);
    return WC24_ERR_FILE_CLOSE;
  }
  return WC24_OK;
}

ErrorCode ReadVolumeFile(const std::string& name, std::vector<u8>& out)
{
  FIL file{};
  if (f_open(&file, name.c_str(), FA_READ) != FR_OK)
  {
    ERROR_LOG_FMT(IOS_WC24, "Failed to open file {} in VFF", name);
    return WC24_ERR_FILE_OPEN;
  }

  const UINT wanted = static_cast<UINT>(out.size());
  UINT got = 0;
  const FRESULT status = f_read(&file, out.data(), wanted, &got);
  if (status != FR_OK || got != wanted)
  {
    if (status != FR_OK)
    {
      ERROR_LOG_FMT(IOS_WC24, "Failed to read file {} in VFF: {}", name,
                    std::to_underlying(status));
    }
    else
      ERROR_LOG_FMT(IOS_WC24, "Failed to read bytes of file {} ({} != {})", name, got, wanted);
    f_close(&file);
    return WC24_ERR_FILE_READ;
  }

  if (f_close(&file) != FR_OK)
  {
    ERROR_LOG_FMT(IOS_WC24, "Failed to close file {} in VFF", name);
    return WC24_ERR_FILE_CLOSE;
  }
  return WC24_OK;
}
}  // namespace

ErrorCode WriteToVFF(const std::string& path, const std::string& filename,
                     const std::shared_ptr<FS::FileSystem>& fs, std::span<const u8> data)
{
  return WithVFF(path, fs, WC24_ERR_FILE_OPEN, WC24_ERR_FILE_READ, true,
                 [&] { return WriteVolumeFile(filename, data); });
}

ErrorCode ReadFromVFF(const std::string& path, const std::string& filename,
                      const std::shared_ptr<FS::FileSystem>& fs, std::vector<u8>& out)
{
  return WithVFF(path, fs, WC24_ERR_NOT_FOUND, WC24_ERR_BROKEN, false,
                 [&] { return ReadVolumeFile(filename, out); });
}

ErrorCode DeleteFileFromVFF(const std::string& path, const std::string& filename,
                            const std::shared_ptr<FS::FileSystem>& fs)
{
  return WithVFF(path, fs, WC24_ERR_NOT_FOUND, WC24_ERR_BROKEN, false, [&] {
    const FRESULT status = f_unlink(filename.c_str());
    if (status == FR_OK)
      return WC24_OK;
    ERROR_LOG_FMT(IOS_WC24, "Failed to delete file {} in VFF at: {} Code: {}", filename, path,
                  std::to_underlying(status));
    return WC24_ERR_BROKEN;
  });
}
}  // namespace IOS::HLE::NWC24
