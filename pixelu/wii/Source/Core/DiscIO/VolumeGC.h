// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Lazy.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

namespace DiscIO
{
class BlobReader;
enum class BlobType;
enum class Country;
class FileSystem;
enum class Language;
enum class Region;
enum class Platform;

// A GameCube disc image (including Triforce arcade discs, which are GameCube discs with a
// boot.id file).
class VolumeGC final : public VolumeDisc
{
public:
  VolumeGC(std::unique_ptr<BlobReader> reader);
  ~VolumeGC() override;
  bool Read(u64 offset, u64 length, u8* buffer,
            const Partition& partition = PARTITION_NONE) const override;
  const FileSystem* GetFileSystem(const Partition& partition = PARTITION_NONE) const override;
  std::string GetGameTDBID(const Partition& partition = PARTITION_NONE) const override;
  std::map<Language, std::string> GetShortNames() const override;
  std::map<Language, std::string> GetLongNames() const override;
  std::map<Language, std::string> GetShortMakers() const override;
  std::map<Language, std::string> GetLongMakers() const override;
  std::map<Language, std::string> GetDescriptions() const override;
  std::vector<u32> GetBanner(u32* width, u32* height) const override;

  Platform GetVolumeType() const override;
  bool IsDatelDisc() const override;
  Region GetRegion() const override;
  BlobType GetBlobType() const override;
  u64 GetDataSize() const override;
  DataSizeType GetDataSizeType() const override;
  u64 GetRawSize() const override;
  const BlobReader& GetBlobReader() const override;
  std::array<u8, 20> GetSyncHash() const override;

  // The boot.id contents. Only meaningful when GetVolumeType() is Platform::Triforce.
  const BootID* GetTriforceBootID() const { return &m_triforce_header; }

private:
  // opening.bnr: "BNR1" (one language, NTSC) or "BNR2" (six languages, PAL), a 96x32 RGB5A3
  // image and the text fields.
  static constexpr u32 BANNER_WIDTH = 96;
  static constexpr u32 BANNER_HEIGHT = 32;
  static constexpr size_t BANNER_LANGUAGES = 6;

  struct BannerText
  {
    char short_name[32];
    char short_maker[32];
    char long_name[64];
    char long_maker[64];
    char description[128];
  };

  struct BannerFile
  {
    u32 magic;
    u32 reserved[7];
    u16 pixels[BANNER_WIDTH * BANNER_HEIGHT];
    BannerText text[BANNER_LANGUAGES];
  };

  static constexpr size_t BNR1_SIZE = sizeof(BannerFile) - sizeof(BannerText) * 5;
  static constexpr size_t BNR2_SIZE = sizeof(BannerFile);

  // The banner after decoding.
  struct DecodedBanner
  {
    DecodedBanner();
    ~DecodedBanner();

    std::map<Language, std::string> short_names;
    std::map<Language, std::string> long_names;
    std::map<Language, std::string> short_makers;
    std::map<Language, std::string> long_makers;
    std::map<Language, std::string> descriptions;

    std::vector<u32> pixels;
    u32 width = 0;
    u32 height = 0;
  };

  DecodedBanner ReadBanner() const;
  DecodedBanner DecodeBanner(const BannerFile& file, size_t languages) const;

  std::unique_ptr<BlobReader> m_reader;
  Common::Lazy<std::unique_ptr<FileSystem>> m_file_system;
  Common::Lazy<DecodedBanner> m_banner;
  BootID m_triforce_header{};
  bool m_is_triforce = false;
};

}  // namespace DiscIO
