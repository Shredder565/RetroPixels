// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/FatFsUtil.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

// ff.h must come before diskio.h.
// clang-format off
#include "ff.h"
#include "diskio.h"
// clang-format on

#include "Common/Align.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/TimeUtil.h"

#include "Core/Config/MainSettings.h"

// FatFs is a C library with one global volume and a fixed set of disk_* hooks. The hooks below
// forward to whichever FatFsCallbacks object is current; s_fatfs_mutex serialises users.

enum : u32
{
  SECTOR_SIZE = 512,
  MAX_CLUSTER_SIZE = 64 * SECTOR_SIZE,  // also the copy buffer and mkfs work area size
};

static std::mutex s_fatfs_mutex;
static Common::FatFsCallbacks* s_callbacks;

namespace
{
// Packed FAT timestamp: years since 1980 (7 bits), month, day, hours, minutes, seconds / 2.
u32 GetSystemTimeFAT()
{
  const std::optional<std::tm> local = Common::LocalTime(std::time(nullptr));
  if (!local)
    return 0;
  const std::tm& t = *local;
  u32 fat_time = 0;
  fat_time |= static_cast<u32>(t.tm_year - 80) << 25;
  fat_time |= static_cast<u32>(t.tm_mon + 1) << 21;
  fat_time |= static_cast<u32>(t.tm_mday) << 16;
  fat_time |= static_cast<u32>(t.tm_hour) << 11;
  fat_time |= static_cast<u32>(t.tm_min) << 5;
  fat_time |= static_cast<u32>(std::min(t.tm_sec, 59)) >> 1;  // no leap seconds
  return fat_time;
}

// Disk hooks backed by an image file.
class ImageFileCallbacks final : public Common::FatFsCallbacks
{
public:
  ImageFileCallbacks(File::IOFile* image, bool deterministic)
      : m_image(image), m_deterministic(deterministic)
  {
  }

  int DiskRead(u8, u8* buff, u32 sector, unsigned int count) override
  {
    const u64 offset = u64{sector} * SECTOR_SIZE;
    const size_t size = size_t{count} * SECTOR_SIZE;
    if (!m_image->Seek(offset, File::SeekOrigin::Begin))
    {
      ERROR_LOG_FMT(COMMON, "SD image: seek to {} failed", offset);
      return RES_ERROR;
    }
    if (!m_image->ReadBytes(buff, size))
    {
      ERROR_LOG_FMT(COMMON, "SD image: reading {} bytes at {} failed", size, offset);
      return RES_ERROR;
    }
    return RES_OK;
  }

  int DiskWrite(u8, const u8* buff, u32 sector, unsigned int count) override
  {
    const u64 offset = u64{sector} * SECTOR_SIZE;
    const size_t size = size_t{count} * SECTOR_SIZE;
    if (!m_image->Seek(offset, File::SeekOrigin::Begin))
    {
      ERROR_LOG_FMT(COMMON, "SD image: seek to {} failed", offset);
      return RES_ERROR;
    }
    if (!m_image->WriteBytes(buff, size))
    {
      ERROR_LOG_FMT(COMMON, "SD image: writing {} bytes at {} failed", size, offset);
      return RES_ERROR;
    }
    return RES_OK;
  }

  int DiskIOCtl(u8, u8 cmd, void* buff) override
  {
    if (cmd == GET_SECTOR_COUNT)
      *static_cast<LBA_t*>(buff) = static_cast<LBA_t>(m_image->GetSize() / SECTOR_SIZE);
    else if (cmd != CTRL_SYNC)
      WARN_LOG_FMT(COMMON, "SD image: unexpected ioctl {}", cmd);
    return RES_OK;
  }

  u32 GetCurrentTimeFAT() override { return m_deterministic ? 0 : GetSystemTimeFAT(); }

private:
  File::IOFile* m_image;
  bool m_deterministic;
};

// Makes `callbacks` current until destroyed. The caller holds s_fatfs_mutex.
class CallbacksScope
{
public:
  explicit CallbacksScope(Common::FatFsCallbacks& callbacks) { s_callbacks = &callbacks; }
  ~CallbacksScope() { s_callbacks = nullptr; }
  CallbacksScope(const CallbacksScope&) = delete;
  CallbacksScope& operator=(const CallbacksScope&) = delete;
};

const char* DescribeResult(FRESULT result)
{
  switch (result)
  {
  case FR_OK:
    return "Succeeded";
  case FR_DISK_ERR:
    return "Disk I/O error";
  case FR_INT_ERR:
    return "Internal FatFs error";
  case FR_NOT_READY:
    return "Drive not ready";
  case FR_NO_FILE:
    return "File not found";
  case FR_NO_PATH:
    return "Path not found";
  case FR_INVALID_NAME:
    return "Invalid path name";
  case FR_DENIED:
    return "Access denied or directory full";
  case FR_EXIST:
    return "Already exists";
  case FR_INVALID_OBJECT:
    return "Invalid file or directory object";
  case FR_WRITE_PROTECTED:
    return "Drive is write protected";
  case FR_INVALID_DRIVE:
    return "Invalid drive number";
  case FR_NOT_ENABLED:
    return "Volume has no work area";
  case FR_NO_FILESYSTEM:
    return "No valid FAT volume";
  case FR_MKFS_ABORTED:
    return "Formatting aborted";
  case FR_TIMEOUT:
    return "Timed out waiting for the volume";
  case FR_LOCKED:
    return "Rejected by the file sharing policy";
  case FR_NOT_ENOUGH_CORE:
    return "Not enough memory for the long file name buffer";
  case FR_TOO_MANY_OPEN_FILES:
    return "Too many open files";
  case FR_INVALID_PARAMETER:
    return "Invalid parameter";
  default:
    return "Unknown error";
  }
}

// Logs and returns false unless `result` is FR_OK.
bool Succeeded(FRESULT result, std::string_view action, std::string_view path)
{
  if (result == FR_OK)
    return true;
  ERROR_LOG_FMT(COMMON, "SD image: {} {} failed: {}", action, path, DescribeResult(result));
  return false;
}

constexpr u64 MiB(u64 n)
{
  return n << 20;
}
constexpr u64 GiB(u64 n)
{
  return n << 30;
}
constexpr u64 MAX_FAT_FILE_SIZE = GiB(4) - 1;
constexpr size_t MAX_DIRECTORY_ENTRIES = 65536;
constexpr size_t MAX_NAME_LENGTH = 255;  // UTF-16 code units

bool IsFatCompatible(const File::FSTEntry& entry)
{
  if (!entry.isDirectory)
    return true;

  if (entry.children.size() > MAX_DIRECTORY_ENTRIES)
  {
    ERROR_LOG_FMT(COMMON, "Directory {} has too many entries ({})", entry.physicalName,
                  entry.children.size());
    return false;
  }

  for (const File::FSTEntry& child : entry.children)
  {
    const size_t name_length = UTF8ToUTF16(child.virtualName).size();
    if (name_length > MAX_NAME_LENGTH)
    {
      ERROR_LOG_FMT(COMMON, "Filename {} (in directory {}) is too long ({})", child.virtualName,
                    entry.physicalName, name_length);
      return false;
    }
    if (child.size > MAX_FAT_FILE_SIZE)
    {
      ERROR_LOG_FMT(COMMON, "File {} (in directory {}) is too large ({})", child.virtualName,
                    entry.physicalName, child.size);
      return false;
    }
    if (!IsFatCompatible(child))
      return false;
  }
  return true;
}

// Upper bound of the space `entry` needs: whole clusters per file, and per directory one
// 32-byte entry plus long-name entries (13 UTF-16 units each) for every child.
u64 EstimateSize(const File::FSTEntry& entry)
{
  if (!entry.isDirectory)
    return Common::AlignUp(entry.size, MAX_CLUSTER_SIZE);

  u64 table = 0;
  for (const File::FSTEntry& child : entry.children)
  {
    const u64 lfn_entries = (UTF8ToUTF16(child.virtualName).size() + 12) / 13;
    table += 32 * (1 + lfn_entries);
  }

  u64 total = Common::AlignUp(table, MAX_CLUSTER_SIZE);
  for (const File::FSTEntry& child : entry.children)
    total += EstimateSize(child);
  return total;
}

void SortByName(File::FSTEntry* entry)
{
  std::ranges::sort(entry->children, {}, &File::FSTEntry::virtualName);
  for (File::FSTEntry& child : entry->children)
    SortByName(&child);
}

// Copies one host file into the current FatFs directory.
bool PackFile(const std::function<bool()>& cancelled, const File::FSTEntry& entry,
              std::vector<u8>& buffer)
{
  File::IOFile source(entry.physicalName, "rb");
  if (!source)
  {
    ERROR_LOG_FMT(COMMON, "Failed to open file {}", entry.physicalName);
    return false;
  }

  FIL target{};
  if (!Succeeded(f_open(&target, entry.virtualName.c_str(), FA_CREATE_ALWAYS | FA_WRITE),
                 "creating", entry.physicalName))
  {
    return false;
  }

  const u64 source_size = source.GetSize();
  if (source_size != entry.size)
  {
    ERROR_LOG_FMT(COMMON, "File at {} changed size while packing ({} != {})", entry.physicalName,
                  entry.size, source_size);
    return false;
  }
  if (entry.size > MAX_FAT_FILE_SIZE)
  {
    ERROR_LOG_FMT(COMMON, "File at {} is too large for FAT ({} bytes)", entry.physicalName,
                  entry.size);
    return false;
  }

  for (u64 remaining = entry.size; remaining != 0;)
  {
    if (cancelled())
      return false;

    const UINT chunk = static_cast<UINT>(std::min<u64>(remaining, buffer.size()));
    if (!source.ReadBytes(buffer.data(), chunk))
    {
      ERROR_LOG_FMT(COMMON, "Failed to read data from file at {}", entry.physicalName);
      return false;
    }

    UINT written = 0;
    if (!Succeeded(f_write(&target, buffer.data(), chunk, &written), "writing",
                   entry.physicalName))
    {
      return false;
    }
    if (written != chunk)
    {
      ERROR_LOG_FMT(COMMON, "SD image: short write for {} ({} of {} bytes); image full?",
                    entry.physicalName, written, chunk);
      return false;
    }
    remaining -= chunk;
  }

  if (!Succeeded(f_close(&target), "closing", entry.physicalName))
    return false;
  if (!source.Close())
  {
    ERROR_LOG_FMT(COMMON, "Failed to close file {}", entry.physicalName);
    return false;
  }
  return true;
}

// Copies `entry` (recursively) into the current FatFs directory. The root's children go
// directly into the current directory.
bool Pack(const std::function<bool()>& cancelled, const File::FSTEntry& entry, bool is_root,
          std::vector<u8>& buffer)
{
  if (cancelled())
    return false;

  if (!entry.isDirectory)
    return PackFile(cancelled, entry, buffer);

  if (!is_root)
  {
    if (!Succeeded(f_mkdir(entry.virtualName.c_str()), "creating directory", entry.physicalName))
      return false;
    if (!Succeeded(f_chdir(entry.virtualName.c_str()), "entering directory", entry.physicalName))
      return false;
  }

  for (const File::FSTEntry& child : entry.children)
  {
    if (!Pack(cancelled, child, false, buffer))
      return false;
  }

  if (!is_root && !Succeeded(f_chdir(".."), "leaving directory", entry.physicalName))
    return false;
  return true;
}

// Copies the FatFs file `name` (in the current directory) to host path `path`.
bool UnpackFile(const std::function<bool()>& cancelled, const std::string& path, const char* name,
                std::vector<u8>& buffer)
{
  FIL source{};
  if (!Succeeded(f_open(&source, name, FA_READ), "opening", path))
    return false;

  File::IOFile target(path, "wb");
  if (!target)
  {
    ERROR_LOG_FMT(COMMON, "Failed to open file {}", path);
    return false;
  }

  for (u32 remaining = static_cast<u32>(f_size(&source)); remaining != 0;)
  {
    if (cancelled())
      return false;

    const UINT chunk = std::min(remaining, static_cast<u32>(buffer.size()));
    UINT read = 0;
    if (!Succeeded(f_read(&source, buffer.data(), chunk, &read), "reading", path))
      return false;
    if (read != chunk)
    {
      ERROR_LOG_FMT(COMMON, "SD image: short read for {} ({} of {} bytes)", path, read, chunk);
      return false;
    }
    if (!target.WriteBytes(buffer.data(), chunk))
    {
      ERROR_LOG_FMT(COMMON, "Failed to write to file {}", path);
      return false;
    }
    remaining -= chunk;
  }

  if (!target.Close())
  {
    ERROR_LOG_FMT(COMMON, "Failed to close file {}", path);
    return false;
  }
  return Succeeded(f_close(&source), "closing", path);
}

// A name from the image that must not be used as a host path component.
bool IsUnsafeName(std::string_view name)
{
  return name.contains('/') || name.contains('\\') ||
         std::ranges::all_of(name, [](char c) { return c == '.'; });
}

// Copies the FatFs entry `name` (in the current directory) to host path `path`. The root is
// passed with an empty name.
bool Unpack(const std::function<bool()>& cancelled, const std::string& path, bool is_directory,
            const char* name, std::vector<u8>& buffer)
{
  if (cancelled())
    return false;

  if (!is_directory)
    return UnpackFile(cancelled, path, name, buffer);

  if (!File::CreateDir(path))
  {
    ERROR_LOG_FMT(COMMON, "Failed to create directory {}", path);
    return false;
  }

  if (!Succeeded(f_chdir(name), "entering directory", path))
    return false;

  DIR directory{};
  if (!Succeeded(f_opendir(&directory, "."), "opening directory", path))
    return false;

  FILINFO info{};
  for (;;)
  {
    if (!Succeeded(f_readdir(&directory, &info), "reading directory", path))
      return false;
    if (info.fname[0] == '\0')
      break;  // end of directory

    // FatFs reports an entry with neither a long nor a short name as "?"; such entries are
    // damaged, so leave them out.
    if (info.fname[0] == '?' && info.fname[1] == '\0' && info.altname[0] == '\0')
      continue;

    const std::string_view child_name = info.fname;
    if (IsUnsafeName(child_name))
    {
      ERROR_LOG_FMT(COMMON,
                    "Path traversal attack detected in directory {} in SD image, child filename "
                    "is {}",
                    path, child_name);
      return false;
    }

    if (!Unpack(cancelled, fmt::format("{}/{}", path, child_name), (info.fattrib & AM_DIR) != 0,
                info.fname, buffer))
    {
      return false;
    }
  }

  if (!Succeeded(f_closedir(&directory), "closing directory", path))
    return false;
  return Succeeded(f_chdir(".."), "leaving directory", path);
}
}  // namespace

namespace Common
{
FatFsCallbacks::FatFsCallbacks() = default;
FatFsCallbacks::~FatFsCallbacks() = default;

u8 FatFsCallbacks::DiskInitialize(u8 pdrv)
{
  return 0;
}

u8 FatFsCallbacks::DiskStatus(u8 pdrv)
{
  return 0;
}

u32 FatFsCallbacks::GetCurrentTimeFAT()
{
  return GetSystemTimeFAT();
}

bool SyncSDFolderToSDImage(const std::function<bool()>& cancelled, bool deterministic)
{
  const std::string source_dir = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
  const std::string image_path = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  if (source_dir.empty() || image_path.empty())
    return false;

  INFO_LOG_FMT(COMMON, "Starting SD card conversion from folder {} to file {}", source_dir,
               image_path);

  if (!File::IsDirectory(source_dir))
  {
    ERROR_LOG_FMT(COMMON, "{} is not a directory, not converting", source_dir);
    return false;
  }

  File::FSTEntry root = File::ScanDirectoryTree(source_dir, true);
  if (deterministic)
    SortByName(&root);
  if (!IsFatCompatible(root))
    return false;

  // A configured size, or the contents plus half as much again (within 512 MiB to 8 GiB).
  u64 image_size = Config::Get(Config::MAIN_WII_SD_CARD_FILESIZE);
  if (image_size == 0)
  {
    image_size = EstimateSize(root);
    image_size += std::clamp(image_size / 2, MiB(512), GiB(8));
  }
  image_size = AlignUp(image_size, MAX_CLUSTER_SIZE);

  std::lock_guard lock(s_fatfs_mutex);
  File::IOFile image;
  ImageFileCallbacks callbacks(&image, deterministic);
  CallbacksScope scope(callbacks);

  // Build into a temporary file and rename it over the image at the end.
  const std::string temp_path = File::GetTempFilenameForAtomicWrite(image_path);
  if (!image.Open(temp_path, "w+b"))
  {
    ERROR_LOG_FMT(COMMON, "Failed to create or overwrite SD image at {}", image_path);
    return false;
  }
  Common::ScopeGuard remove_temp{[&] {
    image.Close();
    File::Delete(temp_path);
  }};

  if (!image.Resize(image_size))
  {
    ERROR_LOG_FMT(COMMON, "Failed to allocate {} bytes for SD image at {}", image_size,
                  image_path);
    return false;
  }

  MKFS_PARM format{};
  format.fmt = FM_FAT32 | FM_SFD;  // FAT32, no partition table
  format.n_fat = 0;                // defaults for FAT count, root entries and cluster size
  format.align = 1;                // data area aligned to 1 sector
  format.n_root = 0;
  format.au_size = 0;

  std::vector<u8> buffer(MAX_CLUSTER_SIZE);
  if (!Succeeded(f_mkfs("", &format, buffer.data(), static_cast<UINT>(buffer.size())),
                 "formatting", image_path))
  {
    return false;
  }

  FATFS volume{};
  if (!Succeeded(f_mount(&volume, "", 0), "mounting", image_path))
    return false;
  Common::ScopeGuard unmount{[] { f_unmount(""); }};

  if (!Pack(cancelled, root, true, buffer))
  {
    ERROR_LOG_FMT(COMMON, "Failed to pack folder {} to SD image at {}", source_dir, temp_path);
    return false;
  }

  unmount.Exit();  // flush FatFs before the file is closed

  if (!image.Close())
  {
    ERROR_LOG_FMT(COMMON, "Failed to close SD image at {}", temp_path);
    return false;
  }
  if (!File::Rename(temp_path, image_path))
  {
    ERROR_LOG_FMT(COMMON, "Failed to rename SD image from {} to {}", temp_path, image_path);
    return false;
  }
  remove_temp.Dismiss();

  INFO_LOG_FMT(COMMON, "Successfully packed folder {} to SD image at {}", source_dir, image_path);
  return true;
}

bool SyncSDImageToSDFolder(const std::function<bool()>& cancelled)
{
  const std::string image_path = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  const std::string target_dir = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
  if (image_path.empty() || target_dir.empty())
    return false;

  std::lock_guard lock(s_fatfs_mutex);
  File::IOFile image;
  // Nothing is written, but keep timestamps fixed anyway.
  ImageFileCallbacks callbacks(&image, true);
  CallbacksScope scope(callbacks);

  INFO_LOG_FMT(COMMON, "Starting SD card conversion from file {} to folder {}", image_path,
               target_dir);

  if (!image.Open(image_path, "r+b"))
  {
    ERROR_LOG_FMT(COMMON, "Failed to open SD image at {}", image_path);
    return false;
  }

  FATFS volume{};
  if (!Succeeded(f_mount(&volume, "", 0), "mounting", image_path))
    return false;
  Common::ScopeGuard unmount{[] { f_unmount(""); }};

  // The user path ends in a separator; renaming and Unpack() want it without.
  const std::string target = target_dir.substr(0, target_dir.size() - 1);

  // Directories cannot be replaced atomically: move the old one aside, unpack, and put it back
  // if unpacking fails.
  const bool had_target = File::IsDirectory(target_dir);
  const std::string backup = File::GetTempFilenameForAtomicWrite(target);
  if (had_target && !File::Rename(target, backup))
  {
    ERROR_LOG_FMT(COMMON, "Failed to move old SD folder to {}", backup);
    return false;
  }

  std::vector<u8> buffer(MAX_CLUSTER_SIZE);
  if (!Unpack(cancelled, target, true, "", buffer))
  {
    ERROR_LOG_FMT(COMMON, "Failed to unpack SD image {} to {}", image_path, target_dir);
    File::DeleteDirRecursively(target);
    if (had_target)
      File::Rename(backup, target);
    return false;
  }

  unmount.Exit();

  if (had_target)
    File::DeleteDirRecursively(backup);

  // The folder is complete at this point, so a failure here does not fail the sync.
  if (!image.Close())
    ERROR_LOG_FMT(COMMON, "Failed to close SD image {}", image_path);

  INFO_LOG_FMT(COMMON, "Successfully unpacked SD image {} to {}", image_path, target_dir);
  return true;
}

void RunInFatFsContext(FatFsCallbacks& callbacks, const std::function<void()>& function)
{
  std::lock_guard lock(s_fatfs_mutex);
  CallbacksScope scope(callbacks);
  function();
}
}  // namespace Common

// FatFs disk and clock hooks.

extern "C" DSTATUS disk_status(BYTE pdrv)
{
  return static_cast<DSTATUS>(s_callbacks->DiskStatus(pdrv));
}

extern "C" DSTATUS disk_initialize(BYTE pdrv)
{
  return static_cast<DSTATUS>(s_callbacks->DiskInitialize(pdrv));
}

extern "C" DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
  return static_cast<DRESULT>(s_callbacks->DiskRead(pdrv, buff, sector, count));
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
  return static_cast<DRESULT>(s_callbacks->DiskWrite(pdrv, buff, sector, count));
}

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
  return static_cast<DRESULT>(s_callbacks->DiskIOCtl(pdrv, cmd, buff));
}

extern "C" DWORD get_fattime(void)
{
  return static_cast<DWORD>(s_callbacks->GetCurrentTimeFAT());
}

#if FF_USE_LFN == 3  // LFN buffers on the heap (not the current configuration)
extern "C" void* ff_memalloc(UINT msize)
{
  return std::malloc(msize);
}

extern "C" void ff_memfree(void* mblock)
{
  std::free(mblock);
}
#endif

#if FF_FS_REENTRANT
extern "C" int ff_cre_syncobj(BYTE vol, FF_SYNC_t* sobj)
{
  *sobj = new std::recursive_mutex();
  return *sobj != nullptr;
}

extern "C" int ff_req_grant(FF_SYNC_t sobj)
{
  static_cast<std::recursive_mutex*>(sobj)->lock();
  return 1;
}

extern "C" void ff_rel_grant(FF_SYNC_t sobj)
{
  static_cast<std::recursive_mutex*>(sobj)->unlock();
}

extern "C" int ff_del_syncobj(FF_SYNC_t sobj)
{
  delete static_cast<std::recursive_mutex*>(sobj);
  return 1;
}
#endif
