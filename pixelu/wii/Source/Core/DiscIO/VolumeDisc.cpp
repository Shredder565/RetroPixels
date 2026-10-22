// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/VolumeDisc.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/VolumeGC.h"

namespace DiscIO
{
// Disc header layout (the first bytes of a GameCube disc or of a Wii partition):
//   0x00 game code (4) + maker code (2), 0x06 disc number, 0x07 revision,
//   0x20 internal name (0x60 bytes). The apploader starts at 0x2440 with its build date.
namespace
{
constexpr u64 HEADER_MAKER_OFFSET = 0x04;
constexpr u64 HEADER_DISC_NUMBER_OFFSET = 0x06;
constexpr u64 HEADER_REVISION_OFFSET = 0x07;
constexpr u64 HEADER_NAME_OFFSET = 0x20;
constexpr size_t HEADER_NAME_SIZE = 0x60;
constexpr size_t APPLOADER_DATE_SIZE = 0x10;
constexpr u64 NKIT_MAGIC_OFFSET = 0x200;
constexpr u32 NKIT_MAGIC = 0x4E4B4954;  // "NKIT"

// Reads from the disc header at the start of `partition`. A Wii disc's own header (outside the
// partitions) carries the same ID, maker, revision and name, so it stands in when an existing
// partition can't be decrypted (no keys file from the owner's Wii): the game list still shows
// the disc by its ID and name, just without its banner.
bool ReadHeader(const DiscIO::VolumeDisc& volume, u64 offset, u64 size, u8* out,
                const DiscIO::Partition& partition)
{
  if (volume.Read(offset, size, out, partition))
    return true;
  return partition != DiscIO::PARTITION_NONE && volume.GetTicket(partition).IsValid() &&
         volume.Read(offset, size, out, DiscIO::PARTITION_NONE);
}

const VolumeDisc::BootID& TriforceBootID(const VolumeDisc& volume)
{
  // Only VolumeGC reports Platform::Triforce.
  return *static_cast<const VolumeGC&>(volume).GetTriforceBootID();
}

// The first byte of boot.id's region flags says which market a Triforce board was sold in.
char TriforceCountryLetter(const VolumeDisc::BootID& boot_id)
{
  switch (boot_id.region_flags & 0xff)
  {
  case 0x08:  // Asia
    return 'W';
  case 0x0e:  // USA
    return 'E';
  case 0x0c:  // export
    return 'P';
  case 0x02:  // Japan
  default:
    return 'J';
  }
}
}  // namespace

std::string VolumeDisc::GetGameID(const Partition& partition) const
{
  std::array<char, 6> id{};

  if (GetVolumeType() != Platform::Triforce)
  {
    if (!ReadHeader(*this, 0, id.size(), reinterpret_cast<u8*>(id.data()), partition))
      return {};
    return FilterGameID(id);
  }

  // Triforce discs have no game ID in the usual sense, so one is put together in the same shape:
  // 'G', two characters of boot.id's game ID, a country letter and the maker code.
  const BootID& boot_id = TriforceBootID(*this);
  id[0] = 'G';
  id[1] = boot_id.game_id[2];
  id[2] = boot_id.game_id[3];
  switch (GetCountry())
  {
  case Country::Taiwan:
    id[3] = 'W';
    break;
  case Country::USA:
    id[3] = 'E';
    break;
  case Country::Europe:
    id[3] = 'P';
    break;
  case Country::Japan:
  default:
    id[3] = 'J';
    break;
  }
  const std::string maker = GetMakerID();
  for (size_t i = 0; i < 2 && i < maker.size(); ++i)
    id[4 + i] = maker[i];
  return FilterGameID(id);
}

Country VolumeDisc::GetCountry(const Partition& partition) const
{
  // A missing byte reads as 0, which means an unknown country.
  u8 country_code = 0;
  if (GetVolumeType() == Platform::Triforce)
    country_code = static_cast<u8>(TriforceCountryLetter(TriforceBootID(*this)));
  else if (!ReadHeader(*this, 3, 1, &country_code, partition))
    country_code = 0;

  const Platform platform = GetVolumeType();
  const Region region = GetRegion();
  const std::optional<u16> revision = GetRevision();

  // When the game ID letter disagrees with the region field, trust the region field.
  if (CountryCodeToRegion(country_code, platform, region, revision) != region)
    return TypicalCountryForRegion(region);
  return CountryCodeToCountry(country_code, platform, region, revision);
}

Region VolumeDisc::RegionCodeToRegion(std::optional<u32> region_code) const
{
  if (!region_code || *region_code > static_cast<u32>(Region::NTSC_K))
    return Region::Unknown;
  return static_cast<Region>(*region_code);
}

std::string VolumeDisc::GetMakerID(const Partition& partition) const
{
  if (GetVolumeType() == Platform::Triforce)
  {
    // boot.id names the maker in full. Only three makers are known (SEGA CORPORATION,
    // Hitmaker co,ltd and NAMCO CORPORATION), so the first letter is enough. Anything else
    // falls back to the disc header.
    switch (TriforceBootID(*this).manufacturer[0])
    {
    case 'S':
    case 'H':
      return "6E";
    case 'N':
      return "82";
    default:
      break;
    }
  }

  std::array<char, 2> maker;
  if (!ReadHeader(*this, HEADER_MAKER_OFFSET, maker.size(), reinterpret_cast<u8*>(maker.data()),
                  partition))
    return {};
  return FilterGameID(maker);
}

std::optional<u16> VolumeDisc::GetRevision(const Partition& partition) const
{
  u8 revision;
  if (!ReadHeader(*this, HEADER_REVISION_OFFSET, 1, &revision, partition))
    return std::nullopt;
  return revision;
}

std::string VolumeDisc::GetInternalName(const Partition& partition) const
{
  if (GetVolumeType() == Platform::Triforce)
    return DecodeString(TriforceBootID(*this).game_name);

  std::array<char, HEADER_NAME_SIZE> name;
  if (!ReadHeader(*this, HEADER_NAME_OFFSET, name.size(), reinterpret_cast<u8*>(name.data()),
                  partition))
    return {};
  return DecodeString(name);
}

std::string VolumeDisc::GetApploaderDate(const Partition& partition) const
{
  std::array<char, APPLOADER_DATE_SIZE> date;
  if (!Read(APPLOADER_ADDRESS, date.size(), reinterpret_cast<u8*>(date.data()), partition))
    return {};
  return DecodeString(date);
}

std::optional<u8> VolumeDisc::GetDiscNumber(const Partition& partition) const
{
  return ReadSwapped<u8>(HEADER_DISC_NUMBER_OFFSET, partition);
}

u32 VolumeDisc::GetSimulatedMemorySize() const
{
  // bi2.bin + 4: the memory size the apploader pretends the console has (debug units with more
  // RAM used it to behave like retail units).
  return ReadSwapped<u32>(BI2_ADDRESS + 0x04, GetGamePartition()).value_or(0);
}

bool VolumeDisc::IsNKit() const
{
  return ReadSwapped<u32>(NKIT_MAGIC_OFFSET, PARTITION_NONE) == NKIT_MAGIC;
}

void VolumeDisc::AddGamePartitionToSyncHash(Common::SHA1::Context* context) const
{
  const Partition partition = GetGamePartition();

  // Disc header, bi2.bin and the apploader.
  const u64 apploader_size = GetApploaderSize(*this, partition).value_or(0);
  ReadAndAddToSyncHash(context, 0, APPLOADER_ADDRESS + apploader_size, partition);

  // The boot DOL, which Datel discs may lack.
  if (const std::optional<u64> dol_offset = GetBootDOLOffset(*this, partition))
  {
    const u64 dol_size = GetBootDOLSize(*this, partition, *dol_offset).value_or(0);
    ReadAndAddToSyncHash(context, *dol_offset, dol_size, partition);
  }

  // The file system table.
  if (const std::optional<u64> fst_offset = GetFSTOffset(*this, partition))
  {
    const u64 fst_size = GetFSTSize(*this, partition).value_or(0);
    ReadAndAddToSyncHash(context, *fst_offset, fst_size, partition);
  }

  // opening.bnr, which holds the title and banner.
  if (const FileSystem* file_system = GetFileSystem(partition))
  {
    const std::unique_ptr<FileInfo> banner = file_system->FindFileInfo("opening.bnr");
    if (banner && !banner->IsDirectory())
      ReadAndAddToSyncHash(context, banner->GetOffset(), banner->GetSize(), partition);
  }
}

}  // namespace DiscIO
