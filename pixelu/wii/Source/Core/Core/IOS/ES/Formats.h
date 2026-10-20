// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// On-disc and on-NAND structures handled by ES (tickets, TMDs, certificates, content.map and
// uid.sys), and small reader classes that pull fields out of them. Field layouts follow WiiBrew
// ("Ticket", "Title metadata", "Certificate chain"); everything is big endian on disk.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOSC.h"
#include "DiscIO/Enums.h"

class PointerWrap;

namespace IOS
{
namespace HLE::FS
{
class FileSystem;
}

namespace ES
{
// Upper 32 bits of a title ID.
enum class TitleType : u32
{
  System = 0x00000001,
  Game = 0x00010000,
  Channel = 0x00010001,
  SystemChannel = 0x00010002,
  GameWithChannel = 0x00010004,
  DLC = 0x00010005,
  HiddenChannel = 0x00010008,
};

bool IsTitleType(u64 title_id, TitleType title_type);
bool IsDiscTitle(u64 title_id);
bool IsChannel(u64 title_id);

// Bits of the TMD title_flags word.
enum TitleFlags : u32
{
  // Set on every official title.
  TITLE_TYPE_DEFAULT = 0x1,
  TITLE_TYPE_0x4 = 0x4,
  // DLC
  TITLE_TYPE_DATA = 0x8,
  TITLE_TYPE_0x10 = 0x10,
  // Seen on titles that use WFS
  TITLE_TYPE_WFS_MAYBE = 0x20,
  TITLE_TYPE_CT = 0x40,
};

#pragma pack(push, 4)
struct TMDHeader
{
  SignatureRSA2048 signature;
  u8 tmd_version;
  u8 ca_crl_version;
  u8 signer_crl_version;
  // Normally zero; 1 marks a vWii TMD.
  u8 is_vwii;
  u64 ios_id;
  u64 title_id;
  u32 title_flags;
  u16 group_id;
  u16 zero;
  u16 region;
  u8 ratings[16];
  u8 reserved[12];
  u8 ipc_mask[12];
  u8 reserved2[18];
  u32 access_rights;
  u16 title_version;
  u16 num_contents;
  u16 boot_index;
  u16 fill2;
};
static_assert(sizeof(TMDHeader) == 0x1e4, "TMDHeader has the wrong size");
static_assert(offsetof(TMDHeader, ios_id) == 0x184);

// One content record of a TMD.
struct Content
{
  bool IsShared() const;
  bool IsOptional() const;
  u32 id;
  u16 index;
  u16 type;
  u64 size;
  std::array<u8, 20> sha1;
};
static_assert(sizeof(Content) == 36, "Content has the wrong size");
bool operator==(const Content&, const Content&);

struct TimeLimit
{
  u32 enabled;
  u32 seconds;
};

// What ES hands out to titles instead of a full ticket.
struct TicketView
{
  u8 version;
  u64 ticket_id;
  u32 device_id;
  u64 title_id;
  u16 access_mask;
  u32 permitted_title_id;
  u32 permitted_title_mask;
  u8 title_export_allowed;
  u8 common_key_index;
  u8 unknown2[0x30];
  u8 content_access_permissions[0x40];
  TimeLimit time_limits[8];
};
static_assert(sizeof(TicketView) == 0xd8, "TicketView has the wrong size");

// A signed ticket. IOS only accepts RSA-2048 signed tickets, so that is the only layout here.
struct Ticket
{
  SignatureRSA2048 signature;
  u8 server_public_key[0x3c];
  u8 version;
  u8 ca_crl_version;
  u8 signer_crl_version;
  u8 title_key[0x10];
  u64 ticket_id;
  u32 device_id;
  u64 title_id;
  u16 access_mask;
  u16 ticket_version;
  u32 permitted_title_id;
  u32 permitted_title_mask;
  u8 title_export_allowed;
  u8 common_key_index;
  u8 unknown2[0x30];
  u8 content_access_permissions[0x40];
  TimeLimit time_limits[8];
};
static_assert(sizeof(Ticket) == 0x2A4, "Ticket has the wrong size");

// Follows the v0 part of a version 1 ticket.
struct V1TicketHeader
{
  u16 version;
  u16 header_size;
  u32 v1_ticket_size;
  u32 section_header_table_offset;
  u16 number_of_section_headers;
  u16 section_header_size;
  u32 flags;
};
static_assert(sizeof(V1TicketHeader) == 0x14, "V1TicketHeader has the wrong size");
#pragma pack(pop)

constexpr u32 MAX_TMD_SIZE = 0x49e4;

// Base for any blob that starts with a signature block.
class SignedBlobReader
{
public:
  SignedBlobReader() = default;
  explicit SignedBlobReader(std::vector<u8> bytes);

  const std::vector<u8>& GetBytes() const;
  void SetBytes(std::vector<u8> bytes);

  // SHA-1 of everything from the issuer field to the end of the blob.
  std::array<u8, 20> GetSha1() const;

  // True if the signature block is complete. This does not check the signature itself.
  bool IsSignatureValid() const;

  SignatureType GetSignatureType() const;
  std::vector<u8> GetSignatureData() const;
  size_t GetSignatureSize() const;
  // The full issuer chain, e.g. "Root-CA00000001-XS00000003".
  std::string GetIssuer() const;

  void DoState(PointerWrap& p);

protected:
  std::vector<u8> m_bytes;
};

bool IsValidTMDSize(size_t size);

class TMDReader final : public SignedBlobReader
{
public:
  TMDReader() = default;
  explicit TMDReader(std::vector<u8> bytes);

  bool IsValid() const;

  // The TMD view returned by ES (header fields and content records without hashes), unswapped.
  std::vector<u8> GetRawView() const;

  u16 GetBootIndex() const;
  u64 GetIOSId() const;
  u64 GetTitleId() const;
  u32 GetTitleFlags() const;
  u16 GetTitleVersion() const;
  u16 GetGroupId() const;
  DiscIO::Region GetRegion() const;
  bool IsvWii() const;

  // Six-character game ID (title ID low word + group ID). Falls back to the title ID as 16 hex
  // digits when those characters are not all alphanumeric.
  std::string GetGameID() const;

  // Four-character GameTDB ID (title ID low word), with the same hex fallback.
  std::string GetGameTDBID() const;

  u16 GetNumContents() const;
  bool GetContent(u16 index, Content* content) const;
  std::vector<Content> GetContents() const;
  bool FindContentById(u32 id, Content* content) const;
};

class TicketReader final : public SignedBlobReader
{
public:
  TicketReader() = default;
  explicit TicketReader(std::vector<u8> bytes);

  bool IsValid() const;
  bool IsV1Ticket() const;

  std::vector<u8> GetRawTicket(u64 ticket_id) const;
  size_t GetNumberOfTickets() const;

  // The unswapped ticket view for the ticket_num-th ticket in the file. A ticket file can hold
  // several v0 tickets, and IOS builds a view for each of them.
  std::vector<u8> GetRawTicketView(u32 ticket_num) const;

  u8 GetVersion() const;
  u32 GetDeviceId() const;
  u32 GetTicketSize() const;
  u64 GetTitleId() const;
  u8 GetCommonKeyIndex() const;
  // The title key, decrypted with the common key held by `iosc`.
  std::array<u8, 16> GetTitleKey(const HLE::IOSC& iosc) const;
  // As above, with a temporary IOSC whose console type is taken from the issuer.
  std::array<u8, 16> GetTitleKey() const;

  // Retail or development, judged from the issuer.
  HLE::IOSC::ConsoleType GetConsoleType() const;

  // Removes every ticket with this ticket ID from the buffer.
  void DeleteTicket(u64 ticket_id);

  // For a console-specific ticket: decrypts the title key in place using an ECDH key derived
  // from the console key and the ticket's server public key.
  HLE::ReturnCode Unpersonalise(HLE::IOSC& iosc);

  // Fakesigned tickets often carry a nonsense common key index; this lets the importer fix it.
  void OverwriteCommonKeyIndex(u8 index);
};

// /shared1/content.map: maps the SHA-1 of shared contents to their /shared1 file names.
class SharedContentMap final
{
public:
  explicit SharedContentMap(HLE::FSCore& fs_core);
  ~SharedContentMap();

  std::optional<std::string> GetFilenameFromSHA1(const std::array<u8, 20>& sha1) const;
  std::string AddSharedContent(const std::array<u8, 20>& sha1);
  bool DeleteSharedContent(const std::array<u8, 20>& sha1);
  std::vector<std::array<u8, 20>> GetHashes() const;

  u64 GetTicks() const { return m_ticks; }

private:
  bool WriteEntries() const;

  struct Entry;
  u32 m_last_id = 0;
  std::vector<Entry> m_entries;
  std::shared_ptr<HLE::FS::FileSystem> m_fs;
  u64 m_ticks = 0;
};

// /sys/uid.sys: assigns a UID to every title that has been run.
class UIDSys final
{
public:
  explicit UIDSys(HLE::FSCore& fs_core);

  u32 GetUIDFromTitle(u64 title_id) const;
  u32 GetOrInsertUIDForTitle(u64 title_id);
  u32 GetNextUID() const;

  u64 GetTicks() const { return m_ticks; }

private:
  std::shared_ptr<HLE::FS::FileSystem> m_fs;
  std::map<u32, u64> m_entries;
  u64 m_ticks = 0;
};

class CertReader final : public SignedBlobReader
{
public:
  explicit CertReader(std::vector<u8>&& bytes);

  bool IsValid() const;

  u32 GetId() const;
  // e.g. "XS00000003" or "CA00000001"
  std::string GetName() const;
  PublicKeyType GetPublicKeyType() const;
  // The key bytes; for RSA keys the 4-byte exponent is appended.
  std::vector<u8> GetPublicKey() const;

private:
  bool m_is_valid = false;
};

std::map<std::string, CertReader> ParseCertChain(const std::vector<u8>& chain);
}  // namespace ES
}  // namespace IOS
