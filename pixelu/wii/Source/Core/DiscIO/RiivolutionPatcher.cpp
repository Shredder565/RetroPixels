// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/RiivolutionPatcher.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/AchievementManager.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/RiivolutionParser.h"

namespace DiscIO::Riivolution
{
namespace
{
using ContentList = std::vector<BuilderContentSource>;

constexpr u32 BLR_INSTRUCTION = 0x4e800020;
constexpr u32 BRANCH_OPCODE = 0x48000000;
constexpr u32 BRANCH_OFFSET_MASK = 0x03fffffc;
constexpr u32 CACHED_SEGMENT = 0x80000000;

// Calls fn(element) for each non-empty '/'-separated element; stops early when fn returns false.
template <typename Fn>
bool ForEachPathElement(std::string_view path, Fn fn)
{
  while (!path.empty())
  {
    const size_t slash = path.find('/');
    const std::string_view element = path.substr(0, slash);
    if (!element.empty() && !fn(element))
      return false;
    if (slash == std::string_view::npos)
      break;
    path.remove_prefix(slash + 1);
  }
  return true;
}

// Riivolution runs on a FAT card and ignores case. If 'directory' + 'name' does not exist, look
// for an entry in 'directory' whose name only differs in case.
std::string MatchHostCase(const std::string& directory, std::string_view name)
{
  if (::File::Exists(directory + std::string(name)))
    return std::string(name);
  const ::File::FSTEntry listing = ::File::ScanDirectoryTree(directory, false);
  for (const ::File::FSTEntry& entry : listing.children)
  {
    if (Common::CaseInsensitiveEquals(name, entry.virtualName))
      return entry.virtualName;
  }
  return std::string(name);
}
}  // namespace

FileDataLoader::~FileDataLoader() = default;

FileDataLoaderHostFS::FileDataLoaderHostFS(std::string sd_root, const std::string& xml_path,
                                           std::string_view patch_root)
    : m_sd_root(std::move(sd_root))
{
  // Relative external paths start at the patch root. Without a 'root' attribute that is the XML's
  // folder; a 'root' attribute is itself resolved like an external path (starting at the XML's
  // folder, or at the SD card root when it begins with '/').
  SplitPath(xml_path, &m_patch_root, nullptr, nullptr);
  if (patch_root.empty())
    return;
  if (std::optional<std::string> resolved = MakeAbsoluteFromRelative(patch_root))
    m_patch_root = std::move(*resolved);
}

std::optional<std::string>
FileDataLoaderHostFS::MakeAbsoluteFromRelative(std::string_view external_relative_path) const
{
#ifdef _WIN32
  // A backslash is an ordinary name character for Riivolution but a separator on Windows, so such
  // paths cannot be represented and are refused.
  if (external_relative_path.find('\\') != std::string_view::npos)
    return std::nullopt;
#endif

  std::string result = external_relative_path.starts_with('/') ? m_sd_root : m_patch_root;

  // Length of 'result' before each element was added, so that ".." can undo it. Going above the
  // starting folder is refused.
  std::vector<size_t> undo_lengths;
  const bool ok = ForEachPathElement(external_relative_path, [&](std::string_view element) {
    if (element == ".")
      return true;
    if (element == "..")
    {
      if (undo_lengths.empty())
        return false;
      result.resize(undo_lengths.back());
      undo_lengths.pop_back();
      return true;
    }
    // "...", "...." and so on are plain names to Riivolution but would not work sensibly here.
    if (std::ranges::all_of(element, [](char c) { return c == '.'; }))
      return false;

    undo_lengths.push_back(result.size());
    result += '/';
    result += MatchHostCase(result, element);
    return true;
  });

  if (!ok)
    return std::nullopt;
  return result;
}

std::optional<u64>
FileDataLoaderHostFS::GetExternalFileSize(std::string_view external_relative_path)
{
  const std::optional<std::string> path = MakeAbsoluteFromRelative(external_relative_path);
  if (!path)
    return std::nullopt;
  const ::File::FileInfo info(*path);
  if (!info.IsFile())
    return std::nullopt;
  return info.GetSize();
}

std::vector<u8> FileDataLoaderHostFS::GetFileContents(std::string_view external_relative_path)
{
  const std::optional<std::string> path = MakeAbsoluteFromRelative(external_relative_path);
  // A folder can be opened like a file on some systems but has no meaningful size.
  if (!path || !::File::FileInfo(*path).IsFile())
    return {};
  ::File::IOFile file(*path, "rb");
  if (!file)
    return {};
  std::vector<u8> data(file.GetSize());
  if (!file.ReadBytes(data.data(), data.size()))
    return {};
  return data;
}

std::vector<FileDataLoader::Node>
FileDataLoaderHostFS::GetFolderContents(std::string_view external_relative_path)
{
  const std::optional<std::string> path = MakeAbsoluteFromRelative(external_relative_path);
  if (!path)
    return {};
  ::File::FSTEntry listing = ::File::ScanDirectoryTree(*path, false);
  std::vector<Node> nodes;
  nodes.reserve(listing.children.size());
  for (::File::FSTEntry& entry : listing.children)
    nodes.push_back(Node{std::move(entry.virtualName), entry.isDirectory});
  return nodes;
}

BuilderContentSource
FileDataLoaderHostFS::MakeContentSource(std::string_view external_relative_path,
                                        u64 external_offset, u64 external_size, u64 disc_offset)
{
  std::optional<std::string> path = MakeAbsoluteFromRelative(external_relative_path);
  if (!path)  // an unusable path reads as zeroes
    return BuilderContentSource{disc_offset, external_size, ContentFixedByte{}};
  return BuilderContentSource{disc_offset, external_size,
                              ContentFile{std::move(*path), external_offset}};
}

std::optional<std::string>
FileDataLoaderHostFS::ResolveSavegameRedirectPath(std::string_view external_relative_path)
{
  return MakeAbsoluteFromRelative(external_relative_path);
}

namespace
{
// The part of 'source' from file position 'position' (strictly inside it) to its end.
BuilderContentSource TailOf(const BuilderContentSource& source, u64 position)
{
  const u64 skipped = position - source.m_offset;
  BuilderContentSource tail = source;
  tail.m_offset = position;
  tail.m_size = source.m_size - skipped;

  if (auto* file = std::get_if<ContentFile>(&tail.m_source))
  {
    file->m_offset += skipped;
  }
  else if (auto* memory = std::get_if<ContentMemory>(&tail.m_source))
  {
    const std::vector<u8>& bytes = **memory;
    tail.m_source = std::make_shared<std::vector<u8>>(bytes.begin() + skipped, bytes.end());
  }
  else if (auto* partition = std::get_if<ContentPartition>(&tail.m_source))
  {
    partition->m_offset += skipped;
  }
  else if (auto* volume = std::get_if<ContentVolume>(&tail.m_source))
  {
    volume->m_offset += skipped;
  }
  // ContentFixedByte has no position.
  return tail;
}

// Cuts every source that has 'position' strictly inside it into two.
void CutSourcesAt(ContentList* content, u64 position)
{
  for (size_t i = 0; i < content->size(); ++i)
  {
    BuilderContentSource& source = (*content)[i];
    if (position <= source.m_offset || position >= source.m_offset + source.m_size)
      continue;
    BuilderContentSource tail = TailOf(source, position);
    source.m_size = position - source.m_offset;
    content->insert(content->begin() + i + 1, std::move(tail));
    ++i;
  }
}

// Places 'patch_size' bytes (0 = all available) of an external file, starting at
// 'external_offset', at 'patch_start' in the file node.
void PlaceExternalData(const Patch& patch, FSTBuilderNode* node, std::string_view external,
                       u64 patch_start, u64 external_offset, u64 patch_size, bool resize)
{
  const std::optional<u64> external_total = patch.m_file_data_loader->GetExternalFileSize(external);
  if (!external_total)
    return;

  ContentList& content = node->GetFileContent();

  const u64 read_offset = std::min(external_offset, *external_total);
  const u64 available = *external_total - read_offset;
  const u64 size = patch_size != 0 ? patch_size : available;
  const u64 patch_end = patch_start + size;
  const u64 new_file_size = resize ? patch_end : std::max(node->m_size, patch_end);

  // Index in 'content' where the new sources go.
  size_t insert_at = 0;
  if (patch_start >= node->m_size)
  {
    // Appending: fill any gap after the current end with zeroes.
    if (patch_start > node->m_size)
    {
      content.push_back(
          BuilderContentSource{node->m_size, patch_start - node->m_size, ContentFixedByte{}});
    }
    insert_at = content.size();
  }
  else
  {
    // Overwriting: cut the existing sources at both ends of the patch, then remove the ones that
    // lie within it. The new data goes where the first removed source was.
    CutSourcesAt(&content, patch_start);
    CutSourcesAt(&content, patch_end);

    const auto first = std::ranges::find(content, patch_start, &BuilderContentSource::m_offset);
    if (first != content.end())
    {
      auto last = first;
      while (last != content.end() && patch_end >= last->m_offset + last->m_size)
        ++last;
      insert_at = static_cast<size_t>(first - content.begin());
      content.erase(first, last);
    }
  }

  if (size > 0 && available > 0)
  {
    content.insert(content.begin() + insert_at,
                   patch.m_file_data_loader->MakeContentSource(
                       external, read_offset, std::min(size, available), patch_start));
    ++insert_at;
  }
  if (available < size)
  {
    // The external file is too short for the requested length: the rest is zeroes.
    content.insert(content.begin() + insert_at,
                   BuilderContentSource{patch_start + available, size - available,
                                        ContentFixedByte{}});
  }

  node->m_size = new_file_size;
  // When the file got shorter, sources that now start past its end are dropped.
  while (!content.empty() && content.back().m_offset >= new_file_size)
    content.pop_back();
}

void ApplyFileEntry(const Patch& patch, const File& file, FSTBuilderNode* node)
{
  // Riivolution ignores the lowest two bits of the offset.
  PlaceExternalData(patch, node, file.m_external, file.m_offset & ~u64{3}, file.m_fileoffset,
                    file.m_length, file.m_resize);
}

// Follows an exact disc path (without the leading '/'), comparing names without case. With
// 'create', missing folders and the file itself are added.
FSTBuilderNode* FindByPath(std::string_view path, std::vector<FSTBuilderNode>* folder, bool create)
{
  while (true)
  {
    const size_t slash = path.find('/');
    const bool last = slash == std::string_view::npos;
    const std::string_view name = path.substr(0, slash);

    const auto it = std::ranges::find_if(*folder, [name](const FSTBuilderNode& node) {
      return Common::CaseInsensitiveEquals(node.m_filename, name);
    });

    FSTBuilderNode* node;
    if (it != folder->end())
    {
      if (it->IsFile() != last)
        return nullptr;
      node = &*it;
    }
    else if (!create)
    {
      return nullptr;
    }
    else if (last)
    {
      return &folder->emplace_back(FSTBuilderNode{std::string(name), 0, ContentList{}});
    }
    else
    {
      node = &folder->emplace_back(
          FSTBuilderNode{std::string(name), 0, std::vector<FSTBuilderNode>{}});
    }

    if (last)
      return node;
    folder = &node->GetFolderContent();
    path.remove_prefix(slash + 1);
  }
}

// The first file anywhere in the tree (depth first, in order) with this name, ignoring case.
FSTBuilderNode* FindByName(std::string_view name, std::vector<FSTBuilderNode>* folder)
{
  for (FSTBuilderNode& node : *folder)
  {
    if (node.IsFolder())
    {
      if (FSTBuilderNode* found = FindByName(name, &node.GetFolderContent()))
        return found;
    }
    else if (Common::CaseInsensitiveEquals(node.m_filename, name))
    {
      return &node;
    }
  }
  return nullptr;
}

void ApplyFilePatch(const Patch& patch, const File& file, std::vector<FSTBuilderNode>* fst,
                    FSTBuilderNode* dol_node)
{
  FSTBuilderNode* target;
  if (file.m_disc.starts_with('/'))
    target = FindByPath(std::string_view(file.m_disc).substr(1), fst, file.m_create);
  else if (dol_node && Common::CaseInsensitiveEquals(file.m_disc, "main.dol"))
    target = dol_node;
  else
    target = FindByName(file.m_disc, fst);

  if (target)
    ApplyFileEntry(patch, file, target);
}

std::string JoinPath(std::string_view a, std::string_view b)
{
  if (a.empty())
    return std::string(b);
  if (b.empty())
    return std::string(a);
  if (a.ends_with('/'))
    a.remove_suffix(1);
  if (b.starts_with('/'))
    b.remove_prefix(1);
  return fmt::format("{}/{}", a, b);
}

void ApplyFolderPatch(const Patch& patch, const Folder& folder, std::vector<FSTBuilderNode>* fst,
                      FSTBuilderNode* dol_node, std::string_view disc_path,
                      std::string_view external_path)
{
  for (const FileDataLoader::Node& entry :
       patch.m_file_data_loader->GetFolderContents(external_path))
  {
    std::string entry_disc = JoinPath(disc_path, entry.m_filename);
    std::string entry_external = JoinPath(external_path, entry.m_filename);
    if (entry.m_is_directory)
    {
      if (folder.m_recursive)
        ApplyFolderPatch(patch, folder, fst, dol_node, entry_disc, entry_external);
      continue;
    }

    File file;
    file.m_disc = std::move(entry_disc);
    file.m_external = std::move(entry_external);
    file.m_resize = folder.m_resize;
    file.m_create = folder.m_create;
    file.m_length = folder.m_length;
    ApplyFilePatch(patch, file, fst, dol_node);
  }
}

bool RamHolds(const Core::CPUThreadGuard& guard, u32 address, std::span<const u8> expected)
{
  for (size_t i = 0; i < expected.size(); ++i)
  {
    const auto byte = PowerPC::MMU::HostTryRead<u8>(guard, address + static_cast<u32>(i));
    if (!byte || byte->value != expected[i])
      return false;
  }
  return true;
}

void WriteRam(const Core::CPUThreadGuard& guard, u32 address, std::span<const u8> bytes,
              std::span<const u8> expected)
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    return;
  if (bytes.empty())
    return;
  if (!expected.empty() && !RamHolds(guard, address, expected))
    return;

  const u32 length = static_cast<u32>(bytes.size());
  for (u32 i = 0; i < length; ++i)
    PowerPC::MMU::HostTryWrite<u8>(guard, bytes[i], address + i);

  // HLE hooks inside the patched range would no longer match the code there.
  const u32 removed = HLE::UnpatchRange(guard.GetSystem(), address, address + length);
  if (removed != 0)
  {
    WARN_LOG_FMT(OSHLE, "Riivolution memory patch overlaps {} HLE hook(s) at {:08x} (size: {})",
                 removed, address, bytes.size());
  }
}

std::vector<u8> PatchBytes(const Patch& patch, const Memory& memory)
{
  if (memory.m_valuefile.empty())
    return memory.m_value;
  return patch.m_file_data_loader->GetFileContents(memory.m_valuefile);
}

void ApplyFixedMemoryPatch(const Core::CPUThreadGuard& guard, const Patch& patch,
                           const Memory& memory)
{
  if (memory.m_offset == 0)
    return;
  WriteRam(guard, memory.m_offset | CACHED_SEGMENT, PatchBytes(patch, memory), memory.m_original);
}

void ApplySearchingMemoryPatch(const Core::CPUThreadGuard& guard, const Patch& patch,
                               const Memory& memory, u32 start, u32 length)
{
  if (memory.m_original.empty() || memory.m_align == 0)
    return;

  // Candidate positions are multiples of the alignment whose aligned unit fits in the range.
  const u64 step = memory.m_align;
  for (u64 position = 0; position + step <= length; position += step)
  {
    const u32 address = start + static_cast<u32>(position);
    if (RamHolds(guard, address, memory.m_original))
    {
      WriteRam(guard, address, PatchBytes(patch, memory), {});
      return;
    }
  }
}

void ApplyOcarinaMemoryPatch(const Core::CPUThreadGuard& guard, const Patch& patch,
                             const Memory& memory, u32 start, u32 length)
{
  if (memory.m_offset == 0)
    return;
  const std::vector<u8> pattern = PatchBytes(patch, memory);
  if (pattern.empty())
    return;

  // Find the first word-aligned occurrence of the pattern...
  u32 position = 0;
  while (position < length && !RamHolds(guard, start + position, pattern))
    position += 4;
  if (position >= length)
    return;

  // ...then the first blr from there on, which becomes a branch to the patch's address.
  for (; position < length; position += 4)
  {
    const u32 address = start + position;
    const auto word = PowerPC::MMU::HostTryRead<u32>(guard, address);
    if (!word || word->value != BLR_INSTRUCTION)
      continue;

    const u32 target = memory.m_offset | CACHED_SEGMENT;
    PowerPC::MMU::HostTryWrite<u32>(
        guard, ((target - address) & BRANCH_OFFSET_MASK) | BRANCH_OPCODE, address);
    if (HLE::UnpatchRange(guard.GetSystem(), address, address + 4) != 0)
      WARN_LOG_FMT(OSHLE, "Riivolution ocarina patch overlaps HLE hook at {}", address);
    return;
  }
}
}  // namespace

void ApplyPatchesToFiles(std::span<const Patch> patches, PatchIndex index,
                         std::vector<FSTBuilderNode>* fst, FSTBuilderNode* dol_node)
{
  const bool system_files = index == PatchIndex::DolphinSysFiles;
  for (const Patch& patch : patches)
  {
    for (const File& file : system_files ? patch.m_sys_file_patches : patch.m_file_patches)
      ApplyFilePatch(patch, file, fst, dol_node);
    for (const Folder& folder : system_files ? patch.m_sys_folder_patches : patch.m_folder_patches)
      ApplyFolderPatch(patch, folder, fst, dol_node, folder.m_disc, folder.m_external);
  }
}

void ApplyGeneralMemoryPatches(const Core::CPUThreadGuard& guard, std::span<const Patch> patches)
{
  const u32 ram_size = guard.GetSystem().GetMemory().GetRamSize();
  for (const Patch& patch : patches)
  {
    for (const Memory& memory : patch.m_memory_patches)
    {
      if (memory.m_ocarina)
        continue;  // applied once the apploader has loaded the code
      if (memory.m_search)
        ApplySearchingMemoryPatch(guard, patch, memory, CACHED_SEGMENT, ram_size);
      else
        ApplyFixedMemoryPatch(guard, patch, memory);
    }
  }
}

void ApplyApploaderMemoryPatches(const Core::CPUThreadGuard& guard, std::span<const Patch> patches,
                                 u32 ram_address, u32 ram_length)
{
  for (const Patch& patch : patches)
  {
    for (const Memory& memory : patch.m_memory_patches)
    {
      if (memory.m_ocarina)
        ApplyOcarinaMemoryPatch(guard, patch, memory, ram_address, ram_length);
      else if (memory.m_search)
        ApplySearchingMemoryPatch(guard, patch, memory, ram_address, ram_length);
    }
  }
}

std::optional<SavegameRedirect> ExtractSavegameRedirect(std::span<const Patch> riivolution_patches)
{
  // Only the first savegame entry of the first patch that has one counts.
  const auto patch = std::ranges::find_if(
      riivolution_patches, [](const Patch& p) { return !p.m_savegame_patches.empty(); });
  if (patch == riivolution_patches.end())
    return std::nullopt;

  const Savegame& savegame = patch->m_savegame_patches.front();
  std::optional<std::string> path =
      patch->m_file_data_loader->ResolveSavegameRedirectPath(savegame.m_external);
  if (!path)
    return std::nullopt;
  return SavegameRedirect{std::move(*path), savegame.m_clone};
}
}  // namespace DiscIO::Riivolution
