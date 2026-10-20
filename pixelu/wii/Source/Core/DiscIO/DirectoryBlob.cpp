// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/DirectoryBlob.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WiiEncryptionCache.h"

// Layout of an assembled disc
// ---------------------------
// GameCube (and each Wii partition's decrypted data):
//   0x0000  boot.bin (0x440 bytes, with the DOL and FST fields filled in)
//   0x0440  bi2.bin (0x2000 bytes)
//   0x2440  apploader.img
//   then    main.dol, 32-byte aligned with 32 bytes of padding
//   then    the FST, likewise; file data starts at the next 32 KiB boundary
// Wii, outside the partitions:
//   0x00000 header.bin (0x100 bytes), 0x40000 partition table, 0x4E000 region data (0x20),
//   and for each partition: ticket, partition header, TMD, certificates, H3 table and
//   then, 0x20000 bytes in, the encrypted data. The update partition goes at 0x50000 and the
//   game partition at 0xF800000 or later; each partition starts on a 64 KiB boundary.

namespace DiscIO
{
namespace
{
constexpr u32 TYPE_GAME = 0;
constexpr u32 TYPE_UPDATE = 1;
constexpr u32 TYPE_CHANNEL = 2;

// Written where a region is expected but none is known. (0 can't be used: it means NTSC-J.)
constexpr u32 UNKNOWN_REGION = 0xFF;

// Offsets within a Wii partition
constexpr u64 TMD_OFFSET = 0x2C0;
constexpr u64 H3_OFFSET = 0x4000;
constexpr u64 DATA_OFFSET = 0x20000;

constexpr u64 FST_ENTRY_SIZE = 12;

void PutBE32(std::vector<u8>& buffer, size_t offset, u32 value)
{
  buffer[offset + 0] = u8(value >> 24);
  buffer[offset + 1] = u8(value >> 16);
  buffer[offset + 2] = u8(value >> 8);
  buffer[offset + 3] = u8(value);
}

// Fills as much of `buffer` as the file has; returns how many bytes were read.
size_t ReadFileStart(const std::string& path, std::vector<u8>* buffer)
{
  File::DirectIOFile file(path, File::AccessMode::Read);
  file.Read(buffer->data(), std::min<u64>(file.GetSize(), buffer->size()));
  return static_cast<size_t>(file.Tell());
}

// --- Paths ------------------------------------------------------------------------------------

bool IsSeparator(char c)
{
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// Like ends_with, except that any directory separator matches any other.
bool PathEndsWith(std::string_view path, std::string_view suffix)
{
  if (suffix.size() > path.size())
    return false;
  const std::string_view tail = path.substr(path.size() - suffix.size());
  for (size_t i = 0; i < suffix.size(); ++i)
  {
    if (tail[i] != suffix[i] && !(IsSeparator(tail[i]) && IsSeparator(suffix[i])))
      return false;
  }
  return true;
}

std::optional<u32> PartitionTypeFromDirectoryName(const std::string& name)
{
  if (name.size() < 2)
    return std::nullopt;
  if (strcasecmp(name.c_str(), "DATA") == 0)
    return TYPE_GAME;
  if (strcasecmp(name.c_str(), "UPDATE") == 0)
    return TYPE_UPDATE;
  if (strcasecmp(name.c_str(), "CHANNEL") == 0)
    return TYPE_CHANNEL;
  if (name[0] != 'P' && name[0] != 'p')
    return std::nullopt;

  // "P-XXXX": the type is the four characters read as a big endian number
  if (name.size() == 6 && name[1] == '-')
    return Common::swap32(reinterpret_cast<const u8*>(name.data() + 2));

  // "P<number>"
  if (std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
  {
    u32 type;
    if (TryParse(name.substr(1), &type))
      return type;
  }
  return std::nullopt;
}

struct DirectoryPaths
{
  std::string partition_root;  // the directory containing sys/, with a trailing separator
  std::string true_root;       // the directory containing the partition directories
};

std::optional<DirectoryPaths> GetDirectoryPaths(const std::string& dol_path)
{
  static constexpr std::string_view DOL_SUFFIX = "/sys/main.dol";
  if (!PathEndsWith(dol_path, DOL_SUFFIX))
    return std::nullopt;

  DirectoryPaths paths;
  paths.partition_root = dol_path.substr(0, dol_path.size() - (DOL_SUFFIX.size() - 1));
  if (File::GetSize(paths.partition_root + "sys/boot.bin") < 0x20)
    return std::nullopt;

#ifdef _WIN32
  static constexpr const char* SEPARATORS = "/\\";
#else
  static constexpr const char* SEPARATORS = "/";
#endif
  // The separator before the partition directory's name
  const size_t pos = dol_path.find_last_of(SEPARATORS, paths.partition_root.size() - 2);
  paths.true_root = dol_path.substr(0, pos + 1);
  return paths;
}

// The name of the partition directory, if there is one.
std::string PartitionDirectoryName(const DirectoryPaths& paths)
{
  if (paths.true_root.size() >= paths.partition_root.size())
    return {};
  return paths.partition_root.substr(paths.true_root.size(),
                                     paths.partition_root.size() - paths.true_root.size() - 1);
}

bool IsExtractedDisc(const std::string& dol_path)
{
  return File::Exists(dol_path) && GetDirectoryPaths(dol_path).has_value();
}

// Is the path inside the files/ directory of an extracted disc?
bool IsInFilesDirectory(const std::string& path)
{
  static constexpr std::string_view FILES = "files";
  size_t search_end = std::string::npos;
  while (true)
  {
    const size_t pos = path.rfind(FILES, search_end);
    if (pos == std::string::npos)
      return false;

    const size_t after = pos + FILES.size();
    const bool whole_name = (pos == 0 || IsSeparator(path[pos - 1])) &&
                            (after == path.size() || IsSeparator(path[after]));
    if (whole_name && IsExtractedDisc(path.substr(0, pos) + "sys/main.dol"))
      return true;

    if (pos == 0)
      return false;
    search_end = pos - 1;
  }
}

// Is the path the main.dol of a partition other than the game partition, in an extracted disc
// that also has a game partition?
bool IsNonGamePartitionDol(const std::string& path)
{
  const std::optional<DirectoryPaths> paths = GetDirectoryPaths(path);
  if (!paths)
    return false;

  const std::optional<u32> type = PartitionTypeFromDirectoryName(PartitionDirectoryName(*paths));
  if (!type || *type == TYPE_GAME)
    return false;

  const File::FSTEntry root = File::ScanDirectoryTree(paths->true_root, false);
  return std::ranges::any_of(root.children, [](const File::FSTEntry& entry) {
    return entry.isDirectory && PartitionTypeFromDirectoryName(entry.virtualName) == TYPE_GAME &&
           IsExtractedDisc(entry.physicalName + "/sys/main.dol");
  });
}

// Is the path `suffix` (e.g. /sys/boot.bin) of a directory that has a sys/main.dol?
bool IsFileOfExtractedDisc(const std::string& path, std::string_view suffix)
{
  if (!PathEndsWith(path, suffix))
    return false;
  return File::Exists(path.substr(0, path.size() - suffix.size()) + "/sys/main.dol");
}
}  // namespace

bool ShouldHideFromGameList(const std::string& volume_path)
{
  return IsInFilesDirectory(volume_path) || IsNonGamePartitionDol(volume_path) ||
         IsFileOfExtractedDisc(volume_path, "/sys/boot.bin") ||
         IsFileOfExtractedDisc(volume_path, "/disc/header.bin");
}

// --- Layouts ------------------------------------------------------------------------------------

// The pieces an address range is assembled from. Pieces are kept ordered by where they end, and
// a piece ending where an earlier one ends is dropped. Gaps read as zeros.
class DirBlobLayout
{
public:
  void Add(u64 offset, u64 size, ContentSource source)
  {
    if (size != 0)
      m_pieces.try_emplace(offset + size, Piece{offset, size, std::move(source)});
  }

  void Add(u64 offset, std::vector<u8> bytes)
  {
    const u64 size = bytes.size();
    Add(offset, size, std::make_shared<std::vector<u8>>(std::move(bytes)));
  }

  // Adds (up to max_size bytes of) a host file and returns the size used.
  u64 AddFile(u64 offset, const std::string& path, u64 max_size = UINT64_MAX)
  {
    const u64 size = std::min(File::GetSize(path), max_size);
    Add(offset, size, ContentFile{path, 0});
    return size;
  }

  bool Read(u64 offset, u64 length, u8* out, DirectoryBlobReader::Disc& disc) const;

private:
  struct Piece
  {
    u64 start;
    u64 size;
    ContentSource source;
  };

  static bool ReadPiece(const Piece& piece, u64 offset_in_piece, u64 size, u8* out,
                        DirectoryBlobReader::Disc& disc);

  std::map<u64, Piece> m_pieces;  // keyed by end offset
};

// One partition's decrypted contents (or a GameCube disc).
class DirBlobPartition
{
public:
  DirBlobPartition() = default;
  // From an extracted partition
  DirBlobPartition(std::string root_directory, std::optional<bool> is_wii);
  // From a partition of the volume a reader wraps
  DirBlobPartition(const VolumeDisc& volume, const Partition& partition, std::optional<bool> is_wii,
                   const DirectoryBlobReader::SysCallback& sys_callback,
                   const DirectoryBlobReader::FSTCallback& fst_callback,
                   DirectoryBlobReader::Disc& disc);

  bool is_wii = false;
  u64 data_size = 0;
  std::string root_directory;  // empty for a wrapped partition
  std::optional<Partition> wrapped_partition;
  std::array<u8, VolumeWii::AES_KEY_SIZE> key{};
  DirBlobLayout layout;

private:
  void DetectDiscType(std::optional<bool> wii, const std::vector<u8>& disc_header);
  void PlaceBI2(std::vector<u8> bi2);
  u64 PlaceApploader(std::vector<u8> apploader, const std::string& log_name);
  void BuildFST(std::vector<FSTBuilderNode> nodes, u64 fst_address, std::vector<u8>* disc_header);
  void WriteFolder(std::vector<FSTBuilderNode>& nodes, u32 parent_index, std::vector<u8>& fst,
                   u64 names_start, u32& entry_count, u32& name_offset, u64& data_address);

  bool m_is_triforce = false;
  u32 m_shift = 0;  // addresses in Wii headers and FSTs are stored divided by 4
};
}  // namespace DiscIO

// The state behind a DirectoryBlobReader.
class DiscIO::DirectoryBlobReader::Disc
{
public:
  explicit Disc(DirectoryBlobReader* owner) : m_cache(owner) {}
  Disc(const Disc& other, DirectoryBlobReader* owner);

  void BuildFromDirectory(const std::string& game_root, const std::string& true_root);
  void BuildFromVolume(std::unique_ptr<VolumeDisc> volume, const SysCallback& sys_callback,
                       const FSTCallback& fst_callback);

  bool Read(u64 offset, u64 length, u8* out);
  const DirBlobPartition* FindPartition(u64 offset, u64 size, u64 partition_data_offset) const;
  bool ReadDecrypted(u64 offset, u64 size, u8* out, u64 partition_data_offset);
  u64 DataSize() const { return m_data_size; }

  // Used by the layouts
  bool EncryptPartitionData(u64 offset, u64 size, u8* out, u64 partition_data_offset,
                            u64 decrypted_size);
  const VolumeDisc* Volume() const { return m_volume.get(); }

private:
  struct PendingPartition
  {
    u32 type;
    DirBlobPartition partition;
  };

  void PlaceOuterHeader(const std::vector<u8>& partition_header, std::vector<u8> header_bin);
  void PlaceRegionData(std::span<const u8> region_data, const std::string& log_name);
  void PlacePartitions(std::vector<PendingPartition> partitions);
  void PlacePartitionHeader(DirBlobPartition& partition, u64 partition_address);

  DirBlobPartition m_gamecube;  // the whole disc, for GameCube

  // Wii
  DirBlobLayout m_outer;
  std::map<u64, DirBlobPartition> m_partitions;  // keyed by partition data offset
  WiiEncryptionCache m_cache;

  bool m_is_wii = false;
  bool m_encrypted = false;
  u64 m_data_size = 0;

  std::unique_ptr<VolumeDisc> m_volume;
};

namespace DiscIO
{
// --- DirBlobLayout ------------------------------------------------------------------------------

bool DirBlobLayout::ReadPiece(const Piece& piece, u64 offset_in_piece, u64 size, u8* out,
                              DirectoryBlobReader::Disc& disc)
{
  struct Visitor
  {
    const Piece& piece;
    u64 at;
    u64 size;
    u8* out;
    DirectoryBlobReader::Disc& disc;

    bool operator()(const ContentFile& file) const
    {
      File::DirectIOFile f(file.m_filename, File::AccessMode::Read);
      return f.OffsetRead(file.m_offset + at, out, size);
    }
    bool operator()(const ContentMemory& memory) const
    {
      std::copy_n(memory->begin() + at, size, out);
      return true;
    }
    bool operator()(const ContentPartition& partition) const
    {
      const u64 decrypted_size =
          piece.size * VolumeWii::BLOCK_DATA_SIZE / VolumeWii::BLOCK_TOTAL_SIZE;
      return disc.EncryptPartitionData(partition.m_offset + at, size, out,
                                       partition.m_partition_data_offset, decrypted_size);
    }
    bool operator()(const ContentVolume& volume) const
    {
      return disc.Volume()->Read(volume.m_offset + at, size, out, volume.m_partition);
    }
    bool operator()(const ContentFixedByte& fixed) const
    {
      std::fill_n(out, size, fixed.m_byte);
      return true;
    }
  };
  return std::visit(Visitor{piece, offset_in_piece, size, out, disc}, piece.source);
}

bool DirBlobLayout::Read(u64 offset, u64 length, u8* out, DirectoryBlobReader::Disc& disc) const
{
  for (auto it = m_pieces.upper_bound(offset); it != m_pieces.end() && length != 0; ++it)
  {
    const Piece& piece = it->second;
    if (piece.start > offset)
    {
      const u64 gap = std::min(piece.start - offset, length);
      std::fill_n(out, gap, u8(0));
      out += gap;
      offset += gap;
      length -= gap;
      if (length == 0)
        return true;
    }

    const u64 offset_in_piece = offset - piece.start;
    if (offset_in_piece >= piece.size)
      continue;
    const u64 n = std::min(piece.size - offset_in_piece, length);
    if (!ReadPiece(piece, offset_in_piece, n, out, disc))
      return false;
    out += n;
    offset += n;
    length -= n;
  }
  std::fill_n(out, length, u8(0));
  return true;
}

// --- DirBlobPartition ---------------------------------------------------------------------------

namespace
{
std::vector<FSTBuilderNode> NodesFromHostTree(const File::FSTEntry& folder)
{
  std::vector<FSTBuilderNode> nodes;
  nodes.reserve(folder.children.size());
  for (const File::FSTEntry& entry : folder.children)
  {
    FSTBuilderNode node{entry.virtualName, entry.size, {}};
    if (entry.isDirectory)
      node.m_content = NodesFromHostTree(entry);
    else
      node.m_content =
          std::vector<BuilderContentSource>{{0, entry.size, ContentFile{entry.physicalName, 0}}};
    nodes.push_back(std::move(node));
  }
  return nodes;
}

std::vector<FSTBuilderNode> NodesFromFileSystem(const Partition& partition, const FileInfo& folder)
{
  std::vector<FSTBuilderNode> nodes;
  for (const FileInfo& info : folder)
  {
    if (info.IsDirectory())
    {
      nodes.push_back(FSTBuilderNode{info.GetName(), info.GetTotalChildren(),
                                     NodesFromFileSystem(partition, info)});
    }
    else
    {
      nodes.push_back(FSTBuilderNode{
          info.GetName(), info.GetSize(),
          std::vector<BuilderContentSource>{
              {0, info.GetSize(), ContentVolume{info.GetOffset(), partition}}}});
    }
  }
  return nodes;
}

void SetFileBytes(FSTBuilderNode* node, std::vector<u8> bytes)
{
  node->m_size = bytes.size();
  const u64 size = bytes.size();
  node->m_content = std::vector<BuilderContentSource>{
      {0, size, std::make_shared<std::vector<u8>>(std::move(bytes))}};
}

// The bytes of the file node tagged with `tag`, or nothing if there is no such file.
std::vector<u8> ReadTaggedNode(std::vector<FSTBuilderNode>& nodes, const void* tag,
                               DirectoryBlobReader::Disc& disc)
{
  const auto it = std::ranges::find_if(
      nodes, [tag](const FSTBuilderNode& node) { return node.m_user_data == tag; });
  if (it == nodes.end() || !it->IsFile())
    return {};

  DirBlobLayout layout;
  for (BuilderContentSource& piece : it->GetFileContent())
    layout.Add(piece.m_offset, piece.m_size, std::move(piece.m_source));
  std::vector<u8> bytes(it->m_size);
  layout.Read(0, bytes.size(), bytes.data(), disc);
  return bytes;
}

void ConvertNamesToShiftJIS(std::vector<FSTBuilderNode>& nodes)
{
  for (FSTBuilderNode& node : nodes)
  {
    if (node.IsFolder())
      ConvertNamesToShiftJIS(node.GetFolderContent());
    node.m_filename = UTF8ToSHIFTJIS(node.m_filename);
  }
}

u32 NameTableBytes(const std::vector<FSTBuilderNode>& nodes)
{
  u32 bytes = 0;
  for (const FSTBuilderNode& node : nodes)
  {
    if (node.IsFolder())
      bytes += NameTableBytes(node.GetFolderContent());
    bytes += static_cast<u32>(node.m_filename.size() + 1);
  }
  return bytes;
}

// Sets each folder's size to the number of entries below it; returns the number of entries.
u64 CountEntries(std::vector<FSTBuilderNode>& nodes)
{
  u64 count = 0;
  for (FSTBuilderNode& node : nodes)
  {
    ++count;
    if (node.IsFolder())
    {
      node.m_size = CountEntries(node.GetFolderContent());
      count += node.m_size;
    }
  }
  return count;
}

void WriteFSTEntry(std::vector<u8>& fst, u32 index, u8 type, u32 name_offset, u32 offset_field,
                   u32 size_field)
{
  const size_t at = index * FST_ENTRY_SIZE;
  fst[at] = type;
  fst[at + 1] = u8(name_offset >> 16);
  fst[at + 2] = u8(name_offset >> 8);
  fst[at + 3] = u8(name_offset);
  PutBE32(fst, at + 4, offset_field);
  PutBE32(fst, at + 8, size_field);
}
}  // namespace

DirBlobPartition::DirBlobPartition(std::string root, std::optional<bool> wii)
    : root_directory(std::move(root))
{
  std::vector<u8> disc_header(DISCHEADER_SIZE);
  if (ReadFileStart(root_directory + "sys/boot.bin", &disc_header) < 0x20)
    ERROR_LOG_FMT(DISCIO, "{}sys/boot.bin is missing or too small", root_directory);
  DetectDiscType(wii, disc_header);

  const std::string bi2_path = root_directory + "sys/bi2.bin";
  std::vector<u8> bi2(BI2_SIZE);
  if (!is_wii)
    PutBE32(bi2, 0x18, UNKNOWN_REGION);
  if (ReadFileStart(bi2_path, &bi2) < 0x1C && !is_wii)
    ERROR_LOG_FMT(DISCIO, "No region in {}", bi2_path);
  layout.Add(BI2_ADDRESS, std::move(bi2));

  const std::string apploader_path = root_directory + "sys/apploader.img";
  File::DirectIOFile apploader_file(apploader_path, File::AccessMode::Read);
  std::vector<u8> apploader(apploader_file.GetSize());
  apploader_file.Read(apploader);
  const u64 dol_address = PlaceApploader(std::move(apploader), apploader_path);

  const u64 dol_size = layout.AddFile(dol_address, root_directory + "sys/main.dol");
  PutBE32(disc_header, 0x420, static_cast<u32>(dol_address >> m_shift));
  const u64 fst_address = Common::AlignUp(dol_address + dol_size + 0x20, 0x20);

  BuildFST(NodesFromHostTree(File::ScanDirectoryTree(root_directory + "files/", true)),
           fst_address, &disc_header);
  layout.Add(DISCHEADER_ADDRESS, disc_header);
}

DirBlobPartition::DirBlobPartition(const VolumeDisc& volume, const Partition& partition,
                                   std::optional<bool> wii,
                                   const DirectoryBlobReader::SysCallback& sys_callback,
                                   const DirectoryBlobReader::FSTCallback& fst_callback,
                                   DirectoryBlobReader::Disc& disc)
    : wrapped_partition(partition), m_is_triforce(volume.GetVolumeType() == Platform::Triforce)
{
  // The system files go through sys_callback as file nodes; the tags find them again after.
  const char boot_tag = 0, bi2_tag = 0, apploader_tag = 0;
  auto read_or_empty = [&](u64 offset, u64 size) {
    std::vector<u8> bytes(size);
    if (!volume.Read(offset, size, bytes.data(), partition))
      bytes.clear();
    return bytes;
  };

  std::vector<FSTBuilderNode> sys_nodes(3);
  sys_nodes[0] = FSTBuilderNode{"boot.bin", 0, {}, (void*)&boot_tag};
  SetFileBytes(&sys_nodes[0], read_or_empty(DISCHEADER_ADDRESS, DISCHEADER_SIZE));
  sys_nodes[1] = FSTBuilderNode{"bi2.bin", 0, {}, (void*)&bi2_tag};
  SetFileBytes(&sys_nodes[1], read_or_empty(BI2_ADDRESS, BI2_SIZE));
  sys_nodes[2] = FSTBuilderNode{"apploader.img", 0, {}, (void*)&apploader_tag};
  if (const std::optional<u64> apploader_size = GetApploaderSize(volume, partition))
    SetFileBytes(&sys_nodes[2], read_or_empty(APPLOADER_ADDRESS, *apploader_size));

  if (sys_callback)
    sys_callback(&sys_nodes);

  std::vector<u8> disc_header = ReadTaggedNode(sys_nodes, &boot_tag, disc);
  disc_header.resize(DISCHEADER_SIZE);
  DetectDiscType(wii, disc_header);
  {
    std::vector<u8> bi2 = ReadTaggedNode(sys_nodes, &bi2_tag, disc);
    const size_t bi2_read = bi2.size();
    bi2.resize(BI2_SIZE);
    if (!is_wii && bi2_read < 0x1C)
      PutBE32(bi2, 0x18, UNKNOWN_REGION);
    layout.Add(BI2_ADDRESS, std::move(bi2));
  }
  const u64 dol_address =
      PlaceApploader(ReadTaggedNode(sys_nodes, &apploader_tag, disc), "apploader");

  FSTBuilderNode dol{"main.dol", 0, {}};
  if (const std::optional<u64> dol_offset = GetBootDOLOffset(volume, partition))
  {
    if (const std::optional<u32> dol_size = GetBootDOLSize(volume, partition, *dol_offset))
    {
      dol.m_size = *dol_size;
      dol.m_content = std::vector<BuilderContentSource>{
          {0, *dol_size, ContentVolume{*dol_offset, partition}}};
    }
  }

  std::vector<FSTBuilderNode> nodes;
  const FileSystem* file_system = volume.GetFileSystem(partition);
  if (file_system && file_system->IsValid())
    nodes = NodesFromFileSystem(partition, file_system->GetRoot());

  if (fst_callback)
    fst_callback(&nodes, &dol);

  for (BuilderContentSource& piece : dol.GetFileContent())
    layout.Add(dol_address + piece.m_offset, piece.m_size, std::move(piece.m_source));
  PutBE32(disc_header, 0x420, static_cast<u32>(dol_address >> m_shift));
  const u64 fst_address = Common::AlignUp(dol_address + dol.m_size + 0x20, 0x20);

  BuildFST(std::move(nodes), fst_address, &disc_header);
  layout.Add(DISCHEADER_ADDRESS, disc_header);
}

void DirBlobPartition::DetectDiscType(std::optional<bool> wii, const std::vector<u8>& disc_header)
{
  if (wii)
  {
    is_wii = *wii;
  }
  else
  {
    is_wii = Common::swap32(&disc_header[0x18]) == WII_DISC_MAGIC;
    const bool gamecube = Common::swap32(&disc_header[0x1C]) == GAMECUBE_DISC_MAGIC;
    if (is_wii == gamecube)
    {
      ERROR_LOG_FMT(DISCIO, "The disc header doesn't say whether this is a Wii disc; using {}",
                    is_wii ? "Wii" : "GameCube");
    }
  }
  m_shift = is_wii ? 2 : 0;
}

u64 DirBlobPartition::PlaceApploader(std::vector<u8> apploader, const std::string& log_name)
{
  // An apploader is a 0x20 byte header followed by code of the sizes at 0x14 and 0x18.
  bool valid = false;
  if (apploader.size() < 0x20)
    ERROR_LOG_FMT(DISCIO, "{} is missing or too small", log_name);
  // (The sum is taken modulo 2^32, as it always has been.)
  else if (static_cast<u32>(0x20 + Common::swap32(&apploader[0x14]) +
                            Common::swap32(&apploader[0x18])) != apploader.size())
    ERROR_LOG_FMT(DISCIO, "{} doesn't have the size its header gives", log_name);
  else
    valid = true;

  if (!valid)
  {
    // A header only, with an entry point of -1 so that HLE booting won't run it
    apploader.resize(0x20);
    PutBE32(apploader, 0x10, 0xFFFFFFFF);
  }

  const u64 size = apploader.size();
  layout.Add(APPLOADER_ADDRESS, std::move(apploader));
  return Common::AlignUp(APPLOADER_ADDRESS + size + 0x20, 0x20);
}

void DirBlobPartition::BuildFST(std::vector<FSTBuilderNode> nodes, u64 fst_address,
                                std::vector<u8>* disc_header)
{
  ConvertNamesToShiftJIS(nodes);

  const u32 names_size = Common::AlignUp(NameTableBytes(nodes), size_t(1) << m_shift);
  const u64 entries = CountEntries(nodes) + 1;  // plus the root
  const u64 names_start = entries * FST_ENTRY_SIZE;
  std::vector<u8> fst(names_start + names_size);

  u64 data_address = Common::AlignUp(fst_address + fst.size(), 0x8000);

  WriteFSTEntry(fst, 0, 1, 0, 0, static_cast<u32>(entries));
  u32 entry_count = 1;
  u32 name_offset = 0;
  WriteFolder(nodes, 0, fst, names_start, entry_count, name_offset, data_address);

  const u32 fst_size_field = static_cast<u32>(fst.size() >> m_shift);
  PutBE32(*disc_header, 0x424, static_cast<u32>(fst_address >> m_shift));
  PutBE32(*disc_header, 0x428, fst_size_field);
  PutBE32(*disc_header, 0x42C, fst_size_field);

  layout.Add(fst_address, std::move(fst));
  data_size = data_address;
}

void DirBlobPartition::WriteFolder(std::vector<FSTBuilderNode>& nodes, u32 parent_index,
                                   std::vector<u8>& fst, u64 names_start, u32& entry_count,
                                   u32& name_offset, u64& data_address)
{
  // Case-insensitive order (ties broken by the exact names), so that the result doesn't
  // depend on the order the host lists files in
  std::ranges::sort(nodes, [](const FSTBuilderNode& a, const FSTBuilderNode& b) {
    std::string upper_a = a.m_filename;
    std::string upper_b = b.m_filename;
    Common::ToUpper(&upper_a);
    Common::ToUpper(&upper_b);
    if (upper_a != upper_b)
      return upper_a < upper_b;
    return a.m_filename < b.m_filename;
  });

  auto write_name = [&](const std::string& name) {
    // Copied up to its first NUL; the rest of the space it takes stays zero
    const size_t chars = std::strlen(name.c_str());
    std::copy_n(name.data(), chars, fst.begin() + names_start + name_offset);
    name_offset += static_cast<u32>(name.size() + 1);
  };

  for (FSTBuilderNode& node : nodes)
  {
    const u32 index = entry_count++;
    if (node.IsFolder())
    {
      WriteFSTEntry(fst, index, 1, name_offset, parent_index,
                    static_cast<u32>(index + node.m_size + 1));
      write_name(node.m_filename);
      WriteFolder(node.GetFolderContent(), index, fst, names_start, entry_count, name_offset,
                  data_address);
      continue;
    }

    // Triforce boards map their DIMM memory over 0x1F000000-0x1F800000, so no file may overlap
    // that range.
    if (m_is_triforce && data_address < 0x1F800000 && data_address + node.m_size > 0x1F000000)
      data_address = 0x1F800000;

    WriteFSTEntry(fst, index, 0, name_offset, static_cast<u32>(data_address >> m_shift),
                  static_cast<u32>(node.m_size));
    write_name(node.m_filename);

    for (BuilderContentSource& piece : node.GetFileContent())
      layout.Add(data_address + piece.m_offset, piece.m_size, std::move(piece.m_source));

    // GameCube streamed audio (DTK) files need 32 KiB alignment; Triforce games pack tighter.
    data_address = Common::AlignUp(data_address + node.m_size, m_is_triforce ? 0x20 : 0x8000);
  }
}

}  // namespace DiscIO

// --- DirectoryBlobReader::Disc --------------------------------------------------------------------

namespace DiscIO
{
DirectoryBlobReader::Disc::Disc(const Disc& other, DirectoryBlobReader* owner)
    : m_gamecube(other.m_gamecube), m_outer(other.m_outer), m_partitions(other.m_partitions),
      m_cache(owner), m_is_wii(other.m_is_wii), m_encrypted(other.m_encrypted),
      m_data_size(other.m_data_size)
{
  if (other.m_volume)
    m_volume = CreateDisc(other.m_volume->GetBlobReader().CopyReader());
}

void DirectoryBlobReader::Disc::BuildFromDirectory(const std::string& game_root,
                                                   const std::string& true_root)
{
  DirBlobPartition game(game_root, std::nullopt);
  m_is_wii = game.is_wii;
  if (!m_is_wii)
  {
    m_gamecube = std::move(game);
    m_data_size = m_gamecube.data_size;
    m_encrypted = false;
    return;
  }

  std::vector<u8> partition_header(DISCHEADER_SIZE);
  game.layout.Read(DISCHEADER_ADDRESS, DISCHEADER_SIZE, partition_header.data(), *this);

  std::vector<u8> header_bin(WII_NONPARTITION_DISCHEADER_SIZE);
  header_bin.resize(ReadFileStart(game_root + "disc/header.bin", &header_bin));
  PlaceOuterHeader(partition_header, std::move(header_bin));

  const std::string region_path = game_root + "disc/region.bin";
  std::vector<u8> region_data(WII_REGION_DATA_SIZE);
  region_data.resize(ReadFileStart(region_path, &region_data));
  PlaceRegionData(region_data, region_path);

  std::vector<PendingPartition> partitions;
  partitions.push_back({TYPE_GAME, std::move(game)});

  // The other partitions are picked up only if the game partition's directory is named DATA
  const DirectoryPaths paths{game_root, true_root};
  if (PartitionTypeFromDirectoryName(PartitionDirectoryName(paths)) == TYPE_GAME)
  {
    for (const File::FSTEntry& entry : File::ScanDirectoryTree(true_root, false).children)
    {
      if (!entry.isDirectory)
        continue;
      const std::optional<u32> type = PartitionTypeFromDirectoryName(entry.virtualName);
      if (type && *type != TYPE_GAME)
        partitions.push_back({*type, DirBlobPartition(entry.physicalName + "/", m_is_wii)});
    }
  }

  PlacePartitions(std::move(partitions));
}

void DirectoryBlobReader::Disc::BuildFromVolume(std::unique_ptr<VolumeDisc> volume,
                                                const SysCallback& sys_callback,
                                                const FSTCallback& fst_callback)
{
  m_volume = std::move(volume);
  const VolumeDisc& v = *m_volume;

  DirBlobPartition game(v, v.GetGamePartition(), std::nullopt, sys_callback, fst_callback, *this);
  m_is_wii = game.is_wii;
  if (!m_is_wii)
  {
    m_gamecube = std::move(game);
    m_data_size = m_gamecube.data_size;
    m_encrypted = false;
    return;
  }

  std::vector<u8> header_bin(WII_NONPARTITION_DISCHEADER_SIZE);
  if (!v.Read(WII_NONPARTITION_DISCHEADER_ADDRESS, header_bin.size(), header_bin.data(),
              PARTITION_NONE))
  {
    header_bin.clear();
  }
  std::vector<u8> partition_header(DISCHEADER_SIZE);
  game.layout.Read(DISCHEADER_ADDRESS, DISCHEADER_SIZE, partition_header.data(), *this);
  PlaceOuterHeader(partition_header, std::move(header_bin));

  std::vector<u8> region_data(WII_REGION_DATA_SIZE);
  if (!v.Read(WII_REGION_DATA_ADDRESS, region_data.size(), region_data.data(), PARTITION_NONE))
    region_data.clear();
  PlaceRegionData(region_data, "volume");

  std::vector<PendingPartition> partitions;
  partitions.push_back({TYPE_GAME, std::move(game)});
  for (const Partition& partition : v.GetPartitions())
  {
    if (partition == v.GetGamePartition())
      continue;
    if (const std::optional<u32> type = v.GetPartitionType(partition))
      partitions.push_back({*type, DirBlobPartition(v, partition, m_is_wii, {}, {}, *this)});
  }

  PlacePartitions(std::move(partitions));
}

void DirectoryBlobReader::Disc::PlaceOuterHeader(const std::vector<u8>& partition_header,
                                                 std::vector<u8> header_bin)
{
  // Whatever header.bin doesn't provide is taken from the game partition's boot.bin, which
  // differs only in bytes 0x60 (no hash checks) and 0x61 (no encryption).
  const size_t provided = header_bin.size();
  header_bin.resize(WII_NONPARTITION_DISCHEADER_SIZE);
  if (provided < header_bin.size())
  {
    std::copy(partition_header.begin() + provided, partition_header.begin() + header_bin.size(),
              header_bin.begin() + provided);
  }
  if (provided < 0x60)
    header_bin[0x60] = 0;
  if (provided < 0x61)
    header_bin[0x61] = 0;

  m_encrypted = std::all_of(header_bin.begin() + 0x60, header_bin.begin() + 0x64,
                            [](u8 b) { return b == 0; });
  m_outer.Add(WII_NONPARTITION_DISCHEADER_ADDRESS, std::move(header_bin));
}

void DirectoryBlobReader::Disc::PlaceRegionData(std::span<const u8> region_data,
                                                const std::string& log_name)
{
  // Defaults: unknown region, then 12 bytes of zeros, then age ratings of 0x80 ("no rating")
  std::vector<u8> data(WII_REGION_DATA_SIZE, 0x80);
  std::fill_n(data.begin(), 0x10, u8(0));
  PutBE32(data, 0, UNKNOWN_REGION);
  std::copy_n(region_data.begin(), std::min<size_t>(region_data.size(), data.size()),
              data.begin());

  if (region_data.size() < 4)
    ERROR_LOG_FMT(DISCIO, "No region in {}", log_name);
  else if (region_data.size() < WII_REGION_DATA_SIZE)
    ERROR_LOG_FMT(DISCIO, "No age ratings in {}", log_name);

  m_outer.Add(WII_REGION_DATA_ADDRESS, std::move(data));
}

void DirectoryBlobReader::Disc::PlacePartitions(std::vector<PendingPartition> partitions)
{
  // By type, except that the update partition comes before the game partition; partitions of
  // the same type by directory name
  std::ranges::sort(partitions, [](const PendingPartition& a, const PendingPartition& b) {
    if (a.type == b.type)
      return a.partition.root_directory < b.partition.root_directory;
    if (a.type <= TYPE_UPDATE && b.type <= TYPE_UPDATE)
      return a.type > b.type;
    return a.type < b.type;
  });

  // The first table holds up to three partitions of the standard types, the second the rest.
  size_t first_count = 0;
  while (first_count < partitions.size() && first_count < 3 &&
         partitions[first_count].type <= TYPE_CHANNEL)
  {
    ++first_count;
  }
  const size_t second_count = partitions.size() - first_count;

  constexpr u64 TABLE_ADDRESS = 0x40000;
  constexpr u64 FIRST_TABLE = 0x20;
  constexpr u64 SECOND_TABLE = 0x40;
  std::vector<u8> table(SECOND_TABLE + second_count * 8);
  PutBE32(table, 0x0, static_cast<u32>(first_count));
  PutBE32(table, 0x4, static_cast<u32>((TABLE_ADDRESS + FIRST_TABLE) >> 2));
  if (second_count != 0)
  {
    PutBE32(table, 0x8, static_cast<u32>(second_count));
    PutBE32(table, 0xC, static_cast<u32>((TABLE_ADDRESS + SECOND_TABLE) >> 2));
  }

  u64 address = 0x50000;
  for (size_t i = 0; i < partitions.size(); ++i)
  {
    PendingPartition& pending = partitions[i];
    if (pending.type == TYPE_GAME)
      address = std::max<u64>(address, 0xF800000);

    const u64 entry = i < first_count ? FIRST_TABLE + i * 8 : SECOND_TABLE + (i - first_count) * 8;
    PutBE32(table, entry, static_cast<u32>(address >> 2));
    PutBE32(table, entry + 4, pending.type);

    PlacePartitionHeader(pending.partition, address);

    const u64 data_size = Common::AlignUp(pending.partition.data_size, VolumeWii::BLOCK_DATA_SIZE);
    pending.partition.data_size = data_size;
    const u64 encrypted_size = data_size / VolumeWii::BLOCK_DATA_SIZE * VolumeWii::BLOCK_TOTAL_SIZE;
    const u64 data_offset = address + DATA_OFFSET;
    m_partitions.emplace(data_offset, std::move(pending.partition));
    m_outer.Add(data_offset, encrypted_size, ContentPartition{0, data_offset});

    address = Common::AlignUp(
        VolumeWii::OffsetInHashedPartitionToRawOffset(data_size, Partition(address), DATA_OFFSET),
        0x10000);
  }
  m_data_size = address;

  m_outer.Add(TABLE_ADDRESS, std::move(table));
}

// The part of a Wii partition before its encrypted data: ticket, header, TMD, certificate
// chain and H3 table.
void DirectoryBlobReader::Disc::PlacePartitionHeader(DirBlobPartition& partition, u64 address)
{
  const std::optional<Partition>& wrapped = partition.wrapped_partition;
  const std::string& root = partition.root_directory;

  auto add_bytes = [&](u64 offset, std::vector<u8> bytes, u64 max_size) {
    if (bytes.size() > max_size)
      bytes.resize(max_size);
    const u64 size = bytes.size();
    m_outer.Add(address + offset, std::move(bytes));
    return size;
  };

  const u64 ticket_size =
      wrapped ? add_bytes(WII_PARTITION_TICKET_ADDRESS, m_volume->GetTicket(*wrapped).GetBytes(),
                          WII_PARTITION_TICKET_SIZE) :
                m_outer.AddFile(address + WII_PARTITION_TICKET_ADDRESS, root + "ticket.bin",
                                WII_PARTITION_TICKET_SIZE);

  const u64 tmd_size =
      wrapped ? add_bytes(TMD_OFFSET, m_volume->GetTMD(*wrapped).GetBytes(), IOS::ES::MAX_TMD_SIZE) :
                m_outer.AddFile(address + TMD_OFFSET, root + "tmd.bin", IOS::ES::MAX_TMD_SIZE);

  const u64 cert_offset = Common::AlignUp(TMD_OFFSET + tmd_size, 0x20);
  const u64 cert_max = H3_OFFSET - cert_offset;
  const u64 cert_size =
      wrapped ? add_bytes(cert_offset, m_volume->GetCertificateChain(*wrapped), cert_max) :
                m_outer.AddFile(address + cert_offset, root + "cert.bin", cert_max);

  if (!wrapped)
  {
    m_outer.AddFile(address + H3_OFFSET, root + "h3.bin", WII_PARTITION_H3_SIZE);
  }
  else if (m_volume->HasWiiHashes())
  {
    const std::optional<u64> h3_offset = m_volume->ReadSwappedAndShifted(
        wrapped->offset + WII_PARTITION_H3_OFFSET_ADDRESS, PARTITION_NONE);
    if (h3_offset)
    {
      std::vector<u8> h3(WII_PARTITION_H3_SIZE);
      if (m_volume->Read(wrapped->offset + *h3_offset, h3.size(), h3.data(), PARTITION_NONE))
        m_outer.Add(address + H3_OFFSET, std::move(h3));
    }
  }

  // The header right after the ticket. Sizes and offsets other than the TMD's and the
  // certificate chain's are stored divided by 4.
  const u64 encrypted_size = Common::AlignUp(partition.data_size, 0x7C00) / 0x7C00 * 0x8000;
  std::vector<u8> header(0x1C);
  PutBE32(header, 0x00, static_cast<u32>(tmd_size));
  PutBE32(header, 0x04, static_cast<u32>(TMD_OFFSET >> 2));
  PutBE32(header, 0x08, static_cast<u32>(cert_size));
  PutBE32(header, 0x0C, static_cast<u32>(cert_offset >> 2));
  PutBE32(header, 0x10, static_cast<u32>(H3_OFFSET >> 2));
  PutBE32(header, 0x14, static_cast<u32>(DATA_OFFSET >> 2));
  PutBE32(header, 0x18, static_cast<u32>(encrypted_size >> 2));
  m_outer.Add(address + WII_PARTITION_TICKET_SIZE, std::move(header));

  // The title key comes from the ticket as it ended up on the disc.
  std::vector<u8> ticket(ticket_size);
  m_outer.Read(address + WII_PARTITION_TICKET_ADDRESS, ticket.size(), ticket.data(), *this);
  const IOS::ES::TicketReader ticket_reader(std::move(ticket));
  if (ticket_reader.IsValid())
    partition.key = ticket_reader.GetTitleKey();
}

bool DirectoryBlobReader::Disc::Read(u64 offset, u64 length, u8* out)
{
  if (offset + length > m_data_size)
    return false;
  return (m_is_wii ? m_outer : m_gamecube.layout).Read(offset, length, out, *this);
}

const DirBlobPartition* DirectoryBlobReader::Disc::FindPartition(u64 offset, u64 size,
                                                                 u64 partition_data_offset) const
{
  const auto it = m_partitions.find(partition_data_offset);
  if (it == m_partitions.end() || offset + size > it->second.data_size)
    return nullptr;
  return &it->second;
}

bool DirectoryBlobReader::Disc::ReadDecrypted(u64 offset, u64 size, u8* out,
                                              u64 partition_data_offset)
{
  const DirBlobPartition* partition = FindPartition(offset, size, partition_data_offset);
  return partition && partition->layout.Read(offset, size, out, *this);
}

bool DirectoryBlobReader::Disc::EncryptPartitionData(u64 offset, u64 size, u8* out,
                                                     u64 partition_data_offset, u64 decrypted_size)
{
  const auto it = m_partitions.find(partition_data_offset);
  if (it == m_partitions.end())
    return false;
  if (!m_encrypted)
    return it->second.layout.Read(offset, size, out, *this);
  return m_cache.EncryptGroups(offset, size, out, partition_data_offset, decrypted_size,
                               it->second.key);
}

// --- DirectoryBlobReader --------------------------------------------------------------------------

DirectoryBlobReader::DirectoryBlobReader() : m_disc(std::make_unique<Disc>(this))
{
}

DirectoryBlobReader::~DirectoryBlobReader() = default;

std::unique_ptr<DirectoryBlobReader> DirectoryBlobReader::Create(const std::string& dol_path)
{
  const std::optional<DirectoryPaths> paths = GetDirectoryPaths(dol_path);
  if (!paths)
    return nullptr;
  std::unique_ptr<DirectoryBlobReader> reader(new DirectoryBlobReader());
  reader->m_disc->BuildFromDirectory(paths->partition_root, paths->true_root);
  return reader;
}

std::unique_ptr<DirectoryBlobReader> DirectoryBlobReader::Create(std::unique_ptr<VolumeDisc> volume,
                                                                 const SysCallback& sys_callback,
                                                                 const FSTCallback& fst_callback)
{
  if (!volume)
    return nullptr;
  std::unique_ptr<DirectoryBlobReader> reader(new DirectoryBlobReader());
  reader->m_disc->BuildFromVolume(std::move(volume), sys_callback, fst_callback);
  return reader;
}

bool DirectoryBlobReader::Read(u64 offset, u64 length, u8* buffer)
{
  return m_disc->Read(offset, length, buffer);
}

bool DirectoryBlobReader::SupportsReadWiiDecrypted(u64 offset, u64 size,
                                                   u64 partition_data_offset) const
{
  return m_disc->FindPartition(offset, size, partition_data_offset) != nullptr;
}

bool DirectoryBlobReader::ReadWiiDecrypted(u64 offset, u64 size, u8* buffer,
                                           u64 partition_data_offset)
{
  return m_disc->ReadDecrypted(offset, size, buffer, partition_data_offset);
}

BlobType DirectoryBlobReader::GetBlobType() const
{
  return BlobType::DIRECTORY;
}

std::unique_ptr<BlobReader> DirectoryBlobReader::CopyReader() const
{
  std::unique_ptr<DirectoryBlobReader> copy(new DirectoryBlobReader());
  copy->m_disc = std::make_unique<Disc>(*m_disc, copy.get());
  return copy;
}

u64 DirectoryBlobReader::GetRawSize() const
{
  // An assembled disc has no file of its own.
  return 0;
}

u64 DirectoryBlobReader::GetDataSize() const
{
  return m_disc->DataSize();
}

}  // namespace DiscIO
