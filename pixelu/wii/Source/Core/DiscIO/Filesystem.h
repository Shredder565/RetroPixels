// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
// One entry (file or directory) of a disc's file system table. Subclasses implement it for a
// concrete FST format; iterating a directory yields its direct children.
class FileInfo
{
  friend class const_iterator;

public:
  // A forward iterator that owns its own FileInfo object. Advancing it changes that object in
  // place, so references obtained through it are only good until the next increment, but copies
  // of the iterator (or of the FileInfo) are independent.
  class const_iterator final
  {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = const FileInfo;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;

    const_iterator() = default;
    const_iterator(std::unique_ptr<FileInfo> file_info) : m_file_info(std::move(file_info)) {}
    const_iterator(const const_iterator& other) : m_file_info(CloneOf(other)) {}
    const_iterator(const_iterator&& other) noexcept = default;
    ~const_iterator() = default;

    const_iterator& operator=(const const_iterator& other)
    {
      if (this != &other)
        m_file_info = CloneOf(other);
      return *this;
    }
    const_iterator& operator=(const_iterator&& other) noexcept = default;

    const_iterator& operator++()
    {
      ++*m_file_info;
      return *this;
    }
    const_iterator operator++(int)
    {
      const_iterator before(*this);
      ++*m_file_info;
      return before;
    }

    // Two iterators are equal when they point at the same entry, or are both empty.
    bool operator==(const const_iterator& other) const
    {
      if (!m_file_info || !other.m_file_info)
        return !m_file_info && !other.m_file_info;
      return *m_file_info == *other.m_file_info;
    }

    const FileInfo& operator*() const { return *m_file_info; }
    const FileInfo* operator->() const { return m_file_info.get(); }

  private:
    static std::unique_ptr<FileInfo> CloneOf(const const_iterator& it)
    {
      return it.m_file_info ? it.m_file_info->clone() : nullptr;
    }

    std::unique_ptr<FileInfo> m_file_info;
  };

  virtual ~FileInfo();

  // Entries are the same when they refer to the same place in the same table.
  bool operator==(const FileInfo& other) const { return GetAddress() == other.GetAddress(); }

  virtual std::unique_ptr<FileInfo> clone() const = 0;
  virtual const_iterator cbegin() const { return begin(); }
  virtual const_iterator cend() const { return end(); }
  virtual const_iterator begin() const = 0;
  virtual const_iterator end() const = 0;

  // Where the file's data starts (relative to the partition, for Wii discs). Meaningless for
  // directories.
  virtual u64 GetOffset() const = 0;
  // The file's length. Meaningless for directories.
  virtual u32 GetSize() const = 0;
  // A file's length, or the summed length of everything inside a directory.
  u64 GetTotalSize() const;
  virtual bool IsRoot() const = 0;
  virtual bool IsDirectory() const = 0;
  // How many entries a directory contains, counting the contents of its subdirectories.
  // Meaningless for files.
  virtual u32 GetTotalChildren() const = 0;
  virtual std::string GetName() const = 0;
  virtual bool NameCaseInsensitiveEquals(std::string_view other) const = 0;
  // The full path, built by walking up to the root. When walking down a tree it is cheaper to
  // build the path along the way.
  virtual std::string GetPath() const = 0;

protected:
  // Identifies the entry for operator==.
  virtual uintptr_t GetAddress() const = 0;

  // Moves to the next entry at the same level (used by const_iterator).
  virtual FileInfo& operator++() = 0;
};

class FileSystem
{
public:
  virtual ~FileSystem();

  // GetRoot may only be called on a valid file system.
  virtual bool IsValid() const = 0;
  // The root directory. It and every FileInfo derived from it live as long as the file system.
  virtual const FileInfo& GetRoot() const = 0;
  // Looks up an entry by path; nullptr if there is none.
  virtual std::unique_ptr<FileInfo> FindFileInfo(std::string_view path) const = 0;
  // Looks up the file containing the given offset; nullptr if there is none.
  virtual std::unique_ptr<FileInfo> FindFileInfo(u64 disc_offset) const = 0;
};

// Prefer Volume::GetFileSystem to constructing a file system directly: it picks the right
// implementation, checks IsValid and keeps the result for later calls.

}  // namespace DiscIO
