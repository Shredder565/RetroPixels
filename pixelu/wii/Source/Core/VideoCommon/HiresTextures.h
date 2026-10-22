// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <fmt/ranges.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "VideoCommon/TextureInfo.h"

namespace VideoCommon
{
class TextureDataResource;
}

enum class TextureFormat;

// Custom texture packs live in Load/Textures/. A pack applies to a game when it is in a folder
// named after the game id (or its first three characters), or when any folder below the root
// holds a "<game id>.txt", "<first three characters>.txt" or "all.txt" marker file; the marker
// selects the top-level folder that contains it.
std::set<std::string> GetTextureDirectoriesWithGameId(const std::string& root_directory,
                                                      const std::string& game_id);

// Tries each id in priority order, returning the directories for the first id that matches
// anything. Returns an empty set if none of the ids match.
std::set<std::string>
GetTextureDirectoriesForFirstMatchingGameId(const std::string& root_directory,
                                            const std::vector<std::string>& game_ids);

// A replacement texture from a pack, identified by its file name without extension (and
// without any "_arb" marker, which flags hand-made mip levels).
class HiresTexture
{
public:
  // Rescans the packs for the current game (or forgets them if custom textures are off).
  static void Update();
  static void Clear();
  static void Shutdown();
  // The replacement for a game texture, or null if no pack provides one.
  static std::shared_ptr<HiresTexture> Search(const TextureInfo& texture_info);

  HiresTexture(bool has_arbitrary_mipmaps, std::string id);

  bool HasArbitraryMipmaps() const { return m_has_arbitrary_mipmaps; }
  VideoCommon::TextureDataResource* LoadTexture() const;
  const std::string& GetId() const { return m_id; }

private:
  bool m_has_arbitrary_mipmaps = false;
  std::string m_id;
};
