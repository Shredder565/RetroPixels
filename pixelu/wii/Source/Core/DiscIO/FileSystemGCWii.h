// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Filesystem.h"

namespace DiscIO
{
class VolumeDisc;
struct Partition;

// An entry of a GameCube/Wii FST. The FST is an array of 12-byte entries (three big-endian
// u32s) followed by a table of null-terminated names:
//   word 0: name offset in the name table (low 24 bits); the top byte is nonzero for directories
//   word 1: files: data offset (shifted right by 2 on Wii); directories: index of the parent
//   word 2: files: size; directories: index of the first entry after the directory's contents
// Entry 0 is the root directory, whose word 2 is the total number of entries.
class FileInfoGCWii final : public FileInfo
{
public:
  // None of these take ownership of the FST bytes.

  // An arbitrary entry.
  FileInfoGCWii(const u8* fst, u8 offset_shift, u32 index, u32 total_file_infos);
  // The root entry (the entry count is read from it).
  FileInfoGCWii(const u8* fst, u8 offset_shift);
  FileInfoGCWii(const FileInfoGCWii& file_info) = default;
  // Another entry of the same FST.
  FileInfoGCWii(const FileInfoGCWii& file_info, u32 index);

  ~FileInfoGCWii() override;

  std::unique_ptr<FileInfo> clone() const override;
  const_iterator begin() const override;
  const_iterator end() const override;

  u64 GetOffset() const override;
  u32 GetSize() const override;
  bool IsRoot() const override;
  bool IsDirectory() const override;
  u32 GetTotalChildren() const override;
  std::string GetName() const override;
  bool NameCaseInsensitiveEquals(std::string_view other) const override;
  std::string GetPath() const override;

  // Checks this entry (and, for directories, everything inside) against the FST size and the
  // given parent.
  bool IsValid(u64 fst_size, const FileInfoGCWii& parent_directory) const;

protected:
  uintptr_t GetAddress() const override;
  FileInfo& operator++() override;

private:
  u32 Word(size_t word) const;
  u32 NextIndex() const;
  u64 NameOffset() const;
  const char* RawName() const;

  const u8* m_fst;
  u8 m_offset_shift;
  u32 m_index;
  u32 m_total_file_infos;
};

class FileSystemGCWii final : public FileSystem
{
public:
  FileSystemGCWii(const VolumeDisc* volume, const Partition& partition);
  ~FileSystemGCWii() override;

  bool IsValid() const override { return m_valid; }
  const FileInfo& GetRoot() const override;
  std::unique_ptr<FileInfo> FindFileInfo(std::string_view path) const override;
  std::unique_ptr<FileInfo> FindFileInfo(u64 disc_offset) const override;

private:
  std::unique_ptr<FileInfo> FindInDirectory(std::string_view path, const FileInfo& directory) const;

  bool m_valid = false;
  std::vector<u8> m_fst;
  FileInfoGCWii m_root;
  // End offset of each non-empty file -> its FST index. Built on first use.
  mutable std::map<u64, u32> m_files_by_end_offset;
};

}  // namespace DiscIO
