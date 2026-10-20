// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/FS/HostBackend/FS.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/IOS.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

namespace IOS::HLE::FS
{
namespace
{
// On-disk record of fst.bin. The tree is stored depth first: each record is followed by its
// children's records. Multi-byte fields are big endian.
struct FstRecord
{
  std::array<char, 12> name{};
  Common::BigEndianValue<Uid> uid{};
  Common::BigEndianValue<Gid> gid{};
  bool is_file = false;
  Modes modes{};
  FileAttribute attribute{};
  Common::BigEndianValue<u32> unknown{};
  Common::BigEndianValue<u32> child_count{};
};
static_assert(std::is_standard_layout<FstRecord>());
static_assert(sizeof(FstRecord) == 0x20);

std::string_view RecordName(const FstRecord& record)
{
  return {record.name.data(), strnlen(record.name.data(), record.name.size())};
}

void SetRecordName(FstRecord& record, std::string_view name)
{
  std::memcpy(record.name.data(), name.data(), std::min(record.name.size(), name.size()));
}

// Prepares a host directory listing for the guest. Returns how many entries (counting every
// descendant) were dropped, and lowers each directory's descendant count accordingly.
u64 SanitiseListing(File::FSTEntry* directory, bool is_nand_root)
{
  u64 dropped = 0;
  auto it = directory->children.begin();
  while (it != directory->children.end())
  {
    // The NAND root folder also holds emulator files (such as fst.bin); hide plain files there.
    if (is_nand_root && !it->isDirectory)
    {
      it = directory->children.erase(it);
      ++dropped;
      continue;
    }

    // Names the host could not store directly are escaped on disk; show the original name.
    // (Harry Potter and the Half-Blood Prince looks for such a file.)
    if (it->virtualName.contains("__"))
      it->virtualName = Common::UnescapeFileName(it->virtualName);

    // Names a real NAND could not hold are hidden along with everything under them.
    if (!IsValidFilename(it->virtualName))
    {
      dropped += 1 + (it->isDirectory ? it->size : 0);
      it = directory->children.erase(it);
      continue;
    }

    dropped += SanitiseListing(&*it, false);
    ++it;
  }
  directory->size -= dropped;
  return dropped;
}

u64 CountClusters(const File::FSTEntry& directory)
{
  u64 total = 0;
  for (const File::FSTEntry& child : directory.children)
  {
    total += child.isDirectory ? CountClusters(child) :
                                 Common::AlignUp(child.size, CLUSTER_SIZE) / CLUSTER_SIZE;
  }
  return total;
}

bool PathIsWithin(const std::string& path, const std::string& prefix)
{
  return path.starts_with(prefix) && (path.size() == prefix.size() || path[prefix.size()] == '/');
}
}  // namespace

HostFileSystem::HostFilename HostFileSystem::BuildFilename(const std::string& wii_path) const
{
  for (const NandRedirect& redirect : m_nand_redirects)
  {
    if (PathIsWithin(wii_path, redirect.source_path))
    {
      const std::string rest = wii_path.substr(redirect.source_path.size());
      return {redirect.target_path + Common::EscapePath(rest), true};
    }
  }

  ASSERT(!m_root_path.empty());

  if (!wii_path.starts_with("/"))
  {
    ASSERT_MSG(IOS_FS, false, "BuildFilename: not an absolute NAND path: '{}'", wii_path);
    return {m_root_path, false};
  }
  return {m_root_path + Common::EscapePath(wii_path), false};
}

bool HostFileSystem::FstEntry::CheckPermission(Uid caller_uid, Gid caller_gid,
                                               Mode requested_mode) const
{
  // The kernel may do anything.
  if (caller_uid == 0)
    return true;

  Mode granted;
  if (data.uid == caller_uid)
    granted = data.modes.owner;
  else if (data.gid == caller_gid)
    granted = data.modes.group;
  else
    granted = data.modes.other;

  const u8 wanted = static_cast<u8>(requested_mode);
  return (static_cast<u8>(granted) & wanted) == wanted;
}

HostFileSystem::HostFileSystem(std::string root_path, std::vector<NandRedirect> nand_redirects)
    : m_root_path{std::move(root_path)}, m_nand_redirects(std::move(nand_redirects))
{
  // Store the root without trailing slashes; NAND paths are appended to it as they are.
  while (!m_root_path.empty() && m_root_path.back() == '/')
    m_root_path.pop_back();
  File::CreateFullPath(m_root_path + '/');
  ResetFst();
  LoadFst();
}

HostFileSystem::~HostFileSystem() = default;

std::string HostFileSystem::GetFstFilePath() const
{
  // The metadata of the whole tree lives in one file at the NAND root.
  return m_root_path + "/fst.bin";
}

void HostFileSystem::ResetFst()
{
  m_root_entry = {};
  m_root_entry.name = "/";
  // The FS module's root has mode 0x16: owner none, group and others read.
  m_root_entry.data.modes = {Mode::None, Mode::Read, Mode::Read};
}

void HostFileSystem::LoadFst()
{
  // A NAND folder that has never been used has no fst.bin; defaults are fine then.
  File::IOFile file{GetFstFilePath(), "rb"};
  if (!file)
    return;

  // Reads one record and, recursively, its children. Fails on truncation or excessive depth.
  struct Reader
  {
    File::IOFile& in;

    std::optional<FstEntry> Read(size_t depth)
    {
      if (depth > MaxPathDepth)
        return std::nullopt;

      FstRecord record;
      if (!in.ReadArray(&record, 1))
        return std::nullopt;

      FstEntry node;
      node.name = RecordName(record);
      node.data.uid = record.uid;
      node.data.gid = record.gid;
      node.data.is_file = record.is_file;
      node.data.modes = record.modes;
      node.data.attribute = record.attribute;

      const u32 count = record.child_count;
      for (u32 i = 0; i < count; ++i)
      {
        std::optional<FstEntry> child = Read(depth + 1);
        if (!child)
          return std::nullopt;
        node.children.push_back(*child);
      }
      return node;
    }
  };

  std::optional<FstEntry> root = Reader{file}.Read(0);
  if (!root)
  {
    ERROR_LOG_FMT(IOS_FS, "fst.bin is damaged; ignoring it");
    return;
  }
  m_root_entry = *root;
}

void HostFileSystem::SaveFst()
{
  std::vector<FstRecord> records;
  const auto flatten = [&records](const auto& self, const FstEntry& node) -> void {
    FstRecord& record = records.emplace_back();
    SetRecordName(record, node.name);
    record.uid = node.data.uid;
    record.gid = node.data.gid;
    record.is_file = node.data.is_file;
    record.modes = node.data.modes;
    record.attribute = node.data.attribute;
    record.child_count = static_cast<u32>(node.children.size());
    for (const FstEntry& child : node.children)
      self(self, child);
  };
  flatten(flatten, m_root_entry);

  // Write a temporary file and move it over the old one, so a crash cannot leave half a FST.
  const std::string final_path = GetFstFilePath();
  const std::string temp_path = File::GetTempFilenameForAtomicWrite(final_path);
  {
    File::IOFile out{temp_path, "wb"};
    if (!out.WriteArray(records.data(), records.size()))
    {
      PanicAlertFmt("IOS_FS: could not write the FST");
      return;
    }
    // `out` has to be closed before the rename.
  }
  if (!File::Rename(temp_path, final_path))
    PanicAlertFmt("IOS_FS: could not replace the FST with the new copy");
}

HostFileSystem::FstEntry* HostFileSystem::GetFstEntryForPath(const std::string& path)
{
  if (path == "/")
    return &m_root_entry;
  if (!IsValidNonRootPath(path))
    return nullptr;

  const HostFilename host = BuildFilename(path);
  const File::FileInfo info{host.host_path};
  if (!info.Exists())
    return nullptr;

  FstEntry* node = host.is_redirect ? &m_redirect_fst : &m_root_entry;
  std::string walked;
  for (const std::string& part : SplitString(path.substr(1), '/'))
  {
    walked += '/';
    walked += part;

    const auto found = std::ranges::find(node->children, part, &FstEntry::name);
    if (found != node->children.end())
    {
      node = &*found;
      continue;
    }

    // Unknown to the FST (a pre-existing host file, or one being created right now): add a
    // permissive entry. Creation fills in the real metadata afterwards.
    INFO_LOG_FMT(IOS_FS, "No FST entry for {} ({}); adding a default one", walked,
                 host.is_redirect ? "redirect" : "NAND");
    node = &node->children.emplace_back();
    node->name = part;
    node->data.modes = {Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};
  }

  // The host decides whether this is a file; a file cannot have children.
  node->data.is_file = info.IsFile();
  if (node->data.is_file && !node->children.empty())
  {
    WARN_LOG_FMT(IOS_FS, "{} is a file on the host; dropping its FST children", path);
    node->children.clear();
  }
  return node;
}

void HostFileSystem::DoState(PointerWrap& p)
{
  // Set all guest handles aside and drop their host streams: loading may have to replace files
  // that are open, and saving reuses the handle slots to read files. The set-aside copy is put
  // back after saving.
  std::array<Handle, 16> parked = std::move(m_handles);
  m_handles = {};
  for (Handle& handle : parked)
    handle.host_file.reset();

  // Layout:
  //   bool  whole NAND included (only while a movie runs on a temporary NAND)
  //   tree  "/tmp"
  //   u32   byte size of the following tree (0 when it is absent)
  //   tree  "/" (only when included)
  //   then the 16 handle slots.
  // A state with the whole NAND can be loaded without a movie; the NAND part is skipped.
  auto& movie = Core::System::GetInstance().GetMovie();
  const bool want_whole_nand = movie.IsMovieActive() && Core::WiiRootIsTemporary();

  bool has_whole_nand = want_whole_nand;
  p.Do(has_whole_nand);

  if (p.IsReadMode())
  {
    u32 nand_bytes = 0;
    DoStateRead(p, "/tmp");
    if (has_whole_nand && want_whole_nand)
    {
      p.Do(nand_bytes);
      DoStateRead(p, "/");
    }
    else
    {
      (void)p.DoExternal(nand_bytes);
    }
  }
  else
  {
    DoStateWriteOrMeasure(p, "/tmp");
    u8* const size_slot = p.ReserveU32();
    if (has_whole_nand)
    {
      DoStateWriteOrMeasure(p, "/");
      if (p.IsWriteMode())
      {
        const u32 nand_bytes =
            static_cast<u32>(p.GetOffsetFromPreviousPosition(size_slot) - sizeof(u32));
        std::memcpy(size_slot, &nand_bytes, sizeof(nand_bytes));
      }
    }
    m_handles = std::move(parked);
  }

  // Open handles get their host file reopened after loading.
  for (Handle& h : m_handles)
  {
    p.Do(h.opened);
    p.Do(h.mode);
    p.Do(h.wii_path);
    p.Do(h.file_offset);
    if (h.opened)
      h.host_file = OpenHostFile(BuildFilename(h.wii_path).host_path);
  }
}

ResultCode HostFileSystem::Format(Uid uid)
{
  if (uid != 0 || m_root_path.empty())
    return ResultCode::AccessDenied;

  const std::string root = BuildFilename("/").host_path;
  if (!File::DeleteDirRecursively(root) || !File::CreateDir(root))
    return ResultCode::UnknownError;

  ResetFst();
  SaveFst();
  // Every descriptor is gone with the files.
  m_handles = {};
  return ResultCode::Success;
}

ResultCode HostFileSystem::CreateFileOrDirectory(Uid uid, Gid gid, const std::string& path,
                                                 FileAttribute attr, Modes modes, bool is_file)
{
  if (!IsValidNonRootPath(path) || !std::ranges::all_of(path, Common::IsPrintableCharacter))
    return ResultCode::Invalid;

  // Directories may not be nested deeper than the FST allows.
  if (!is_file && std::ranges::count(path, '/') > static_cast<int>(MaxPathDepth))
    return ResultCode::TooManyPathComponents;

  const SplitPathResult parts = SplitPathAndBasename(path);
  const std::string host_path = BuildFilename(path).host_path;

  FstEntry* parent = GetFstEntryForPath(parts.parent);
  if (parent == nullptr)
    return ResultCode::NotFound;
  if (!parent->CheckPermission(uid, gid, Mode::Write))
    return ResultCode::AccessDenied;
  if (File::Exists(host_path))
    return ResultCode::AlreadyExists;

  const bool created = is_file ? File::CreateEmptyFile(host_path) : File::CreateDir(host_path);
  if (!created)
  {
    ERROR_LOG_FMT(IOS_FS, "Host refused to create {}", host_path);
    return ResultCode::UnknownError;
  }

  // Now that the host object exists, its FST node can be made and filled in.
  FstEntry* node = GetFstEntryForPath(path);
  *node = {};
  node->name = parts.file_name;
  node->data.is_file = is_file;
  node->data.modes = modes;
  node->data.uid = uid;
  node->data.gid = gid;
  node->data.attribute = attr;
  SaveFst();
  return ResultCode::Success;
}

ResultCode HostFileSystem::CreateDirectory(Uid uid, Gid gid, const std::string& path,
                                           FileAttribute attr, Modes modes)
{
  constexpr bool is_file = false;
  return CreateFileOrDirectory(uid, gid, path, attr, modes, is_file);
}

ResultCode HostFileSystem::CreateFile(Uid uid, Gid gid, const std::string& path, FileAttribute attr,
                                      Modes modes)
{
  constexpr bool is_file = true;
  return CreateFileOrDirectory(uid, gid, path, attr, modes, is_file);
}

bool HostFileSystem::IsFileOpened(const std::string& path) const
{
  for (const Handle& handle : m_handles)
  {
    if (handle.opened && handle.wii_path == path)
      return true;
  }
  return false;
}

bool HostFileSystem::IsDirectoryInUse(const std::string& path) const
{
  // Plain prefix test, as before: "/a" also counts as in use while "/ab" is open.
  for (const Handle& handle : m_handles)
  {
    if (handle.opened && handle.wii_path.starts_with(path))
      return true;
  }
  return false;
}

ResultCode HostFileSystem::Delete(Uid uid, Gid gid, const std::string& path)
{
  if (!IsValidNonRootPath(path))
    return ResultCode::Invalid;

  const std::string host_path = BuildFilename(path).host_path;
  const SplitPathResult parts = SplitPathAndBasename(path);

  FstEntry* parent = GetFstEntryForPath(parts.parent);
  if (parent == nullptr)
    return ResultCode::NotFound;
  if (!parent->CheckPermission(uid, gid, Mode::Write))
    return ResultCode::AccessDenied;
  if (!File::Exists(host_path))
    return ResultCode::NotFound;

  const bool removable_file = File::IsFile(host_path) && !IsFileOpened(path);
  const bool removable_dir =
      !removable_file && File::IsDirectory(host_path) && !IsDirectoryInUse(path);
  if (removable_file)
    File::Delete(host_path);
  else if (removable_dir)
    File::DeleteDirRecursively(host_path);
  else
    return ResultCode::InUse;

  auto& siblings = parent->children;
  if (const auto it = std::ranges::find(siblings, parts.file_name, &FstEntry::name);
      it != siblings.end())
  {
    siblings.erase(it);
  }
  SaveFst();
  return ResultCode::Success;
}

ResultCode HostFileSystem::Rename(Uid uid, Gid gid, const std::string& old_path,
                                  const std::string& new_path)
{
  if (!IsValidNonRootPath(old_path) || !IsValidNonRootPath(new_path))
    return ResultCode::Invalid;

  const SplitPathResult from = SplitPathAndBasename(old_path);
  const SplitPathResult to = SplitPathAndBasename(new_path);

  FstEntry* from_parent = GetFstEntryForPath(from.parent);
  FstEntry* to_parent = GetFstEntryForPath(to.parent);
  if (from_parent == nullptr || to_parent == nullptr)
    return ResultCode::NotFound;

  const bool may_write = from_parent->CheckPermission(uid, gid, Mode::Write) &&
                         to_parent->CheckPermission(uid, gid, Mode::Write);
  if (!may_write)
    return ResultCode::AccessDenied;

  FstEntry* moving = GetFstEntryForPath(old_path);
  if (moving == nullptr)
    return ResultCode::NotFound;

  // A file can move to another directory but must keep its name.
  const bool is_file = moving->data.is_file;
  if (is_file && from.file_name != to.file_name)
    return ResultCode::Invalid;

  const bool busy = is_file ? IsFileOpened(old_path) : IsDirectoryInUse(old_path);
  if (busy)
    return ResultCode::InUse;

  const HostFilename src = BuildFilename(old_path);
  const HostFilename dst = BuildFilename(new_path);

  // An existing target of the same kind is replaced; a target of the other kind is an error.
  if (File::Exists(dst.host_path))
  {
    const bool src_is_file = File::IsFile(src.host_path);
    const bool dst_is_file = File::IsFile(dst.host_path);
    if (src_is_file != dst_is_file)
      return ResultCode::Invalid;
    if (src_is_file)
      File::Delete(dst.host_path);
    else
      File::DeleteDirRecursively(dst.host_path);
  }

  if (!File::Rename(src.host_path, dst.host_path))
  {
    if (!src.is_redirect && !dst.is_redirect)
    {
      ERROR_LOG_FMT(IOS_FS, "Host rename of {} to {} failed", src.host_path, dst.host_path);
      return ResultCode::NotFound;
    }

    // A redirect may live on another volume, where a rename cannot work; copy and delete.
    if (!File::CopyRegularFile(src.host_path, dst.host_path))
    {
      ERROR_LOG_FMT(IOS_FS, "Rename fallback: copying {} to {} failed", src.host_path,
                    dst.host_path);
      return ResultCode::NotFound;
    }
    if (!File::Delete(src.host_path))
    {
      ERROR_LOG_FMT(IOS_FS, "Rename fallback: removing {} failed", src.host_path);
      return ResultCode::Invalid;
    }
  }

  // Look up (or add) the node at the destination, then move the old node's metadata and
  // children into it and drop the old node.
  FstEntry* arrived = GetFstEntryForPath(new_path);
  arrived->name = to.file_name;

  auto& old_siblings = from_parent->children;
  const auto old_node = std::ranges::find(old_siblings, from.file_name, &FstEntry::name);
  if (old_node != old_siblings.end())
  {
    arrived->data = old_node->data;
    arrived->children = old_node->children;
    old_siblings.erase(old_node);
  }

  SaveFst();
  return ResultCode::Success;
}

Result<std::vector<std::string>> HostFileSystem::ReadDirectory(Uid uid, Gid gid,
                                                               const std::string& path)
{
  if (!IsValidPath(path))
    return std::unexpected{ResultCode::Invalid};

  const FstEntry* node = GetFstEntryForPath(path);
  if (node == nullptr)
    return std::unexpected{ResultCode::NotFound};
  if (!node->CheckPermission(uid, gid, Mode::Read))
    return std::unexpected{ResultCode::AccessDenied};
  if (node->data.is_file)
    return std::unexpected{ResultCode::Invalid};

  File::FSTEntry listing = File::ScanDirectoryTree(BuildFilename(path).host_path, false);
  SanitiseListing(&listing, path == "/");

  // IOS keeps a directory's children in a list with new entries at the front, so the newest
  // file comes first. Entries the FST does not know about come last, in descending name order
  // so that the result does not depend on the host file system.
  std::unordered_map<std::string_view, int> age;
  age.reserve(node->children.size());
  for (size_t i = 0; i < node->children.size(); ++i)
    age.emplace(node->children[i].name, static_cast<int>(i));

  const auto rank = [&age](std::string_view name) {
    const auto found = age.find(name);
    return found == age.end() ? -1 : found->second;
  };
  std::ranges::sort(listing.children, [&rank](const File::FSTEntry& a, const File::FSTEntry& b) {
    const int ra = rank(a.virtualName);
    const int rb = rank(b.virtualName);
    if (ra == rb)
      return a.virtualName > b.virtualName;
    return ra > rb;
  });

  std::vector<std::string> names;
  names.reserve(listing.children.size());
  for (const File::FSTEntry& child : listing.children)
    names.push_back(child.virtualName);
  return names;
}

Result<Metadata> HostFileSystem::GetMetadata(Uid uid, Gid gid, const std::string& path)
{
  const FstEntry* node;
  if (path == "/")
  {
    node = &m_root_entry;
  }
  else
  {
    if (!IsValidNonRootPath(path))
      return std::unexpected{ResultCode::Invalid};

    // Looking at an entry needs read access to the directory that holds it.
    const FstEntry* parent = GetFstEntryForPath(SplitPathAndBasename(path).parent);
    if (parent == nullptr)
      return std::unexpected{ResultCode::NotFound};
    if (!parent->CheckPermission(uid, gid, Mode::Read))
      return std::unexpected{ResultCode::AccessDenied};
    node = GetFstEntryForPath(path);
    if (node == nullptr)
      return std::unexpected{ResultCode::NotFound};
  }

  Metadata metadata = node->data;
  // The size always comes from the host (32 bits, as on the NAND).
  metadata.size = static_cast<u32>(File::GetSize(BuildFilename(path).host_path));
  return metadata;
}

ResultCode HostFileSystem::SetMetadata(Uid caller_uid, const std::string& path, Uid uid, Gid gid,
                                       FileAttribute attr, Modes modes)
{
  if (!IsValidPath(path))
    return ResultCode::Invalid;

  FstEntry* node = GetFstEntryForPath(path);
  if (node == nullptr)
    return ResultCode::NotFound;

  // Only the kernel can change ownership; others may only touch their own entries.
  if (caller_uid != 0 && (caller_uid != node->data.uid || uid != node->data.uid))
    return ResultCode::AccessDenied;

  // A file that already holds data cannot be given away.
  const bool host_empty = File::GetSize(BuildFilename(path).host_path) == 0;
  if (node->data.is_file && node->data.uid != uid && !host_empty)
    return ResultCode::FileNotEmpty;

  Metadata& data = node->data;
  const bool changed =
      data.gid != gid || data.uid != uid || data.attribute != attr || data.modes != modes;
  if (changed)
  {
    data.gid = gid;
    data.uid = uid;
    data.attribute = attr;
    data.modes = modes;
    SaveFst();
  }
  return ResultCode::Success;
}

Result<NandStats> HostFileSystem::GetNandStats()
{
  const Result<DirectoryStats> root = GetDirectoryStats("/");
  if (!root)
    return std::unexpected{root.error()};

  NandStats stats{};
  stats.cluster_size = CLUSTER_SIZE;
  stats.used_clusters = root->used_clusters;
  stats.free_clusters = USABLE_CLUSTERS - root->used_clusters;
  stats.bad_clusters = 0;
  stats.reserved_clusters = RESERVED_CLUSTERS;
  stats.used_inodes = root->used_inodes;
  stats.free_inodes = TOTAL_INODES - root->used_inodes;
  return stats;
}

Result<DirectoryStats> HostFileSystem::GetDirectoryStats(const std::string& wii_path)
{
  const Result<ExtendedDirectoryStats> wide = GetExtendedDirectoryStats(wii_path);
  if (!wide)
    return std::unexpected{wide.error()};

  // A real NAND cannot report more than it has.
  DirectoryStats stats{};
  stats.used_clusters = static_cast<u32>(std::min<u64>(wide->used_clusters, USABLE_CLUSTERS));
  stats.used_inodes = static_cast<u32>(std::min<u64>(wide->used_inodes, TOTAL_INODES));
  return stats;
}

Result<ExtendedDirectoryStats>
HostFileSystem::GetExtendedDirectoryStats(const std::string& wii_path)
{
  if (!IsValidPath(wii_path))
    return std::unexpected{ResultCode::Invalid};

  const std::string host_path = BuildFilename(wii_path).host_path;
  const File::FileInfo info(host_path);
  if (!info.Exists())
    return std::unexpected{ResultCode::NotFound};
  if (!info.IsDirectory())
    return std::unexpected{ResultCode::Invalid};

  File::FSTEntry tree = File::ScanDirectoryTree(host_path, true);
  SanitiseListing(&tree, wii_path == "/");

  ExtendedDirectoryStats stats{};
  // The directory itself takes one inode as well.
  stats.used_inodes = tree.size + 1;
  stats.used_clusters = CountClusters(tree);
  return stats;
}

void HostFileSystem::SetNandRedirects(std::vector<NandRedirect> nand_redirects)
{
  m_nand_redirects = std::move(nand_redirects);
}
}  // namespace IOS::HLE::FS
