// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Core
{
class CPUThreadGuard;
}

// Gecko codes are run by the Gecko code handler (Sys/codehandler.bin), a PowerPC program copied
// into low game RAM together with a table of the active codes (the "GCT").
namespace Gecko
{
class GeckoCode
{
public:
  GeckoCode() = default;

  struct Code
  {
    u32 address = 0;
    u32 data = 0;
    // The line as written in the ini, saved back unchanged.
    std::string original_line;
  };

  std::vector<Code> codes;
  std::string name, creator;
  std::vector<std::string> notes;

  bool enabled = false;
  bool default_enabled = false;
  bool user_defined = false;
};

// Codes are equal when their code lines are (names and notes don't matter).
bool operator==(const GeckoCode& lhs, const GeckoCode& rhs);
bool operator==(const GeckoCode::Code& lhs, const GeckoCode::Code& rhs);

// Where the code handler and its code table live in game RAM.
constexpr u32 INSTALLER_BASE_ADDRESS = 0x80001800;
constexpr u32 INSTALLER_END_ADDRESS = 0x80003000;
constexpr u32 ENTRY_POINT = INSTALLER_BASE_ADDRESS + 0xA8;
// The last word of the area. The handler returns here, where HLE takes over (with a full-size table
// it is the second word of the 0xF0000000 end code).
constexpr u32 HLE_TRAMPOLINE_ADDRESS = INSTALLER_END_ADDRESS - 4;

// Written to the handler's (unused) game ID word when it is installed. The HLE hook for the
// handler counts it up for a few frames and resets the instruction cache meanwhile, because the
// handler patches game code without invalidating the cache (see HLE_Misc).
constexpr u32 MAGIC_GAMEID = 0xD01F1BAD;

size_t CountEnabledCodes();
void SetActiveCodes(std::span<const GeckoCode> gcodes, const std::string& game_id, u16 revision);
void SetSyncedCodesAsActive();
void UpdateSyncedCodes(std::span<const GeckoCode> gcodes);
std::vector<GeckoCode> SetAndReturnActiveCodes(std::span<const GeckoCode> gcodes);
// Installs the handler if needed and makes the CPU call it (once per frame).
void RunCodeHandler(const Core::CPUThreadGuard& guard);
void Shutdown();
void DoState(PointerWrap&);

}  // namespace Gecko
