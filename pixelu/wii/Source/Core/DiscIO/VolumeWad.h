// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
class BlobReader;
enum class BlobType;
enum class Country;
class FileSystem;
enum class Language;
enum class Region;
enum class Platform;

// A WAD file (an installable Wii title): a header followed by the certificate chain, ticket,
// TMD, the encrypted contents and an optional banner, each aligned to 0x40 bytes.
class VolumeWAD final : public Volume
{
public:
  VolumeWAD(std::unique_ptr<BlobReader> reader);
  bool Read(u64 offset, u64 length, u8* buffer,
            const Partition& partition = PARTITION_NONE) const override;
  const FileSystem* GetFileSystem(const Partition& partition = PARTITION_NONE) const override;
  std::optional<u64> GetTitleID(const Partition& partition = PARTITION_NONE) const override;
  const IOS::ES::TicketReader&
  GetTicket(const Partition& partition = PARTITION_NONE) const override;
  const IOS::ES::TMDReader& GetTMD(const Partition& partition = PARTITION_NONE) const override;
  const std::vector<u8>&
  GetCertificateChain(const Partition& partition = PARTITION_NONE) const override;
  std::vector<u8> GetContent(u16 index) const override;
  std::vector<u64> GetContentOffsets() const override;
  bool CheckContentIntegrity(const IOS::ES::Content& content, std::span<const u8> encrypted_data,
                             const IOS::ES::TicketReader& ticket) const override;
  IOS::ES::TicketReader GetTicketWithFixedCommonKey() const override;
  std::string GetGameID(const Partition& partition = PARTITION_NONE) const override;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const override;
  std::string GetMakerID(const Partition& partition = PARTITION_NONE) const override;
  std::optional<u16> GetRevision(const Partition& partition = PARTITION_NONE) const override;
  std::string GetInternalName(const Partition& partition = PARTITION_NONE) const override
  {
    return {};
  }
  std::map<Language, std::string> GetLongNames() const override;
  std::vector<u32> GetBanner(u32* width, u32* height) const override;
  std::string GetApploaderDate(const Partition& partition = PARTITION_NONE) const override
  {
    return {};
  }
  Platform GetVolumeType() const override;
  bool IsDatelDisc() const override;
  bool IsNKit() const override;
  Region GetRegion() const override;
  Country GetCountry(const Partition& partition = PARTITION_NONE) const override;
  u32 GetSimulatedMemorySize() const override { return 0; }

  BlobType GetBlobType() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override;
  u64 GetRawSize() const override;
  const BlobReader& GetBlobReader() const override;

  std::array<u8, 20> GetSyncHash() const override;

private:
  // Calls `visit(content, offset)` for each content in TMD order, with its offset in the file.
  template <typename Visit>
  void ForEachContent(const std::vector<IOS::ES::Content>& contents, Visit visit) const;

  std::unique_ptr<BlobReader> m_reader;
  IOS::ES::TicketReader m_ticket;
  IOS::ES::TMDReader m_tmd;
  std::vector<u8> m_cert_chain;

  // Where each part starts in the file, and the sizes from the header.
  u32 m_cert_chain_offset = 0;
  u32 m_ticket_offset = 0;
  u32 m_tmd_offset = 0;
  u32 m_data_offset = 0;
  u32 m_opening_bnr_offset = 0;
  u32 m_hdr_size = 0;
  u32 m_cert_chain_size = 0;
  u32 m_ticket_size = 0;
  u32 m_tmd_size = 0;
  u32 m_data_size = 0;
  u32 m_opening_bnr_size = 0;
};

}  // namespace DiscIO
