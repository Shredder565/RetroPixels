// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/FileSystemGCWii.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/VolumeDisc.h"

namespace DiscIO
{
namespace
{
constexpr size_t ENTRY_SIZE = 12;
constexpr size_t WORD_NAME = 0;
constexpr size_t WORD_OFFSET_OR_PARENT = 1;
constexpr size_t WORD_SIZE_OR_NEXT = 2;
constexpr u32 NAME_OFFSET_MASK = 0x00ffffff;
constexpr u32 DIRECTORY_FLAG_MASK = 0xff000000;

// No real FST is anywhere near this big (it is more than all of a Wii's RAM); the limit keeps a
// corrupt header from making us allocate gigabytes.
constexpr u64 MAX_FST_SIZE = 128 * 1024 * 1024;
}  // namespace

FileInfoGCWii::FileInfoGCWii(const u8* fst, u8 offset_shift, u32 index, u32 total_file_infos)
    : m_fst(fst), m_offset_shift(offset_shift), m_index(index), m_total_file_infos(total_file_infos)
{
}

FileInfoGCWii::FileInfoGCWii(const u8* fst, u8 offset_shift)
    : m_fst(fst), m_offset_shift(offset_shift), m_index(0), m_total_file_infos(0)
{
  m_total_file_infos = Word(WORD_SIZE_OR_NEXT);
}

FileInfoGCWii::FileInfoGCWii(const FileInfoGCWii& file_info, u32 index)
    : FileInfoGCWii(file_info.m_fst, file_info.m_offset_shift, index, file_info.m_total_file_infos)
{
}

FileInfoGCWii::~FileInfoGCWii() = default;

u32 FileInfoGCWii::Word(size_t word) const
{
  return Common::swap32(m_fst + ENTRY_SIZE * m_index + sizeof(u32) * word);
}

uintptr_t FileInfoGCWii::GetAddress() const
{
  return reinterpret_cast<uintptr_t>(m_fst + ENTRY_SIZE * m_index);
}

// Directories are followed by their contents, so the next sibling of a directory comes after all
// of them.
u32 FileInfoGCWii::NextIndex() const
{
  return IsDirectory() ? Word(WORD_SIZE_OR_NEXT) : m_index + 1;
}

FileInfo& FileInfoGCWii::operator++()
{
  m_index = NextIndex();
  return *this;
}

std::unique_ptr<FileInfo> FileInfoGCWii::clone() const
{
  return std::make_unique<FileInfoGCWii>(*this);
}

FileInfo::const_iterator FileInfoGCWii::begin() const
{
  return const_iterator(std::make_unique<FileInfoGCWii>(*this, m_index + 1));
}

FileInfo::const_iterator FileInfoGCWii::end() const
{
  return const_iterator(std::make_unique<FileInfoGCWii>(*this, NextIndex()));
}

u32 FileInfoGCWii::GetSize() const
{
  return Word(WORD_SIZE_OR_NEXT);
}

u64 FileInfoGCWii::GetOffset() const
{
  return u64{Word(WORD_OFFSET_OR_PARENT)} << m_offset_shift;
}

bool FileInfoGCWii::IsRoot() const
{
  return m_index == 0;
}

bool FileInfoGCWii::IsDirectory() const
{
  return (Word(WORD_NAME) & DIRECTORY_FLAG_MASK) != 0;
}

u32 FileInfoGCWii::GetTotalChildren() const
{
  return Word(WORD_SIZE_OR_NEXT) - (m_index + 1);
}

// The name table follows the last entry.
u64 FileInfoGCWii::NameOffset() const
{
  return u64{m_total_file_infos} * ENTRY_SIZE + (Word(WORD_NAME) & NAME_OFFSET_MASK);
}

const char* FileInfoGCWii::RawName() const
{
  return reinterpret_cast<const char*>(m_fst + NameOffset());
}

std::string FileInfoGCWii::GetName() const
{
  // Names are taken to be Shift-JIS on every disc, which is what makes some names on non-Japanese
  // discs (Pikmin, for one) come out right.
  return SHIFTJISToUTF8(RawName());
}

bool FileInfoGCWii::NameCaseInsensitiveEquals(std::string_view other) const
{
  // Compares in place without converting, except when both sides have a non-ASCII character at
  // the same position: then the rest of our (Shift-JIS) name is converted to UTF-8 to compare.
  const char* name = RawName();
  for (size_t i = 0; i < other.size(); ++i)
  {
    const char ours = name[i];
    const char theirs = other[i];
    if (ours == '\0')
      return false;  // our name is shorter

    if (static_cast<u8>(ours) >= 0x80 && static_cast<u8>(theirs) >= 0x80)
    {
      const std::string converted = SHIFTJISToUTF8(name + i);
      const std::string_view rest = other.substr(i);
      return std::ranges::equal(converted, rest, [](char a, char b) {
        return Common::ToLower(a) == Common::ToLower(b);
      });
    }

    if (Common::ToLower(ours) != Common::ToLower(theirs))
      return false;
  }
  return name[other.size()] == '\0';
}

std::string FileInfoGCWii::GetPath() const
{
  if (m_index == 0)
    return {};  // the root has no name

  if (IsDirectory())
  {
    const FileInfoGCWii parent(*this, Word(WORD_OFFSET_OR_PARENT));
    return parent.GetPath() + GetName() + '/';
  }

  // A file's directory is the closest entry before it that is a directory still containing it.
  // The root contains everything, so this always ends.
  u32 candidate = m_index - 1;
  while (true)
  {
    const FileInfoGCWii entry(*this, candidate);
    if (entry.IsDirectory() && entry.Word(WORD_SIZE_OR_NEXT) > m_index)
      return entry.GetPath() + GetName();
    --candidate;
  }
}

bool FileInfoGCWii::IsValid(u64 fst_size, const FileInfoGCWii& parent_directory) const
{
  if (NameOffset() >= fst_size)
  {
    ERROR_LOG_FMT(DISCIO, "Impossibly large name offset in file system");
    return false;
  }

  if (!IsDirectory())
    return true;

  if (Word(WORD_OFFSET_OR_PARENT) != parent_directory.m_index)
  {
    ERROR_LOG_FMT(DISCIO, "Incorrect parent offset in file system");
    return false;
  }

  const u32 next = Word(WORD_SIZE_OR_NEXT);
  if (next <= m_index)
  {
    ERROR_LOG_FMT(DISCIO, "Impossibly small directory size in file system");
    return false;
  }
  if (next > parent_directory.Word(WORD_SIZE_OR_NEXT))
  {
    ERROR_LOG_FMT(DISCIO, "Impossibly large directory size in file system");
    return false;
  }

  return std::ranges::all_of(*this, [&](const FileInfo& child) {
    return static_cast<const FileInfoGCWii&>(child).IsValid(fst_size, *this);
  });
}

FileSystemGCWii::FileSystemGCWii(const VolumeDisc* volume, const Partition& partition)
    : m_root(nullptr, 0, 0, 0)
{
  // Wii discs store FST offsets divided by 4.
  u8 offset_shift;
  if (volume->ReadSwapped<u32>(0x18, partition) == WII_DISC_MAGIC)
    offset_shift = 2;
  else if (volume->ReadSwapped<u32>(0x1c, partition) == GAMECUBE_DISC_MAGIC)
    offset_shift = 0;
  else
    return;  // no disc header, e.g. a partition whose data was removed

  const std::optional<u64> fst_offset = GetFSTOffset(*volume, partition);
  const std::optional<u64> fst_size = GetFSTSize(*volume, partition);
  if (!fst_offset || !fst_size)
    return;
  if (*fst_size < ENTRY_SIZE)
  {
    ERROR_LOG_FMT(DISCIO, "File system is too small");
    return;
  }
  if (*fst_size > MAX_FST_SIZE)
  {
    ERROR_LOG_FMT(DISCIO, "File system is abnormally large! Aborting loading");
    return;
  }

  m_fst.resize(*fst_size);
  if (!volume->Read(*fst_offset, m_fst.size(), m_fst.data(), partition))
  {
    ERROR_LOG_FMT(DISCIO, "Couldn't read file system table");
    return;
  }

  m_root = FileInfoGCWii(m_fst.data(), offset_shift);
  if (!m_root.IsDirectory())
  {
    ERROR_LOG_FMT(DISCIO, "File system root is not a directory");
    return;
  }
  if (u64{m_root.GetSize()} * ENTRY_SIZE > *fst_size)
  {
    ERROR_LOG_FMT(DISCIO, "File system has too many entries for its size");
    return;
  }
  // With a null byte at the very end, no name can run past the table.
  if (m_fst.back() != 0)
  {
    ERROR_LOG_FMT(DISCIO, "File system does not end with a null byte");
    return;
  }

  m_valid = m_root.IsValid(*fst_size, m_root);
}

FileSystemGCWii::~FileSystemGCWii() = default;

std::unique_ptr<FileInfo> FileSystemGCWii::FindFileInfo(std::string_view path) const
{
  // Paths are looked up from the root; an invalid file system has no files.
  return IsValid() ? FindInDirectory(path, m_root) : nullptr;
}

const FileInfo& FileSystemGCWii::GetRoot() const
{
  return m_root;
}

std::unique_ptr<FileInfo> FileSystemGCWii::FindInDirectory(std::string_view path,
                                                           const FileInfo& directory) const
{
  // Takes the first component of the path and looks for it here; the rest is looked up inside
  // the match. Repeated slashes are ignored. An empty path means the directory itself.
  const size_t start = path.find_first_not_of('/');
  if (start == std::string_view::npos)
    return directory.clone();

  const size_t slash = path.find('/', start);
  const std::string_view name =
      slash == std::string_view::npos ? path.substr(start) : path.substr(start, slash - start);
  const std::string_view rest =
      slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);

  for (const FileInfo& child : directory)
  {
    // Case-insensitive, because some discs have OPENING.BNR rather than opening.bnr. If a match
    // does not lead anywhere, another entry with the same name might.
    if (!child.NameCaseInsensitiveEquals(name))
      continue;
    if (std::unique_ptr<FileInfo> found = FindInDirectory(rest, child))
      return found;
  }
  return nullptr;
}

std::unique_ptr<FileInfo> FileSystemGCWii::FindFileInfo(u64 disc_offset) const
{
  if (!IsValid())
    return nullptr;

  if (m_files_by_end_offset.empty())
  {
    const u32 entries = m_root.GetSize();
    for (u32 index = 0; index < entries; ++index)
    {
      const FileInfoGCWii entry(m_root, index);
      if (entry.IsDirectory() || entry.GetSize() == 0)
        continue;
      // If several files end at the same place, the first one is kept.
      m_files_by_end_offset.try_emplace(entry.GetOffset() + entry.GetSize(), index);
    }
  }

  // The first file ending after the offset contains it, unless it starts after it.
  const auto it = m_files_by_end_offset.upper_bound(disc_offset);
  if (it == m_files_by_end_offset.end())
    return nullptr;
  auto file = std::make_unique<FileInfoGCWii>(m_root, it->second);
  if (file->GetOffset() > disc_offset)
    return nullptr;
  return file;
}

}  // namespace DiscIO
