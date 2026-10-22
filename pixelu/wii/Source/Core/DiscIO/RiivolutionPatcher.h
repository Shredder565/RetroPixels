// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/RiivolutionParser.h"

namespace Core
{
class CPUThreadGuard;
}

// Applies parsed Riivolution patches: file and folder patches to a DirectoryBlob file tree,
// memory patches to emulated RAM, and savegame redirects to the NAND setup.
namespace DiscIO::Riivolution
{
struct SavegameRedirect
{
  std::string m_target_path;
  bool m_clone;
};

// Supplies the external (SD card) data that a patch refers to. Paths are as written in the XML.
class FileDataLoader
{
public:
  struct Node
  {
    std::string m_filename;
    bool m_is_directory;
  };

  virtual ~FileDataLoader();
  virtual std::optional<u64> GetExternalFileSize(std::string_view external_relative_path) = 0;
  virtual std::vector<u8> GetFileContents(std::string_view external_relative_path) = 0;
  virtual std::vector<Node> GetFolderContents(std::string_view external_relative_path) = 0;
  virtual BuilderContentSource MakeContentSource(std::string_view external_relative_path,
                                                 u64 external_offset, u64 external_size,
                                                 u64 disc_offset) = 0;
  virtual std::optional<std::string>
  ResolveSavegameRedirectPath(std::string_view external_relative_path) = 0;
};

// Loads external data from a host folder that plays the part of the SD card.
class FileDataLoaderHostFS final : public FileDataLoader
{
public:
  // sd_root: absolute host path of the SD card folder.
  // xml_path: absolute host path of the patch XML.
  // patch_root: the 'root' attribute of the patch (may be empty).
  FileDataLoaderHostFS(std::string sd_root, const std::string& xml_path,
                       std::string_view patch_root);

  std::optional<u64> GetExternalFileSize(std::string_view external_relative_path) override;
  std::vector<u8> GetFileContents(std::string_view external_relative_path) override;
  std::vector<FileDataLoader::Node>
  GetFolderContents(std::string_view external_relative_path) override;
  BuilderContentSource MakeContentSource(std::string_view external_relative_path,
                                         u64 external_offset, u64 external_size,
                                         u64 disc_offset) override;
  std::optional<std::string>
  ResolveSavegameRedirectPath(std::string_view external_relative_path) override;

private:
  // Turns an XML path into a host path, or nullopt if it is not allowed.
  std::optional<std::string>
  MakeAbsoluteFromRelative(std::string_view external_relative_path) const;

  std::string m_sd_root;
  std::string m_patch_root;
};

// Which file patches to apply: the ones for the game's file system, or the dolphin_sys_* ones for
// the partition's system files.
enum class PatchIndex
{
  FileSystem,
  DolphinSysFiles,
};

void ApplyPatchesToFiles(std::span<const Patch> patches, PatchIndex index,
                         std::vector<FSTBuilderNode>* fst, FSTBuilderNode* dol_node);
void ApplyGeneralMemoryPatches(const Core::CPUThreadGuard& guard, std::span<const Patch> patches);
void ApplyApploaderMemoryPatches(const Core::CPUThreadGuard& guard, std::span<const Patch> patches,
                                 u32 ram_address, u32 ram_length);
std::optional<SavegameRedirect> ExtractSavegameRedirect(std::span<const Patch> riivolution_patches);
}  // namespace DiscIO::Riivolution
