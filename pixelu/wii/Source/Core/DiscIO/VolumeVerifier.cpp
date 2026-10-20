// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/VolumeVerifier.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <mbedtls/md5.h>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <pugixml.hpp>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MinizipUtil.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Version.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscScrubber.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeWii.h"

namespace DiscIO
{
namespace
{
constexpr u64 DEFAULT_READ_SIZE = 0x20000;

std::string DatfileCachePath(const std::string& system)
{
  return File::GetUserPath(D_REDUMPCACHE_IDX) + DIR_SEP + system + ".zip";
}

// Value of one hex digit, or 0 for anything else.
u8 HexDigitValue(char c)
{
  if (c >= '0' && c <= '9')
    return static_cast<u8>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<u8>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F')
    return static_cast<u8>(c - 'A' + 10);
  return 0;
}

// A hex string as bytes. An odd final digit is ignored.
std::vector<u8> HexToHash(std::string_view text)
{
  std::vector<u8> hash(text.size() / 2);
  for (size_t i = 0; i < hash.size(); ++i)
    hash[i] = static_cast<u8>(HexDigitValue(text[2 * i]) << 4 | HexDigitValue(text[2 * i + 1]));
  return hash;
}

// An empty calculated hash means "not calculated" and matches anything.
bool HashMatches(const std::vector<u8>& calculated, const std::vector<u8>& expected)
{
  return calculated.empty() || calculated == expected;
}

using ESCore = IOS::HLE::ESCore;

bool IsSignedCorrectly(ESCore& es, ESCore::VerifyContainerType type,
                       const IOS::ES::SignedBlobReader& blob, const std::vector<u8>& cert_chain)
{
  return es.VerifyContainer(type, ESCore::VerifyMode::DoNotUpdateCertStore, blob, cert_chain) ==
         IOS::HLE::IPC_SUCCESS;
}
}  // namespace

RedumpVerifier::DownloadState RedumpVerifier::s_gc_download;
RedumpVerifier::DownloadState RedumpVerifier::s_wii_download;

void RedumpVerifier::Start(const Volume& volume)
{
  // Redump lists games by four-character ID. Datel discs have none and are matched by serial.
  if (!volume.IsDatelDisc())
    m_game_id = volume.GetGameID().substr(0, 4);

  m_revision = volume.GetRevision().value_or(0);
  m_disc_number = volume.GetDiscNumber().value_or(0);
  m_size = volume.GetDataSize();

  const Platform platform = volume.GetVolumeType();
  m_candidates = std::async(std::launch::async, [this, platform]() -> std::vector<Candidate> {
    if (platform == Platform::GameCubeDisc)
      return LoadCandidates("gc", &s_gc_download);
    if (platform == Platform::WiiDisc)
      return LoadCandidates("wii", &s_wii_download);
    m_result.status = Status::Error;
    return {};
  });
}

std::vector<RedumpVerifier::Candidate> RedumpVerifier::LoadCandidates(const std::string& system,
                                                                      DownloadState* state)
{
  DownloadStatus status;
  {
    std::lock_guard lock(state->mutex);
    state->status = Download(system, state->status);
    status = state->status;
  }

  if (status == DownloadStatus::FailButOldCacheAvailable)
    ERROR_LOG_FMT(DISCIO, "Failed to fetch data from Redump.info, using old cached data instead");
  else if (status != DownloadStatus::Success)
  {
    m_result = {Status::Error, Common::GetStringT("Failed to connect to Redump.info")};
    return {};
  }
  return FindCandidates(ReadCachedDatfile(system), system);
}

RedumpVerifier::DownloadStatus RedumpVerifier::Download(const std::string& system,
                                                        DownloadStatus previous)
{
  // One successful download per session is enough.
  if (previous == DownloadStatus::Success)
    return previous;

  const std::string cache_path = DatfileCachePath(system);
  const auto failure = [&cache_path] {
    return File::Exists(cache_path) ? DownloadStatus::FailButOldCacheAvailable :
                                      DownloadStatus::Fail;
  };

  Common::HttpRequest request;
  const Common::HttpRequest::Response response =
      request.Get("https://redump.info/datfile/" + system + "/serial,version",
                  {{"User-Agent", Common::GetEmulatorName()}});
  if (!response)
    return failure();

  // An error page instead of the zip file.
  if (response->size() > 1 && (*response)[0] == '<' && (*response)[1] == '!')
    return failure();

  File::CreateFullPath(cache_path);
  if (!File::IOFile(cache_path, "wb").WriteBytes(response->data(), response->size()))
    ERROR_LOG_FMT(DISCIO, "Failed to write downloaded datfile to {}", cache_path);
  return DownloadStatus::Success;
}

std::vector<u8> RedumpVerifier::ReadCachedDatfile(const std::string& system)
{
  void* reader = mz_zip_reader_create();
  if (!reader)
    return {};
  Common::ScopeGuard delete_reader{[&reader] { mz_zip_reader_delete(&reader); }};

  if (mz_zip_reader_open_file(reader, DatfileCachePath(system).c_str()) != MZ_OK)
    return {};

  // The zip must hold exactly one entry: the datfile.
  if (mz_zip_reader_goto_first_entry(reader) != MZ_OK ||
      mz_zip_reader_goto_next_entry(reader) != MZ_END_OF_LIST ||
      mz_zip_reader_goto_first_entry(reader) != MZ_OK)
  {
    return {};
  }

  mz_zip_file* info;
  mz_zip_reader_entry_get_info(reader, &info);
  std::vector<u8> datfile(info->uncompressed_size);
  if (!Common::ReadFileFromZip(reader, datfile.data(), info->uncompressed_size))
    return {};
  return datfile;
}

bool RedumpVerifier::SerialListMatches(const std::string& serials) const
{
  // Serials look like "DL-DOL-GALE-USA" or "RVL-RSBE-USA" (sometimes with a disc number after the
  // ID, e.g. "DL-DOL-GZ2E-0-USA"), and a game can list several, separated by commas.
  for (const std::string& entry : SplitString(serials, ','))
  {
    const std::string_view serial = StripWhitespace(entry);
    const size_t dash = serial.find_first_of('-', 3);
    if (dash == std::string_view::npos || serial.size() < dash + 1 + 4)
    {
      ERROR_LOG_FMT(DISCIO, "Invalid serial in redump datfile: {}", entry);
      continue;
    }

    const size_t id_start = dash + 1;
    if (serial.substr(id_start, 4) != m_game_id)
      continue;

    const size_t disc_position = id_start + 5;
    u8 disc = 0;
    if (serial.size() > disc_position && serial[disc_position] >= '0' &&
        serial[disc_position] <= '9')
    {
      disc = static_cast<u8>(serial[disc_position] - '0');
    }
    if (disc == m_disc_number)
      return true;
  }
  return false;
}

std::vector<RedumpVerifier::Candidate> RedumpVerifier::FindCandidates(std::span<const u8> datfile,
                                                                      const std::string& system)
{
  pugi::xml_document document;
  if (!document.load_buffer(datfile.data(), datfile.size()))
  {
    m_result = {Status::Error, Common::GetStringT("Failed to parse Redump.info data")};
    return {};
  }

  std::vector<Candidate> candidates;
  bool any_serial = false;
  bool any_version = false;
  for (const pugi::xml_node game : document.child("datafile").children("game"))
  {
    // Only the trailing number counts ("v1.02" is 2, "Rev 2" is 2).
    std::string version_text = game.child("version").text().as_string();
    any_version |= !version_text.empty();
    const size_t prefix_end = version_text.find_last_not_of("0123456789");
    if (prefix_end != std::string::npos)
      version_text.erase(0, prefix_end + 1);
    const int version = version_text.empty() ? 0 : std::stoi(version_text);

    const std::string serials = game.child("serial").text().as_string();
    any_serial |= !serials.empty();

    // Korean GameCube revisions start at 0x30 on the disc but at 0 in Redump.
    if (version % 0x30 != m_revision % 0x30)
      continue;

    // Datel discs have no serial (GameCube) or a "DS..." serial (Wii); they can only be
    // candidates when the verified disc is a Datel disc too.
    const bool datel_entry = serials.empty() || serials.starts_with("DS");
    if (datel_entry ? !m_game_id.empty() : !SerialListMatches(serials))
      continue;

    const pugi::xml_node rom = game.child("rom");
    Candidate& candidate = candidates.emplace_back();
    candidate.size = rom.attribute("size").as_ullong();
    candidate.hashes.crc32 = HexToHash(rom.attribute("crc").value());
    candidate.hashes.md5 = HexToHash(rom.attribute("md5").value());
    candidate.hashes.sha1 = HexToHash(rom.attribute("sha1").value());
  }

  if (!any_serial || !any_version)
  {
    // Most likely a datfile that the user downloaded by hand without the needed fields.
    // i18n: "Serial" means a product serial number such as RVL-RSBE-USA.
    PanicAlertFmtT(
        "Serial and/or version data is missing from {0}\n"
        "Please append \"{1}\" (without the quotes) to the datfile URL when downloading\n"
        "Example: {2}",
        DatfileCachePath(system), "serial,version", "http://redump.info/datfile/gc/serial,version");
    m_result = {Status::Error, Common::GetStringT("Failed to parse Redump.info data")};
    return {};
  }
  return candidates;
}

RedumpVerifier::Result RedumpVerifier::Finish(const Hashes<std::vector<u8>>& hashes)
{
  // Let the background task finish first, so that a failure it reports is never missed.
  if (m_candidates.valid())
    m_candidates.wait();

  if (m_result.status == Status::Error)
    return m_result;
  if (hashes.crc32.empty() && hashes.md5.empty() && hashes.sha1.empty())
    return m_result;

  const std::vector<Candidate> candidates = m_candidates.get();
  const bool any_match = std::ranges::any_of(candidates, [&](const Candidate& candidate) {
    return HashMatches(hashes.crc32, candidate.hashes.crc32) &&
           HashMatches(hashes.md5, candidate.hashes.md5) &&
           HashMatches(hashes.sha1, candidate.hashes.sha1) && m_size == candidate.size;
  });
  if (any_match)
    return {Status::GoodDump, Common::GetStringT("Good dump")};

  // Only call it a bad dump when Redump knows this disc. A Datel dump that matches nothing may
  // just be missing from Redump (or use different filler for unreadable sectors).
  if (!candidates.empty() && !m_game_id.empty())
    return {Status::BadDump, Common::GetStringT("Bad dump")};

  return {Status::Unknown, Common::GetStringT("Unknown disc")};
}

VolumeVerifier::VolumeVerifier(const Volume& volume, bool redump_verification,
                               Hashes<bool> hashes_to_calculate)
    : m_volume(volume), m_redump_verification(redump_verification),
      m_hashes_to_calculate(hashes_to_calculate),
      m_calculating_any_hash(hashes_to_calculate.crc32 || hashes_to_calculate.md5 ||
                             hashes_to_calculate.sha1),
      m_max_progress(volume.GetDataSize()), m_data_size_type(volume.GetDataSizeType())
{
  // A Redump comparison needs at least one hash.
  m_redump_verification = m_redump_verification && m_calculating_any_hash;
}

VolumeVerifier::~VolumeVerifier()
{
  WaitForAsyncOperations();
}

Hashes<bool> VolumeVerifier::GetDefaultHashesToCalculate()
{
  // With hardware support for CRC32 or SHA-1, only the fast hashes are on by default. SHA-1 always
  // is, so that CRC32 is never the only one; MD5 is never accelerated.
  const bool fast_sha1 = Common::SHA1::CreateContext()->HwAccelerated();
  const bool fast_crc32 = cpu_info.bCRC32;
  if (!fast_crc32 && !fast_sha1)
    return {.crc32 = true, .md5 = true, .sha1 = true};
  return {.crc32 = fast_crc32, .md5 = false, .sha1 = true};
}

void VolumeVerifier::Start()
{
  ASSERT(!m_started);
  m_started = true;

  if (m_redump_verification)
    m_redump_verifier.Start(m_volume);

  const Platform platform = m_volume.GetVolumeType();
  m_is_tgc = m_volume.GetBlobType() == BlobType::TGC;
  m_is_datel = m_volume.IsDatelDisc();
  m_is_triforce = platform == Platform::Triforce;
  m_is_not_retail = (platform == Platform::WiiDisc && !m_volume.HasWiiHashes()) || IsDebugSigned();

  const std::vector<Partition> partitions = CheckPartitions();
  if (IsDisc(platform))
    m_biggest_referenced_offset = GetBiggestReferencedOffset(m_volume, partitions);

  CheckMisc();
  SetUpHashing();
}

std::vector<Partition> VolumeVerifier::CheckPartitions()
{
  if (m_volume.GetVolumeType() == Platform::WiiWAD)
    return {};

  const std::vector<Partition> partitions = m_volume.GetPartitions();
  if (partitions.empty())
  {
    // GameCube-style volume: just the one file system.
    if (m_volume.GetFileSystem(m_volume.GetGamePartition()))
      return {m_volume.GetGamePartition()};
    AddProblem(Severity::High,
               Common::GetStringT("The filesystem is invalid or could not be read."));
    return {};
  }

  CheckPartitionTable(partitions);

  std::vector<Partition> usable;
  std::ranges::copy_if(partitions, std::back_inserter(usable),
                       [this](const Partition& partition) { return CheckPartition(partition); });
  return usable;
}

void VolumeVerifier::CheckPartitionTable(const std::vector<Partition>& partitions)
{
  // No released disc has more than 8 partitions in the first table (Brawl puts its many
  // Masterpiece partitions in the second one).
  const std::optional<u32> first_table_count = m_volume.ReadSwapped<u32>(0x40000, PARTITION_NONE);
  if (first_table_count && *first_table_count > 8)
  {
    AddProblem(Severity::Low,
               Common::GetStringT("There are too many partitions in the first partition table."));
  }

  std::vector<u32> types;
  for (const Partition& partition : partitions)
  {
    if (const std::optional<u32> type = m_volume.GetPartitionType(partition))
      types.push_back(*type);
  }
  const auto has_type = [&types](u32 type) { return Common::Contains(types, type); };

  if (!has_type(PARTITION_UPDATE))
    AddProblem(Severity::Low, Common::GetStringT("The update partition is missing."));

  if (!m_is_datel && !has_type(PARTITION_DATA))
    AddProblem(Severity::High, Common::GetStringT("The data partition is missing."));

  const bool has_channel = has_type(PARTITION_CHANNEL);
  if (ShouldHaveChannelPartition() && !has_channel)
    AddProblem(Severity::Medium, Common::GetStringT("The channel partition is missing."));

  const bool has_install = has_type(PARTITION_INSTALL);
  if (ShouldHaveInstallPartition() && !has_install)
    AddProblem(Severity::High, Common::GetStringT("The install partition is missing."));

  if (ShouldHaveMasterpiecePartitions() &&
      std::ranges::none_of(types, [](u32 type) { return type >= 0xFF; }))
  {
    // i18n: "Masterpieces" is the Super Smash Bros. Brawl mode with trial versions of NES, SNES
    // and N64 games. Use the game's own name for it: 名作トライアル (ja), Meisterstücke (de),
    // Chefs-d'œuvre (fr), Clásicos (es), Capolavori (it), 클래식 게임 체험판 (ko). For other
    // languages, keeping the English name helps players recognise it.
    AddProblem(Severity::Medium, Common::GetStringT("The Masterpiece partitions are missing."));
  }

  // Partitions that are not where retail discs have them change the emulated seek times.
  const u64 usual_data_offset = m_volume.HasWiiHashes() ? 0xF800000 : 0x838000;
  for (const Partition& partition : partitions)
  {
    const std::optional<u32> type = m_volume.GetPartitionType(partition);
    if (type == PARTITION_UPDATE && partition.offset != 0x50000)
    {
      AddProblem(Severity::Low,
                 Common::GetStringT("The update partition is not at its normal position."));
    }
    if (type == PARTITION_DATA && partition.offset != usual_data_offset && !has_channel &&
        !has_install)
    {
      AddProblem(Severity::Low,
                 Common::GetStringT(
                     "The data partition is not at its normal position. This will affect the "
                     "emulated loading times. You will be unable to share input recordings and use "
                     "NetPlay with anyone who is using a good dump."));
    }
  }
}

bool VolumeVerifier::CheckPartition(const Partition& partition)
{
  const std::optional<u32> type = m_volume.GetPartitionType(partition);
  if (!type)
  {
    AddProblem(Severity::Medium, Common::GetStringT("The type of a partition could not be read."));
    return false;
  }

  // How bad a broken partition is depends on what it is for.
  Severity severity = Severity::Medium;
  if (*type == PARTITION_DATA || *type == PARTITION_INSTALL)
    severity = Severity::High;
  else if (*type == PARTITION_UPDATE)
    severity = Severity::Low;

  const std::string name = GetPartitionName(type);

  if (partition.offset % VolumeWii::BLOCK_TOTAL_SIZE != 0 ||
      m_volume.PartitionOffsetToRawOffset(0, partition) % VolumeWii::BLOCK_TOTAL_SIZE != 0)
  {
    AddProblem(Severity::Medium,
               Common::FmtFormatT("The {0} partition is not properly aligned.", name));
  }

  // The partition's own disc header must be readable and carry the Wii magic. The one allowed
  // exception is a blank update partition filled with the counting pattern 0, 4, 8, ... (seen on
  // discs made without updates, e.g. the Movie-Ch Install Disc).
  std::array<u8, 0x80> header;
  bool readable = m_volume.Read(0, header.size(), header.data(), partition);
  bool blank = false;
  if (readable && Common::swap32(header.data() + 0x18) != WII_DISC_MAGIC)
  {
    for (u32 i = 0; i < header.size() && readable; i += 4)
      readable = Common::swap32(header.data() + i) == i;
    blank = readable;
  }
  if (!readable)
  {
    // Seen when WBFS tools blank Brawl's Masterpiece partitions but keep them in the table.
    AddProblem(severity,
               Common::FmtFormatT("The {0} partition does not seem to contain valid data.", name));
    return false;
  }

  if (!m_is_datel)
  {
    IOS::HLE::Kernel ios(IsDebugSigned() ? IOS::HLE::IOSC::ConsoleType::RVT :
                                           IOS::HLE::IOSC::ConsoleType::Retail);
    ESCore& es = ios.GetESCore();
    const std::vector<u8>& cert_chain = m_volume.GetCertificateChain(partition);
    if (!IsSignedCorrectly(es, ESCore::VerifyContainerType::Ticket, m_volume.GetTicket(partition),
                           cert_chain) ||
        !IsSignedCorrectly(es, ESCore::VerifyContainerType::TMD, m_volume.GetTMD(partition),
                           cert_chain))
    {
      AddProblem(Severity::Low,
                 Common::FmtFormatT("The {0} partition is not correctly signed.", name));
    }
  }

  if (m_volume.HasWiiHashes())
  {
    if (!m_volume.CheckH3TableIntegrity(partition))
    {
      AddProblem(Severity::Low,
                 Common::FmtFormatT("The H3 hash table for the {0} partition is not correct.",
                                    name));
    }

    // Queue the partition's hash groups for Process().
    const u64 data_size =
        m_volume.ReadSwappedAndShifted(partition.offset + 0x2bc, PARTITION_NONE).value_or(0);
    if (data_size % VolumeWii::BLOCK_TOTAL_SIZE != 0)
    {
      AddProblem(Severity::Low,
                 Common::FmtFormatT("The data size for the {0} partition is not evenly divisible "
                                    "by the block size.",
                                    name));
    }

    const size_t block_count = static_cast<size_t>(data_size / VolumeWii::BLOCK_TOTAL_SIZE);
    const u64 first_raw_offset = m_volume.PartitionOffsetToRawOffset(0, partition);
    for (size_t first = 0; first < block_count; first += VolumeWii::BLOCKS_PER_GROUP)
    {
      const u64 group_number = first / VolumeWii::BLOCKS_PER_GROUP;
      m_groups.push_back(HashGroupJob{
          partition, first_raw_offset + group_number * VolumeWii::GROUP_TOTAL_SIZE, first,
          std::min<size_t>(first + VolumeWii::BLOCKS_PER_GROUP, block_count)});
    }
    m_block_errors.emplace(partition, 0);
  }

  if (blank)
    return false;

  const FileSystem* filesystem = m_volume.GetFileSystem(partition);
  if (!filesystem)
  {
    // Datel's Wii Freeloader ships an invalid FST in its only partition.
    if (m_is_datel)
      return true;
    AddProblem(severity,
               Common::FmtFormatT("The {0} partition does not have a valid file system.", name));
    return false;
  }

  if (*type == PARTITION_UPDATE)
    CheckUpdatePartitionIOS(*filesystem);

  if (!m_volume.GetTicket(partition).IsValid())
  {
    AddProblem(severity,
               // i18n: A "ticket" is the signed licence that allows a title (e.g. a game) to run.
               Common::FmtFormatT("The {0} partition does not have a valid ticket.", name));
  }
  if (!m_volume.GetTMD(partition).IsValid())
  {
    AddProblem(severity, Common::FmtFormatT("The {0} partition does not have a valid TMD.", name));
  }
  return true;
}

void VolumeVerifier::CheckUpdatePartitionIOS(const FileSystem& update_filesystem)
{
  const IOS::ES::TMDReader& game_tmd = m_volume.GetTMD(m_volume.GetGamePartition());
  if (!game_tmd.IsValid())
    return;

  // IOS9 exists on every Wii whatever its update state, so games may rely on it without shipping
  // it (Mario Party 8 revision 2 does). Otherwise the update partition should contain the IOS;
  // when it does not, the partition was likely swapped for an old one to avoid updates.
  const u64 ios = game_tmd.GetIOSId() & 0xFF;
  bool found = ios == 9;
  if (!found)
  {
    if (const std::unique_ptr<FileInfo> sys = update_filesystem.FindFileInfo("_sys"))
    {
      const std::string number = std::to_string(ios);
      const std::string prefix =
          IsDebugSigned() ? "firmware.64." + number + "." : "ios" + number + "-";
      found = std::ranges::any_of(*sys, [&prefix](const FileInfo& file) {
        std::string file_name = file.GetName();
        Common::ToLower(&file_name);
        return file_name.starts_with(prefix);
      });
    }
  }

  if (!found)
  {
    AddProblem(Severity::Low,
               Common::GetStringT(
                   "The update partition does not contain the IOS used by this title."));
  }
}

std::string VolumeVerifier::GetPartitionName(std::optional<u32> type) const
{
  if (!type)
    return "???";

  const std::string name = NameForPartitionType(*type, false);
  if (!ShouldHaveMasterpiecePartitions() || *type <= 0xFF)
    return name;

  // i18n: One trial game of the Super Smash Bros. Brawl "Masterpieces" mode (so singular). The
  // game calls the mode 名作トライアル (ja), Meisterstücke (de), Chefs-d'œuvre (fr), Clásicos (es),
  // Capolavori (it), 클래식 게임 체험판 (ko). For other languages, keeping the English name helps
  // players recognise it.
  return Common::FmtFormatT("{0} (Masterpiece)", name);
}

bool VolumeVerifier::IsDebugSigned() const
{
  const IOS::ES::TicketReader& ticket = m_volume.GetTicket(m_volume.GetGamePartition());
  return ticket.IsValid() && ticket.GetConsoleType() == IOS::HLE::IOSC::ConsoleType::RVT;
}

bool VolumeVerifier::ShouldHaveChannelPartition() const
{
  // Discs that install a channel (Wii Fit, Wii Fit Plus, Rayman RR2 and Mario Kart Wii).
  static constexpr std::array<std::string_view, 18> IDS = {
      "RFNE01", "RFNJ01", "RFNK01", "RFNP01", "RFNW01", "RFPE01", "RFPJ01", "RFPK01", "RFPP01",
      "RFPW01", "RGWE41", "RGWJ41", "RGWP41", "RGWX41", "RMCE01", "RMCJ01", "RMCK01", "RMCP01",
  };
  static_assert(std::ranges::is_sorted(IDS));
  return std::ranges::binary_search(IDS, m_volume.GetGameID());
}

bool VolumeVerifier::ShouldHaveInstallPartition() const
{
  // Dragon Quest X.
  static constexpr std::array<std::string_view, 4> IDS = {"S4MJGD", "S4SJGD", "S6TJGD", "SDQJGD"};
  return Common::Contains(IDS, std::string_view(m_volume.GetGameID()));
}

bool VolumeVerifier::ShouldHaveMasterpiecePartitions() const
{
  // Super Smash Bros. Brawl.
  static constexpr std::array<std::string_view, 4> IDS = {"RSBE01", "RSBJ01", "RSBK01", "RSBP01"};
  return Common::Contains(IDS, std::string_view(m_volume.GetGameID()));
}

bool VolumeVerifier::ShouldBeDualLayer() const
{
  // Games released on dual-layer discs. (The Japanese releases of Xenoblade and The Last Story are
  // single-layer and must stay out of this list.)
  static constexpr std::array<std::string_view, 33> IDS = {
      "R3ME01", "R3MP01", "R3OE01", "R3OJ01", "R3OP01", "RSBE01", "RSBJ01", "RSBK01", "RSBP01",
      "RXMJ8P", "S59E01", "S59JC8", "S59P01", "S5QJC8", "SAKENS", "SAKPNS", "SK8V52", "SK8X52",
      "SLSEXJ", "SLSP01", "SQIE4Q", "SQIP4Q", "SQIY4Q", "SR5E41", "SR5P41", "SUOE41", "SUOP41",
      "SVXX52", "SVXY52", "SX4E01", "SX4P01", "SZ3EGT", "SZ3PGT",
  };
  static_assert(std::ranges::is_sorted(IDS));
  return std::ranges::binary_search(IDS, m_volume.GetGameID());
}

void VolumeVerifier::CheckVolumeSize()
{
  const bool is_disc = IsDisc(m_volume.GetVolumeType());
  const bool dual_layer_game = is_disc && ShouldBeDualLayer();
  u64 size = m_volume.GetDataSize();
  bool size_known = m_data_size_type != DataSizeType::UpperBound;

  if (dual_layer_game && m_biggest_referenced_offset <= SL_DVD_R_SIZE)
  {
    AddProblem(Severity::Medium,
               Common::GetStringT(
                   "This game has been hacked to fit on a single-layer DVD. Some content such as "
                   "pre-rendered videos, extra languages or entire game modes will be broken. "
                   "This problem generally only exists in illegal copies of games."));
  }

  if (m_data_size_type != DataSizeType::Accurate)
  {
    AddProblem(Severity::Low,
               Common::GetStringT("The format that the disc image is saved in does not "
                                  "store the size of the disc image."));

    // With Wii hashes, the end of the last good block is a usable size estimate.
    if (!size_known && m_volume.HasWiiHashes())
    {
      size = m_biggest_verified_offset;
      size_known = true;
    }
  }

  const bool unread_parts = m_content_index != m_content_offsets.size() ||
                            m_group_index != m_groups.size();
  if (unread_parts || (!m_is_datel && size_known && m_biggest_referenced_offset > size))
  {
    const bool looks_like_one_layer =
        is_disc && size_known && size >= SL_DVD_SIZE && size <= SL_DVD_R_SIZE;
    AddProblem(Severity::High,
               looks_like_one_layer ?
                   Common::GetStringT(
                       "This disc image is too small and lacks some data. The problem is "
                       "most likely that this is a dual-layer disc that has been dumped "
                       "as a single-layer disc.") :
                   Common::GetStringT(
                       "This disc image is too small and lacks some data. If your "
                       "dumping program saved the disc image as several parts, you need "
                       "to merge them into one file."));
    return;
  }

  // LowerBound (NFS) is included on purpose so that its users get the NetPlay warning.
  if (!is_disc || m_data_size_type == DataSizeType::UpperBound || m_is_tgc)
    return;

  const bool gamecube_sized = m_volume.GetVolumeType() == Platform::GameCubeDisc || m_is_datel;
  const bool debug = IsDebugSigned();
  const bool retail_wii_size = size == SL_DVD_SIZE || size == DL_DVD_SIZE;
  const bool debug_wii_size = size == SL_DVD_R_SIZE || size == DL_DVD_R_SIZE;

  bool size_ok;
  if (gamecube_sized)
    size_ok = size == MINI_DVD_SIZE;
  else
    size_ok = debug ? debug_wii_size : retail_wii_size;
  if (size_ok)
    return;

  if (debug && retail_wii_size)
  {
    AddProblem(Severity::Low,
               Common::GetStringT("This debug disc image has the size of a retail disc image."));
    return;
  }

  const u64 expected = gamecube_sized ? MINI_DVD_SIZE : dual_layer_game ? DL_DVD_SIZE : SL_DVD_SIZE;
  if (size < expected)
  {
    AddProblem(Severity::Low,
               Common::GetStringT(
                   "This disc image has an unusual size. This will likely make the emulated "
                   "loading times longer. You will likely be unable to share input recordings "
                   "and use NetPlay with anyone who is using a good dump."));
  }
  else
  {
    AddProblem(Severity::Low, Common::GetStringT("This disc image has an unusual size."));
  }
}

void VolumeVerifier::CheckMisc()
{
  CheckGameID();
  CheckIOS();
  CheckTicketAndTMD();

  if (m_volume.IsNKit())
  {
    AddProblem(
        Severity::Low,
        Common::GetStringT("This disc image is in the NKit format. It is not a good dump in its "
                           "current form, but it might become a good dump if converted back. "
                           "The CRC32 of this file might match the CRC32 of a good dump even "
                           "though the files are not identical."));
  }

  if (IsDisc(m_volume.GetVolumeType()) &&
      m_volume.GetGameID(PARTITION_NONE).starts_with("R8P"))  // Super Paper Mario
  {
    CheckSuperPaperMario();
  }
}

void VolumeVerifier::CheckGameID()
{
  const std::string outer_id = m_volume.GetGameID(PARTITION_NONE);
  const std::string inner_id = m_volume.GetGameID(m_volume.GetGamePartition());

  if (outer_id != inner_id)
  {
    // The Wii Backup Disc ("pinkfish") legitimately has "410..." outside and RELSAB inside;
    // hacked copies of it have "010..." outside.
    const bool pinkfish = inner_id == "RELSAB";
    if (pinkfish && outer_id.starts_with("410"))
    {
    }
    else if (pinkfish && outer_id.starts_with("010"))
    {
      std::string expected_id = outer_id;
      expected_id[0] = '4';
      AddProblem(Severity::Low, Common::FmtFormatT("The game ID is {0} but should be {1}.",
                                                   outer_id, expected_id));
    }
    else
    {
      AddProblem(Severity::Low, Common::GetStringT("The game ID is inconsistent."));
    }
  }

  // IDs used by development discs and other placeholders; they say nothing about the region.
  constexpr std::string_view GAMECUBE_PLACEHOLDER_ID = "RELSAB";
  constexpr std::string_view WII_PLACEHOLDER_ID = "RABAZZ";

  if (inner_id.size() < 4)
  {
    AddProblem(Severity::Low, Common::GetStringT("The game ID is unusually short."));
    return;
  }
  if (m_is_datel || inner_id == GAMECUBE_PLACEHOLDER_ID || inner_id == WII_PLACEHOLDER_ID)
    return;

  const Platform platform = m_volume.GetVolumeType();
  const char country = IsDisc(platform) ?
                           inner_id[3] :
                           static_cast<char>(m_volume.GetTitleID().value_or(0) & 0xff);
  const Region region = m_volume.GetRegion();
  if (CountryCodeToRegion(country, platform, region, m_volume.GetRevision()) != region)
  {
    AddProblem(Severity::Medium,
               Common::GetStringT(
                   "The region code does not match the game ID. If this is because the "
                   "region code has been modified, the game might run at the wrong speed, "
                   "graphical elements might be offset, or the game might not run at all."));
  }
}

void VolumeVerifier::CheckIOS()
{
  const IOS::ES::TMDReader& tmd = m_volume.GetTMD(m_volume.GetGamePartition());
  if (!tmd.IsValid())
    return;

  const u64 ios = tmd.GetIOSId() & 0xFF;

  // Below IOS40, Korean Wiis launched with only IOS 4, 9, 21 and 37
  // (https://hackmii.com/2008/09/korean-wii/).
  // A Korean game asking for another low IOS was usually fakesigned after its common key index
  // was changed, typically ending up with IOS36. https://bugs.dolphin-emu.org/issues/10319
  constexpr std::array<u64, 4> KOREAN_LAUNCH_IOSES = {4, 9, 21, 37};
  if (m_volume.GetRegion() == Region::NTSC_K && ios < 40 &&
      !Common::Contains(KOREAN_LAUNCH_IOSES, ios))
  {
    AddProblem(
        Severity::High,
        // i18n: "ERROR #002" is shown in English by the Wii software itself, so it is best left
        // as it is.
        Common::GetStringT("This Korean title is set to use an IOS that typically isn't used on "
                           "Korean consoles. This is likely to lead to ERROR #002."));
  }

  // The same kind of fakesigned game, pointed at a cIOS slot instead.
  if (ios >= 0x80)
    AddProblem(Severity::High, Common::GetStringT("This title is set to use an invalid IOS."));
}

void VolumeVerifier::CheckTicketAndTMD()
{
  const Platform platform = m_volume.GetVolumeType();

  m_ticket = m_volume.GetTicket(m_volume.GetGamePartition());
  if (m_ticket.IsValid())
  {
    const u8 index = m_ticket.GetCommonKeyIndex();

    // Discs only use common keys 0 (normal) and 1 (Korean); 2 is the vWii key.
    if (platform == Platform::WiiDisc && index > 1)
    {
      AddProblem(Severity::High,
                 // i18n: "Common key": the key all consoles share ("common" as in "shared").
                 Common::GetStringT("This title is set to use an invalid common key."));
    }

    // Fakesigned WADs often carry a junk index. The WAD code finds the key that actually works,
    // so this is only worth a low-severity note.
    if (platform == Platform::WiiWAD)
    {
      m_ticket = m_volume.GetTicketWithFixedCommonKey();
      const u8 fixed_index = m_ticket.GetCommonKeyIndex();
      if (index != fixed_index)
      {
        AddProblem(Severity::Low,
                   // i18n: "Common key": the key all consoles share ("common" as in "shared").
                   Common::FmtFormatT("The specified common key index is {0} but should be {1}.",
                                      index, fixed_index));
      }
    }
  }

  if (platform != Platform::WiiWAD)
    return;

  IOS::HLE::Kernel ios(m_ticket.GetConsoleType());
  ESCore& es = ios.GetESCore();
  const std::vector<u8>& cert_chain = m_volume.GetCertificateChain(PARTITION_NONE);

  if (!m_ticket.IsValid())
  {
    // i18n: A "ticket" is the signed licence that allows a title (e.g. a game) to run.
    AddProblem(Severity::High, Common::GetStringT("The ticket is invalid."));
  }
  else if (!IsSignedCorrectly(es, ESCore::VerifyContainerType::Ticket, m_ticket, cert_chain))
  {
    // i18n: A "ticket" is the signed licence that allows a title (e.g. a game) to run.
    AddProblem(Severity::Low, Common::GetStringT("The ticket is not correctly signed."));
  }

  const IOS::ES::TMDReader& tmd = m_volume.GetTMD(m_volume.GetGamePartition());
  if (!tmd.IsValid())
  {
    AddProblem(Severity::High, Common::GetStringT("The TMD is invalid."));
  }
  else if (!IsSignedCorrectly(es, ESCore::VerifyContainerType::TMD, tmd, cert_chain))
  {
    AddProblem(
        Severity::Medium,
        Common::GetStringT("The TMD is not correctly signed. If you move or copy this title to "
                           "the SD Card, the Wii System Menu will not launch it anymore and will "
                           "also refuse to copy or move it back to the NAND."));
  }
}

void VolumeVerifier::CheckSuperPaperMario()
{
  // Super Paper Mario reads setup/aa1_01.dat rounded up to 0x20 bytes. Non-zero bytes after the
  // file (which good dumps do not have) can freeze the game.
  // https://bugs.dolphin-emu.org/issues/11900
  const Partition partition = m_volume.GetGamePartition();
  const FileSystem* filesystem = m_volume.GetFileSystem(partition);
  if (!filesystem)
    return;
  const std::unique_ptr<FileInfo> file = filesystem->FindFileInfo("setup/aa1_01.dat");
  if (!file)
    return;

  const u64 file_end = file->GetOffset() + file->GetSize();
  std::vector<u8> tail(Common::AlignUp(file_end, 0x20) - file_end);
  if (!m_volume.Read(file_end, tail.size(), tail.data(), partition))
    return;

  if (std::ranges::any_of(tail, [](u8 byte) { return byte != 0; }))
  {
    AddProblem(Severity::High,
               Common::GetStringT("Some padding data that should be zero is not zero. "
                                  "This can make the game freeze at certain points."));
  }
}

void VolumeVerifier::SetUpHashing()
{
  const Platform platform = m_volume.GetVolumeType();
  if (platform == Platform::WiiWAD)
    m_content_offsets = m_volume.GetContentOffsets();
  else if (platform == Platform::WiiDisc)
    m_scrubber.SetupScrub(m_volume);  // tells whether a bad block is in use

  std::ranges::sort(m_groups, {}, &HashGroupJob::raw_offset);

  if (m_hashes_to_calculate.crc32)
    m_crc32_context = Common::StartCRC32();
  if (m_hashes_to_calculate.md5)
  {
    mbedtls_md5_init(&m_md5_context);
    mbedtls_md5_starts_ret(&m_md5_context);
  }
  if (m_hashes_to_calculate.sha1)
    m_sha1_context = Common::SHA1::CreateContext();
}

void VolumeVerifier::WaitForAsyncOperations() const
{
  for (const std::future<void>* future :
       {&m_crc32_future, &m_md5_future, &m_sha1_future, &m_content_future, &m_group_future})
  {
    if (future->valid())
      future->wait();
  }
}

bool VolumeVerifier::ReadChunkAndWaitForAsyncOperations(u64 length)
{
  std::vector<u8> chunk(length);

  // The start of this chunk may already be at the end of the previous one.
  const u64 reused = std::min(m_carried_over, length);
  if (reused > 0)
    std::memcpy(chunk.data(), m_data.data() + m_data.size() - m_carried_over, reused);

  if (length > reused &&
      !m_volume.Read(m_progress + reused, length - reused, chunk.data() + reused, PARTITION_NONE))
  {
    return false;
  }

  // The background work still reads m_data, so it must be done before m_data is replaced.
  WaitForAsyncOperations();
  m_data = std::move(chunk);
  return true;
}

VolumeVerifier::Chunk VolumeVerifier::PlanChunk() const
{
  Chunk chunk;
  chunk.length = DEFAULT_READ_SIZE;

  const bool more_contents = m_content_index < m_content_offsets.size();
  const bool more_groups = m_group_index < m_groups.size();

  if (more_contents && m_content_offsets[m_content_index] == m_progress)
  {
    // A whole WAD content (padded to 0x40), so that it can be hashed in one go.
    m_volume.GetTMD(PARTITION_NONE).GetContent(m_content_index, &chunk.content);
    chunk.length = Common::AlignUp(chunk.content.size, 0x40);
    chunk.is_content = true;

    const size_t next = m_content_index + 1;
    if (next < m_content_offsets.size() && m_content_offsets[next] < m_progress + chunk.length)
      chunk.overlap = m_progress + chunk.length - m_content_offsets[next];
  }
  else if (more_contents && m_content_offsets[m_content_index] > m_progress)
  {
    // Stop at the next content.
    chunk.length = std::min(chunk.length, m_content_offsets[m_content_index] - m_progress);
  }
  else if (more_groups && m_groups[m_group_index].raw_offset == m_progress)
  {
    // A whole hash group.
    const HashGroupJob& group = m_groups[m_group_index];
    chunk.length = VolumeWii::BLOCK_TOTAL_SIZE * (group.end_block - group.first_block);
    chunk.is_group = true;

    const size_t next = m_group_index + 1;
    if (next < m_groups.size() && m_groups[next].raw_offset < m_progress + chunk.length)
      chunk.overlap = m_progress + chunk.length - m_groups[next].raw_offset;
  }
  else if (more_groups && m_groups[m_group_index].raw_offset > m_progress)
  {
    // Stop at the next hash group.
    chunk.length = std::min(chunk.length, m_groups[m_group_index].raw_offset - m_progress);
  }

  if (m_progress + chunk.length > m_max_progress)
  {
    const u64 past_end = m_progress + chunk.length - m_max_progress;
    if (m_data_size_type == DataSizeType::LowerBound)
    {
      // NFS images can reference blocks past the stated size, and reading those works, so read
      // them but do not count them as progress.
      chunk.overlap = std::max(chunk.overlap, past_end);
    }
    else
    {
      // Never read past the end; what remains of the content or group cannot be checked.
      chunk.length -= past_end;
      chunk.overlap -= std::min(chunk.overlap, past_end);
      chunk.is_content = false;
      chunk.is_group = false;
    }
  }
  return chunk;
}

void VolumeVerifier::StartHashing(u64 length)
{
  if (m_hashes_to_calculate.crc32)
  {
    m_crc32_future = std::async(std::launch::async, [this, length] {
      m_crc32_context =
          Common::UpdateCRC32(m_crc32_context, m_data.data(), static_cast<size_t>(length));
    });
  }
  if (m_hashes_to_calculate.md5)
  {
    m_md5_future = std::async(std::launch::async, [this, length] {
      mbedtls_md5_update_ret(&m_md5_context, m_data.data(), length);
    });
  }
  if (m_hashes_to_calculate.sha1)
  {
    m_sha1_future = std::async(std::launch::async,
                               [this, length] { m_sha1_context->Update(m_data.data(), length); });
  }
}

void VolumeVerifier::StartContentCheck(const IOS::ES::Content& content, bool read_failed)
{
  m_content_future = std::async(std::launch::async, [this, content, read_failed] {
    if (read_failed || !m_volume.CheckContentIntegrity(content, m_data, m_ticket))
      AddProblem(Severity::High, Common::FmtFormatT("Content {0:08x} is corrupt.", content.id));
  });
}

void VolumeVerifier::StartGroupCheck(size_t job_index, bool read_failed)
{
  m_group_future = std::async(std::launch::async, [this, job_index, read_failed] {
    const HashGroupJob& group = m_groups[job_index];
    for (u64 block = group.first_block; block < group.end_block; ++block)
    {
      const u64 offset_in_chunk = (block - group.first_block) * VolumeWii::BLOCK_TOTAL_SIZE;
      const u64 block_offset = group.raw_offset + offset_in_chunk;

      if (!read_failed &&
          m_volume.CheckBlockIntegrity(block, m_data.data() + offset_in_chunk, group.partition))
      {
        m_biggest_verified_offset =
            std::max(m_biggest_verified_offset, block_offset + VolumeWii::BLOCK_TOTAL_SIZE);
      }
      else if (m_scrubber.CanBlockBeScrubbed(block_offset))
      {
        WARN_LOG_FMT(DISCIO, "Integrity check failed for unused block at {:#x}", block_offset);
        ++m_unused_block_errors[group.partition];
      }
      else
      {
        WARN_LOG_FMT(DISCIO, "Integrity check failed for block at {:#x}", block_offset);
        ++m_block_errors[group.partition];
      }
    }
  });
}

void VolumeVerifier::Process()
{
  ASSERT(m_started);
  ASSERT(!m_done);

  if (m_progress >= m_max_progress)
    return;

  const Chunk chunk = PlanChunk();

  const bool data_needed = m_calculating_any_hash || chunk.is_content || chunk.is_group;
  const bool read_failed = data_needed && !ReadChunkAndWaitForAsyncOperations(chunk.length);
  if (read_failed)
  {
    ERROR_LOG_FMT(DISCIO, "Read failed at {:#x} to {:#x}", m_progress, m_progress + chunk.length);
    m_read_errors_occurred = true;
    m_calculating_any_hash = false;  // the hashes cannot be right any more
  }

  m_carried_over = chunk.overlap;
  const u64 advance = chunk.length - chunk.overlap;

  if (m_calculating_any_hash)
    StartHashing(advance);

  if (chunk.is_content)
  {
    StartContentCheck(chunk.content, read_failed);
    ++m_content_index;
  }

  if (chunk.is_group)
  {
    StartGroupCheck(m_group_index, read_failed);
    ++m_group_index;
  }

  m_progress += advance;
}

u64 VolumeVerifier::GetBytesProcessed() const
{
  return m_progress;
}

u64 VolumeVerifier::GetTotalBytes() const
{
  return m_max_progress;
}

void VolumeVerifier::ReportBlockErrors()
{
  for (const auto& [partition, count] : m_block_errors)
  {
    if (count == 0)
      continue;
    const std::string name = GetPartitionName(m_volume.GetPartitionType(partition));
    AddProblem(Severity::Medium,
               Common::FmtFormatT("Errors were found in {0} blocks in the {1} partition.", count,
                                  name));
  }

  for (const auto& [partition, count] : m_unused_block_errors)
  {
    if (count == 0)
      continue;
    const std::string name = GetPartitionName(m_volume.GetPartitionType(partition));
    AddProblem(Severity::Low,
               Common::FmtFormatT("Errors were found in {0} unused blocks in the {1} partition.",
                                  count, name));
  }
}

void VolumeVerifier::Finish()
{
  if (m_done)
    return;
  m_done = true;

  WaitForAsyncOperations();

  if (m_calculating_any_hash)
  {
    if (m_hashes_to_calculate.crc32)
    {
      const u32 crc32_be = Common::swap32(m_crc32_context);
      m_result.hashes.crc32.resize(sizeof(crc32_be));
      std::memcpy(m_result.hashes.crc32.data(), &crc32_be, sizeof(crc32_be));
    }
    if (m_hashes_to_calculate.md5)
    {
      m_result.hashes.md5.resize(16);
      mbedtls_md5_finish_ret(&m_md5_context, m_result.hashes.md5.data());
    }
    if (m_hashes_to_calculate.sha1)
    {
      const auto digest = m_sha1_context->Finish();
      m_result.hashes.sha1.assign(digest.begin(), digest.end());
    }
  }

  if (m_read_errors_occurred)
    AddProblem(Severity::Medium, Common::GetStringT("Some of the data could not be read."));

  CheckVolumeSize();
  ReportBlockErrors();

  // Most serious first; equal severities keep the order they were found in.
  std::ranges::stable_sort(m_result.problems, std::ranges::greater{}, &Problem::severity);

  if (m_redump_verification)
    m_result.redump = m_redump_verifier.Finish(m_result.hashes);

  WriteSummary();
}

void VolumeVerifier::WriteSummary()
{
  const Platform platform = m_volume.GetVolumeType();
  const bool no_problems = m_result.problems.empty();
  const Severity worst = no_problems ? Severity::None : m_result.problems.front().severity;
  const bool redump_good = m_result.redump.status == RedumpVerifier::Status::GoodDump;
  const bool redump_bad = m_result.redump.status == RedumpVerifier::Status::BadDump;
  const bool nfs = m_volume.GetBlobType() == BlobType::NFS;
  std::string& summary = m_result.summary_text;

  // A retail WAD without problems has passed every signature and hash check there is.
  if (redump_good || (platform == Platform::WiiWAD && !m_is_not_retail && no_problems))
  {
    summary = no_problems ?
                  Common::GetStringT("This is a good dump.") :
                  Common::GetStringT(
                      "This is a good dump according to Redump.info, but Dolphin has found "
                      "problems. This might be a bug in Dolphin.");
    return;
  }

  if (m_is_datel)
  {
    summary = Common::GetStringT("Dolphin is unable to verify unlicensed discs.");
    return;
  }
  if (m_is_tgc)
  {
    summary = Common::GetStringT("Dolphin is unable to verify typical TGC files properly, "
                                 "since they are not dumps of actual discs.");
    return;
  }
  if (m_is_triforce)
  {
    summary = Common::GetStringT("Dolphin is currently unable to verify Triforce games.");
    return;
  }

  if (redump_bad && worst <= Severity::Low)
  {
    summary = nfs ?
                  Common::GetStringT(
                      "Compared to the Wii disc release of the game, this is a bad dump. "
                      "Despite this, it's possible that this is a good dump compared to the "
                      "Wii U eShop release of the game. Dolphin can't verify this.") :
                  Common::GetStringT("This is a bad dump. This doesn't necessarily mean that the "
                                     "game won't run correctly.");
  }
  else
  {
    // Here a bad Redump result always comes with medium or high severity problems.
    if (redump_bad)
      summary = Common::GetStringT("This is a bad dump.") + "\n\n";

    switch (worst)
    {
    case Severity::None:
      summary = IsWii(platform) && !m_is_not_retail ?
                    Common::GetStringT(
                        "No problems were found. This does not guarantee that this is a good "
                        "dump, but since Wii titles contain a lot of verification data, it does "
                        "mean that there most likely are no problems that will affect emulation.") :
                    Common::GetStringT("No problems were found.");
      break;
    case Severity::Low:
      summary = nfs ? Common::GetStringT(
                          "Compared to the Wii disc release of the game, problems of low severity "
                          "were found. Despite this, it's possible that this is a good dump "
                          "compared to the Wii U eShop release of the game. Dolphin can't verify "
                          "this.") :
                      Common::GetStringT("Problems with low severity were found. They will most "
                                         "likely not prevent the game from running.");
      break;
    case Severity::Medium:
      summary += Common::GetStringT("Problems with medium severity were found. The whole game "
                                    "or certain parts of the game might not work correctly.");
      break;
    case Severity::High:
      summary += Common::GetStringT("Problems with high severity were found. The game will most "
                                    "likely not work at all.");
      break;
    }
  }

  if (platform == Platform::GameCubeDisc)
  {
    summary += Common::GetStringT("\n\nBecause GameCube disc images contain little verification "
                                  "data, there may be problems that Dolphin is unable to detect.");
  }
  else if (m_is_not_retail)
  {
    summary += Common::GetStringT("\n\nBecause this title is not for retail Wii consoles, "
                                  "Dolphin cannot ensure that it hasn't been tampered with, even "
                                  "if signatures appear valid.");
  }
}

const VolumeVerifier::Result& VolumeVerifier::GetResult() const
{
  return m_result;
}

void VolumeVerifier::AddProblem(Severity severity, std::string text)
{
  m_result.problems.push_back(Problem{severity, std::move(text)});
}

}  // namespace DiscIO
