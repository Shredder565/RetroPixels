// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DVD/DVDThread.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Timer.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"

namespace DVD
{
DVDThread::DVDThread(Core::System& system) : m_system(system)
{
}

DVDThread::~DVDThread() = default;

void DVDThread::Start()
{
  m_finish_read = m_system.GetCoreTiming().RegisterEvent("FinishReadDVDThread", OnReadDue);

  // Request IDs never reach the game; restarting them keeps runs deterministic.
  m_next_id = 0;

  m_dvd_thread.Reset("DVD thread", [this](ReadRequest&& request) {
    ProcessReadRequest(std::move(request));
  });
}

void DVDThread::Stop()
{
  m_dvd_thread.Cancel();
  m_dvd_thread.Shutdown();

  m_result_queue.Clear();
  m_result_map.clear();
  m_disc.reset();
}

void DVDThread::DoState(PointerWrap& p)
{
  // Let the worker finish, then gather every finished result into the map,
  // which is the only container the savestate stores.
  WaitUntilIdle();
  for (ReadResult finished; m_result_queue.Pop(finished);)
  {
    const u64 id = finished.first.id;
    m_result_map.emplace(id, std::move(finished));
  }

  p.Do(m_result_map);
  p.Do(m_next_id);

  // The disc itself is a host file and isn't stored. Only check that a disc
  // is present exactly when the state expects one.
  bool state_had_disc = HasDisc();
  p.Do(state_had_disc);
  if (state_had_disc == HasDisc())
    return;

  if (state_had_disc)
    PanicAlertFmtT("An inserted disc was expected but not found.");
  else
    m_disc.reset();
}

void DVDThread::SetDisc(std::unique_ptr<DiscIO::Volume> disc)
{
  WaitUntilIdle();
  m_disc = std::move(disc);
}

bool DVDThread::HasDisc() const
{
  return m_disc != nullptr;
}

// The next three only read immutable volume properties, which the volume
// classes allow from any thread, so there's no need to wait for the worker.
bool DVDThread::HasWiiHashes() const
{
  return m_disc->HasWiiHashes();
}

DiscIO::Platform DVDThread::GetDiscType() const
{
  return m_disc->GetVolumeType();
}

u64 DVDThread::PartitionOffsetToRawOffset(u64 offset, const DiscIO::Partition& partition)
{
  return m_disc->PartitionOffsetToRawOffset(offset, partition);
}

IOS::ES::TMDReader DVDThread::GetTMD(const DiscIO::Partition& partition)
{
  WaitUntilIdle();
  return m_disc->GetTMD(partition);
}

IOS::ES::TicketReader DVDThread::GetTicket(const DiscIO::Partition& partition)
{
  WaitUntilIdle();
  return m_disc->GetTicket(partition);
}

bool DVDThread::IsInsertedDiscRunning()
{
  if (!HasDisc())
    return false;

  WaitUntilIdle();
  return m_disc->GetGameID() == SConfig::GetInstance().GetGameID();
}

bool DVDThread::UpdateRunningGameMetadata(const DiscIO::Partition& partition,
                                          std::optional<u64> title_id)
{
  if (!HasDisc())
    return false;

  WaitUntilIdle();

  if (title_id.has_value() && m_disc->GetTitleID(partition) != title_id)
    return false;

  SConfig::GetInstance().SetRunningGameMetadata(*m_disc, partition);
  return true;
}

void DVDThread::WaitUntilIdle()
{
  ASSERT(Core::IsCPUThread());
  m_dvd_thread.WaitForCompletion();
}

void DVDThread::StartRead(u64 dvd_offset, u32 length, const DiscIO::Partition& partition,
                          DVD::ReplyType reply_type, s64 ticks_until_completion)
{
  ReadRequest request;
  request.dvd_offset = dvd_offset;
  request.length = length;
  request.partition = partition;
  request.reply_type = reply_type;
  Enqueue(std::move(request), ticks_until_completion);
}

void DVDThread::StartReadToEmulatedRAM(u32 output_address, u64 dvd_offset, u32 length,
                                       const DiscIO::Partition& partition,
                                       DVD::ReplyType reply_type, s64 ticks_until_completion)
{
  ReadRequest request;
  request.copy_to_ram = true;
  request.output_address = output_address;
  request.dvd_offset = dvd_offset;
  request.length = length;
  request.partition = partition;
  request.reply_type = reply_type;
  Enqueue(std::move(request), ticks_until_completion);
}

void DVDThread::Enqueue(ReadRequest request, s64 ticks_until_completion)
{
  ASSERT(Core::IsCPUThread());

  auto& core_timing = m_system.GetCoreTiming();

  const u64 id = m_next_id++;
  request.id = id;
  request.time_started_ticks = core_timing.GetTicks();
  request.realtime_started_us = Common::Timer::NowUs();

  m_dvd_thread.Push(std::move(request));
  core_timing.ScheduleEvent(ticks_until_completion, m_finish_read, id);
}

// Runs on the worker thread.
void DVDThread::ProcessReadRequest(ReadRequest&& request)
{
  m_file_logger.Log(*m_disc, request.partition, request.dvd_offset);

  std::vector<u8> data(request.length);
  const bool ok = m_disc->Read(request.dvd_offset, request.length, data.data(), request.partition);
  if (!ok)
    data.clear();

  request.realtime_done_us = Common::Timer::NowUs();
  m_result_queue.Push(ReadResult{std::move(request), std::move(data)});
}

void DVDThread::OnReadDue(Core::System& system, u64 id, s64 cycles_late)
{
  system.GetDVDThread().CompleteRead(id, cycles_late);
}

DVDThread::ReadResult DVDThread::TakeResult(u64 id)
{
  ReadResult result;

  if (const auto parked = m_result_map.find(id); parked != m_result_map.end())
  {
    result = std::move(parked->second);
    m_result_map.erase(parked);
    return result;
  }

  // Drain the queue until the wanted result shows up, parking the others.
  // The queue has a single consumer (this thread), so nothing can be pushed
  // back into it.
  for (;;)
  {
    m_result_queue.WaitForData();
    m_result_queue.Pop(result);
    if (result.first.id == id)
      return result;
    const u64 other_id = result.first.id;
    m_result_map.emplace(other_id, std::move(result));
  }
}

void DVDThread::CompleteRead(u64 id, s64 cycles_late)
{
  const ReadResult result = TakeResult(id);
  const ReadRequest& request = result.first;
  const std::vector<u8>& data = result.second;

  const u64 ticks_per_us = m_system.GetSystemTimers().GetTicksPerSecond() / 1000000;
  DEBUG_LOG_FMT(DVDINTERFACE,
                "DVD read {} done: host {} us, host incl. wait {} us, emulated {} us", id,
                request.realtime_done_us - request.realtime_started_us,
                Common::Timer::NowUs() - request.realtime_started_us,
                (m_system.GetCoreTiming().GetTicks() - request.time_started_ticks) / ticks_per_us);

  auto& di = m_system.GetDVDInterface();
  DIInterruptType interrupt = DIInterruptType::TCINT;

  if (data.size() == request.length)
  {
    if (request.copy_to_ram)
      m_system.GetMemory().CopyToEmu(request.output_address, data.data(), request.length);
  }
  else
  {
    PanicAlertFmtT("The disc could not be read (at {0:#x} - {1:#x}).", request.dvd_offset,
                   request.dvd_offset + request.length);
    di.SetDriveError(DriveError::ReadError);
    interrupt = DIInterruptType::DEINT;
  }

  di.FinishExecutingCommand(request.reply_type, interrupt, cycles_late, data);
}
}  // namespace DVD
