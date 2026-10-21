// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace UICommon
{
class GameFile;

// Every file under the given folders whose extension the game list can show.
std::vector<std::string> FindAllGamePaths(std::span<const std::string_view> directories_to_scan,
                                          bool recursive_scan);

// The game list's set of GameFiles, persisted to <cache dir>/gamelist.cache. Entries are shared
// with other threads as const objects, so an entry is never changed in place: an updated copy
// replaces it.
class GameFileCache
{
public:
  enum class DeleteOnDisk
  {
    No = 0,
    Yes = 1,
  };

  using ForEachFn = std::function<void(const std::shared_ptr<const GameFile>&)>;
  using GameAddedToCacheFn = std::function<void(const std::shared_ptr<const GameFile>&)>;
  using GameRemovedFromCacheFn = std::function<void(const std::string&)>;
  using GameUpdatedFn = std::function<void(const std::shared_ptr<const GameFile>&)>;

  GameFileCache();

  void ForEach(const ForEachFn& f) const;

  size_t GetSize() const;
  void Clear(DeleteOnDisk delete_on_disk);

  // The entry for `path`, created if needed; nullptr if the file cannot be listed.
  std::shared_ptr<const GameFile> AddOrGet(const std::string& path, bool* cache_changed);

  // Both return whether the cache contents changed.
  bool Update(std::span<const std::string> all_game_paths,
              const GameAddedToCacheFn& game_added_to_cache = {},
              const GameRemovedFromCacheFn& game_removed_from_cache = {},
              const std::atomic_bool& processing_halted = false);
  bool UpdateAdditionalMetadata(const GameUpdatedFn& game_updated = {},
                                const std::atomic_bool& processing_halted = false);

  bool Load();
  bool Save();

private:
  // Refreshes one entry's optional metadata; replaces it with an updated copy if anything changed.
  static bool RefreshEntry(std::shared_ptr<GameFile>* entry);

  bool WriteFile();
  bool ReadFile();
  void Serialize(PointerWrap* p, u64 total_size);

  std::string m_path;
  std::vector<std::shared_ptr<GameFile>> m_cached_files;
};

}  // namespace UICommon
