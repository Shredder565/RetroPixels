// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Swap.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/Enums.h"

namespace DiscIO
{
class BlobReader;
enum class BlobType;
enum class DataSizeType;
class FileSystem;
class VolumeDisc;
class VolumeWAD;

// A Wii partition, identified by where its header starts on the disc.
struct Partition final
{
  constexpr Partition() = default;
  constexpr explicit Partition(u64 offset_) : offset(offset_) {}
  constexpr bool operator==(const Partition& other) const { return offset == other.offset; }
  constexpr auto operator<=>(const Partition other) const { return offset <=> other.offset; }
  u64 offset{std::numeric_limits<u64>::max()};
};

// Stands for "the raw disc" (no partition): GameCube discs, WADs and the unencrypted areas of
// Wii discs.
constexpr Partition PARTITION_NONE(std::numeric_limits<u64>::max() - 1);

// A GameCube or Wii disc image, or a WAD file. Offsets passed to the read functions are relative
// to the given partition's decrypted data (or to the start of the image for PARTITION_NONE).
class Volume
{
public:
  Volume() {}
  virtual ~Volume() {}

  virtual bool Read(u64 offset, u64 length, u8* buffer, const Partition& partition) const = 0;

  // Reads a big-endian value.
  template <typename T>
  std::optional<T> ReadSwapped(u64 offset, const Partition& partition) const
  {
    T value;
    if (!Read(offset, sizeof(T), reinterpret_cast<u8*>(&value), partition))
      return std::nullopt;
    return Common::FromBigEndian(value);
  }

  // Reads a big-endian u32 that holds an offset, scaled by the format's shift (Wii discs store
  // offsets divided by 4).
  std::optional<u64> ReadSwappedAndShifted(u64 offset, const Partition& partition) const
  {
    const std::optional<u32> stored = ReadSwapped<u32>(offset, partition);
    if (!stored)
      return std::nullopt;
    return u64{*stored} << GetOffsetShift();
  }

  virtual bool HasWiiHashes() const { return false; }
  virtual bool HasWiiEncryption() const { return false; }
  virtual std::vector<Partition> GetPartitions() const { return {}; }
  virtual Partition GetGamePartition() const { return PARTITION_NONE; }
  virtual std::optional<u32> GetPartitionType(const Partition& partition) const
  {
    return std::nullopt;
  }
  std::optional<u64> GetTitleID() const { return GetTitleID(GetGamePartition()); }
  virtual std::optional<u64> GetTitleID(const Partition& partition) const { return std::nullopt; }
  virtual const IOS::ES::TicketReader& GetTicket(const Partition& partition) const
  {
    return INVALID_TICKET;
  }
  virtual const IOS::ES::TMDReader& GetTMD(const Partition& partition) const { return INVALID_TMD; }
  virtual const std::vector<u8>& GetCertificateChain(const Partition& partition) const
  {
    return INVALID_CERT_CHAIN;
  }
  virtual std::vector<u8> GetContent(u16 index) const { return {}; }
  virtual std::vector<u64> GetContentOffsets() const { return {}; }
  virtual bool CheckContentIntegrity(const IOS::ES::Content& content,
                                     std::span<const u8> encrypted_data,
                                     const IOS::ES::TicketReader& ticket) const
  {
    return false;
  }
  virtual IOS::ES::TicketReader GetTicketWithFixedCommonKey() const { return {}; }
  // The volume keeps ownership. nullptr if there is no readable file system.
  virtual const FileSystem* GetFileSystem(const Partition& partition) const = 0;
  virtual u64 PartitionOffsetToRawOffset(u64 offset, const Partition& partition) const
  {
    return offset;
  }
  virtual std::string GetGameID(const Partition& partition = PARTITION_NONE) const = 0;
  virtual std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const = 0;
  virtual std::string GetMakerID(const Partition& partition = PARTITION_NONE) const = 0;
  virtual std::optional<u16> GetRevision(const Partition& partition = PARTITION_NONE) const = 0;
  virtual std::string GetInternalName(const Partition& partition = PARTITION_NONE) const = 0;
  virtual std::map<Language, std::string> GetShortNames() const { return {}; }
  virtual std::map<Language, std::string> GetLongNames() const { return {}; }
  virtual std::map<Language, std::string> GetShortMakers() const { return {}; }
  virtual std::map<Language, std::string> GetLongMakers() const { return {}; }
  virtual std::map<Language, std::string> GetDescriptions() const { return {}; }
  virtual std::vector<u32> GetBanner(u32* width, u32* height) const = 0;
  std::string GetApploaderDate() const { return GetApploaderDate(GetGamePartition()); }
  virtual std::string GetApploaderDate(const Partition& partition) const = 0;
  // Counted from 0 (the first disc of a set is 0).
  virtual std::optional<u8> GetDiscNumber(const Partition& partition = PARTITION_NONE) const
  {
    return 0;
  }
  virtual Platform GetVolumeType() const = 0;
  virtual bool IsDatelDisc() const = 0;
  virtual bool IsNKit() const = 0;
  virtual bool CheckH3TableIntegrity(const Partition& partition) const { return false; }
  virtual bool CheckBlockIntegrity(u64 block_index, const u8* encrypted_data,
                                   const Partition& partition) const
  {
    return false;
  }
  virtual bool CheckBlockIntegrity(u64 block_index, const Partition& partition) const
  {
    return false;
  }
  virtual Region GetRegion() const = 0;
  virtual Country GetCountry(const Partition& partition = PARTITION_NONE) const = 0;
  virtual u32 GetSimulatedMemorySize() const = 0;
  virtual BlobType GetBlobType() const = 0;
  // The size of the image's contents. Some formats only know it approximately.
  virtual u64 GetDataSize() const = 0;
  virtual DataSizeType GetDataSizeType() const = 0;
  // The size of the image file itself (after compression).
  virtual u64 GetRawSize() const = 0;
  virtual const BlobReader& GetBlobReader() const = 0;

  // A quick fingerprint of what matters for NetPlay and input recordings: it should match between
  // dumps that behave the same and differ between ones that do not, without hashing everything.
  // It is not stable across emulator versions.
  virtual std::array<u8, 20> GetSyncHash() const = 0;

protected:
  // Text in disc headers and GameCube banners: Shift-JIS on NTSC-J discs, Windows-1252
  // elsewhere, ending at the first null byte.
  std::string DecodeString(std::span<const char> data) const;
  // Turns every character that is not an ASCII letter or digit into '-'.
  static std::string FilterGameID(std::span<const char> data);

  void ReadAndAddToSyncHash(Common::SHA1::Context* context, u64 offset, u64 length,
                            const Partition& partition) const;
  void AddTMDToSyncHash(Common::SHA1::Context* context, const Partition& partition) const;

  virtual u32 GetOffsetShift() const { return 0; }
  // Splits the ten 42-character UTF-16BE title fields of a Wii banner.
  static std::map<Language, std::string> ReadWiiNames(std::span<const char16_t> data);

  static constexpr size_t NUMBER_OF_LANGUAGES = 10;
  static constexpr size_t NAME_CHARS_LENGTH = 42;
  static constexpr size_t NAME_BYTES_LENGTH = NAME_CHARS_LENGTH * sizeof(char16_t);
  static constexpr size_t NAMES_TOTAL_CHARS = NAME_CHARS_LENGTH * NUMBER_OF_LANGUAGES;
  static constexpr size_t NAMES_TOTAL_BYTES = NAME_BYTES_LENGTH * NUMBER_OF_LANGUAGES;

  static const IOS::ES::TicketReader INVALID_TICKET;
  static const IOS::ES::TMDReader INVALID_TMD;
  static const std::vector<u8> INVALID_CERT_CHAIN;
};

std::unique_ptr<VolumeDisc> CreateDisc(std::unique_ptr<BlobReader> reader);
std::unique_ptr<VolumeDisc> CreateDisc(const std::string& path);
// Like CreateDisc, but honours the "Load Games into Memory" setting.
std::unique_ptr<VolumeDisc> CreateDiscForCore(const std::string& path);

std::unique_ptr<VolumeWAD> CreateWAD(std::unique_ptr<BlobReader> reader);
std::unique_ptr<VolumeWAD> CreateWAD(const std::string& path);
std::unique_ptr<Volume> CreateVolume(std::unique_ptr<BlobReader> reader);
std::unique_ptr<Volume> CreateVolume(const std::string& path);

}  // namespace DiscIO
