// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"

namespace DiscIO
{
class FileInfo;
struct Partition;
class Volume;

// Sizes of complete disc images.
constexpr u64 MINI_DVD_SIZE = 1459978240;  // GameCube discs
constexpr u64 SL_DVD_SIZE = 4699979776;    // single-layer retail Wii discs
constexpr u64 SL_DVD_R_SIZE = 4707319808;  // single-layer RVT-R (debug) discs
constexpr u64 DL_DVD_SIZE = 8511160320;    // dual-layer retail Wii discs
constexpr u64 DL_DVD_R_SIZE = 8543666176;  // dual-layer RVT-R (debug) discs

// Magic words in the disc header (at 0x1c on GameCube discs, 0x18 on Wii discs).
constexpr u32 GAMECUBE_DISC_MAGIC = 0xC2339F3D;
constexpr u32 WII_DISC_MAGIC = 0x5D1C9EA3;

// Wii partition types.
constexpr u32 PARTITION_DATA = 0;
constexpr u32 PARTITION_UPDATE = 1;
constexpr u32 PARTITION_CHANNEL = 2;  // a few titles install a channel from the disc
constexpr u32 PARTITION_INSTALL = 3;  // only used by Dragon Quest X

// The start of a GameCube disc or of a Wii partition's data.
constexpr u32 DISCHEADER_ADDRESS = 0;
constexpr u32 DISCHEADER_SIZE = 0x440;
constexpr u32 BI2_ADDRESS = 0x440;
constexpr u32 BI2_SIZE = 0x2000;
constexpr u32 APPLOADER_ADDRESS = 0x2440;

// The header of a Wii partition (offsets from the partition's start; stored offsets are >> 2).
constexpr u32 WII_PARTITION_TICKET_ADDRESS = 0;
constexpr u32 WII_PARTITION_TICKET_SIZE = 0x2a4;
constexpr u32 WII_PARTITION_TMD_SIZE_ADDRESS = 0x2a4;
constexpr u32 WII_PARTITION_TMD_OFFSET_ADDRESS = 0x2a8;
constexpr u32 WII_PARTITION_CERT_CHAIN_SIZE_ADDRESS = 0x2ac;
constexpr u32 WII_PARTITION_CERT_CHAIN_OFFSET_ADDRESS = 0x2b0;
constexpr u32 WII_PARTITION_H3_OFFSET_ADDRESS = 0x2b4;
constexpr u32 WII_PARTITION_H3_SIZE = 0x18000;

// Unencrypted areas at the start of a Wii disc.
constexpr u32 WII_NONPARTITION_DISCHEADER_ADDRESS = 0;
constexpr u32 WII_NONPARTITION_DISCHEADER_SIZE = 0x100;
constexpr u32 WII_REGION_DATA_ADDRESS = 0x4E000;
constexpr u32 WII_REGION_DATA_SIZE = 0x20;

// Block sizes for the compressed formats.
// The default for GCZ and RVZ: 128 KiB.
constexpr int GCZ_RVZ_PREFERRED_BLOCK_SIZE = 0x20000;
// Smallest size worth offering: emulated DVD reads are timed in whole 32 KiB ECC blocks anyway.
constexpr int PREFERRED_MIN_BLOCK_SIZE = 0x8000;
// Largest size worth offering: 2 MiB (also WIA's minimum). Bigger blocks make reads slow.
constexpr int PREFERRED_MAX_BLOCK_SIZE = 0x200000;
// The block size old emulator versions always used for GCZ (16 KiB); used when nothing better
// fits, so that such files stay readable by those versions as before.
constexpr int GCZ_FALLBACK_BLOCK_SIZE = 0x4000;
// WIA block sizes are multiples of 2 MiB.
constexpr int WIA_MIN_BLOCK_SIZE = 0x200000;
// RVZ block sizes start at 32 KiB...
constexpr int RVZ_MIN_BLOCK_SIZE = 0x8000;
// ...and above 2 MiB must be multiples of 2 MiB.
constexpr int RVZ_BIG_BLOCK_SIZE_LCM = 0x200000;

// The name used for a partition type in the UI and in folder names (DATA, UPDATE, CHANNEL,
// INSTALL, a four-character code, or the number).
std::string NameForPartitionType(u32 partition_type, bool include_prefix);

std::optional<u64> GetApploaderSize(const Volume& volume, const Partition& partition);
std::optional<u64> GetBootDOLOffset(const Volume& volume, const Partition& partition);
std::optional<u32> GetBootDOLSize(const Volume& volume, const Partition& partition, u64 dol_offset);
std::optional<u64> GetFSTOffset(const Volume& volume, const Partition& partition);
std::optional<u64> GetFSTSize(const Volume& volume, const Partition& partition);

// The end of the last thing on the disc that the headers or file systems point at (in raw image
// offsets).
u64 GetBiggestReferencedOffset(const Volume& volume);
u64 GetBiggestReferencedOffset(const Volume& volume, std::span<const Partition> partitions);

bool IsGCZBlockSizeLegacyCompatible(int block_size, u64 file_size);
bool IsDiscImageBlockSizeValid(int block_size, BlobType format);
}  // namespace DiscIO
