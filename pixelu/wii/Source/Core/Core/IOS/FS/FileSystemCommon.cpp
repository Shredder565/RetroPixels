// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/FS/FileSystem.h"

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <utility>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/FileUtil.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/FS/HostBackend/FS.h"

namespace IOS::HLE::FS
{
namespace
{
// File contents go through the savestate in pieces of this size.
constexpr u32 STATE_COPY_CHUNK = 0x10000;

std::string JoinPath(const std::string& directory, const std::string& name)
{
  std::string joined;
  joined.reserve(directory.size() + 1 + name.size());
  joined += directory;
  if (directory.back() != '/')
    joined += '/';
  joined += name;
  return joined;
}

// The directory's own metadata is stored field by field (not as one block).
void DoMetadataFields(PointerWrap& p, Metadata& m)
{
  p.Do(m.uid);
  p.Do(m.gid);
  p.Do(m.attribute);
  p.Do(m.modes);
  p.Do(m.is_file);
  p.Do(m.size);
  p.Do(m.fst_index);
}
}  // namespace

bool IsValidPath(std::string_view path)
{
  return path == "/" || IsValidNonRootPath(path);
}

bool IsValidNonRootPath(std::string_view path)
{
  if (path.size() < 2 || path.size() > MaxPathLength)
    return false;
  return path.front() == '/' && path.back() != '/';
}

bool IsValidFilename(std::string_view filename)
{
  return filename.size() <= MaxFilenameLength && filename.find('/') == std::string_view::npos;
}

SplitPathResult SplitPathAndBasename(std::string_view path)
{
  const size_t slash = path.rfind('/');
  // A slash at position 0 means the parent is the root, which keeps its "/".
  const size_t parent_length = slash == 0 ? 1 : slash;
  return {std::string(path.substr(0, parent_length)), std::string(path.substr(slash + 1))};
}

std::unique_ptr<FileSystem> MakeFileSystem(Location location,
                                           std::vector<NandRedirect> nand_redirects)
{
  const auto root_index = location == Location::Session ? D_SESSION_WIIROOT_IDX : D_WIIROOT_IDX;
  return std::make_unique<HostFileSystem>(File::GetUserPath(root_index),
                                          std::move(nand_redirects));
}

IOS::HLE::ReturnCode ConvertResult(ResultCode code)
{
  if (code == ResultCode::Success)
    return IPC_SUCCESS;
  // The enum lists the FS errors in IOS order, starting at -101 for Invalid.
  return static_cast<ReturnCode>(-100 - static_cast<s32>(code));
}

FileHandle::FileHandle(FileSystem* fs, Fd fd) : m_fs{fs}, m_fd{fd}
{
}

FileHandle::FileHandle(FileHandle&& other) : m_fs{other.m_fs}, m_fd{std::exchange(other.m_fd, {})}
{
}

FileHandle& FileHandle::operator=(FileHandle&& other)
{
  if (this != &other)
  {
    // Give back what this handle owns, then take over the other one's descriptor.
    if (m_fd && m_fs)
      ASSERT(m_fs->Close(*m_fd) == FS::ResultCode::Success);
    m_fs = other.m_fs;
    m_fd = std::exchange(other.m_fd, {});
  }
  return *this;
}

FileHandle::~FileHandle()
{
  if (m_fs != nullptr && m_fd.has_value())
    ASSERT(m_fs->Close(*m_fd) == FS::ResultCode::Success);
}

Fd FileHandle::Release()
{
  const std::optional<Fd> taken = std::exchange(m_fd, std::nullopt);
  return taken.value();
}

Result<u32> FileHandle::Seek(u32 offset, SeekMode mode) const
{
  return m_fs->SeekFile(*m_fd, offset, mode);
}

Result<FileStatus> FileHandle::GetStatus() const
{
  return m_fs->GetFileStatus(*m_fd);
}

Result<FileHandle> FileSystem::CreateAndOpenFile(Uid uid, Gid gid, const std::string& path,
                                                 Modes modes)
{
  Result<FileHandle> existing = OpenFile(uid, gid, path, Mode::ReadWrite);
  if (existing.has_value())
    return existing;

  if (const ResultCode created = CreateFile(uid, gid, path, 0, modes);
      created != ResultCode::Success)
  {
    return std::unexpected{created};
  }
  return OpenFile(uid, gid, path, Mode::ReadWrite);
}

ResultCode FileSystem::CreateFullPath(Uid uid, Gid gid, const std::string& path,
                                      FileAttribute attribute, Modes modes)
{
  // Visit each prefix that ends just before a '/', skipping the leading one.
  for (size_t slash = path.find('/', 1); slash != std::string::npos;
       slash = path.find('/', slash + 1))
  {
    const std::string prefix = path.substr(0, slash);
    const Result<Metadata> existing = GetMetadata(uid, gid, prefix);
    if (existing.has_value())
    {
      if (existing->is_file)
        return ResultCode::Invalid;
      continue;
    }
    if (existing.error() != ResultCode::NotFound)
      return existing.error();

    if (const ResultCode made = CreateDirectory(uid, gid, prefix, attribute, modes);
        made != ResultCode::Success)
    {
      return made;
    }
  }
  return ResultCode::Success;
}

// Savestate layout of a directory (written by DoStateWriteOrMeasure, read by DoStateRead):
//   the directory's metadata, field by field;
//   a u32 child count, then per child: its Metadata block, its name, and either its contents
//   (for a file, `size` raw bytes) or the same layout again (for a directory).
void FileSystem::DoStateRead(PointerWrap& p, const std::string& directory_path)
{
  // Clears the target; a missing entry is fine.
  const auto remove = [this](const std::string& target) {
    const ResultCode result = Delete(0, 0, target);
    if (result == ResultCode::Success || result == ResultCode::NotFound)
      return true;
    ERROR_LOG_FMT(IOS_FS, "Savestate load: cannot delete {}: {}", target, result);
    return false;
  };

  const bool is_root = directory_path == "/";
  if (is_root)
  {
    // The root itself cannot be deleted, so empty it instead.
    const auto children = ReadDirectory(0, 0, directory_path);
    if (!children)
    {
      ERROR_LOG_FMT(IOS_FS, "Savestate load: cannot list {}: {}", directory_path,
                    children.error());
      p.SetVerifyMode();
      return;
    }
    for (const std::string& child : *children)
    {
      if (!remove(directory_path + child))
      {
        p.SetVerifyMode();
        return;
      }
    }
  }
  else if (!remove(directory_path))
  {
    p.SetVerifyMode();
    return;
  }

  Metadata own{};
  DoMetadataFields(p, own);

  // Rebuild as the kernel (to get past permission checks), then hand ownership over. The root
  // always exists and is not created.
  if (!is_root)
  {
    const ResultCode made = CreateDirectory(0, 0, directory_path, own.attribute, own.modes);
    if (made != ResultCode::Success)
    {
      ERROR_LOG_FMT(IOS_FS, "Savestate load: cannot create {}: {}", directory_path, made);
      p.SetVerifyMode();
      return;
    }
  }
  if (const ResultCode set =
          SetMetadata(0, directory_path, own.uid, own.gid, own.attribute, own.modes);
      set != ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_FS, "Savestate load: cannot set metadata of {}: {}", directory_path, set);
    p.SetVerifyMode();
    return;
  }

  std::vector<std::string> names;
  p.DoEachElement(names, [this, &directory_path](PointerWrap& q, std::string& name) {
    Metadata meta;
    q.Do(meta);
    q.Do(name);
    const std::string path = JoinPath(directory_path, name);

    if (!meta.is_file)
    {
      DoStateRead(q, path);
      return;
    }

    const auto fail = [&q](const char* what, const std::string& target, ResultCode code) {
      ERROR_LOG_FMT(IOS_FS, "Savestate load: {} failed for {}: {}", what, target, code);
      q.SetVerifyMode();
    };

    if (const ResultCode made = CreateFile(0, 0, path, meta.attribute, meta.modes);
        made != ResultCode::Success)
    {
      fail("CreateFile", path, made);
      return;
    }
    if (const ResultCode set = SetMetadata(0, path, meta.uid, meta.gid, meta.attribute, meta.modes);
        set != ResultCode::Success)
    {
      fail("SetMetadata", path, set);
      return;
    }

    std::array<u8, STATE_COPY_CHUNK> chunk;
    const Result<FileHandle> file = OpenFile(0, 0, path, Mode::Write);
    if (!file)
    {
      fail("OpenFile", path, file.error());
      return;
    }

    for (u32 done = 0; done < meta.size;)
    {
      const u32 piece = std::min(meta.size - done, STATE_COPY_CHUNK);
      q.DoArray(chunk.data(), piece);

      const Result<size_t> written = file->Write(chunk.data(), piece);
      if (!written)
      {
        fail("Write", path, written.error());
        return;
      }
      if (*written != piece)
      {
        ERROR_LOG_FMT(IOS_FS, "Savestate load: wrote {} of {} bytes to {}", *written, piece, path);
        q.SetVerifyMode();
        return;
      }
      done += piece;
    }
  });
}

void FileSystem::DoStateWriteOrMeasure(PointerWrap& p, const std::string& directory_path)
{
  const Result<Metadata> own = GetMetadata(0, 0, directory_path);
  if (!own)
  {
    ERROR_LOG_FMT(IOS_FS, "Savestate save: cannot stat {}: {}", directory_path, own.error());
    p.SetVerifyMode();
    return;
  }
  Metadata own_copy = *own;
  DoMetadataFields(p, own_copy);

  auto names = ReadDirectory(0, 0, directory_path);
  if (!names)
  {
    ERROR_LOG_FMT(IOS_FS, "Savestate save: cannot list {}: {}", directory_path, names.error());
    p.SetVerifyMode();
    return;
  }

  p.DoEachElement(*names, [this, &directory_path](PointerWrap& q, std::string& name) {
    const std::string path = JoinPath(directory_path, name);

    Result<Metadata> meta = GetMetadata(0, 0, path);
    if (!meta)
    {
      ERROR_LOG_FMT(IOS_FS, "Savestate save: cannot stat {}: {}", name, meta.error());
      q.SetVerifyMode();
      return;
    }
    q.Do(*meta);
    q.Do(name);

    if (!meta->is_file)
    {
      DoStateWriteOrMeasure(q, path);
      return;
    }

    std::array<u8, STATE_COPY_CHUNK> chunk;
    const Result<FileHandle> file = OpenFile(0, 0, path, Mode::Read);
    if (!file)
    {
      ERROR_LOG_FMT(IOS_FS, "Savestate save: cannot open {}: {}", name, file.error());
      q.SetVerifyMode();
      return;
    }

    for (u32 done = 0; done < meta->size;)
    {
      const u32 piece = std::min(meta->size - done, STATE_COPY_CHUNK);
      const Result<size_t> got = file->Read(chunk.data(), piece);
      if (!got)
      {
        ERROR_LOG_FMT(IOS_FS, "Savestate save: cannot read {}: {}", name, got.error());
        q.SetVerifyMode();
        return;
      }
      if (*got != piece)
      {
        ERROR_LOG_FMT(IOS_FS, "Savestate save: read {} of {} bytes from {}", *got, piece, name);
        q.SetVerifyMode();
        return;
      }
      q.DoArray(chunk.data(), piece);
      done += piece;
    }
  });
}
}  // namespace IOS::HLE::FS
