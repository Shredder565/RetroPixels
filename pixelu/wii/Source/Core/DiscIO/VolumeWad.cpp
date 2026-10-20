// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/VolumeWad.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/IOS/IOSC.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/WiiSaveBanner.h"

namespace DiscIO
{
namespace
{
// Everything in a WAD file starts on a 0x40-byte boundary.
constexpr size_t WAD_ALIGNMENT = 0x40;

// WAD header fields (big-endian u32 sizes).
constexpr u64 HEADER_SIZE_FIELD = 0x00;
constexpr u64 CERT_CHAIN_SIZE_FIELD = 0x08;
constexpr u64 TICKET_SIZE_FIELD = 0x10;
constexpr u64 TMD_SIZE_FIELD = 0x14;
constexpr u64 DATA_SIZE_FIELD = 0x18;
constexpr u64 OPENING_BNR_SIZE_FIELD = 0x1c;

// Offsets inside the ticket, the TMD and the banner (IMET header). They are added to the u32
// part offsets in u32 arithmetic, like the header fields themselves.
constexpr u32 TICKET_TITLE_ID_OFFSET = 0x1dc;
constexpr u32 TMD_GROUP_ID_OFFSET = 0x198;
constexpr u32 OPENING_BNR_NAMES_OFFSET = 0x9c;

u64 AlignedContentSize(const IOS::ES::Content& content)
{
  return Common::AlignUp(content.size, WAD_ALIGNMENT);
}
}  // namespace

VolumeWAD::VolumeWAD(std::unique_ptr<BlobReader> reader) : m_reader(std::move(reader))
{
  ASSERT(m_reader);

  // The layout is described on WiiBrew ("WAD files").
  const auto header_field = [this](u64 offset) {
    return m_reader->ReadSwapped<u32>(offset).value_or(0);
  };
  m_hdr_size = header_field(HEADER_SIZE_FIELD);
  m_cert_chain_size = header_field(CERT_CHAIN_SIZE_FIELD);
  m_ticket_size = header_field(TICKET_SIZE_FIELD);
  m_tmd_size = header_field(TMD_SIZE_FIELD);
  m_data_size = header_field(DATA_SIZE_FIELD);
  m_opening_bnr_size = header_field(OPENING_BNR_SIZE_FIELD);

  // Each part follows the previous one (all in u32, as in the header).
  m_cert_chain_offset = Common::AlignUp(m_hdr_size, WAD_ALIGNMENT);
  m_ticket_offset = m_cert_chain_offset + Common::AlignUp(m_cert_chain_size, WAD_ALIGNMENT);
  m_tmd_offset = m_ticket_offset + Common::AlignUp(m_ticket_size, WAD_ALIGNMENT);
  m_data_offset = m_tmd_offset + Common::AlignUp(m_tmd_size, WAD_ALIGNMENT);
  m_opening_bnr_offset = m_data_offset + Common::AlignUp(m_data_size, WAD_ALIGNMENT);

  // A part that cannot be read in full is used as far as it was read.
  const auto read_part = [this](u32 offset, u32 size) {
    std::vector<u8> bytes(size);
    Read(offset, size, bytes.data());
    return bytes;
  };

  m_ticket.SetBytes(read_part(m_ticket_offset, m_ticket_size));

  if (!IOS::ES::IsValidTMDSize(m_tmd_size))
  {
    ERROR_LOG_FMT(DISCIO, "TMD has an invalid size: {} bytes", m_tmd_size);
    return;
  }
  m_tmd.SetBytes(read_part(m_tmd_offset, m_tmd_size));
  m_cert_chain = read_part(m_cert_chain_offset, m_cert_chain_size);
}

bool VolumeWAD::Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const
{
  return partition == PARTITION_NONE && m_reader->Read(offset, length, buffer);
}

const FileSystem* VolumeWAD::GetFileSystem(const Partition& partition) const
{
  // The contents of a WAD are not exposed as a file system.
  return nullptr;
}

Region VolumeWAD::GetRegion() const
{
  return m_tmd.IsValid() ? m_tmd.GetRegion() : Region::Unknown;
}

Country VolumeWAD::GetCountry(const Partition& partition) const
{
  if (!m_tmd.IsValid())
    return Country::Unknown;

  // The last byte of the title ID plays the role of a disc's country letter.
  const u8 country_code = static_cast<u8>(m_tmd.GetTitleId() & 0xff);
  if (country_code == 2)  // the Wii Menu, which is the same title in every region
    return TypicalCountryForRegion(GetSysMenuRegion(m_tmd.GetTitleVersion()));

  const Region region = GetRegion();
  const std::optional<u16> revision = GetRevision();
  if (CountryCodeToRegion(country_code, Platform::WiiWAD, region, revision) != region)
    return TypicalCountryForRegion(region);
  return CountryCodeToCountry(country_code, Platform::WiiWAD, region, revision);
}

const IOS::ES::TicketReader& VolumeWAD::GetTicket(const Partition& partition) const
{
  return m_ticket;
}

const IOS::ES::TMDReader& VolumeWAD::GetTMD(const Partition& partition) const
{
  return m_tmd;
}

const std::vector<u8>& VolumeWAD::GetCertificateChain(const Partition& partition) const
{
  return m_cert_chain;
}

template <typename Visit>
void VolumeWAD::ForEachContent(const std::vector<IOS::ES::Content>& contents, Visit visit) const
{
  u64 offset = m_data_offset;
  for (const IOS::ES::Content& content : contents)
  {
    if (!visit(content, offset))
      return;
    offset += AlignedContentSize(content);
  }
}

std::vector<u8> VolumeWAD::GetContent(u16 index) const
{
  std::vector<u8> result;
  ForEachContent(m_tmd.GetContents(), [&](const IOS::ES::Content& content, u64 offset) {
    if (content.index != index)
      return true;
    std::vector<u8> data(AlignedContentSize(content));
    if (m_reader->Read(offset, data.size(), data.data()))
      result = std::move(data);
    return false;
  });
  return result;
}

std::vector<u64> VolumeWAD::GetContentOffsets() const
{
  const std::vector<IOS::ES::Content> contents = m_tmd.GetContents();
  std::vector<u64> offsets;
  offsets.reserve(contents.size());
  ForEachContent(contents, [&](const IOS::ES::Content&, u64 offset) {
    offsets.push_back(offset);
    return true;
  });
  return offsets;
}

bool VolumeWAD::CheckContentIntegrity(const IOS::ES::Content& content,
                                      std::span<const u8> encrypted_data,
                                      const IOS::ES::TicketReader& ticket) const
{
  if (encrypted_data.size() != AlignedContentSize(content))
    return false;

  // Contents are encrypted with the title key; the IV is the content index followed by zeros.
  std::array<u8, Common::AES::Context::BLOCK_SIZE> iv{};
  iv[0] = static_cast<u8>(content.index >> 8);
  iv[1] = static_cast<u8>(content.index);

  const std::unique_ptr<Common::AES::Context> aes =
      Common::AES::CreateContextDecrypt(ticket.GetTitleKey().data());
  std::vector<u8> plain(encrypted_data.size());
  aes->Crypt(iv.data(), encrypted_data.data(), plain.data(), plain.size());

  return Common::SHA1::CalculateDigest(plain.data(), content.size) == content.sha1;
}

IOS::ES::TicketReader VolumeWAD::GetTicketWithFixedCommonKey() const
{
  if (!m_ticket.IsValid() || !m_tmd.IsValid())
    return m_ticket;

  // Fakesigned WADs (all-zero signature) often have a bogus common key index. A ticket with a
  // real signature is assumed to be right, which also saves the decryption below.
  const std::vector<u8> signature = m_ticket.GetSignatureData();
  if (std::ranges::any_of(signature, [](u8 b) { return b != 0; }))
    return m_ticket;

  const std::vector<IOS::ES::Content> contents = m_tmd.GetContents();
  if (contents.empty())
    return m_ticket;

  // Test the keys on the smallest content (the first one, if several are equally small).
  const IOS::ES::Content* smallest = nullptr;
  u64 smallest_offset = 0;
  ForEachContent(contents, [&](const IOS::ES::Content& content, u64 offset) {
    if (!smallest || content.size < smallest->size)
    {
      smallest = &content;
      smallest_offset = offset;
    }
    return true;
  });

  std::vector<u8> encrypted(AlignedContentSize(*smallest));
  if (!m_reader->Read(smallest_offset, encrypted.size(), encrypted.data()))
    return m_ticket;

  constexpr size_t key_count = IOS::HLE::IOSC::COMMON_KEY_HANDLES.size();
  const u8 specified = m_ticket.GetCommonKeyIndex();
  if (specified < key_count && CheckContentIntegrity(*smallest, encrypted, m_ticket))
    return m_ticket;

  IOS::ES::TicketReader candidate = m_ticket;
  for (size_t index = 0; index < key_count; ++index)
  {
    if (index == specified)
      continue;
    candidate.OverwriteCommonKeyIndex(static_cast<u8>(index));
    if (CheckContentIntegrity(*smallest, encrypted, candidate))
      return candidate;
  }

  ERROR_LOG_FMT(DISCIO, "Couldn't find valid common key for WAD file ({} specified)", specified);
  return m_ticket;
}

std::string VolumeWAD::GetGameID(const Partition& partition) const
{
  return m_tmd.IsValid() ? m_tmd.GetGameID() : std::string{};
}

std::string VolumeWAD::GetGameTDBID(const Partition& partition) const
{
  return m_tmd.IsValid() ? m_tmd.GetGameTDBID() : std::string{};
}

std::string VolumeWAD::GetMakerID(const Partition& partition) const
{
  // The TMD's group ID holds the maker code. Some channels have 0x0000 there instead.
  std::array<char, 2> maker;
  if (!Read(u32{m_tmd_offset + TMD_GROUP_ID_OFFSET}, maker.size(),
            reinterpret_cast<u8*>(maker.data()), partition))
  {
    return "00";
  }
  if (!Common::IsPrintableCharacter(maker[0]) || !Common::IsPrintableCharacter(maker[1]))
    return "00";
  return FilterGameID(maker);
}

std::optional<u64> VolumeWAD::GetTitleID(const Partition& partition) const
{
  return ReadSwapped<u64>(u32{m_ticket_offset + TICKET_TITLE_ID_OFFSET}, partition);
}

std::optional<u16> VolumeWAD::GetRevision(const Partition& partition) const
{
  if (!m_tmd.IsValid())
    return std::nullopt;
  return m_tmd.GetTitleVersion();
}

// A WAD is never a Datel disc or an NKit image.
bool VolumeWAD::IsNKit() const
{
  return false;
}

bool VolumeWAD::IsDatelDisc() const
{
  return false;
}

Platform VolumeWAD::GetVolumeType() const
{
  return Platform::WiiWAD;
}

std::map<Language, std::string> VolumeWAD::GetLongNames() const
{
  // Only channels have a banner with names.
  if (!m_tmd.IsValid() || !IOS::ES::IsChannel(m_tmd.GetTitleId()))
    return {};

  std::vector<char16_t> names(NAMES_TOTAL_CHARS);
  if (!Read(u32{m_opening_bnr_offset + OPENING_BNR_NAMES_OFFSET}, NAMES_TOTAL_BYTES,
            reinterpret_cast<u8*>(names.data())))
  {
    return {};
  }
  return ReadWiiNames(names);
}

std::vector<u32> VolumeWAD::GetBanner(u32* width, u32* height) const
{
  *width = 0;
  *height = 0;

  const std::optional<u64> title_id = GetTitleID();
  if (!title_id)
    return {};
  return WiiSaveBanner(*title_id).GetBanner(width, height);
}

// The blob the WAD is read from answers the size and format questions.
const BlobReader& VolumeWAD::GetBlobReader() const
{
  return *m_reader;
}

u64 VolumeWAD::GetRawSize() const
{
  return m_reader->GetRawSize();
}

DataSizeType VolumeWAD::GetDataSizeType() const
{
  return m_reader->GetDataSizeType();
}

u64 VolumeWAD::GetDataSize() const
{
  return m_reader->GetDataSize();
}

BlobType VolumeWAD::GetBlobType() const
{
  return m_reader->GetBlobType();
}

std::array<u8, 20> VolumeWAD::GetSyncHash() const
{
  // The TMD already covers the contents through their hashes. The ticket is left out because
  // its console ID legitimately differs between copies.
  const std::unique_ptr<Common::SHA1::Context> context = Common::SHA1::CreateContext();
  AddTMDToSyncHash(context.get(), PARTITION_NONE);
  ReadAndAddToSyncHash(context.get(), m_opening_bnr_offset, m_opening_bnr_size, PARTITION_NONE);
  return context->Finish();
}

}  // namespace DiscIO
