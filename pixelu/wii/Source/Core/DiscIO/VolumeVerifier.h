// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <mbedtls/md5.h>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Core/IOS/ES/Formats.h"
#include "DiscIO/DiscScrubber.h"
#include "DiscIO/Volume.h"

// Checks a game image for problems and optionally hashes it and looks it up in the Redump.info
// database. Usage:
//
//   VolumeVerifier verifier(volume, redump_verification, hashes_to_calculate);
//   verifier.Start();
//   while (verifier.GetBytesProcessed() != verifier.GetTotalBytes())
//     verifier.Process();
//   verifier.Finish();
//   const VolumeVerifier::Result& result = verifier.GetResult();
//
// Each call may take a while. GetResult() may be called at any time, but only holds everything
// after Finish().

namespace DiscIO
{
template <typename T>
struct Hashes
{
  T crc32;
  T md5;
  T sha1;
};

// Compares the image's hashes against the Redump.info datfile for its platform. The datfile is
// downloaded (or the cached copy used) in the background as soon as Start() is called.
class RedumpVerifier final
{
public:
  enum class Status
  {
    Unknown,
    GoodDump,
    BadDump,
    Error,
  };

  struct Result
  {
    Status status = Status::Unknown;
    std::string message;
  };

  void Start(const Volume& volume);
  Result Finish(const Hashes<std::vector<u8>>& hashes);

private:
  enum class DownloadStatus
  {
    NotAttempted,
    Success,
    Fail,
    FailButOldCacheAvailable,
  };

  // Shared by every verifier of one platform, so that the datfile is downloaded once per session.
  struct DownloadState
  {
    std::mutex mutex;
    DownloadStatus status = DownloadStatus::NotAttempted;
  };

  // A datfile entry for this game (ID, revision and disc number match).
  struct Candidate
  {
    u64 size = 0;
    Hashes<std::vector<u8>> hashes;
  };

  std::vector<Candidate> LoadCandidates(const std::string& system, DownloadState* state);
  std::vector<Candidate> FindCandidates(std::span<const u8> datfile, const std::string& system);
  bool SerialListMatches(const std::string& serials) const;

  static DownloadStatus Download(const std::string& system, DownloadStatus previous);
  static std::vector<u8> ReadCachedDatfile(const std::string& system);

  std::string m_game_id;  // first four characters; empty for Datel discs
  u16 m_revision = 0;
  u8 m_disc_number = 0;
  u64 m_size = 0;

  std::future<std::vector<Candidate>> m_candidates;
  Result m_result;  // set by the background task when it fails

  static DownloadState s_gc_download;
  static DownloadState s_wii_download;
};

class VolumeVerifier final
{
public:
  enum class Severity
  {
    None,  // Only used internally
    Low,
    Medium,
    High,
  };

  struct Problem
  {
    Severity severity;
    std::string text;
  };

  struct Result
  {
    Hashes<std::vector<u8>> hashes;
    std::string summary_text;
    std::vector<Problem> problems;
    RedumpVerifier::Result redump;
  };

  VolumeVerifier(const Volume& volume, bool redump_verification, Hashes<bool> hashes_to_calculate);
  ~VolumeVerifier();

  static Hashes<bool> GetDefaultHashesToCalculate();
  void Start();
  void Process();
  u64 GetBytesProcessed() const;
  u64 GetTotalBytes() const;
  void Finish();
  const Result& GetResult() const;

private:
  // One hash group (or the tail of one) of a Wii partition, checked as a unit.
  struct HashGroupJob
  {
    Partition partition;
    u64 raw_offset;
    size_t first_block;
    size_t end_block;
  };

  // What the next Process() call reads.
  struct Chunk
  {
    u64 length = 0;
    u64 overlap = 0;  // bytes at the end that belong to the next chunk as well
    bool is_content = false;  // a whole WAD content, to be checked against the TMD
    bool is_group = false;    // a whole hash group, to be checked against the H3 table
    IOS::ES::Content content{};
  };

  std::vector<Partition> CheckPartitions();
  void CheckPartitionTable(const std::vector<Partition>& partitions);
  bool CheckPartition(const Partition& partition);  // false: leave the partition out
  void CheckUpdatePartitionIOS(const FileSystem& update_filesystem);
  void CheckGameID();
  void CheckIOS();
  void CheckTicketAndTMD();
  void CheckMisc();
  void CheckSuperPaperMario();
  void CheckVolumeSize();
  void ReportBlockErrors();
  void WriteSummary();
  std::string GetPartitionName(std::optional<u32> type) const;
  bool IsDebugSigned() const;
  bool ShouldHaveChannelPartition() const;
  bool ShouldHaveInstallPartition() const;
  bool ShouldHaveMasterpiecePartitions() const;
  bool ShouldBeDualLayer() const;

  void SetUpHashing();
  Chunk PlanChunk() const;
  void StartHashing(u64 length);
  void StartContentCheck(const IOS::ES::Content& content, bool read_failed);
  void StartGroupCheck(size_t job_index, bool read_failed);
  void WaitForAsyncOperations() const;
  bool ReadChunkAndWaitForAsyncOperations(u64 length);

  void AddProblem(Severity severity, std::string text);

  const Volume& m_volume;
  Result m_result;
  bool m_is_tgc = false;
  bool m_is_datel = false;
  bool m_is_triforce = false;
  bool m_is_not_retail = false;

  bool m_redump_verification;
  RedumpVerifier m_redump_verifier;

  bool m_read_errors_occurred = false;

  Hashes<bool> m_hashes_to_calculate{};
  bool m_calculating_any_hash = false;
  u32 m_crc32_context = 0;
  mbedtls_md5_context m_md5_context{};
  std::unique_ptr<Common::SHA1::Context> m_sha1_context;

  // The last chunk read. Its final m_carried_over bytes start the next chunk.
  std::vector<u8> m_data;
  u64 m_carried_over = 0;

  std::future<void> m_crc32_future;
  std::future<void> m_md5_future;
  std::future<void> m_sha1_future;
  std::future<void> m_content_future;
  std::future<void> m_group_future;

  DiscScrubber m_scrubber;
  IOS::ES::TicketReader m_ticket;
  std::vector<u64> m_content_offsets;  // WADs only
  u16 m_content_index = 0;
  std::vector<HashGroupJob> m_groups;  // sorted by raw_offset
  size_t m_group_index = 0;
  std::map<Partition, size_t> m_block_errors;
  std::map<Partition, size_t> m_unused_block_errors;

  u64 m_biggest_referenced_offset = 0;
  u64 m_biggest_verified_offset = 0;

  bool m_started = false;
  bool m_done = false;
  u64 m_progress = 0;
  u64 m_max_progress = 0;
  DataSizeType m_data_size_type;
};

}  // namespace DiscIO
