// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/GameFile.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <pugixml.hpp>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/Image.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/Config/UISettings.h"
#include "Core/ConfigManager.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/TitleDatabase.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Enums.h"
#include "DiscIO/GameModDescriptor.h"
#include "DiscIO/Volume.h"
#include "DiscIO/WiiSaveBanner.h"

namespace UICommon
{
namespace
{
using Digest = Common::SHA1::Digest;

const std::string NO_STRING;

// Three-letter game ID prefixes of GameCube and Wii games released on two discs (kept sorted for
// the binary search below).
constexpr std::array<std::string_view, 30> TWO_DISC_PREFIXES = {
    "DBJ", "G3A", "G3Q", "G4B", "G5T", "GBI", "GBZ", "GC9", "GCD", "GCJ",
    "GFF", "GGI", "GGS", "GK4", "GK7", "GKB", "GL3", "GMX", "GNI", "GOY",
    "GQS", "GR8", "GRZ", "GTO", "GW4", "GWY", "S4M", "S4S", "S6T", "SDQ",
};
static_assert(std::ranges::is_sorted(TWO_DISC_PREFIXES));

bool IsTwoDiscGameID(std::string_view game_id)
{
  if (game_id.size() < 3)
    return false;
  return std::ranges::binary_search(TWO_DISC_PREFIXES, game_id.substr(0, 3));
}

bool CoversEnabled()
{
  return Config::Get(Config::MAIN_USE_GAME_COVERS);
}

// Splits the file path into its folder (with trailing separator) and its name without extension.
std::pair<std::string, std::string> FolderAndStem(const std::string& file_path)
{
  std::string folder;
  std::string stem;
  SplitPath(file_path, &folder, &stem, nullptr);
  return {std::move(folder), std::move(stem)};
}

// Reads a whole file that exists into `out`.
bool ReadIfExists(const std::string& path, std::string* out)
{
  return File::Exists(path) && File::ReadFileToString(path, *out);
}

Digest DigestOf(std::string_view text)
{
  return Common::SHA1::CalculateDigest(text);
}

Digest DigestOf(u32 number)
{
  // The value's bytes in host order.
  return Common::SHA1::CalculateDigest(Common::BitCastToArray<u8>(number));
}

std::optional<Digest> DigestOfFile(const std::string& path)
{
  std::string contents;
  if (!File::ReadFileToString(path, contents))
    return std::nullopt;
  return DigestOf(contents);
}

// Folds two optional digests together. A missing side leaves the other unchanged; otherwise each
// byte of `a` is XORed with the next byte (wrapping) of `b`.
std::optional<Digest> Combine(const std::optional<Digest>& a, const std::optional<Digest>& b)
{
  if (!a.has_value())
    return b;
  if (!b.has_value())
    return a;

  Digest mixed{};
  const std::size_t n = mixed.size();
  for (std::size_t i = 0; i < n; ++i)
    mixed[i] = static_cast<u8>((*a)[i] ^ (*b)[(i + 1) % n]);
  return mixed;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Language lookup

DiscIO::Language GameFile::ConfigLanguage() const
{
  return SConfig::GetInstance().GetLanguageAdjustedForRegion(DiscIO::IsWii(m_platform), m_region);
}

const std::string& GameFile::PickString(DiscIO::Language language,
                                        const std::map<DiscIO::Language, std::string>& strings)
{
  // The requested language, else English, else whichever language sorts first.
  if (const auto wanted = strings.find(language); wanted != strings.end())
    return wanted->second;

  if (language != DiscIO::Language::English)
  {
    if (const auto english = strings.find(DiscIO::Language::English); english != strings.end())
      return english->second;
  }

  return strings.empty() ? NO_STRING : strings.begin()->second;
}

const std::string&
GameFile::PickForConfig(const std::map<DiscIO::Language, std::string>& strings) const
{
  return PickString(ConfigLanguage(), strings);
}

// ---------------------------------------------------------------------------------------------
// Construction

GameFile::GameFile() = default;
GameFile::~GameFile() = default;

GameFile::GameFile(std::string path) : m_file_path(std::move(path))
{
  m_file_name = PathToFileName(m_file_path);

  // 1. Anything DiscIO can open as a volume: discs, compressed images and WADs.
  if (const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(m_file_path))
  {
    const DiscIO::BlobReader& blob = volume->GetBlobReader();

    m_platform = volume->GetVolumeType();
    m_short_names = volume->GetShortNames();
    m_long_names = volume->GetLongNames();
    m_short_makers = volume->GetShortMakers();
    m_long_makers = volume->GetLongMakers();
    m_descriptions = volume->GetDescriptions();
    m_region = volume->GetRegion();
    m_country = volume->GetCountry();
    m_blob_type = volume->GetBlobType();
    m_block_size = blob.GetBlockSize();
    m_compression_method = blob.GetCompressionMethod();
    m_file_size = volume->GetRawSize();
    m_volume_size = volume->GetDataSize();
    m_volume_size_type = volume->GetDataSizeType();
    m_is_datel_disc = volume->IsDatelDisc();
    m_is_nkit = volume->IsNKit();
    m_internal_name = volume->GetInternalName();
    m_game_id = volume->GetGameID();
    m_gametdb_id = volume->GetGameTDBID();
    m_title_id = volume->GetTitleID().value_or(0);
    m_maker_id = volume->GetMakerID();
    m_revision = volume->GetRevision().value_or(0);
    m_disc_number = volume->GetDiscNumber().value_or(0);
    m_is_two_disc_game = IsTwoDiscGameID(m_game_id);
    m_apploader_date = volume->GetApploaderDate();
    m_volume_banner.buffer = volume->GetBanner(&m_volume_banner.width, &m_volume_banner.height);
    m_valid = true;
  }

  // 2. A bare DOL or ELF executable, recognised by its extension.
  if (!IsValid() && IsExecutable())
  {
    m_valid = true;
    m_platform = DiscIO::Platform::ELFOrDOL;
    m_blob_type = DiscIO::BlobType::DIRECTORY;
    m_file_size = File::GetSize(m_file_path);
    m_volume_size = m_file_size;
    m_volume_size_type = DiscIO::DataSizeType::Accurate;
    m_is_datel_disc = false;
    m_is_nkit = false;
    m_game_id = SConfig::MakeGameID(m_file_name);
  }

  // 3. A game mod descriptor (.json) whose base game is itself listable.
  if (!IsValid() && LowerExtension() == ".json")
  {
    std::optional<DiscIO::GameModDescriptor> mod = DiscIO::ParseGameModDescriptorFile(m_file_path);
    if (!mod)
      return;

    const GameFile base(mod->base_file);
    if (!base.IsValid())
      return;

    m_valid = true;
    m_blob_type = DiscIO::BlobType::MOD_DESCRIPTOR;
    m_file_size = File::GetSize(m_file_path);
    m_long_names.emplace(DiscIO::Language::English, std::move(mod->display_name));
    if (!mod->maker.empty())
      m_long_makers.emplace(DiscIO::Language::English, std::move(mod->maker));

    m_internal_name = base.GetInternalName();
    m_game_id = base.GetGameID();
    m_gametdb_id = base.GetGameTDBID();
    m_title_id = base.GetTitleID();
    m_maker_id = base.GetMakerID();
    m_region = base.GetRegion();
    m_country = base.GetCountry();
    m_platform = base.GetPlatform();
    m_revision = base.GetRevision();
    m_disc_number = base.GetDiscNumber();
  }
}

bool GameFile::IsValid() const
{
  // WADs are only listed when they hold a channel.
  if (m_platform == DiscIO::Platform::WiiWAD)
    return m_valid && IOS::ES::IsChannel(m_title_id);
  return m_valid;
}

std::string GameFile::LowerExtension() const
{
  std::string extension;
  SplitPath(m_file_path, nullptr, nullptr, &extension);
  Common::ToLower(&extension);
  return extension;
}

bool GameFile::IsExecutable() const
{
  const std::string extension = LowerExtension();
  return extension == ".dol" || extension == ".elf";
}

// ---------------------------------------------------------------------------------------------
// Cache serialisation

void GameBanner::DoState(PointerWrap& p)
{
  p.Do(buffer);
  p.Do(width);
  p.Do(height);
}

void GameCover::DoState(PointerWrap& p)
{
  p.Do(buffer);
}

void GameFile::DoState(PointerWrap& p)
{
  // The field order is the cache file layout.
  p.Do(m_valid);
  p.Do(m_file_path);
  p.Do(m_file_name);

  p.Do(m_file_size);
  p.Do(m_volume_size);
  p.Do(m_volume_size_type);
  p.Do(m_is_datel_disc);
  p.Do(m_is_nkit);

  for (auto* strings : {&m_short_names, &m_long_names, &m_short_makers, &m_long_makers,
                        &m_descriptions})
  {
    p.Do(*strings);
  }
  p.Do(m_internal_name);
  p.Do(m_game_id);
  p.Do(m_gametdb_id);
  p.Do(m_title_id);
  p.Do(m_maker_id);

  p.Do(m_region);
  p.Do(m_country);
  p.Do(m_platform);
  p.Do(m_blob_type);
  p.Do(m_block_size);
  p.Do(m_compression_method);
  p.Do(m_revision);
  p.Do(m_disc_number);
  p.Do(m_is_two_disc_game);
  p.Do(m_apploader_date);

  p.Do(m_custom_name);
  p.Do(m_custom_description);
  p.Do(m_custom_maker);
  for (GameBanner* banner : {&m_volume_banner, &m_custom_banner})
    banner->DoState(p);
  for (GameCover* cover : {&m_default_cover, &m_custom_cover})
    cover->DoState(p);
}

// ---------------------------------------------------------------------------------------------
// Covers

bool GameFile::CustomCoverChanged()
{
  if (!m_custom_cover.buffer.empty() || !CoversEnabled())
    return false;

  // "<name>.cover.png" next to the game, or a shared "cover.png" in its folder.
  const auto [folder, stem] = FolderAndStem(m_file_path);
  std::string png;
  const bool found =
      ReadIfExists(folder + stem + ".cover.png", &png) || ReadIfExists(folder + "cover.png", &png);
  if (found)
    m_pending.custom_cover.buffer.assign(png.begin(), png.end());
  return found;
}

void GameFile::CustomCoverCommit()
{
  m_custom_cover = std::move(m_pending.custom_cover);
}

void GameFile::DownloadDefaultCover()
{
  if (!m_default_cover.buffer.empty() || !CoversEnabled() || m_gametdb_id.empty())
    return;

  const std::string target = File::GetUserPath(D_COVERCACHE_IDX) + DIR_SEP + m_gametdb_id + ".png";
  if (File::Exists(target))
    return;

  const std::string region =
      SConfig::GetInstance().GetGameTDBImageRegionCode(DiscIO::IsWii(GetPlatform()), m_region);
  const std::string url =
      fmt::format("https://art.gametdb.com/wii/cover/{}/{}.png", region, m_gametdb_id);

  Common::HttpRequest http;
  const Common::HttpRequest::Response reply = http.Get(url);
  if (reply)
    File::WriteStringToFile(target, std::string(reply->begin(), reply->end()));
}

bool GameFile::DefaultCoverChanged()
{
  if (!m_default_cover.buffer.empty() || !CoversEnabled())
    return false;

  // Whatever DownloadDefaultCover left in the cover cache; the read's result is not checked,
  // only whether any bytes came back.
  std::string png;
  File::ReadFileToString(File::GetUserPath(D_COVERCACHE_IDX) + DIR_SEP + m_gametdb_id + ".png",
                         png);
  if (png.empty())
    return false;

  m_pending.default_cover.buffer.assign(png.begin(), png.end());
  return true;
}

void GameFile::DefaultCoverCommit()
{
  m_default_cover = std::move(m_pending.default_cover);
}

// ---------------------------------------------------------------------------------------------
// Homebrew-style metadata and banners

bool GameFile::LoadXMLMetadata(const std::string& path)
{
  std::string xml;
  if (!File::ReadFileToString(path, xml))
    return false;

  // Parsing from memory sidesteps path encoding issues on Windows.
  pugi::xml_document document;
  if (!document.load_buffer(xml.data(), xml.size()))
    return false;

  // Only these three fields of the Homebrew Channel meta.xml are shown.
  const pugi::xml_node app = document.child("app");
  m_pending.custom_name = app.child("name").text().as_string();
  m_pending.custom_maker = app.child("coder").text().as_string();
  m_pending.custom_description = app.child("short_description").text().as_string();
  return true;
}

bool GameFile::XMLMetadataChanged()
{
  // "<name>.xml" next to the game, or the Homebrew Channel's "meta.xml" in its folder.
  const auto [folder, stem] = FolderAndStem(m_file_path);
  if (!LoadXMLMetadata(folder + stem + ".xml") && !LoadXMLMetadata(folder + "meta.xml"))
  {
    // Long-standing behaviour kept as is: with no XML, the staged custom banner is cleared
    // (not the staged name, maker or description).
    m_pending.custom_banner = {};
  }

  // Also kept as is: this only reports a change when all three fields differ.
  const bool name_differs = m_pending.custom_name != m_custom_name;
  const bool maker_differs = m_pending.custom_maker != m_custom_maker;
  const bool description_differs = m_pending.custom_description != m_custom_description;
  return name_differs && maker_differs && description_differs;
}

void GameFile::XMLMetadataCommit()
{
  m_custom_name = std::move(m_pending.custom_name);
  m_custom_description = std::move(m_pending.custom_description);
  m_custom_maker = std::move(m_pending.custom_maker);
}

bool GameFile::WiiBannerChanged()
{
  // A Wii banner comes from the game's save data, which may have appeared since the cache entry
  // was made. Only an empty banner is ever retried.
  if (!m_volume_banner.empty() || !DiscIO::IsWii(m_platform))
    return false;

  GameBanner& staged = m_pending.volume_banner;
  staged.buffer = DiscIO::WiiSaveBanner(m_title_id).GetBanner(&staged.width, &staged.height);
  return !staged.buffer.empty();
}

void GameFile::WiiBannerCommit()
{
  m_volume_banner = std::move(m_pending.volume_banner);
}

bool GameFile::LoadPNGBanner(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file)
    return false;

  std::vector<u8> png(file.GetSize());
  if (!file.ReadBytes(png.data(), png.size()))
    return false;

  GameBanner& staged = m_pending.custom_banner;
  Common::UniqueBuffer<u8> rgba;
  if (!Common::LoadPNG(png, &rgba, &staged.width, &staged.height))
    return false;

  // RGBA bytes to 0xAARRGGBB words. Kept bit for bit from the long-standing conversion: the top
  // byte is filled from the pixel's red byte, not its alpha byte.
  const std::size_t pixels = rgba.size() / 4;
  staged.buffer.resize(pixels);
  for (std::size_t i = 0; i < pixels; ++i)
  {
    const u8* px = rgba.data() + i * 4;
    staged.buffer[i] = (u32{px[0]} << 24) | (u32{px[0]} << 16) | (u32{px[1]} << 8) | px[2];
  }
  return true;
}

bool GameFile::LoadModDescriptorBanner()
{
  if (!IsModDescriptor())
    return false;

  const std::optional<DiscIO::GameModDescriptor> mod =
      DiscIO::ParseGameModDescriptorFile(m_file_path);
  return mod && LoadPNGBanner(mod->banner);
}

bool GameFile::CustomBannerChanged()
{
  // "<name>.png" next to the game, the Homebrew Channel's "icon.png", or a mod descriptor's own
  // banner; with none of them the plain volume banner is used again.
  const auto [folder, stem] = FolderAndStem(m_file_path);
  const bool found = LoadPNGBanner(folder + stem + ".png") || LoadPNGBanner(folder + "icon.png") ||
                     LoadModDescriptorBanner();
  if (!found)
    m_pending.custom_banner = {};

  return m_pending.custom_banner != m_custom_banner;
}

void GameFile::CustomBannerCommit()
{
  m_custom_banner = std::move(m_pending.custom_banner);
}

// ---------------------------------------------------------------------------------------------
// Display strings

const std::string& GameFile::GetName(const Core::TitleDatabase& title_database) const
{
  if (!m_custom_name.empty())
    return m_custom_name;

  if (!IsModDescriptor())
  {
    const std::string& listed = title_database.GetTitleName(m_gametdb_id, ConfigLanguage());
    if (!listed.empty())
      return listed;
  }

  return GetName(Variant::LongAndPossiblyCustom);
}

const std::string& GameFile::GetName(Variant variant) const
{
  if (variant == Variant::LongAndPossiblyCustom && !m_custom_name.empty())
    return m_custom_name;

  const std::string& from_volume =
      variant == Variant::ShortAndNotCustom ? GetShortName() : GetLongName();
  // The file name is the last resort.
  return from_volume.empty() ? m_file_name : from_volume;
}

const std::string& GameFile::GetMaker(Variant variant) const
{
  if (variant == Variant::LongAndPossiblyCustom && !m_custom_maker.empty())
    return m_custom_maker;

  const std::string& from_volume =
      variant == Variant::ShortAndNotCustom ? GetShortMaker() : GetLongMaker();
  if (!from_volume.empty())
    return from_volume;

  // Fall back to the company table, but only for six-character game IDs.
  return m_game_id.size() < 6 ? NO_STRING : DiscIO::GetCompanyFromID(m_maker_id);
}

const std::string& GameFile::GetDescription(Variant variant) const
{
  const bool use_custom = variant == Variant::LongAndPossiblyCustom && !m_custom_description.empty();
  return use_custom ? m_custom_description : PickForConfig(m_descriptions);
}

std::vector<DiscIO::Language> GameFile::GetLanguages() const
{
  // The languages that have a long name.
  std::vector<DiscIO::Language> result;
  result.reserve(m_long_names.size());
  for (const auto& entry : m_long_names)
    result.push_back(entry.first);
  return result;
}

std::string GameFile::GetNetPlayName(const Core::TitleDatabase& title_database) const
{
  const std::string& name = GetName(title_database);

  std::vector<std::string> details;
  if (!m_game_id.empty())
    details.push_back(m_game_id);
  if (m_revision != 0)
    details.push_back(fmt::format("Revision {}", m_revision));

  // Add the disc number from disc 2 on, unless the title already says which disc it is.
  const int disc = m_disc_number + 1;
  if (disc > 1)
  {
    std::string lowered = name;
    Common::ToLower(&lowered);
    const bool title_has_disc = lowered.contains(fmt::format("disc {}", disc)) ||
                                lowered.contains(fmt::format("disc{}", disc));
    if (!title_has_disc)
      details.push_back(fmt::format("Disc {}", disc));
  }

  if (details.empty())
    return name;
  return fmt::format("{} ({})", name, fmt::join(details, ", "));
}

// ---------------------------------------------------------------------------------------------
// NetPlay identity

std::array<u8, 20> GameFile::GetSyncHash() const
{
  std::optional<Digest> digest;

  if (m_platform == DiscIO::Platform::ELFOrDOL)
  {
    digest = DigestOfFile(m_file_path);
  }
  else if (IsModDescriptor())
  {
    const std::optional<DiscIO::GameModDescriptor> mod =
        DiscIO::ParseGameModDescriptorFile(m_file_path);
    if (mod)
    {
      const GameFile base(mod->base_file);
      if (base.IsValid())
        digest = base.GetSyncHash();

      // Fold in each Riivolution patch file and its selected options.
      if (mod->riivolution)
      {
        for (const DiscIO::GameModDescriptorRiivolutionPatch& patch : mod->riivolution->patches)
        {
          digest = Combine(digest, DigestOfFile(patch.xml));
          for (const DiscIO::GameModDescriptorRiivolutionPatchOption& option : patch.options)
          {
            digest = Combine(digest, DigestOf(option.section_name));
            digest = Combine(digest, DigestOf(option.option_id));
            digest = Combine(digest, DigestOf(option.option_name));
            digest = Combine(digest, DigestOf(option.choice));
          }
        }
      }
    }
  }
  else if (const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(m_file_path))
  {
    digest = volume->GetSyncHash();
  }

  return digest.value_or(Digest{});
}

NetPlay::SyncIdentifier GameFile::GetSyncIdentifier() const
{
  NetPlay::SyncIdentifier id{};
  id.dol_elf_size = m_platform == DiscIO::Platform::ELFOrDOL ? m_file_size : 0;
  id.game_id = m_game_id;
  id.revision = m_revision;
  id.disc_number = m_disc_number;
  id.is_datel = m_is_datel_disc;
  id.sync_hash = GetSyncHash();
  return id;
}

NetPlay::SyncIdentifierComparison
GameFile::CompareSyncIdentifier(const NetPlay::SyncIdentifier& sync_identifier) const
{
  using Result = NetPlay::SyncIdentifierComparison;

  const bool executable = m_platform == DiscIO::Platform::ELFOrDOL;
  const u64 own_executable_size = executable ? m_file_size : 0;
  const std::string& other_id = sync_identifier.game_id;

  if (own_executable_size != sync_identifier.dol_elf_size ||
      m_is_datel_disc != sync_identifier.is_datel || m_game_id.size() != other_id.size())
  {
    return Result::DifferentGame;
  }

  const bool structured_id =
      !executable && !m_is_datel_disc && m_game_id.size() >= 4 && m_game_id.size() <= 6;
  if (structured_id)
  {
    // Characters 0-2 name the game, 3 the region, 4-5 (if present) the maker. A different maker
    // usually means a re-release, so it counts as another revision.
    if (m_game_id.compare(0, 3, other_id, 0, 3) != 0)
      return Result::DifferentGame;
    if (m_game_id[3] != other_id[3])
      return Result::DifferentRegion;
    if (std::string_view(m_game_id.c_str() + 4) != std::string_view(other_id.c_str() + 4))
      return Result::DifferentRevision;
  }
  else if (m_game_id != other_id)
  {
    // Title IDs and generated IDs are compared whole.
    return Result::DifferentGame;
  }

  if (m_revision != sync_identifier.revision)
    return Result::DifferentRevision;
  if (m_disc_number != sync_identifier.disc_number)
    return Result::DifferentDiscNumber;

  if (GetSyncHash() == sync_identifier.sync_hash)
    return Result::SameGame;

  // Many Datel discs share one game ID, so for them a different hash means a different disc.
  return m_is_datel_disc ? Result::DifferentGame : Result::DifferentHash;
}

// ---------------------------------------------------------------------------------------------
// File details

std::string GameFile::GetWiiFSPath() const
{
  ASSERT(DiscIO::IsWii(m_platform));
  return Common::GetTitleDataPath(m_title_id, Common::FromWhichRoot::Configured);
}

bool GameFile::ShouldShowFileFormatDetails() const
{
  if (m_blob_type == DiscIO::BlobType::DRIVE || m_blob_type == DiscIO::BlobType::MOD_DESCRIPTOR)
    return false;

  // Any container other than a plain file is worth describing; a plain file only when it is a
  // disc image.
  if (m_blob_type != DiscIO::BlobType::PLAIN)
    return true;
  return m_platform != DiscIO::Platform::WiiWAD && m_platform != DiscIO::Platform::ELFOrDOL;
}

std::string GameFile::GetFileFormatName() const
{
  if (m_platform == DiscIO::Platform::WiiWAD)
    return "WAD";

  if (m_platform == DiscIO::Platform::ELFOrDOL)
  {
    // "DOL" or "ELF": the extension in capitals, without its dot.
    std::string extension = LowerExtension();
    Common::ToUpper(&extension);
    return extension.empty() ? extension : extension.substr(1);
  }

  std::string container = DiscIO::GetName(m_blob_type, true);
  if (!m_is_nkit)
    return container;
  return Common::FmtFormatT("{0} (NKit)", container);
}

bool GameFile::ShouldAllowConversion() const
{
  return DiscIO::IsDisc(m_platform) && m_volume_size_type == DiscIO::DataSizeType::Accurate &&
         !IsModDescriptor();
}

bool GameFile::IsModDescriptor() const
{
  return m_blob_type == DiscIO::BlobType::MOD_DESCRIPTOR;
}

const GameBanner& GameFile::GetBannerImage() const
{
  return m_custom_banner.empty() ? m_volume_banner : m_custom_banner;
}

const GameCover& GameFile::GetCoverImage() const
{
  return m_custom_cover.empty() ? m_default_cover : m_custom_cover;
}

}  // namespace UICommon
