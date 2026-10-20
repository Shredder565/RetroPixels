// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/DiscScrubber.h"

#include <cstddef>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
namespace
{
// Bytes of payload per cluster in hashed Wii partition data (the rest is the hash block).
constexpr u64 HASHED_PAYLOAD = 0x7C00;

// Everything below the first partition (disc header, partition tables, region data...).
constexpr u64 WII_HEADER_AREA = 0x50000;
// Partition header: ticket and the size/offset fields that follow it.
constexpr u64 PARTITION_HEADER_SIZE = 0x2C0;
constexpr u64 PARTITION_DATA_OFFSET_ADDRESS = 0x2B8;

// Within the boot area of a partition (or a GameCube disc).
constexpr u64 APPLOADER_START = 0x2440;
constexpr u64 APPLOADER_SIZE_ADDRESS = APPLOADER_START + 0x14;
constexpr u64 APPLOADER_TRAILER_SIZE_ADDRESS = APPLOADER_START + 0x18;
}  // namespace

DiscScrubber::DiscScrubber() = default;

bool DiscScrubber::SetupScrub(const Volume& disc)
{
  m_disc_size = disc.GetDataSize();
  m_hashed = disc.HasWiiHashes();
  m_used.assign((m_disc_size + CLUSTER_SIZE - 1) / CLUSTER_SIZE, false);

  m_ready = WalkDisc(disc);
  return m_ready;
}

bool DiscScrubber::CanBlockBeScrubbed(u64 offset) const
{
  if (!m_ready)
    return false;
  const u64 cluster = offset / CLUSTER_SIZE;
  return cluster >= m_used.size() || !m_used[cluster];
}

void DiscScrubber::UseRaw(u64 offset, u64 size)
{
  const u64 end = offset + size;
  for (u64 pos = offset - offset % CLUSTER_SIZE; pos < end && pos < m_disc_size;
       pos += CLUSTER_SIZE)
  {
    m_used[pos / CLUSTER_SIZE] = true;
  }
}

void DiscScrubber::UseData(u64 data_start, u64 offset, u64 size)
{
  if (data_start == 0)
  {
    UseRaw(offset, size);
    return;
  }

  // Data offset -> raw offset of the cluster that holds it.
  const auto cluster_start = [this](u64 data_offset) {
    return m_hashed ? data_offset / HASHED_PAYLOAD * CLUSTER_SIZE :
                      data_offset - data_offset % CLUSTER_SIZE;
  };

  const u64 first = data_start + cluster_start(offset);
  const u64 end = size == 0 ? first : data_start + cluster_start(offset + size - 1) + CLUSTER_SIZE;
  UseRaw(first, end - first);
}

bool DiscScrubber::WalkDisc(const Volume& disc)
{
  const std::vector<Partition> partitions = disc.GetPartitions();
  if (partitions.empty())
    return WalkPartition(disc, PARTITION_NONE);

  UseRaw(0, WII_HEADER_AREA);

  for (const Partition& partition : partitions)
  {
    const u64 base = partition.offset;
    const std::optional<u32> tmd_size =
        disc.ReadSwapped<u32>(base + WII_PARTITION_TMD_SIZE_ADDRESS, PARTITION_NONE);
    const std::optional<u64> tmd_offset =
        disc.ReadSwappedAndShifted(base + WII_PARTITION_TMD_OFFSET_ADDRESS, PARTITION_NONE);
    const std::optional<u32> cert_size =
        disc.ReadSwapped<u32>(base + WII_PARTITION_CERT_CHAIN_SIZE_ADDRESS, PARTITION_NONE);
    const std::optional<u64> cert_offset =
        disc.ReadSwappedAndShifted(base + WII_PARTITION_CERT_CHAIN_OFFSET_ADDRESS, PARTITION_NONE);
    const std::optional<u64> h3_offset =
        disc.ReadSwappedAndShifted(base + WII_PARTITION_H3_OFFSET_ADDRESS, PARTITION_NONE);
    if (!tmd_size || !tmd_offset || !cert_size || !cert_offset || !h3_offset)
      return false;

    UseRaw(base, PARTITION_HEADER_SIZE);
    UseRaw(base + *tmd_offset, *tmd_size);
    UseRaw(base + *cert_offset, *cert_size);
    UseRaw(base + *h3_offset, WII_PARTITION_H3_SIZE);

    if (!WalkPartition(disc, partition))
      return false;
  }
  return true;
}

bool DiscScrubber::WalkPartition(const Volume& disc, const Partition& partition)
{
  const FileSystem* file_system = disc.GetFileSystem(partition);
  if (!file_system)
  {
    ERROR_LOG_FMT(DISCIO, "Scrubber: no file system in the partition at {:#x}", partition.offset);
    return false;
  }

  u64 data_start = 0;
  if (partition != PARTITION_NONE)
  {
    const std::optional<u64> data_offset =
        disc.ReadSwappedAndShifted(partition.offset + PARTITION_DATA_OFFSET_ADDRESS, PARTITION_NONE);
    if (!data_offset)
      return false;
    data_start = partition.offset + *data_offset;
  }

  // Disc header, BI2 and apploader. The sum is 32-bit like the fields it's made of.
  const std::optional<u32> apploader_size = disc.ReadSwapped<u32>(APPLOADER_SIZE_ADDRESS, partition);
  const std::optional<u32> trailer_size =
      disc.ReadSwapped<u32>(APPLOADER_TRAILER_SIZE_ADDRESS, partition);
  if (!apploader_size || !trailer_size)
    return false;
  UseData(data_start, 0, static_cast<u32>(APPLOADER_START + *apploader_size + *trailer_size));

  const std::optional<u64> dol_offset = GetBootDOLOffset(disc, partition);
  if (!dol_offset)
    return false;
  const std::optional<u32> dol_size = GetBootDOLSize(disc, partition, *dol_offset);
  if (!dol_size)
    return false;
  UseData(data_start, *dol_offset, *dol_size);

  const std::optional<u64> fst_offset = GetFSTOffset(disc, partition);
  const std::optional<u64> fst_size = GetFSTSize(disc, partition);
  if (!fst_offset || !fst_size)
    return false;
  UseData(data_start, *fst_offset, *fst_size);

  WalkDirectory(data_start, file_system->GetRoot());
  return true;
}

void DiscScrubber::WalkDirectory(u64 data_start, const FileInfo& directory)
{
  for (const FileInfo& entry : directory)
  {
    if (entry.IsDirectory())
      WalkDirectory(data_start, entry);
    else
      UseData(data_start, entry.GetOffset(), entry.GetSize());
  }
}

}  // namespace DiscIO
