// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/TimePlayed.h"

#include <chrono>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/NandPaths.h"

namespace
{
constexpr const char* TIME_PLAYED_FILE = "TimePlayed.ini";
constexpr const char* TIME_PLAYED_SECTION = "TimePlayed";
}  // namespace

TimePlayed::TimePlayed()
{
  m_file_path = File::GetUserPath(D_CONFIG_IDX) + TIME_PLAYED_FILE;
  Reload();
}

TimePlayed::~TimePlayed() = default;

void TimePlayed::Reload()
{
  m_file.Load(m_file_path);
  m_section = m_file.GetOrCreateSection(TIME_PLAYED_SECTION);
}

u64 TimePlayed::StoredMilliseconds(const std::string& key) const
{
  // A missing or unparsable entry counts as zero.
  u64 milliseconds = 0;
  m_section->Get(key, &milliseconds);
  return milliseconds;
}

void TimePlayed::AddTime(const std::string& game_id, std::chrono::milliseconds time_emulated)
{
  // Game IDs can hold characters that are not allowed in INI keys.
  const std::string key = Common::EscapeFileName(game_id);
  const u64 total = StoredMilliseconds(key) + static_cast<u64>(time_emulated.count());
  m_section->Set(key, total);
  m_file.Save(m_file_path);
}

std::chrono::milliseconds TimePlayed::GetTimePlayed(const std::string& game_id) const
{
  const u64 stored = StoredMilliseconds(Common::EscapeFileName(game_id));
  return std::chrono::milliseconds(stored);
}
