// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <vector>

#include "Common/CommonTypes.h"

namespace DiscIO
{
class FileInfo;
class Volume;
struct Partition;

// Works out which 32 KiB clusters of a disc hold nothing the game can reach (padding and junk
// between files), so that converters and caches can skip them.
class DiscScrubber final
{
public:
  DiscScrubber();

  // Walks the disc's headers and file systems. Returns false (and scrubs nothing) when some
  // part of the disc couldn't be understood.
  bool SetupScrub(const Volume& disc);

  // True if the cluster containing `offset` is unused. Always false before a successful
  // SetupScrub.
  bool CanBlockBeScrubbed(u64 offset) const;

  static constexpr size_t CLUSTER_SIZE = 0x8000;

private:
  bool WalkDisc(const Volume& disc);
  bool WalkPartition(const Volume& disc, const Partition& partition);
  void WalkDirectory(u64 data_start, const FileInfo& directory);

  // Marks raw disc bytes [offset, offset + size) as used.
  void UseRaw(u64 offset, u64 size);
  // Marks bytes of a partition's (decrypted) data as used. data_start is the raw offset where
  // the partition's data begins, or 0 for a disc without partitions.
  void UseData(u64 data_start, u64 offset, u64 size);

  std::vector<bool> m_used;
  u64 m_disc_size = 0;
  bool m_hashed = false;  // partition data is stored as 0x7C00 bytes per cluster
  bool m_ready = false;
};

}  // namespace DiscIO
