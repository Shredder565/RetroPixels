// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/GameFileCache.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "DiscIO/DirectoryBlob.h"

#include "UICommon/GameFile.h"

namespace UICommon
{
namespace
{
// Bump whenever GameFile::DoState changes what it stores.
constexpr u32 CACHE_REVISION = 27;

// The cache file starts with this, followed by the GameFile entries.
struct CacheHeader
{
  u32 revision;
  u32 unused;  // Alignment gap before the u64; always written as zero.
  u64 file_size;
};
static_assert(sizeof(CacheHeader) == 16);

constexpr auto GAME_EXTENSIONS =
    std::to_array<std::string_view>({".gcm", ".tgc", ".bin", ".iso", ".ciso", ".gcz", ".wbfs",
                                     ".wia", ".rvz", ".nfs", ".wad", ".dol", ".elf", ".json"});
}  // namespace

std::vector<std::string> FindAllGamePaths(std::span<const std::string_view> directories_to_scan,
                                          bool recursive_scan)
{
  return Common::DoFileSearch(directories_to_scan, GAME_EXTENSIONS, recursive_scan);
}

GameFileCache::GameFileCache() : m_path(File::GetUserPath(D_CACHE_IDX) + "gamelist.cache")
{
}

void GameFileCache::ForEach(const ForEachFn& f) const
{
  for (const std::shared_ptr<GameFile>& entry : m_cached_files)
    f(entry);
}

size_t GameFileCache::GetSize() const
{
  return m_cached_files.size();
}

void GameFileCache::Clear(DeleteOnDisk delete_on_disk)
{
  if (delete_on_disk != DeleteOnDisk::No)
    File::Delete(m_path);
  m_cached_files.clear();
}

std::shared_ptr<const GameFile> GameFileCache::AddOrGet(const std::string& path,
                                                        bool* cache_changed)
{
  const auto existing = std::ranges::find_if(
      m_cached_files, [&path](const auto& entry) { return entry->GetFilePath() == path; });

  std::shared_ptr<GameFile>* slot = nullptr;
  bool added = false;
  if (existing != m_cached_files.end())
  {
    slot = &*existing;
  }
  else
  {
    auto created = std::make_shared<GameFile>(path);
    if (!created->IsValid())
      return nullptr;
    slot = &m_cached_files.emplace_back(std::move(created));
    added = true;
  }

  // The metadata refresh runs for found and new entries alike.
  const bool refreshed = RefreshEntry(slot);
  if (refreshed || added)
    *cache_changed = true;
  return *slot;
}

bool GameFileCache::Update(std::span<const std::string> all_game_paths,
                           const GameAddedToCacheFn& game_added_to_cache,
                           const GameRemovedFromCacheFn& game_removed_from_cache,
                           const std::atomic_bool& processing_halted)
{
  // The paths still to be matched against cached entries. Files inside extracted-disc folders
  // are not listed on their own.
  std::unordered_set<std::string> unmatched;
  unmatched.reserve(all_game_paths.size());
  for (const std::string& path : all_game_paths)
  {
    if (!DiscIO::ShouldHideFromGameList(path))
      unmatched.insert(path);
  }

  bool changed = false;

  // Pass 1: keep entries whose path is still present, ticking it off. A vanished entry is
  // replaced by the last live entry (order is not preserved) and the tail is dropped at the end.
  // If halted, everything from the current position on is dropped as well.
  std::size_t pos = 0;
  std::size_t live = m_cached_files.size();
  while (pos < live && !processing_halted)
  {
    const std::string& path = m_cached_files[pos]->GetFilePath();
    if (unmatched.erase(path) != 0)
    {
      ++pos;
      continue;
    }

    if (game_removed_from_cache)
      game_removed_from_cache(path);
    changed = true;
    --live;
    m_cached_files[pos] = std::move(m_cached_files[live]);
  }
  m_cached_files.resize(pos);

  // Pass 2: whatever is left is new.
  for (const std::string& path : unmatched)
  {
    if (processing_halted)
      break;

    auto entry = std::make_shared<GameFile>(path);
    if (!entry->IsValid())
      continue;

    if (game_added_to_cache)
      game_added_to_cache(entry);
    changed = true;
    m_cached_files.push_back(std::move(entry));
  }

  return changed;
}

bool GameFileCache::UpdateAdditionalMetadata(const GameUpdatedFn& game_updated,
                                             const std::atomic_bool& processing_halted)
{
  bool changed = false;
  for (std::shared_ptr<GameFile>& entry : m_cached_files)
  {
    if (processing_halted)
      break;

    if (!RefreshEntry(&entry))
      continue;

    changed = true;
    if (game_updated)
      game_updated(entry);
  }
  return changed;
}

bool GameFileCache::RefreshEntry(std::shared_ptr<GameFile>* entry)
{
  GameFile& current = **entry;

  // Every check runs (and the cover download is attempted) before anything is committed.
  const bool xml = current.XMLMetadataChanged();
  const bool wii_banner = current.WiiBannerChanged();
  const bool custom_banner = current.CustomBannerChanged();
  current.DownloadDefaultCover();
  const bool default_cover = current.DefaultCoverChanged();
  const bool custom_cover = current.CustomCoverChanged();

  if (!(xml || wii_banner || custom_banner || default_cover || custom_cover))
    return false;

  // Commit into a copy, then swap the copy in; readers holding the old entry are unaffected.
  auto updated = std::make_shared<GameFile>(current);
  if (xml)
    updated->XMLMetadataCommit();
  if (wii_banner)
    updated->WiiBannerCommit();
  if (custom_banner)
    updated->CustomBannerCommit();
  if (default_cover)
    updated->DefaultCoverCommit();
  if (custom_cover)
    updated->CustomCoverCommit();

  *entry = std::move(updated);
  return true;
}

bool GameFileCache::Load()
{
  return ReadFile();
}

bool GameFileCache::Save()
{
  return WriteFile();
}

bool GameFileCache::WriteFile()
{
  File::IOFile file(m_path, "wb");
  if (!file)
    return false;

  // Size the image with a measuring pass, then serialise into it.
  u8* cursor = nullptr;
  PointerWrap measure(&cursor, 0, PointerWrap::Mode::Measure);
  Serialize(&measure, 0);
  const std::size_t total = reinterpret_cast<std::size_t>(cursor);

  std::vector<u8> image(total);
  cursor = image.data();
  PointerWrap writer(&cursor, total, PointerWrap::Mode::Write);
  Serialize(&writer, total);

  if (file.WriteBytes(image.data(), image.size()))
    return true;

  // A partial file would only fail to load later; remove it now.
  file.Close();
  File::Delete(m_path);
  return false;
}

bool GameFileCache::ReadFile()
{
  File::IOFile file(m_path, "rb");
  if (!file)
    return false;

  std::vector<u8> image(file.GetSize());
  bool loaded = false;
  if (!image.empty() && file.ReadBytes(image.data(), image.size()))
  {
    u8* cursor = image.data();
    PointerWrap reader(&cursor, image.size(), PointerWrap::Mode::Read);
    Serialize(&reader, image.size());
    // Serialize leaves read mode on a header mismatch, and PointerWrap does on a short read.
    loaded = reader.IsReadMode();
  }

  if (!loaded)
  {
    // Unreadable or stale: delete it so the next save starts clean.
    file.Close();
    File::Delete(m_path);
  }
  return loaded;
}

void GameFileCache::Serialize(PointerWrap* p, u64 total_size)
{
  CacheHeader header{CACHE_REVISION, 0, total_size};
  p->Do(header);

  if (p->IsReadMode() && (header.revision != CACHE_REVISION || header.file_size != total_size))
  {
    p->SetMeasureMode();
    return;
  }

  p->DoEachElement(m_cached_files, [](PointerWrap& state, std::shared_ptr<GameFile>& entry) {
    if (state.IsReadMode())
      entry = std::make_shared<GameFile>();
    entry->DoState(state);
  });
}

}  // namespace UICommon
