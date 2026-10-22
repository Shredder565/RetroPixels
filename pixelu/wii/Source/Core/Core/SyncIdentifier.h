// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"

namespace NetPlay
{
// What NetPlay and movies use to decide whether two players (or a movie and
// the running game) have the same game. Two identifiers are equal only when
// every field matches.
struct SyncIdentifier
{
  u64 dol_elf_size = 0;
  std::string game_id;
  u16 revision = 0;
  u8 disc_number = 0;
  bool is_datel = false;

  // A cheap fingerprint of the parts of the game that matter for staying in
  // sync. It is not a hash of the whole image, and how it is computed may
  // change between versions.
  std::array<u8, 20> sync_hash{};

  bool operator==(const SyncIdentifier&) const = default;
};

// Ordered from best to worst match: callers pick the smallest value.
enum class SyncIdentifierComparison : u8
{
  SameGame,
  DifferentHash,
  DifferentDiscNumber,
  DifferentRevision,
  DifferentRegion,
  DifferentGame,
  Unknown,
};
}  // namespace NetPlay
