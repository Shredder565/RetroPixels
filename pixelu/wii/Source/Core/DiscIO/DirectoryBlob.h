// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// A "directory blob" is a disc image assembled on the fly, either from an extracted disc on the
// host file system (sys/, files/, disc/ and the Wii partition files) or from another disc whose
// file system is rebuilt with changes (used by Riivolution patching). The disc is described as
// a list of pieces, each of which says where its bytes come from.

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeWii.h"
#include "DiscIO/WiiEncryptionCache.h"

namespace DiscIO
{
class VolumeDisc;

// True for paths that lie inside an extracted disc but aren't the file that opens it (its
// sys/main.dol), so that the game list shows each extracted disc once.
bool ShouldHideFromGameList(const std::string& volume_path);

// Bytes taken from a host file, starting at m_offset in it.
struct ContentFile
{
  std::string m_filename;
  u64 m_offset = 0;
};

// Bytes held in memory.
typedef std::shared_ptr<std::vector<u8>> ContentMemory;

// Bytes of an encrypted Wii partition of the directory blob itself; m_offset is the offset in
// the partition's encrypted data and m_partition_data_offset identifies the partition.
struct ContentPartition
{
  u64 m_offset = 0;
  u64 m_partition_data_offset = 0;
};

// Bytes read from the volume a directory blob was built from, at m_offset in m_partition.
struct ContentVolume
{
  u64 m_offset = 0;
  Partition m_partition;
};

// A run of one repeated byte.
struct ContentFixedByte
{
  u8 m_byte = 0;
};

using ContentSource =
    std::variant<ContentFile, ContentMemory, ContentPartition, ContentVolume, ContentFixedByte>;

// One piece of a file being built: m_size bytes at m_offset within the file.
struct BuilderContentSource
{
  u64 m_offset = 0;
  u64 m_size = 0;
  ContentSource m_source;
};

// A file (a list of pieces) or a folder (a list of nodes) of a file system being built. For a
// folder, m_size is recalculated when the file system table is written.
struct FSTBuilderNode
{
  std::string m_filename;
  u64 m_size = 0;
  std::variant<std::vector<BuilderContentSource>, std::vector<FSTBuilderNode>> m_content;
  void* m_user_data = nullptr;

  bool IsFile() const
  {
    return std::holds_alternative<std::vector<BuilderContentSource>>(m_content);
  }
  std::vector<BuilderContentSource>& GetFileContent()
  {
    return std::get<std::vector<BuilderContentSource>>(m_content);
  }
  const std::vector<BuilderContentSource>& GetFileContent() const
  {
    return std::get<std::vector<BuilderContentSource>>(m_content);
  }

  bool IsFolder() const { return std::holds_alternative<std::vector<FSTBuilderNode>>(m_content); }
  std::vector<FSTBuilderNode>& GetFolderContent()
  {
    return std::get<std::vector<FSTBuilderNode>>(m_content);
  }
  const std::vector<FSTBuilderNode>& GetFolderContent() const
  {
    return std::get<std::vector<FSTBuilderNode>>(m_content);
  }
};

class DirectoryBlobReader final : public BlobReader
{
public:
  using SysCallback = std::function<void(std::vector<FSTBuilderNode>* fst_nodes)>;
  using FSTCallback =
      std::function<void(std::vector<FSTBuilderNode>* fst_nodes, FSTBuilderNode* dol_node)>;

  // `dol_path` is the sys/main.dol of an extracted disc (of its game partition, for Wii).
  static std::unique_ptr<DirectoryBlobReader> Create(const std::string& dol_path);
  // Rebuilds `volume`. sys_callback can change the game partition's boot.bin, bi2.bin and
  // apploader.img; fst_callback its file system and main.dol.
  static std::unique_ptr<DirectoryBlobReader> Create(std::unique_ptr<VolumeDisc> volume,
                                                     const SysCallback& sys_callback,
                                                     const FSTCallback& fst_callback);

  ~DirectoryBlobReader() override;

  bool Read(u64 offset, u64 length, u8* buffer) override;
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override;
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* buffer, u64 partition_data_offset) override;

  BlobType GetBlobType() const override;
  std::unique_ptr<BlobReader> CopyReader() const override;

  u64 GetRawSize() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override { return DataSizeType::Accurate; }

  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  // The disc being assembled (defined in DirectoryBlob.cpp).
  class Disc;

private:
  DirectoryBlobReader();

  std::unique_ptr<Disc> m_disc;
};

}  // namespace DiscIO
