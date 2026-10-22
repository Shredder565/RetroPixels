// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Core/ActionReplay.h"

namespace ActionReplay
{
// Taken from the first decrypted word of an encrypted code. These are AR's own numbers, not the
// game IDs used elsewhere.
struct GameIDAndRegion
{
  u32 game_id;
  u32 region;
};

// Decrypts "XXXXXXXXXXXXX" lines (dashes already removed) into `result`. Returns nothing when the
// lines are malformed or fail their checksum.
std::optional<GameIDAndRegion> DecryptARCode(std::span<const std::string> encrypted_lines,
                                             std::vector<AREntry>* result);
}  // namespace ActionReplay
