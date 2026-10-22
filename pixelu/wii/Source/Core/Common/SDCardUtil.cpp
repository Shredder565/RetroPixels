// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Creates an empty FAT32 volume image (no partition table), laid out per Microsoft's FAT
// specification (fatgen103):
//
//   sector 0            boot sector (BPB)
//   sector 1            FSInfo
//   sector 6, 7         backup boot sector and FSInfo
//   sectors 2-5, 8-31   zero (the rest of the 32 reserved sectors)
//   2 x FAT             each starting with the reserved entries and the root directory's chain
//   data region         zero; cluster 2 is the (empty) root directory

#include "Common/SDCardUtil.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace Common
{
namespace
{
// Changing any of these changes the on-disk format.
constexpr u32 SECTOR_SIZE = 512;
constexpr u32 RESERVED_SECTOR_COUNT = 32;
constexpr u32 FSINFO_SECTOR = 1;
constexpr u32 BACKUP_BOOT_SECTOR = 6;
constexpr u32 FAT_COUNT = 2;
constexpr u32 ROOT_DIR_CLUSTER = 2;

constexpr u64 MIN_SIZE = 8ull << 20;
constexpr u64 MAX_SIZE = 32ull << 30;

constexpr std::string_view VOLUME_LABEL = "DOLPHINSD";

using Sector = std::array<u8, SECTOR_SIZE>;

void Put8(Sector& sector, size_t offset, u8 value)
{
  sector[offset] = value;
}
void Put16(Sector& sector, size_t offset, u16 value)
{
  sector[offset] = static_cast<u8>(value);
  sector[offset + 1] = static_cast<u8>(value >> 8);
}
void Put32(Sector& sector, size_t offset, u32 value)
{
  for (size_t i = 0; i < 4; ++i)
    sector[offset + i] = static_cast<u8>(value >> (8 * i));
}
void PutText(Sector& sector, size_t offset, std::string_view text, size_t field_size)
{
  // Copies the text and zero-fills the rest of the field.
  std::fill_n(sector.begin() + offset, field_size, u8{0});
  std::copy_n(text.begin(), std::min(text.size(), field_size), sector.begin() + offset);
}

// The usual DOS-style volume serial derived from the creation time (UTC).
u32 MakeVolumeSerial()
{
  const std::time_t now = std::time(nullptr);
  const std::tm utc = *std::gmtime(&now);
  const u16 low = static_cast<u16>(utc.tm_mday + ((utc.tm_mon + 1) << 8) + (utc.tm_sec << 8));
  const u16 high = static_cast<u16>(utc.tm_min + (utc.tm_hour << 8) + (utc.tm_year + 1900));
  return low + (static_cast<u32>(high) << 16);
}

// Cluster size by volume size, in 512-byte sectors.
u32 SectorsPerCluster(u64 size_bytes)
{
  const u64 mib = size_bytes >> 20;
  if (mib < 260)
    return 1;
  if (mib < 8 * 1024)
    return 4;
  if (mib < 16 * 1024)
    return 8;
  if (mib < 32 * 1024)
    return 16;
  return 32;
}

// FAT size per the specification's approximation (slightly generous, never too small).
u32 SectorsPerFat(u64 size_bytes, u32 sectors_per_cluster)
{
  const u64 sectors = (size_bytes - RESERVED_SECTOR_COUNT * SECTOR_SIZE) / SECTOR_SIZE;
  const u64 divisor = (256 * u64{sectors_per_cluster} + FAT_COUNT) / 2;
  return static_cast<u32>((sectors + divisor - 1) / divisor);
}

struct Geometry
{
  u32 total_sectors;
  u32 sectors_per_cluster;
  u32 sectors_per_fat;
};

Sector MakeBootSector(const Geometry& geometry, u32 serial)
{
  Sector s{};
  // Jump instruction and OEM name.
  Put8(s, 0x00, 0xeb);
  Put8(s, 0x01, 0x5a);
  Put8(s, 0x02, 0x90);
  PutText(s, 0x03, "MSWIN4.1", 8);
  // BIOS parameter block.
  Put16(s, 0x0b, SECTOR_SIZE);
  Put8(s, 0x0d, static_cast<u8>(geometry.sectors_per_cluster));
  Put16(s, 0x0e, RESERVED_SECTOR_COUNT);
  Put8(s, 0x10, FAT_COUNT);
  Put16(s, 0x11, 0);     // root entries: none on FAT32
  Put16(s, 0x13, 0);     // 16-bit total sectors: use the 32-bit field
  Put8(s, 0x15, 0xf8);   // media: fixed disk
  Put16(s, 0x16, 0);     // 16-bit FAT size: use the 32-bit field
  Put16(s, 0x18, 9);     // sectors per track (unused)
  Put16(s, 0x1a, 2);     // heads (unused)
  Put32(s, 0x1c, 0);     // hidden sectors
  Put32(s, 0x20, geometry.total_sectors);
  // FAT32 extension.
  Put32(s, 0x24, geometry.sectors_per_fat);
  Put16(s, 0x28, 0);     // FAT mirroring flags
  Put16(s, 0x2a, 0);     // version 0.0
  Put32(s, 0x2c, ROOT_DIR_CLUSTER);
  Put16(s, 0x30, FSINFO_SECTOR);
  Put16(s, 0x32, BACKUP_BOOT_SECTOR);
  Put8(s, 0x40, 0x80);   // drive number
  Put8(s, 0x42, 0x29);   // extended boot signature: serial, label and type follow
  Put32(s, 0x43, serial);
  PutText(s, 0x47, VOLUME_LABEL, 11);
  PutText(s, 0x52, "FAT32   ", 8);
  Put8(s, 0x1fe, 0x55);
  Put8(s, 0x1ff, 0xaa);
  return s;
}

Sector MakeFsInfoSector(const Geometry& geometry)
{
  Sector s{};
  Put32(s, 0, 0x41615252);    // lead signature "RRaA"
  Put32(s, 484, 0x61417272);  // structure signature "rrAa"
  // Free cluster hint. (Computed in sectors, as it always has been; it is only a hint.)
  Put32(s, 488,
        geometry.total_sectors - RESERVED_SECTOR_COUNT - FAT_COUNT * geometry.sectors_per_fat);
  Put32(s, 492, ROOT_DIR_CLUSTER + 1);  // next free cluster hint
  Put32(s, 508, 0xaa550000);            // trail signature
  return s;
}

Sector MakeFirstFatSector()
{
  Sector s{};
  Put32(s, 0, 0x0ffffff8);  // entry 0: media byte
  Put32(s, 4, 0x0fffffff);  // entry 1: reserved
  Put32(s, 8, 0x0fffffff);  // entry 2: root directory, end of chain
  return s;
}

class ImageWriter
{
public:
  explicit ImageWriter(File::IOFile& file) : m_file(file) {}

  bool Write(const Sector& sector) { return m_file.WriteBytes(sector.data(), sector.size()); }

  bool WriteZeroSectors(u64 count)
  {
    static constexpr std::array<u8, 64 * 1024> zeros{};
    u64 bytes = count * SECTOR_SIZE;
    while (bytes != 0)
    {
      const size_t chunk = static_cast<size_t>(std::min<u64>(bytes, zeros.size()));
      if (!m_file.WriteBytes(zeros.data(), chunk))
        return false;
      bytes -= chunk;
    }
    return true;
  }

private:
  File::IOFile& m_file;
};

bool WriteImage(File::IOFile& file, const Geometry& geometry)
{
  const Sector boot = MakeBootSector(geometry, MakeVolumeSerial());
  const Sector fsinfo = MakeFsInfoSector(geometry);
  const Sector fat_head = MakeFirstFatSector();

  ImageWriter out(file);

  // Reserved area: boot + FSInfo, padding, their backups, padding.
  if (!out.Write(boot) || !out.Write(fsinfo))
    return false;
  if (!out.WriteZeroSectors(BACKUP_BOOT_SECTOR - 2))
    return false;
  if (!out.Write(boot) || !out.Write(fsinfo))
    return false;
  if (!out.WriteZeroSectors(RESERVED_SECTOR_COUNT - BACKUP_BOOT_SECTOR - 2))
    return false;

  for (u32 i = 0; i < FAT_COUNT; ++i)
  {
    if (!out.Write(fat_head) || !out.WriteZeroSectors(geometry.sectors_per_fat - 1))
      return false;
  }

  const u64 data_sectors = u64{geometry.total_sectors} - RESERVED_SECTOR_COUNT -
                           u64{FAT_COUNT} * geometry.sectors_per_fat;
  return out.WriteZeroSectors(data_sectors);
}
}  // namespace

bool SDCardCreate(u64 disk_size /*in MB*/, const std::string& filename)
{
  const u64 size_bytes = disk_size << 20;
  if (size_bytes < MIN_SIZE || size_bytes > MAX_SIZE)
  {
    ERROR_LOG_FMT(COMMON, "Trying to create SD Card image of size {}MB is out of range (8MB-32GB)",
                  size_bytes >> 20);
    return false;
  }

  Geometry geometry;
  geometry.total_sectors = static_cast<u32>(size_bytes / SECTOR_SIZE);
  geometry.sectors_per_cluster = SectorsPerCluster(size_bytes);
  geometry.sectors_per_fat = SectorsPerFat(size_bytes, geometry.sectors_per_cluster);

  bool written;
  {
    File::IOFile file(filename, "wb");
    if (!file)
    {
      ERROR_LOG_FMT(COMMON, "Could not create file '{}', aborting...", filename);
      return false;
    }
    written = WriteImage(file, geometry);
  }

  if (!written)
  {
    // The file is closed now, so it can be removed on Windows as well.
    ERROR_LOG_FMT(COMMON, "Could not write to '{}', aborting...", filename);
    File::Delete(filename);
    return false;
  }
  return true;
}
}  // namespace Common
