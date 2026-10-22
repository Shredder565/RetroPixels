// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

struct GraphicsModConfig;

// All graphics mods available for one game, together with the user's per-game settings for
// them (enabled flag, weight), which are stored in the game's mod profile file.
class GraphicsModGroupConfig
{
public:
  explicit GraphicsModGroupConfig(std::string game_id);
  ~GraphicsModGroupConfig();

  GraphicsModGroupConfig(const GraphicsModGroupConfig&);
  GraphicsModGroupConfig(GraphicsModGroupConfig&&) noexcept;

  GraphicsModGroupConfig& operator=(const GraphicsModGroupConfig&);
  GraphicsModGroupConfig& operator=(GraphicsModGroupConfig&&) noexcept;

  // Reads the profile, then discovers mods in the user and system mod directories.
  void Load();
  // Writes the profile.
  void Save() const;

  // Bumped by every Load so users can tell the list changed.
  void SetChangeCount(u32 change_count);
  u32 GetChangeCount() const;

  const std::vector<GraphicsModConfig>& GetMods() const;
  std::vector<GraphicsModConfig>& GetMods();

  // The mod whose file is at `absolute_path`, or null.
  GraphicsModConfig* GetMod(std::string_view absolute_path) const;

  const std::string& GetGameID() const;

private:
  std::string ProfilePath() const;

  std::string m_game_id;
  std::vector<GraphicsModConfig> m_mods;
  std::map<std::string, GraphicsModConfig*, std::less<>> m_mods_by_path;
  u32 m_change_count = 0;
};
