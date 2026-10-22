// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/FS/FileSystem.h"

namespace IOS::HLE::FS
{
// A NAND kept as an ordinary folder on the host.
//
// File data and the set of existing files come straight from the host folder. Ownership,
// permissions, attributes and the order of directory entries are kept in a small FST file
// (fst.bin) next to the data; entries missing from it get permissive defaults, so a folder
// populated by hand still works. Checks that need real NAND metadata are only approximated.
class HostFileSystem final : public FileSystem
{
public:
  HostFileSystem(std::string root_path, std::vector<NandRedirect> nand_redirects = {});
  ~HostFileSystem() override;

  void DoState(PointerWrap& p) override;

  ResultCode Format(Uid uid) override;

  Result<FileHandle> OpenFile(Uid uid, Gid gid, const std::string& path, Mode mode) override;
  ResultCode Close(Fd fd) override;
  Result<u32> ReadBytesFromFile(Fd fd, u8* ptr, u32 size) override;
  Result<u32> WriteBytesToFile(Fd fd, const u8* ptr, u32 size) override;
  Result<u32> SeekFile(Fd fd, u32 offset, SeekMode mode) override;
  Result<FileStatus> GetFileStatus(Fd fd) override;

  ResultCode CreateFile(Uid caller_uid, Gid caller_gid, const std::string& path,
                        FileAttribute attribute, Modes modes) override;

  ResultCode CreateDirectory(Uid caller_uid, Gid caller_gid, const std::string& path,
                             FileAttribute attribute, Modes modes) override;

  ResultCode Delete(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode Rename(Uid caller_uid, Gid caller_gid, const std::string& old_path,
                    const std::string& new_path) override;

  Result<std::vector<std::string>> ReadDirectory(Uid caller_uid, Gid caller_gid,
                                                 const std::string& path) override;

  Result<Metadata> GetMetadata(Uid caller_uid, Gid caller_gid, const std::string& path) override;
  ResultCode SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                         FileAttribute attribute, Modes modes) override;

  Result<NandStats> GetNandStats() override;
  Result<DirectoryStats> GetDirectoryStats(const std::string& path) override;
  Result<ExtendedDirectoryStats> GetExtendedDirectoryStats(const std::string& path) override;

  void SetNandRedirects(std::vector<NandRedirect> nand_redirects) override;

private:
  // One node of the in-memory FST.
  struct FstEntry
  {
    bool CheckPermission(Uid uid, Gid gid, Mode requested_mode) const;
    std::string name;
    Metadata data{};
    // Directory contents, oldest first. Empty for files.
    std::vector<FstEntry> children;
  };

  // An open guest descriptor.
  struct Handle
  {
    bool opened = false;
    Mode mode = Mode::None;
    std::string wii_path;
    std::shared_ptr<File::IOFile> host_file;
    u32 file_offset = 0;
  };
  Handle* AssignFreeHandle();
  Handle* GetHandleFromFd(Fd fd);
  Fd ConvertHandleToFd(const Handle* handle) const;

  struct HostFilename
  {
    std::string host_path;
    bool is_redirect;
  };
  HostFilename BuildFilename(const std::string& wii_path) const;
  std::shared_ptr<File::IOFile> OpenHostFile(const std::string& host_path);

  ResultCode CreateFileOrDirectory(Uid uid, Gid gid, const std::string& path,
                                   FileAttribute attribute, Modes modes, bool is_file);
  bool IsFileOpened(const std::string& path) const;
  bool IsDirectoryInUse(const std::string& path) const;

  std::string GetFstFilePath() const;
  void ResetFst();
  void LoadFst();
  void SaveFst();
  // Finds the FST node of an existing host file or directory, adding default nodes for any part
  // of the path the FST does not know yet. nullptr when the path is invalid or nothing exists on
  // the host.
  FstEntry* GetFstEntryForPath(const std::string& path);

  // FST of the NAND root. It is authoritative for metadata and entry order only; whether a file
  // exists is always decided by the host folder.
  FstEntry m_root_entry{};
  std::string m_root_path;
  // Host files currently open, shared between guest handles to the same file.
  std::map<std::string, std::weak_ptr<File::IOFile>> m_open_files;
  std::array<Handle, 16> m_handles{};

  // Separate FST for paths that are redirected elsewhere; never saved.
  FstEntry m_redirect_fst{};
  std::vector<NandRedirect> m_nand_redirects;
};

}  // namespace IOS::HLE::FS
