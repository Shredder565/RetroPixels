// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/SPSCQueue.h"
#include "Common/WorkQueueThread.h"

#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/FileMonitor.h"

#include "DiscIO/Volume.h"

class PointerWrap;
namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}
namespace DiscIO
{
enum class Platform;
class Volume;
struct Partition;
}  // namespace DiscIO
namespace IOS::ES
{
class TMDReader;
class TicketReader;
}  // namespace IOS::ES

namespace DVD
{
enum class ReplyType : u32;

// Owns the inserted disc and performs the actual disc reads on a worker
// thread. The emulated completion time of each read is a CoreTiming event; when
// it fires, the CPU thread collects the data the worker produced and hands it
// to DVDInterface.
class DVDThread
{
public:
  explicit DVDThread(Core::System& system);
  DVDThread(const DVDThread&) = delete;
  DVDThread(DVDThread&&) = delete;
  DVDThread& operator=(const DVDThread&) = delete;
  DVDThread& operator=(DVDThread&&) = delete;
  ~DVDThread();

  void Start();
  void Stop();
  void DoState(PointerWrap& p);

  void SetDisc(std::unique_ptr<DiscIO::Volume> disc);
  bool HasDisc() const;

  bool HasWiiHashes() const;
  DiscIO::Platform GetDiscType() const;
  u64 PartitionOffsetToRawOffset(u64 offset, const DiscIO::Partition& partition);
  IOS::ES::TMDReader GetTMD(const DiscIO::Partition& partition);
  IOS::ES::TicketReader GetTicket(const DiscIO::Partition& partition);
  bool IsInsertedDiscRunning();
  // Calls SConfig::SetRunningGameMetadata(Volume&, Partition&) and returns true
  // when a disc is inserted and either no title_id is given or it equals the
  // disc's title ID. Returns false otherwise.
  bool UpdateRunningGameMetadata(const DiscIO::Partition& partition,
                                 std::optional<u64> title_id = {});

  void StartRead(u64 dvd_offset, u32 length, const DiscIO::Partition& partition,
                 DVD::ReplyType reply_type, s64 ticks_until_completion);
  void StartReadToEmulatedRAM(u32 output_address, u64 dvd_offset, u32 length,
                              const DiscIO::Partition& partition, DVD::ReplyType reply_type,
                              s64 ticks_until_completion);

private:
  // One queued read. This struct is stored in savestates byte for byte, so
  // its members, their types and their order must not change.
  struct ReadRequest
  {
    bool copy_to_ram = false;
    u32 output_address = 0;
    u64 dvd_offset = 0;
    u32 length = 0;
    DiscIO::Partition partition{};
    // Which DVDInterface reply path runs when the read completes (a plain
    // enum rather than a callback so it can be savestated).
    DVD::ReplyType reply_type = DVD::ReplyType::NoReply;
    // Unique among requests that are still outstanding.
    u64 id = 0;
    // Diagnostics only.
    u64 time_started_ticks = 0;
    u64 realtime_started_us = 0;
    u64 realtime_done_us = 0;
  };

  using ReadResult = std::pair<ReadRequest, std::vector<u8>>;

  void WaitUntilIdle();
  void Enqueue(ReadRequest request, s64 ticks_until_completion);
  ReadResult TakeResult(u64 id);
  void ProcessReadRequest(ReadRequest&& request);

  static void OnReadDue(Core::System& system, u64 id, s64 cycles_late);
  void CompleteRead(u64 id, s64 cycles_late);

  CoreTiming::EventType* m_finish_read = nullptr;
  u64 m_next_id = 0;

  Common::WorkQueueThreadSP<ReadRequest> m_dvd_thread;

  // Worker -> CPU thread. Results can arrive out of the order the CPU thread
  // wants them, so ones taken early are parked in m_result_map.
  Common::WaitableSPSCQueue<ReadResult> m_result_queue;
  std::map<u64, ReadResult> m_result_map;

  std::unique_ptr<DiscIO::Volume> m_disc;
  FileMonitor::FileLogger m_file_logger;

  Core::System& m_system;
};
}  // namespace DVD
