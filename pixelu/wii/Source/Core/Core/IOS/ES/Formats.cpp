// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/ES/Formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/CommonTitles.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/FileSystemProxy.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"

namespace IOS::ES
{
namespace
{
// A content record as it appears in the TMD view: everything except the SHA-1.
constexpr size_t CONTENT_VIEW_SIZE = offsetof(Content, sha1);
static_assert(CONTENT_VIEW_SIZE == 0x10);

constexpr char CONTENT_MAP_PATH[] = "/shared1/content.map";
// Staging file for content.map; it must have the same 12-character file name.
constexpr char CONTENT_MAP_TEMP_PATH[] = "/tmp/content.map";
constexpr char UID_SYS_PATH[] = "/sys/uid.sys";

constexpr HLE::FS::Modes KERNEL_RW_MODES{HLE::FS::Mode::ReadWrite, HLE::FS::Mode::ReadWrite,
                                         HLE::FS::Mode::None};

// Layout facts for each signature type: total block size, where the signature bytes are and
// how long they are, and where the issuer string starts.
struct SignatureLayout
{
  size_t block_size;
  size_t sig_offset;
  size_t sig_size;
  size_t issuer_offset;
  size_t issuer_size;
};

template <typename T> constexpr SignatureLayout LayoutOf()
{
  return {sizeof(T), offsetof(T, sig), sizeof(T::sig), offsetof(T, issuer), sizeof(T::issuer)};
}

std::optional<SignatureLayout> GetSignatureLayout(SignatureType type)
{
  switch (type)
  {
  case SignatureType::RSA4096:
    return LayoutOf<SignatureRSA4096>();
  case SignatureType::RSA2048:
    return LayoutOf<SignatureRSA2048>();
  case SignatureType::ECC:
    return LayoutOf<SignatureECC>();
  }
  return std::nullopt;
}

// Reads a C string from a fixed-size, possibly unterminated, field.
std::string FixedString(const u8* field, size_t max_size)
{
  const char* chars = reinterpret_cast<const char*>(field);
  return std::string(chars, strnlen(chars, max_size));
}

std::string SharedContentPath(const std::array<u8, 8>& id)
{
  const std::string_view name{reinterpret_cast<const char*>(id.data()), id.size()};
  return fmt::format("/shared1/{}.app", name);
}

// Big-endian field readers for the raw blobs.
u8 Be8(const std::vector<u8>& bytes, size_t offset)
{
  return bytes.data()[offset];
}

u16 Be16(const std::vector<u8>& bytes, size_t offset)
{
  return Common::swap16(bytes.data() + offset);
}

u32 Be32(const std::vector<u8>& bytes, size_t offset)
{
  return Common::swap32(bytes.data() + offset);
}

u64 Be64(const std::vector<u8>& bytes, size_t offset)
{
  return Common::swap64(bytes.data() + offset);
}

bool AllAlnum(const u8* begin, size_t count)
{
  for (size_t i = 0; i < count; ++i)
  {
    if (!Common::IsAlnum(static_cast<char>(begin[i])))
      return false;
  }
  return true;
}
}  // namespace

// -----------------------------------------------------------------------------------------------
// Title IDs

bool IsTitleType(u64 title_id, TitleType title_type)
{
  return static_cast<u32>(title_id >> 32) == static_cast<u32>(title_type);
}

bool IsDiscTitle(u64 title_id)
{
  const auto type = static_cast<TitleType>(title_id >> 32);
  return type == TitleType::Game || type == TitleType::GameWithChannel;
}

bool IsChannel(u64 title_id)
{
  if (title_id == Titles::SYSTEM_MENU)
    return true;

  switch (static_cast<TitleType>(title_id >> 32))
  {
  case TitleType::Channel:
  case TitleType::SystemChannel:
  case TitleType::GameWithChannel:
  case TitleType::HiddenChannel:
    return true;
  default:
    return false;
  }
}

bool Content::IsShared() const
{
  return (type & 0x8000) != 0;
}

bool Content::IsOptional() const
{
  return (type & 0x4000) != 0;
}

bool operator==(const Content& lhs, const Content& rhs)
{
  return lhs.id == rhs.id && lhs.index == rhs.index && lhs.type == rhs.type &&
         lhs.size == rhs.size && lhs.sha1 == rhs.sha1;
}

// -----------------------------------------------------------------------------------------------
// Signed blobs

SignedBlobReader::SignedBlobReader(std::vector<u8> bytes) : m_bytes(std::move(bytes))
{
}

const std::vector<u8>& SignedBlobReader::GetBytes() const
{
  return m_bytes;
}

void SignedBlobReader::SetBytes(std::vector<u8> bytes)
{
  m_bytes = std::move(bytes);
}

std::array<u8, 20> SignedBlobReader::GetSha1() const
{
  const auto layout = GetSignatureLayout(GetSignatureType());
  const size_t start = layout ? layout->issuer_offset : 0;
  return Common::SHA1::CalculateDigest(m_bytes.data() + start, m_bytes.size() - start);
}

bool SignedBlobReader::IsSignatureValid() const
{
  if (m_bytes.size() < sizeof(SignatureType))
    return false;
  const size_t needed = GetSignatureSize();
  return needed != 0 && m_bytes.size() >= needed;
}

SignatureType SignedBlobReader::GetSignatureType() const
{
  return static_cast<SignatureType>(Common::swap32(m_bytes.data()));
}

std::vector<u8> SignedBlobReader::GetSignatureData() const
{
  const auto layout = GetSignatureLayout(GetSignatureType());
  if (!layout)
    return {};
  const u8* sig = m_bytes.data() + layout->sig_offset;
  return std::vector<u8>(sig, sig + layout->sig_size);
}

size_t SignedBlobReader::GetSignatureSize() const
{
  const auto layout = GetSignatureLayout(GetSignatureType());
  return layout ? layout->block_size : 0;
}

std::string SignedBlobReader::GetIssuer() const
{
  const auto layout = GetSignatureLayout(GetSignatureType());
  if (!layout)
    return "";
  return FixedString(m_bytes.data() + layout->issuer_offset, layout->issuer_size);
}

void SignedBlobReader::DoState(PointerWrap& p)
{
  p.Do(m_bytes);
}

// -----------------------------------------------------------------------------------------------
// TMD

bool IsValidTMDSize(size_t size)
{
  return size >= sizeof(TMDHeader) && size <= MAX_TMD_SIZE;
}

TMDReader::TMDReader(std::vector<u8> bytes) : SignedBlobReader(std::move(bytes))
{
}

bool TMDReader::IsValid() const
{
  if (!IsSignatureValid() || m_bytes.size() < sizeof(TMDHeader))
    return false;
  // Every content record listed in the header has to be present.
  const size_t records = GetNumContents();
  return m_bytes.size() >= sizeof(TMDHeader) + records * sizeof(Content);
}

std::vector<u8> TMDReader::GetRawView() const
{
  const u8* tmd = m_bytes.data();
  std::vector<u8> view;

  // tmd_version up to (not including) access_rights, then the version and content count.
  view.insert(view.end(), tmd + offsetof(TMDHeader, tmd_version),
              tmd + offsetof(TMDHeader, access_rights));
  view.insert(view.end(), tmd + offsetof(TMDHeader, title_version),
              tmd + offsetof(TMDHeader, title_version) + sizeof(u16));
  view.insert(view.end(), tmd + offsetof(TMDHeader, num_contents),
              tmd + offsetof(TMDHeader, num_contents) + sizeof(u16));

  const size_t records = GetNumContents();
  for (size_t i = 0; i < records; ++i)
  {
    const u8* record = tmd + sizeof(TMDHeader) + sizeof(Content) * i;
    view.insert(view.end(), record, record + CONTENT_VIEW_SIZE);
  }
  return view;
}

u16 TMDReader::GetBootIndex() const
{
  return Be16(m_bytes, offsetof(TMDHeader, boot_index));
}

u64 TMDReader::GetIOSId() const
{
  return Be64(m_bytes, offsetof(TMDHeader, ios_id));
}

u64 TMDReader::GetTitleId() const
{
  return Be64(m_bytes, offsetof(TMDHeader, title_id));
}

u32 TMDReader::GetTitleFlags() const
{
  return Be32(m_bytes, offsetof(TMDHeader, title_flags));
}

u16 TMDReader::GetTitleVersion() const
{
  return Be16(m_bytes, offsetof(TMDHeader, title_version));
}

u16 TMDReader::GetGroupId() const
{
  return Be16(m_bytes, offsetof(TMDHeader, group_id));
}

DiscIO::Region TMDReader::GetRegion() const
{
  const u64 title_id = GetTitleId();
  if (!IsChannel(title_id))
    return DiscIO::Region::Unknown;
  if (title_id == Titles::SYSTEM_MENU)
    return DiscIO::GetSysMenuRegion(GetTitleVersion());

  const auto region = static_cast<DiscIO::Region>(Be16(m_bytes, offsetof(TMDHeader, region)));
  if (region > DiscIO::Region::NTSC_K)
    return DiscIO::Region::Unknown;
  return region;
}

bool TMDReader::IsvWii() const
{
  return m_bytes[offsetof(TMDHeader, is_vwii)] != 0;
}

std::string TMDReader::GetGameID() const
{
  std::array<u8, 6> id;
  std::memcpy(id.data(), m_bytes.data() + offsetof(TMDHeader, title_id) + 4, 4);
  std::memcpy(id.data() + 4, m_bytes.data() + offsetof(TMDHeader, group_id), 2);
  if (!AllAlnum(id.data(), id.size()))
    return fmt::format("{:016x}", GetTitleId());
  return std::string(reinterpret_cast<const char*>(id.data()), id.size());
}

std::string TMDReader::GetGameTDBID() const
{
  const u8* code = m_bytes.data() + offsetof(TMDHeader, title_id) + 4;
  if (!AllAlnum(code, 4))
    return fmt::format("{:016x}", GetTitleId());
  return std::string(reinterpret_cast<const char*>(code), 4);
}

u16 TMDReader::GetNumContents() const
{
  return Be16(m_bytes, offsetof(TMDHeader, num_contents));
}

bool TMDReader::GetContent(u16 index, Content* content) const
{
  if (!IsValid() || index >= GetNumContents())
    return false;

  const u8* record = m_bytes.data() + sizeof(TMDHeader) + size_t{index} * sizeof(Content);
  content->id = Common::swap32(record + offsetof(Content, id));
  content->index = Common::swap16(record + offsetof(Content, index));
  content->type = Common::swap16(record + offsetof(Content, type));
  content->size = Common::swap64(record + offsetof(Content, size));
  std::memcpy(content->sha1.data(), record + offsetof(Content, sha1), content->sha1.size());
  return true;
}

std::vector<Content> TMDReader::GetContents() const
{
  if (!IsValid())
    return {};
  std::vector<Content> contents(GetNumContents());
  u16 index = 0;
  for (Content& content : contents)
    GetContent(index++, &content);
  return contents;
}

bool TMDReader::FindContentById(u32 id, Content* content) const
{
  const u16 count = GetNumContents();
  for (u16 index = 0; index < count; ++index)
  {
    // GetContent fails for every index once the TMD is invalid, so stop at the first failure.
    if (!GetContent(index, content))
      return false;
    if (content->id == id)
      return true;
  }
  return false;
}

// -----------------------------------------------------------------------------------------------
// Tickets

TicketReader::TicketReader(std::vector<u8> bytes) : SignedBlobReader(std::move(bytes))
{
}

bool TicketReader::IsValid() const
{
  if (m_bytes.empty() || !IsSignatureValid())
    return false;
  if (IsV1Ticket())
    return m_bytes.size() == GetTicketSize();
  return m_bytes.size() % sizeof(Ticket) == 0;
}

bool TicketReader::IsV1Ticket() const
{
  return GetVersion() == 1;
}

size_t TicketReader::GetNumberOfTickets() const
{
  return IsV1Ticket() ? 1 : m_bytes.size() / sizeof(Ticket);
}

u32 TicketReader::GetTicketSize() const
{
  if (!IsV1Ticket())
    return sizeof(Ticket);
  // A v1 ticket is the v0 part followed by a header that gives the size of the rest.
  const u8* v1_header = m_bytes.data() + sizeof(Ticket);
  const u32 extra = Common::swap32(v1_header + offsetof(V1TicketHeader, v1_ticket_size));
  return extra + static_cast<u32>(sizeof(Ticket));
}

std::vector<u8> TicketReader::GetRawTicket(u64 ticket_id_to_find) const
{
  const size_t count = GetNumberOfTickets();
  for (size_t n = 0; n < count; ++n)
  {
    const size_t size = GetTicketSize();
    const u8* ticket = m_bytes.data() + size * n;
    if (Common::swap64(ticket + offsetof(Ticket, ticket_id)) == ticket_id_to_find)
      return std::vector<u8>(ticket, ticket + size);
  }
  return {};
}

std::vector<u8> TicketReader::GetRawTicketView(u32 ticket_num) const
{
  // The view is a 4-byte word holding the (first ticket's) version, followed by the ticket from
  // ticket_id onwards.
  std::vector<u8> view(sizeof(TicketView));
  view[0] = GetVersion();

  const u8* source = m_bytes.data() + sizeof(Ticket) * ticket_num + offsetof(Ticket, ticket_id);
  std::memcpy(view.data() + sizeof(u32), source, sizeof(TicketView) - sizeof(u32));
  return view;
}

u8 TicketReader::GetVersion() const
{
  return Be8(m_bytes, offsetof(Ticket, version));
}

u32 TicketReader::GetDeviceId() const
{
  return Be32(m_bytes, offsetof(Ticket, device_id));
}

u64 TicketReader::GetTitleId() const
{
  return Be64(m_bytes, offsetof(Ticket, title_id));
}

u8 TicketReader::GetCommonKeyIndex() const
{
  return Be8(m_bytes, offsetof(Ticket, common_key_index));
}

std::optional<std::array<u8, 16>> TicketReader::GetTitleKey(const HLE::IOSC& iosc) const
{
  // The IV is the title ID followed by eight zero bytes.
  std::array<u8, 16> iv{};
  std::memcpy(iv.data(), m_bytes.data() + offsetof(Ticket, title_id), sizeof(u64));

  size_t key_index = m_bytes.at(offsetof(Ticket, common_key_index));
  if (key_index >= HLE::IOSC::COMMON_KEY_HANDLES.size())
  {
    PanicAlertFmt("Bad common key index for title {:016x}: {} -- using common key 0", GetTitleId(),
                  key_index);
    key_index = 0;
  }

  std::array<u8, 16> title_key;
  const HLE::ReturnCode result =
      iosc.Decrypt(HLE::IOSC::COMMON_KEY_HANDLES[key_index], iv.data(),
                   m_bytes.data() + offsetof(Ticket, title_key), title_key.size(),
                   title_key.data(), PID_ES);
  if (result != HLE::IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "No common key {} to decrypt the title key of {:016x} (no keys file?)",
                  key_index, GetTitleId());
    return std::nullopt;
  }
  return title_key;
}

std::optional<std::array<u8, 16>> TicketReader::GetTitleKey() const
{
  const HLE::IOSC iosc{GetConsoleType()};
  return GetTitleKey(iosc);
}

HLE::IOSC::ConsoleType TicketReader::GetConsoleType() const
{
  // Development tickets are signed by XS00000006 under CA00000002.
  if (GetIssuer() == "Root-CA00000002-XS00000006")
    return HLE::IOSC::ConsoleType::RVT;
  return HLE::IOSC::ConsoleType::Retail;
}

void TicketReader::DeleteTicket(u64 ticket_id_to_delete)
{
  std::vector<u8> kept;
  const size_t count = GetNumberOfTickets();
  const size_t size = GetTicketSize();
  for (size_t n = 0; n < count; ++n)
  {
    const u8* ticket = m_bytes.data() + size * n;
    if (Common::swap64(ticket + offsetof(Ticket, ticket_id)) != ticket_id_to_delete)
      kept.insert(kept.end(), ticket, ticket + size);
  }
  m_bytes = std::move(kept);
}

HLE::ReturnCode TicketReader::Unpersonalise(HLE::IOSC& iosc)
{
  using HLE::IOSC;

  IOSC::Handle server_key;
  HLE::ReturnCode ret =
      iosc.CreateObject(&server_key, IOSC::TYPE_PUBLIC_KEY, IOSC::ObjectSubType::ECC233, PID_ES);
  if (ret != HLE::IPC_SUCCESS)
    return ret;

  ret = iosc.ImportPublicKey(server_key, m_bytes.data() + offsetof(Ticket, server_public_key),
                             nullptr, PID_ES);
  if (ret != HLE::IPC_SUCCESS)
    return ret;

  IOSC::Handle shared_key;
  ret = iosc.CreateObject(&shared_key, IOSC::TYPE_SECRET_KEY, IOSC::ObjectSubType::AES128, PID_ES);
  if (ret != HLE::IPC_SUCCESS)
    return ret;

  ret = iosc.ComputeSharedKey(shared_key, IOSC::HANDLE_CONSOLE_KEY, server_key, PID_ES);
  if (ret != HLE::IPC_SUCCESS)
    return ret;

  // IV: the ticket ID, zero padded to 16 bytes.
  std::array<u8, 16> iv{};
  std::memcpy(iv.data(), m_bytes.data() + offsetof(Ticket, ticket_id), sizeof(u64));

  u8* title_key = m_bytes.data() + offsetof(Ticket, title_key);
  std::array<u8, 16> plain{};
  ret = iosc.Decrypt(shared_key, iv.data(), title_key, sizeof(Ticket::title_key), plain.data(),
                     PID_ES);
  if (ret == HLE::IPC_SUCCESS)
    std::memcpy(title_key, plain.data(), plain.size());
  return ret;
}

void TicketReader::OverwriteCommonKeyIndex(u8 index)
{
  m_bytes[offsetof(Ticket, common_key_index)] = index;
}

// -----------------------------------------------------------------------------------------------
// content.map

// One 28-byte record: an 8-character hex file name and the content's SHA-1.
struct SharedContentMap::Entry
{
  std::array<u8, 8> id;
  std::array<u8, 20> sha1;
};
static_assert(sizeof(std::array<u8, 8>) + sizeof(std::array<u8, 20>) == 28);

SharedContentMap::SharedContentMap(HLE::FSCore& fs_core) : m_fs{fs_core.GetFS()}
{
  static_assert(sizeof(Entry) == 28);

  const auto fd =
      fs_core.Open(PID_KERNEL, PID_KERNEL, CONTENT_MAP_PATH, HLE::FS::Mode::Read, {}, &m_ticks);
  if (fd.Get() < 0)
    return;

  const auto fd_value = static_cast<u64>(fd.Get());
  Entry record;
  while (fs_core.Read(fd_value, &record, 1, &m_ticks) == static_cast<s32>(sizeof(Entry)))
  {
    m_entries.push_back(record);
    ++m_last_id;
  }
}

SharedContentMap::~SharedContentMap() = default;

std::optional<std::string>
SharedContentMap::GetFilenameFromSHA1(const std::array<u8, 20>& sha1) const
{
  for (const Entry& entry : m_entries)
  {
    if (entry.sha1 == sha1)
      return SharedContentPath(entry.id);
  }
  return std::nullopt;
}

std::vector<std::array<u8, 20>> SharedContentMap::GetHashes() const
{
  std::vector<std::array<u8, 20>> hashes;
  hashes.reserve(m_entries.size());
  std::ranges::transform(m_entries, std::back_inserter(hashes),
                         [](const Entry& entry) { return entry.sha1; });
  return hashes;
}

std::string SharedContentMap::AddSharedContent(const std::array<u8, 20>& sha1)
{
  if (std::optional<std::string> existing = GetFilenameFromSHA1(sha1))
    return std::move(*existing);

  // New contents are named after a running counter, as eight lowercase hex digits.
  Entry entry{};
  const std::string name = fmt::format("{:08x}", m_last_id);
  std::memcpy(entry.id.data(), name.data(), entry.id.size());
  entry.sha1 = sha1;
  m_entries.push_back(entry);

  WriteEntries();
  ++m_last_id;
  return SharedContentPath(entry.id);
}

bool SharedContentMap::DeleteSharedContent(const std::array<u8, 20>& sha1)
{
  std::erase_if(m_entries, [&sha1](const Entry& entry) { return entry.sha1 == sha1; });
  return WriteEntries();
}

bool SharedContentMap::WriteEntries() const
{
  // Write a complete new map, then move it over the old one.
  {
    const auto file =
        m_fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, CONTENT_MAP_TEMP_PATH, KERNEL_RW_MODES);
    if (!file)
      return false;
    if (!file->Write(m_entries.data(), m_entries.size()))
      return false;
  }
  const auto result = m_fs->Rename(PID_KERNEL, PID_KERNEL, CONTENT_MAP_TEMP_PATH, CONTENT_MAP_PATH);
  return result == HLE::FS::ResultCode::Success;
}

// -----------------------------------------------------------------------------------------------
// uid.sys

UIDSys::UIDSys(HLE::FSCore& fs_core) : m_fs{fs_core.GetFS()}
{
  const auto fd =
      fs_core.Open(PID_KERNEL, PID_KERNEL, UID_SYS_PATH, HLE::FS::Mode::Read, {}, &m_ticks);
  if (fd.Get() >= 0)
  {
    const auto fd_value = static_cast<u64>(fd.Get());
    // Each record is a big-endian title ID followed by a big-endian UID. Reading stops at the
    // first incomplete record, or at a record that is all zeroes.
    for (;;)
    {
      u64 be_title_id = 0;
      u32 be_uid = 0;
      if (fs_core.Read(fd_value, &be_title_id, 1, &m_ticks) != static_cast<s32>(sizeof(u64)))
        break;
      if (fs_core.Read(fd_value, &be_uid, 1, &m_ticks) != static_cast<s32>(sizeof(u32)))
        break;
      const u32 uid = Common::swap32(be_uid);
      const u64 title_id = Common::swap64(be_title_id);
      if (uid == 0 && title_id == 0)
        break;
      m_entries.emplace(uid, title_id);
    }
  }

  if (m_entries.empty())
    GetOrInsertUIDForTitle(Titles::SYSTEM_MENU);
}

u32 UIDSys::GetUIDFromTitle(u64 title_id) const
{
  for (const auto& [uid, entry_title_id] : m_entries)
  {
    if (entry_title_id == title_id)
      return uid;
  }
  return 0;
}

u32 UIDSys::GetNextUID() const
{
  return m_entries.empty() ? FIRST_PPC_UID : m_entries.rbegin()->first + 1;
}

u32 UIDSys::GetOrInsertUIDForTitle(const u64 title_id)
{
  if (const u32 existing = GetUIDFromTitle(title_id); existing != 0)
  {
    INFO_LOG_FMT(IOS_ES, "Title {:016x} already exists in uid.sys", title_id);
    return existing;
  }

  const u32 uid = GetNextUID();
  m_entries.emplace(uid, title_id);

  const u64 be_title_id = Common::swap64(title_id);
  const u32 be_uid = Common::swap32(uid);
  const auto file = m_fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, UID_SYS_PATH, KERNEL_RW_MODES);
  const bool written = file && file->Seek(0, HLE::FS::SeekMode::End) &&
                       file->Write(&be_title_id, 1) && file->Write(&be_uid, 1);
  if (!written)
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to write to /sys/uid.sys");
    return 0;
  }
  return uid;
}

// -----------------------------------------------------------------------------------------------
// Certificates

namespace
{
// The certificate layouts IOS knows: signature type, key type, total size, and where the key is.
struct CertLayout
{
  SignatureType signature_type;
  PublicKeyType key_type;
  size_t size;
  size_t key_offset;
  // Key length, including the 4-byte exponent for RSA keys.
  size_t key_size;
};

template <typename T, size_t ExtraKeyBytes>
constexpr CertLayout CertLayoutOf(SignatureType signature_type, PublicKeyType key_type)
{
  return {signature_type, key_type, sizeof(T), offsetof(T, public_key),
          sizeof(T::public_key) + ExtraKeyBytes};
}

constexpr std::array<CertLayout, 4> CERT_LAYOUTS{{
    CertLayoutOf<CertRSA4096RSA2048, 4>(SignatureType::RSA4096, PublicKeyType::RSA2048),
    CertLayoutOf<CertRSA2048RSA2048, 4>(SignatureType::RSA2048, PublicKeyType::RSA2048),
    CertLayoutOf<CertRSA2048ECC, 0>(SignatureType::RSA2048, PublicKeyType::ECC),
    CertLayoutOf<CertECC, 0>(SignatureType::ECC, PublicKeyType::ECC),
}};
}  // namespace

CertReader::CertReader(std::vector<u8>&& bytes) : SignedBlobReader(std::move(bytes))
{
  if (!IsSignatureValid())
    return;

  const SignatureType signature_type = GetSignatureType();
  for (const CertLayout& layout : CERT_LAYOUTS)
  {
    // The size test comes first: the key type word is only safe to read once the buffer is
    // known to be big enough for this layout.
    if (m_bytes.size() < layout.size || layout.signature_type != signature_type ||
        layout.key_type != GetPublicKeyType())
    {
      continue;
    }
    // A chain is parsed by cutting the leading certificate off, so drop whatever follows.
    m_bytes.resize(layout.size);
    m_is_valid = true;
    return;
  }
}

bool CertReader::IsValid() const
{
  return m_is_valid;
}

u32 CertReader::GetId() const
{
  return Common::swap32(m_bytes.data() + GetSignatureSize() + offsetof(CertHeader, id));
}

std::string CertReader::GetName() const
{
  return FixedString(m_bytes.data() + GetSignatureSize() + offsetof(CertHeader, name),
                     sizeof(CertHeader::name));
}

PublicKeyType CertReader::GetPublicKeyType() const
{
  const u8* word = m_bytes.data() + GetSignatureSize() + offsetof(CertHeader, public_key_type);
  return static_cast<PublicKeyType>(Common::swap32(word));
}

std::vector<u8> CertReader::GetPublicKey() const
{
  const SignatureType signature_type = GetSignatureType();
  const CertLayout* layout = nullptr;
  switch (signature_type)
  {
  case SignatureType::RSA4096:
    layout = &CERT_LAYOUTS[0];
    break;
  case SignatureType::RSA2048:
    layout = GetPublicKeyType() == PublicKeyType::RSA2048 ? &CERT_LAYOUTS[1] : &CERT_LAYOUTS[2];
    break;
  case SignatureType::ECC:
    layout = &CERT_LAYOUTS[3];
    break;
  default:
    return {};
  }
  const u8* key = m_bytes.data() + layout->key_offset;
  return std::vector<u8>(key, key + layout->key_size);
}

std::map<std::string, CertReader> ParseCertChain(const std::vector<u8>& chain)
{
  std::map<std::string, CertReader> certs;
  size_t offset = 0;
  while (offset != chain.size())
  {
    CertReader cert{
        std::vector<u8>(chain.begin() + static_cast<std::ptrdiff_t>(offset), chain.end())};
    if (!cert.IsValid())
      break;
    offset += cert.GetBytes().size();
    // The first certificate with a given name wins.
    std::string name = cert.GetName();
    certs.emplace(std::move(name), std::move(cert));
  }
  return certs;
}
}  // namespace IOS::ES
