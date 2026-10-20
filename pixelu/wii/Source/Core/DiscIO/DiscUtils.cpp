// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/DiscUtils.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
namespace
{
// Disc header fields (offsets are stored shifted on Wii).
constexpr u64 DOL_OFFSET_FIELD = 0x420;
constexpr u64 FST_OFFSET_FIELD = 0x424;
constexpr u64 FST_SIZE_FIELD = 0x428;

// The apploader header: date (0x10), entry point, then the sizes of the code and the trailer.
constexpr u64 APPLOADER_HEADER_SIZE = 0x20;
constexpr u64 APPLOADER_CODE_SIZE_FIELD = APPLOADER_ADDRESS + 0x14;
constexpr u64 APPLOADER_TRAILER_SIZE_FIELD = APPLOADER_ADDRESS + 0x18;

// A DOL header holds 7 text and 11 data sections: file offsets first, then load addresses, then
// sizes.
constexpr u32 DOL_TEXT_SECTIONS = 7;
constexpr u32 DOL_DATA_SECTIONS = 11;
constexpr u64 DOL_TEXT_OFFSETS = 0x00;
constexpr u64 DOL_DATA_OFFSETS = 0x1c;
constexpr u64 DOL_TEXT_SIZES = 0x90;
constexpr u64 DOL_DATA_SIZES = 0xac;

std::optional<u64> ReadHeaderOffset(const Volume& volume, const Partition& partition, u64 field)
{
  if (!IsDisc(volume.GetVolumeType()))
    return std::nullopt;
  return volume.ReadSwappedAndShifted(field, partition);
}

u64 BiggestOffsetInDirectory(const FileInfo& entry)
{
  if (!entry.IsDirectory())
    return entry.GetOffset() + entry.GetSize();

  u64 biggest = 0;
  for (const FileInfo& child : entry)
    biggest = std::max(biggest, BiggestOffsetInDirectory(child));
  return biggest;
}
}  // namespace

std::string NameForPartitionType(u32 partition_type, bool include_prefix)
{
  switch (partition_type)
  {
  case PARTITION_DATA:
    return "DATA";
  case PARTITION_UPDATE:
    return "UPDATE";
  case PARTITION_CHANNEL:
    return "CHANNEL";
  case PARTITION_INSTALL:
    // With the prefix the name is used for folder names, and wit does not know "INSTALL" there.
    if (!include_prefix)
      return "INSTALL";
    break;
  default:
    break;
  }

  // Other types are often four ASCII characters (like a game ID); otherwise use the number.
  std::string as_text(4, '\0');
  for (int i = 0; i < 4; ++i)
    as_text[i] = static_cast<char>(partition_type >> (24 - 8 * i));
  if (std::ranges::all_of(as_text, Common::IsAlnum))
    return include_prefix ? "P-" + as_text : as_text;
  return include_prefix ? fmt::format("P{}", partition_type) : fmt::format("{}", partition_type);
}

std::optional<u64> GetApploaderSize(const Volume& volume, const Partition& partition)
{
  const std::optional<u32> code_size =
      volume.ReadSwapped<u32>(APPLOADER_CODE_SIZE_FIELD, partition);
  const std::optional<u32> trailer_size =
      volume.ReadSwapped<u32>(APPLOADER_TRAILER_SIZE_FIELD, partition);
  if (!code_size || !trailer_size)
    return std::nullopt;
  return APPLOADER_HEADER_SIZE + *code_size + *trailer_size;
}

std::optional<u64> GetBootDOLOffset(const Volume& volume, const Partition& partition)
{
  // Datel discs have 0 here (their apploader does not use the field), which is never valid.
  const std::optional<u64> offset = ReadHeaderOffset(volume, partition, DOL_OFFSET_FIELD);
  if (offset == u64{0})
    return std::nullopt;
  return offset;
}

std::optional<u32> GetBootDOLSize(const Volume& volume, const Partition& partition, u64 dol_offset)
{
  if (!IsDisc(volume.GetVolumeType()))
    return std::nullopt;

  // The DOL ends where its furthest section ends (computed in u32, like the header fields).
  u32 end = 0;
  const auto scan = [&](u64 offsets, u64 sizes, u32 count) {
    for (u32 i = 0; i < count; ++i)
    {
      const std::optional<u32> offset =
          volume.ReadSwapped<u32>(dol_offset + offsets + i * 4, partition);
      const std::optional<u32> size =
          volume.ReadSwapped<u32>(dol_offset + sizes + i * 4, partition);
      if (!offset || !size)
        return false;
      end = std::max<u32>(end, *offset + *size);
    }
    return true;
  };
  if (!scan(DOL_TEXT_OFFSETS, DOL_TEXT_SIZES, DOL_TEXT_SECTIONS) ||
      !scan(DOL_DATA_OFFSETS, DOL_DATA_SIZES, DOL_DATA_SECTIONS))
  {
    return std::nullopt;
  }
  return end;
}

std::optional<u64> GetFSTOffset(const Volume& volume, const Partition& partition)
{
  return ReadHeaderOffset(volume, partition, FST_OFFSET_FIELD);
}

std::optional<u64> GetFSTSize(const Volume& volume, const Partition& partition)
{
  return ReadHeaderOffset(volume, partition, FST_SIZE_FIELD);
}

u64 GetBiggestReferencedOffset(const Volume& volume)
{
  // Partitions without a disc header are skipped. Some WBFS tools blank out Super Smash Bros.
  // Brawl's Masterpiece partitions completely but leave them in the partition table.
  std::vector<Partition> partitions;
  for (const Partition& partition : volume.GetPartitions())
  {
    if (volume.ReadSwapped<u32>(0x18, partition) == WII_DISC_MAGIC)
      partitions.push_back(partition);
  }
  if (partitions.empty())
    partitions.push_back(PARTITION_NONE);

  return GetBiggestReferencedOffset(volume, partitions);
}

u64 GetBiggestReferencedOffset(const Volume& volume, std::span<const Partition> partitions)
{
  // A GameCube disc header with bi2.bin ends at 0x460; on Wii discs, the area up to the first
  // partition (0x50000) is always used.
  u64 biggest = volume.GetVolumeType() == Platform::GameCubeDisc ? 0x460 : 0x50000;
  const auto take = [&](u64 offset_in_partition, const Partition& partition) {
    biggest = std::max(biggest, volume.PartitionOffsetToRawOffset(offset_in_partition, partition));
  };

  for (const Partition& partition : partitions)
  {
    if (partition != PARTITION_NONE)
      take(BI2_ADDRESS, partition);

    if (const std::optional<u64> dol_offset = GetBootDOLOffset(volume, partition))
    {
      if (const std::optional<u64> dol_size = GetBootDOLSize(volume, partition, *dol_offset))
        take(*dol_offset + *dol_size, partition);
    }

    const std::optional<u64> fst_offset = GetFSTOffset(volume, partition);
    const std::optional<u64> fst_size = GetFSTSize(volume, partition);
    if (fst_offset && fst_size)
      take(*fst_offset + *fst_size, partition);

    if (const FileSystem* file_system = volume.GetFileSystem(partition))
      take(BiggestOffsetInDirectory(file_system->GetRoot()), partition);
  }
  return biggest;
}

bool IsGCZBlockSizeLegacyCompatible(int block_size, u64 file_size)
{
  // Versions older than 5.0-11893 can only convert a GCZ back to ISO correctly when the file size
  // is a multiple of the block size but not of 32 times the block size.
  const bool whole_blocks = file_size % block_size == 0;
  const bool multiple_of_32_blocks = file_size % (block_size * 32) == 0;
  return whole_blocks && !multiple_of_32_blocks;
}

bool IsDiscImageBlockSizeValid(int block_size, BlobType format)
{
  switch (format)
  {
  case BlobType::GCZ:
    return MathUtil::IsPow2(block_size);
  case BlobType::WIA:
    return block_size >= WIA_MIN_BLOCK_SIZE && block_size % WIA_MIN_BLOCK_SIZE == 0;
  case BlobType::RVZ:
    if (block_size < RVZ_MIN_BLOCK_SIZE)
      return false;
    if (block_size < RVZ_BIG_BLOCK_SIZE_LCM)
      return MathUtil::IsPow2(block_size);
    return block_size % RVZ_BIG_BLOCK_SIZE_LCM == 0;
  default:
    ASSERT(false);
    return true;
  }
}

}  // namespace DiscIO
