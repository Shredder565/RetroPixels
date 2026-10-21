// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/SyncIdentifier.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"

class PointerWrap;

namespace Core
{
class TitleDatabase;
}

namespace UICommon
{
// A banner as 32-bit ARGB pixels, row by row.
struct GameBanner
{
  std::vector<u32> buffer;
  u32 width{};
  u32 height{};

  bool operator==(const GameBanner&) const = default;

  bool empty() const { return buffer.empty(); }
  void DoState(PointerWrap& p);
};

// A cover image, kept as the PNG file's bytes.
struct GameCover
{
  std::vector<u8> buffer;
  bool empty() const { return buffer.empty(); }
  void DoState(PointerWrap& p);
};

// What the game list knows about one file: a disc image, WAD, DOL/ELF or game mod descriptor.
// Instances are cached on disk by GameFileCache through DoState.
class GameFile final
{
public:
  enum class Variant
  {
    LongAndPossiblyCustom,
    LongAndNotCustom,
    ShortAndNotCustom,
  };

  GameFile();
  explicit GameFile(std::string path);
  ~GameFile();

  bool IsValid() const;
  const std::string& GetFilePath() const { return m_file_path; }
  const std::string& GetFileName() const { return m_file_name; }
  const std::string& GetName(const Core::TitleDatabase& title_database) const;
  const std::string& GetName(Variant variant) const;
  const std::string& GetMaker(Variant variant) const;
  const std::string& GetShortName(DiscIO::Language l) const { return PickString(l, m_short_names); }
  const std::string& GetShortName() const { return PickForConfig(m_short_names); }
  const std::string& GetLongName(DiscIO::Language l) const { return PickString(l, m_long_names); }
  const std::string& GetLongName() const { return PickForConfig(m_long_names); }
  const std::string& GetShortMaker(DiscIO::Language l) const
  {
    return PickString(l, m_short_makers);
  }
  const std::string& GetShortMaker() const { return PickForConfig(m_short_makers); }
  const std::string& GetLongMaker(DiscIO::Language l) const { return PickString(l, m_long_makers); }
  const std::string& GetLongMaker() const { return PickForConfig(m_long_makers); }
  const std::string& GetDescription(DiscIO::Language l) const
  {
    return PickString(l, m_descriptions);
  }
  const std::string& GetDescription(Variant variant) const;
  std::vector<DiscIO::Language> GetLanguages() const;
  const std::string& GetInternalName() const { return m_internal_name; }
  const std::string& GetGameID() const { return m_game_id; }
  const std::string& GetGameTDBID() const { return m_gametdb_id; }
  u64 GetTitleID() const { return m_title_id; }
  const std::string& GetMakerID() const { return m_maker_id; }
  u16 GetRevision() const { return m_revision; }
  // Zero-based: 0 is disc 1.
  u8 GetDiscNumber() const { return m_disc_number; }
  bool IsTwoDiscGame() const { return m_is_two_disc_game; }
  std::string GetNetPlayName(const Core::TitleDatabase& title_database) const;

  // The next three read the whole file, so they are slow.
  std::array<u8, 20> GetSyncHash() const;
  NetPlay::SyncIdentifier GetSyncIdentifier() const;
  // Only reads the file when everything cheaper to compare matches.
  NetPlay::SyncIdentifierComparison
  CompareSyncIdentifier(const NetPlay::SyncIdentifier& sync_identifier) const;

  std::string GetWiiFSPath() const;
  DiscIO::Region GetRegion() const { return m_region; }
  DiscIO::Country GetCountry() const { return m_country; }
  DiscIO::Platform GetPlatform() const { return m_platform; }
  DiscIO::BlobType GetBlobType() const { return m_blob_type; }
  u64 GetBlockSize() const { return m_block_size; }
  const std::string& GetCompressionMethod() const { return m_compression_method; }
  bool ShouldShowFileFormatDetails() const;
  std::string GetFileFormatName() const;
  bool ShouldAllowConversion() const;
  const std::string& GetApploaderDate() const { return m_apploader_date; }
  u64 GetFileSize() const { return m_file_size; }
  u64 GetVolumeSize() const { return m_volume_size; }
  DiscIO::DataSizeType GetVolumeSizeType() const { return m_volume_size_type; }
  bool IsDatelDisc() const { return m_is_datel_disc; }
  bool IsNKit() const { return m_is_nkit; }
  bool IsModDescriptor() const;
  const GameBanner& GetBannerImage() const;
  const GameCover& GetCoverImage() const;
  void DoState(PointerWrap& p);

  // GameFileCache refreshes optional metadata in two steps: a *Changed call loads the current
  // data into a staging area and says whether it differs, and the *Commit call adopts it.
  bool XMLMetadataChanged();
  void XMLMetadataCommit();
  bool WiiBannerChanged();
  void WiiBannerCommit();
  bool CustomBannerChanged();
  void CustomBannerCommit();
  void DownloadDefaultCover();
  bool DefaultCoverChanged();
  void DefaultCoverCommit();
  bool CustomCoverChanged();
  void CustomCoverCommit();

private:
  DiscIO::Language ConfigLanguage() const;
  static const std::string& PickString(DiscIO::Language language,
                                       const std::map<DiscIO::Language, std::string>& strings);
  const std::string& PickForConfig(const std::map<DiscIO::Language, std::string>& strings) const;
  std::string LowerExtension() const;
  bool IsExecutable() const;
  bool LoadXMLMetadata(const std::string& path);
  bool LoadPNGBanner(const std::string& path);
  bool LoadModDescriptorBanner();

  // Everything below up to m_pending is written by DoState. Changing what is stored needs a
  // matching DoState change and a new CACHE_REVISION in GameFileCache.cpp.

  bool m_valid{};
  std::string m_file_path;
  std::string m_file_name;

  u64 m_file_size{};
  u64 m_volume_size{};
  DiscIO::DataSizeType m_volume_size_type{};
  bool m_is_datel_disc{};
  bool m_is_nkit{};

  std::map<DiscIO::Language, std::string> m_short_names;
  std::map<DiscIO::Language, std::string> m_long_names;
  std::map<DiscIO::Language, std::string> m_short_makers;
  std::map<DiscIO::Language, std::string> m_long_makers;
  std::map<DiscIO::Language, std::string> m_descriptions;
  std::string m_internal_name;
  std::string m_game_id;
  std::string m_gametdb_id;
  u64 m_title_id{};
  std::string m_maker_id;

  DiscIO::Region m_region{DiscIO::Region::Unknown};
  DiscIO::Country m_country{DiscIO::Country::Unknown};
  DiscIO::Platform m_platform{};
  DiscIO::BlobType m_blob_type{};
  u64 m_block_size{};
  std::string m_compression_method{};
  u16 m_revision{};
  u8 m_disc_number{};
  bool m_is_two_disc_game{};
  std::string m_apploader_date;

  std::string m_custom_name;
  std::string m_custom_description;
  std::string m_custom_maker;
  GameBanner m_volume_banner{};
  GameBanner m_custom_banner{};
  GameCover m_default_cover{};
  GameCover m_custom_cover{};

  // Staging area for the *Changed/*Commit pairs, so GameFileCache can build an updated copy
  // without touching one that other threads may be reading. Never cached.
  struct
  {
    std::string custom_name;
    std::string custom_description;
    std::string custom_maker;
    GameBanner volume_banner;
    GameBanner custom_banner;
    GameCover default_cover;
    GameCover custom_cover;
  } m_pending{};
};

}  // namespace UICommon
