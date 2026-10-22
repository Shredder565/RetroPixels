// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IniFile.h"

// Per-game play time, kept in User/Config/TimePlayed.ini under [TimePlayed]
// as "<escaped game id> = <milliseconds>".
class TimePlayed
{
public:
  TimePlayed();
  ~TimePlayed();

  // Holds a pointer into its own IniFile, so it can be neither copied nor moved.
  TimePlayed(const TimePlayed& other) = delete;
  TimePlayed(TimePlayed&& other) = delete;
  TimePlayed& operator=(const TimePlayed& other) = delete;
  TimePlayed& operator=(TimePlayed&& other) = delete;

  // Adds to the game's total and writes the file straight away.
  void AddTime(const std::string& game_id, std::chrono::milliseconds time_emulated);

  std::chrono::milliseconds GetTimePlayed(const std::string& game_id) const;

  // Re-reads the file from disk.
  void Reload();

private:
  u64 StoredMilliseconds(const std::string& key) const;

  std::string m_file_path;
  Common::IniFile m_file;
  Common::IniFile::Section* m_section = nullptr;
};
