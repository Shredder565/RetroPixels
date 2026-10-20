// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/Volume.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/IOS/ES/Formats.h"

#include "DiscIO/Blob.h"
#include "DiscIO/CachedBlob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeGC.h"
#include "DiscIO/VolumeWad.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
const IOS::ES::TicketReader Volume::INVALID_TICKET{};
const IOS::ES::TMDReader Volume::INVALID_TMD{};
const std::vector<u8> Volume::INVALID_CERT_CHAIN{};

std::string Volume::DecodeString(std::span<const char> data) const
{
  const size_t length = strnlen(data.data(), data.size());
  const std::string raw(data.data(), length);
  if (GetRegion() == Region::NTSC_J)
    return SHIFTJISToUTF8(raw);
  return CP1252ToUTF8(raw);
}

std::string Volume::FilterGameID(std::span<const char> data)
{
  // Game IDs end up in file names, so only ASCII letters and digits are kept. (Lowercase letters
  // are allowed because at least one real disc, GNHE5d, has one.)
  std::string id;
  id.reserve(data.size());
  for (const char c : data)
    id.push_back(Common::IsAlnum(c) ? c : '-');
  return id;
}

namespace
{
// Hashes the in-memory bytes of a value. The sync hash is only compared between instances of the
// same build, so host byte order is fine.
template <typename T>
void HashValue(Common::SHA1::Context* context, const T& value)
{
  static_assert(std::is_trivially_copyable_v<T>);
  context->Update(reinterpret_cast<const u8*>(&value), sizeof(value));
}
}  // namespace

void Volume::ReadAndAddToSyncHash(Common::SHA1::Context* context, u64 offset, u64 length,
                                  const Partition& partition) const
{
  std::vector<u8> data(length);
  if (Read(offset, length, data.data(), partition))
    context->Update(data);
}

void Volume::AddTMDToSyncHash(Common::SHA1::Context* context, const Partition& partition) const
{
  // Only fields that matter for emulation and that fakesigning leaves alone: fakesigned WADs are
  // common, and a properly signed copy should still match them.
  const IOS::ES::TMDReader& tmd = GetTMD(partition);
  if (!tmd.IsValid())
    return;

  HashValue(context, tmd.GetIOSId());
  HashValue(context, tmd.GetTitleId());
  HashValue(context, tmd.GetTitleFlags());
  HashValue(context, tmd.GetGroupId());
  HashValue(context, tmd.GetRegion());
  HashValue(context, tmd.GetTitleVersion());
  HashValue(context, tmd.GetBootIndex());

  for (const IOS::ES::Content& content : tmd.GetContents())
    HashValue(context, content);
}

std::map<Language, std::string> Volume::ReadWiiNames(std::span<const char16_t> data)
{
  std::map<Language, std::string> names;
  const size_t complete_fields = std::min(NUMBER_OF_LANGUAGES, data.size() / NAME_CHARS_LENGTH);
  for (size_t language = 0; language < complete_fields; ++language)
  {
    const char16_t* field = data.data() + language * NAME_CHARS_LENGTH;
    std::string name = UTF16BEToUTF8(field, NAME_CHARS_LENGTH);
    if (!name.empty())
      names.emplace(static_cast<Language>(language), std::move(name));
  }
  return names;
}

namespace
{
// Looks at the magic words of a disc header. `wrap` may put another reader around the blob (for
// example a caching one); it is only applied once the image is known to be a disc, so it never
// sees things like DOL files.
template <typename Wrap>
std::unique_ptr<VolumeDisc> OpenDisc(std::unique_ptr<BlobReader>& reader, Wrap wrap)
{
  if (!reader)
    return nullptr;

  if (reader->ReadSwapped<u32>(0x18) == WII_DISC_MAGIC)
    return std::make_unique<VolumeWii>(wrap(std::move(reader)));
  if (reader->ReadSwapped<u32>(0x1c) == GAMECUBE_DISC_MAGIC)
    return std::make_unique<VolumeGC>(wrap(std::move(reader)));
  return nullptr;
}

std::unique_ptr<BlobReader> Unwrapped(std::unique_ptr<BlobReader> reader)
{
  return reader;
}

// WAD files start with their header size (0x20) followed by the type: "Is" for normal WADs and
// "ib" for boot2 WADs.
std::unique_ptr<VolumeWAD> OpenWAD(std::unique_ptr<BlobReader>& reader)
{
  if (!reader)
    return nullptr;

  constexpr u32 WAD_TYPE_NORMAL = 0x00204973;
  constexpr u32 WAD_TYPE_BOOT2 = 0x00206962;
  const std::optional<u32> type = reader->ReadSwapped<u32>(0x02);
  if (type != WAD_TYPE_NORMAL && type != WAD_TYPE_BOOT2)
    return nullptr;
  return std::make_unique<VolumeWAD>(std::move(reader));
}
}  // namespace

std::unique_ptr<VolumeDisc> CreateDisc(std::unique_ptr<BlobReader> reader)
{
  return OpenDisc(reader, Unwrapped);
}

std::unique_ptr<VolumeDisc> CreateDisc(const std::string& path)
{
  return CreateDisc(CreateBlobReader(path));
}

std::unique_ptr<VolumeDisc> CreateDiscForCore(const std::string& path)
{
  std::unique_ptr<BlobReader> reader = CreateBlobReader(path);
  if (!Config::Get(Config::MAIN_LOAD_GAME_INTO_MEMORY))
    return OpenDisc(reader, Unwrapped);
  return OpenDisc(reader, [](std::unique_ptr<BlobReader> r) {
    return CreateScrubbingCachedBlobReader(std::move(r));
  });
}

std::unique_ptr<VolumeWAD> CreateWAD(std::unique_ptr<BlobReader> reader)
{
  return OpenWAD(reader);
}

std::unique_ptr<VolumeWAD> CreateWAD(const std::string& path)
{
  return CreateWAD(CreateBlobReader(path));
}

std::unique_ptr<Volume> CreateVolume(std::unique_ptr<BlobReader> reader)
{
  if (std::unique_ptr<VolumeDisc> disc = OpenDisc(reader, Unwrapped))
    return disc;
  if (std::unique_ptr<VolumeWAD> wad = OpenWAD(reader))
    return wad;
  return nullptr;
}

std::unique_ptr<Volume> CreateVolume(const std::string& path)
{
  return CreateVolume(CreateBlobReader(path));
}
}  // namespace DiscIO
