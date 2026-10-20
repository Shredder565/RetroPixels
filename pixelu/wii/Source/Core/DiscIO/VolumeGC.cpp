// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/VolumeGC.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/ColorUtil.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/FileSystemGCWii.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
namespace
{
// Magic words as they appear when the first four bytes are loaded into a little-endian u32.
constexpr u32 BOOT_ID_MAGIC = 0x44495442;  // "BTID"
constexpr u32 BNR1_MAGIC = 0x31524e42;     // "BNR1"
constexpr u32 BNR2_MAGIC = 0x32524e42;     // "BNR2"

// The disc header's region field (in bi2.bin).
constexpr u64 REGION_CODE_OFFSET = 0x458;
}  // namespace

VolumeGC::VolumeGC(std::unique_ptr<BlobReader> reader) : m_reader(std::move(reader))
{
  ASSERT(m_reader);

  m_file_system = [this]() -> std::unique_ptr<FileSystem> {
    auto file_system = std::make_unique<FileSystemGCWii>(this, PARTITION_NONE);
    if (!file_system->IsValid())
      return nullptr;
    return file_system;
  };
  m_banner = [this] { return ReadBanner(); };

  // A disc with a boot.id file whose magic is right is a Triforce disc.
  const FileSystem* file_system = GetFileSystem(PARTITION_NONE);
  if (!file_system)
    return;
  const std::unique_ptr<FileInfo> boot_id = file_system->FindFileInfo("boot.id");
  if (!boot_id)
    return;
  const u64 bytes_read = ReadFile(*this, PARTITION_NONE, boot_id.get(),
                                  reinterpret_cast<u8*>(&m_triforce_header), sizeof(BootID));
  m_is_triforce = bytes_read >= sizeof(u32) && m_triforce_header.magic == BOOT_ID_MAGIC;
}

VolumeGC::~VolumeGC() = default;

bool VolumeGC::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  return partition == PARTITION_NONE && m_reader->Read(offset, length, buffer);
}

const FileSystem* VolumeGC::GetFileSystem(const Partition& partition) const
{
  return m_file_system->get();
}

std::string VolumeGC::GetGameTDBID(const Partition& partition) const
{
  // Datel used GNHE5d (which the licensed NHL Hitz 2002 also has) and DTLX01 for many different
  // products, so their IDs say nothing about which title or cover to show.
  if (IsDatelDisc())
    return {};
  return GetGameID(partition);
}

Region VolumeGC::GetRegion() const
{
  if (!m_is_triforce)
    return RegionCodeToRegion(m_reader->ReadSwapped<u32>(REGION_CODE_OFFSET));

  switch (m_triforce_header.region_flags & 0xff)
  {
  case 0x0e:  // USA
    return Region::NTSC_U;
  case 0x0c:  // export
    return Region::PAL;
  case 0x02:  // Japan
  case 0x08:  // Asia
  default:
    return Region::NTSC_J;
  }
}

std::map<Language, std::string> VolumeGC::GetShortNames() const
{
  return m_banner->short_names;
}

std::map<Language, std::string> VolumeGC::GetLongNames() const
{
  return m_banner->long_names;
}

std::map<Language, std::string> VolumeGC::GetShortMakers() const
{
  return m_banner->short_makers;
}

std::map<Language, std::string> VolumeGC::GetLongMakers() const
{
  return m_banner->long_makers;
}

std::map<Language, std::string> VolumeGC::GetDescriptions() const
{
  return m_banner->descriptions;
}

std::vector<u32> VolumeGC::GetBanner(u32* width, u32* height) const
{
  const DecodedBanner& banner = *m_banner;
  *width = banner.width;
  *height = banner.height;
  return banner.pixels;
}

// Size and format questions go to the blob the disc is read from.
const BlobReader& VolumeGC::GetBlobReader() const
{
  return *m_reader;
}

u64 VolumeGC::GetRawSize() const
{
  return m_reader->GetRawSize();
}

DataSizeType VolumeGC::GetDataSizeType() const
{
  return m_reader->GetDataSizeType();
}

u64 VolumeGC::GetDataSize() const
{
  return m_reader->GetDataSize();
}

BlobType VolumeGC::GetBlobType() const
{
  return m_reader->GetBlobType();
}

Platform VolumeGC::GetVolumeType() const
{
  return m_is_triforce ? Platform::Triforce : Platform::GameCubeDisc;
}

bool VolumeGC::IsDatelDisc() const
{
  // Datel discs either use DTLX01 or have no boot DOL offset in the header.
  if (GetGameID() == "DTLX01")
    return true;
  return !GetBootDOLOffset(*this, PARTITION_NONE).has_value();
}

std::array<u8, 20> VolumeGC::GetSyncHash() const
{
  const std::unique_ptr<Common::SHA1::Context> context = Common::SHA1::CreateContext();
  AddGamePartitionToSyncHash(context.get());
  return context->Finish();
}

VolumeGC::DecodedBanner VolumeGC::ReadBanner() const
{
  // At least one Triforce disc ships another game's opening.bnr, so Triforce banners are ignored.
  if (m_is_triforce)
    return {};

  // Large (about 6 KiB), so it lives on the heap.
  auto file = std::make_unique<BannerFile>();
  const u64 size = ReadFile(*this, PARTITION_NONE, "opening.bnr", reinterpret_cast<u8*>(file.get()),
                            sizeof(BannerFile));
  if (size < sizeof(u32))
  {
    WARN_LOG_FMT(DISCIO, "Could not read opening.bnr.");
    return {};
  }

  if (file->magic == BNR1_MAGIC && size == BNR1_SIZE)
    return DecodeBanner(*file, 1);
  if (file->magic == BNR2_MAGIC && size == BNR2_SIZE)
    return DecodeBanner(*file, BANNER_LANGUAGES);

  WARN_LOG_FMT(DISCIO, "Invalid opening.bnr. Type: {:#0x} Size: {:#0x}", file->magic, size);
  return {};
}

VolumeGC::DecodedBanner VolumeGC::DecodeBanner(const BannerFile& file, size_t languages) const
{
  DecodedBanner banner;

  banner.width = BANNER_WIDTH;
  banner.height = BANNER_HEIGHT;
  banner.pixels.resize(BANNER_WIDTH * BANNER_HEIGHT);
  Common::Decode5A3Image(banner.pixels.data(), file.pixels, BANNER_WIDTH, BANNER_HEIGHT);

  // A BNR1 banner has one language: Japanese on Japanese discs, English otherwise. BNR2 banners
  // have the six PAL GameCube languages, starting with English.
  Language first = Language::English;
  if (languages == 1 && GetRegion() == Region::NTSC_J)
    first = Language::Japanese;

  const auto add = [this](std::map<Language, std::string>* map, Language language,
                          std::span<const char> field) {
    std::string text = DecodeString(field);
    if (!text.empty())
      map->emplace(language, std::move(text));
  };

  for (size_t i = 0; i < languages; ++i)
  {
    const BannerText& text = file.text[i];
    const Language language = static_cast<Language>(static_cast<int>(first) + static_cast<int>(i));
    add(&banner.descriptions, language, text.description);
    add(&banner.short_names, language, text.short_name);
    add(&banner.long_names, language, text.long_name);
    add(&banner.short_makers, language, text.short_maker);
    add(&banner.long_makers, language, text.long_maker);
  }

  return banner;
}

VolumeGC::DecodedBanner::DecodedBanner() = default;
VolumeGC::DecodedBanner::~DecodedBanner() = default;
}  // namespace DiscIO
