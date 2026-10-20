// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#ifdef _WIN32
// Several users of this header rely on it pulling in windows.h. Keep it until they include
// what they need themselves.
#include <windows.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"

class PointerWrap;

namespace IOS::HLE
{
enum ReturnCode : s32;

namespace FS
{
// Outcomes of file system operations. The order matters: entry N maps to IOS error -(100 + N)
// (see ConvertResult), matching the FS module's error numbering.
enum class ResultCode
{
  Success,
  Invalid,
  AccessDenied,
  SuperblockWriteFailed,
  SuperblockInitFailed,
  AlreadyExists,
  NotFound,
  FstFull,
  NoFreeSpace,
  NoFreeHandle,
  TooManyPathComponents,
  InUse,
  BadBlock,
  EccError,
  CriticalEccError,
  FileNotEmpty,
  CheckFailed,
  UnknownError,
  ShortRead,
};

template <typename T>
using Result = std::expected<T, ResultCode>;

using Uid = u32;
using Gid = u16;
using Fd = u32;

// Access bits, used both for open modes and for the owner/group/other permissions.
enum class Mode : u8
{
  None = 0,
  Read = 1,
  Write = 2,
  ReadWrite = 3,
};

enum class SeekMode : u32
{
  Set = 0,
  Current = 1,
  End = 2,
};

using FileAttribute = u8;

// Maps a NAND subtree onto a directory elsewhere on the host.
struct NandRedirect
{
  // NAND path being redirected, e.g. "/title/00010000/534d4e45/data".
  std::string source_path;

  // Absolute host directory that takes its place.
  std::string target_path;
};

struct Modes
{
  Mode owner, group, other;
};
inline bool operator==(const Modes& lhs, const Modes& rhs)
{
  return lhs.owner == rhs.owner && lhs.group == rhs.group && lhs.other == rhs.other;
}

struct Metadata
{
  Uid uid;
  Gid gid;
  FileAttribute attribute;
  Modes modes;
  bool is_file;
  u32 size;
  u16 fst_index;
};

// Geometry of the 512 MiB NAND, in 16 KiB clusters.
constexpr u16 CLUSTER_SIZE = 16384;
constexpr u16 TOTAL_CLUSTERS = 0x7ec0;
// Kept back for bad-block replacement; never handed out for files.
constexpr u16 RESERVED_CLUSTERS = 0x0300;
constexpr u16 USABLE_CLUSTERS = TOTAL_CLUSTERS - RESERVED_CLUSTERS;

// The System Menu counts space in "blocks" of 8 clusters (128 KiB) and gives titles 2176 of them.
constexpr u16 CLUSTERS_PER_BLOCK = 8;
constexpr u16 USER_BLOCKS = 2176;
constexpr u16 USER_CLUSTERS = USER_BLOCKS * CLUSTERS_PER_BLOCK;
// Whatever is left is used by system titles and files.
constexpr u16 SYSTEM_CLUSTERS = USABLE_CLUSTERS - USER_CLUSTERS;

// Size of the FST (file and directory entries).
constexpr u16 TOTAL_INODES = 0x17ff;

struct NandStats
{
  u32 cluster_size;
  u32 free_clusters;
  u32 used_clusters;
  u32 bad_clusters;
  u32 reserved_clusters;
  u32 free_inodes;
  u32 used_inodes;
};

struct DirectoryStats
{
  u32 used_clusters;
  u32 used_inodes;
};

// Same as DirectoryStats but wide enough for a host NAND folder that is bigger than a real NAND
// could ever be. Emulator-only.
struct ExtendedDirectoryStats
{
  u64 used_clusters;
  u64 used_inodes;
};

struct FileStatus
{
  u32 offset;
  u32 size;
};

// Limits on paths: number of components, total characters, characters per name.
constexpr size_t MaxPathDepth = 8;
constexpr size_t MaxPathLength = 64;
constexpr size_t MaxFilenameLength = 12;

// "/" or a valid non-root path.
bool IsValidPath(std::string_view path);
// Absolute, at most MaxPathLength characters, and not ending in '/'.
bool IsValidNonRootPath(std::string_view path);
// A single path component of acceptable length.
bool IsValidFilename(std::string_view filename);

struct SplitPathResult
{
  std::string parent;
  std::string file_name;
};
inline bool operator==(const SplitPathResult& lhs, const SplitPathResult& rhs)
{
  return lhs.parent == rhs.parent && lhs.file_name == rhs.file_name;
}

// Splits a valid non-root path at its last '/': "/shared2/sys/SYSCONF" gives
// {"/shared2/sys", "SYSCONF"}, and "/tmp" gives {"/", "tmp"}.
SplitPathResult SplitPathAndBasename(std::string_view path);

class FileSystem;

// Owns an open file descriptor and closes it when destroyed.
class FileHandle final
{
public:
  FileHandle(FileSystem* fs, Fd fd);
  FileHandle(FileHandle&&);
  ~FileHandle();
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle& operator=(FileHandle&&);

  // Gives up ownership; the descriptor stays open.
  Fd Release();

  template <typename T>
  Result<size_t> Read(T* ptr, size_t count) const;

  template <typename T>
  Result<size_t> Write(const T* ptr, size_t count) const;

  Result<u32> Seek(u32 offset, SeekMode mode) const;
  Result<FileStatus> GetStatus() const;

private:
  FileSystem* m_fs;
  std::optional<Fd> m_fd;
};

// Interface of a NAND file system implementation.
class FileSystem
{
public:
  virtual ~FileSystem() = default;

  virtual void DoState(PointerWrap& p) = 0;

  // Erases everything. Only the kernel (UID 0) may do this.
  virtual ResultCode Format(Uid uid) = 0;

  virtual Result<FileHandle> OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode) = 0;
  // Opens a file for reading and writing, creating it with `modes` first when it is missing.
  Result<FileHandle> CreateAndOpenFile(Uid uid, Gid gid, const std::string& path, Modes modes);
  virtual ResultCode Close(Fd fd) = 0;
  // Reads/writes up to `size` bytes at the current position and returns how many were done.
  virtual Result<u32> ReadBytesFromFile(Fd fd, u8* ptr, u32 size) = 0;
  virtual Result<u32> WriteBytesToFile(Fd fd, const u8* ptr, u32 size) = 0;
  virtual Result<u32> SeekFile(Fd fd, u32 offset, SeekMode mode) = 0;
  // Never fails for a descriptor that is open.
  virtual Result<FileStatus> GetFileStatus(Fd fd) = 0;

  virtual ResultCode CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                                FileAttribute attribute, Modes modes) = 0;
  virtual ResultCode CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                                     FileAttribute attribute, Modes modes) = 0;

  // Creates every missing directory before the last '/' of `path`: "/a/b" makes "/a", and
  // "/a/b/" makes "/a" and "/a/b".
  ResultCode CreateFullPath(Uid caller_uid, Gid caller_gid, const std::string& path,
                            FileAttribute attribute, Modes modes);

  // Removes a file or a whole directory tree.
  virtual ResultCode Delete(Uid caller_uid, Gid caller_gid, const std::string& path) = 0;
  virtual ResultCode Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                            const std::string& new_path) = 0;

  // Names of the direct children of a directory.
  virtual Result<std::vector<std::string>> ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                         const std::string& path) = 0;

  virtual Result<Metadata> GetMetadata(Uid caller_uid, Gid caller_gid, const std::string& path) = 0;
  virtual ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                                 FileAttribute attribute, Modes modes) = 0;

  // Whole-NAND usage (cluster size, cluster and inode counts).
  virtual Result<NandStats> GetNandStats() = 0;
  // Usage of one directory tree, capped to what a real NAND can hold.
  virtual Result<DirectoryStats> GetDirectoryStats(const std::string& path) = 0;
  // Usage of one directory tree, uncapped.
  virtual Result<ExtendedDirectoryStats> GetExtendedDirectoryStats(const std::string& path) = 0;

  virtual void SetNandRedirects(std::vector<NandRedirect> nand_redirects) = 0;

protected:
  // Savestate helpers that serialise a directory tree (metadata and file contents).
  void DoStateWriteOrMeasure(PointerWrap& p, const std::string& directory_path);
  void DoStateRead(PointerWrap& p, const std::string& directory_path);
};

template <typename T>
Result<size_t> FileHandle::Read(T* ptr, size_t count) const
{
  const auto wanted = static_cast<u32>(sizeof(T) * count);
  const Result<u32> got = m_fs->ReadBytesFromFile(*m_fd, reinterpret_cast<u8*>(ptr), wanted);
  if (!got.has_value())
    return std::unexpected{got.error()};
  // Anything less than a full read counts as an error.
  if (*got != sizeof(T) * count)
    return std::unexpected{ResultCode::ShortRead};
  return count;
}

template <typename T>
Result<size_t> FileHandle::Write(const T* ptr, size_t count) const
{
  const auto length = static_cast<u32>(sizeof(T) * count);
  const Result<u32> put = m_fs->WriteBytesToFile(*m_fd, reinterpret_cast<const u8*>(ptr), length);
  if (!put.has_value())
    return std::unexpected{put.error()};
  return count;
}

enum class Location
{
  // The user's configured NAND folder.
  Configured,
  // The NAND used by the running session (may be a temporary copy).
  Session,
};

std::unique_ptr<FileSystem> MakeFileSystem(Location location = Location::Session,
                                           std::vector<NandRedirect> nand_redirects = {});

// Maps a ResultCode to the value IOS puts in the reply.
IOS::HLE::ReturnCode ConvertResult(ResultCode code);

}  // namespace FS
}  // namespace IOS::HLE

template <>
struct fmt::formatter<IOS::HLE::FS::ResultCode> : EnumFormatter<IOS::HLE::FS::ResultCode::ShortRead>
{
  constexpr formatter()
      : EnumFormatter({"Success", "Invalid", "AccessDenied", "SuperblockWriteFailed",
                       "SuperblockInitFailed", "AlreadyExists", "NotFound", "FstFull",
                       "NoFreeSpace", "NoFreeHandle", "TooManyPathComponents", "InUse", "BadBlock",
                       "EccError", "CriticalEccError", "FileNotEmpty", "CheckFailed",
                       "UnknownError", "ShortRead"})
  {
  }
};
