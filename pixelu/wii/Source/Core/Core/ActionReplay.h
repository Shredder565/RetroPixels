// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <string>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
class IniFile;
}

namespace Core
{
class CPUThreadGuard;
}

// GameCube Action Replay codes: a list of (command/address, value) word pairs interpreted once per
// frame by a small code engine.
namespace ActionReplay
{
struct AREntry
{
  AREntry() = default;
  AREntry(u32 address_word, u32 value_word) : cmd_addr(address_word), value(value_word) {}

  u32 cmd_addr = 0;
  u32 value = 0;
};

constexpr bool operator==(const AREntry& a, const AREntry& b)
{
  return a.cmd_addr == b.cmd_addr && a.value == b.value;
}

struct ARCode
{
  std::string name;
  std::vector<AREntry> ops;
  bool enabled = false;
  bool default_enabled = false;
  bool user_defined = false;
};

// Runs every active code once (called each frame on the CPU thread).
void RunAllActive(const Core::CPUThreadGuard& cpu_guard);

// Makes the enabled codes of `codes` (that may run for this game) the active set.
void ApplyCodes(std::span<const ARCode> codes, const std::string& game_id, u16 revision);
// NetPlay code sync: the synced set replaces the active set.
void SetSyncedCodesAsActive();
void UpdateSyncedCodes(std::span<const ARCode> codes);
std::vector<ARCode> ApplyAndReturnCodes(std::span<const ARCode> codes);
void AddCode(ARCode new_code);
size_t CountEnabledCodes();
void LoadAndApplyCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini,
                       const std::string& game_id, u16 revision);

// [ActionReplay] ini sections.
std::vector<ARCode> LoadCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini);
void SaveCodes(Common::IniFile* local_ini, std::span<const ARCode> codes);

// One code line: "XXXXXXXX YYYYYYYY" (decrypted) or "XXXX-XXXX-XXXXX" (encrypted, returned with
// the dashes removed).
using EncryptedLine = std::string;
std::variant<std::monostate, AREntry, EncryptedLine> DeserializeLine(const std::string& line);
std::string SerializeLine(const AREntry& op);

// Trace of what the code engine did (shown in the cheats dialog).
void EnableSelfLogging(bool enable);
std::vector<std::string> GetSelfLog();
void ClearSelfLog();
bool IsSelfLogging();
}  // namespace ActionReplay
