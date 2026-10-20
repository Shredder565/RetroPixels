// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DVD/DVDInterface.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "AudioCommon/AudioCommon.h"

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/BitField.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/DVD/DVDMath.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/StreamADPCM.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/DI/DI.h"
#include "Core/IOS/IOS.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWii.h"

#include "VideoCommon/OnScreenDisplay.h"

namespace DVD
{
namespace
{
// Register offsets inside the DI block (0x0C006000 / 0x0D006000).
enum : u32
{
  REG_DISR = 0x00,
  REG_DICVR = 0x04,
  REG_DICMDBUF0 = 0x08,
  REG_DICMDBUF1 = 0x0C,
  REG_DICMDBUF2 = 0x10,
  REG_DIMAR = 0x14,
  REG_DILENGTH = 0x18,
  REG_DICR = 0x1C,
  REG_DIIMMBUF = 0x20,
  REG_DICFG = 0x24,
};

// Drive timing constants.
constexpr u64 COMMAND_LATENCY_US = 300;     // any command, lower bound
constexpr u64 READ_START_LATENCY_US = 600;  // before a read starts moving data
constexpr u64 READ_AHEAD_SIZE = 1024 * 1024;
constexpr u64 SECTOR_SIZE = 0x800;
constexpr u64 ECC_BLOCK_SIZE = 16 * SECTOR_SIZE;  // smallest unit the drive reads
constexpr u64 BUFFERED_BYTES_PER_SECOND = 32 * 1024 * 1024;
// The drive keeps the block it last read and the one before it.
constexpr u64 READ_AHEAD_KEEP_BEHIND = ECC_BLOCK_SIZE * 2;

// The completion event carries the reply type in the high word and the
// interrupt in the low word.
u64 EncodeCompletion(ReplyType reply_type, DIInterruptType interrupt)
{
  return (u64{static_cast<u32>(reply_type)} << 32) | static_cast<u32>(interrupt);
}

// Reported disc end. Discs are treated as standard pressed DVD sizes; the
// extra space on recordable development discs isn't readable by those drives.
u64 ComputeDiscEndOffset(const DiscIO::VolumeDisc& disc)
{
  const bool size_known = disc.GetDataSizeType() == DiscIO::DataSizeType::Accurate;
  const u64 size = size_known ? disc.GetDataSize() : DiscIO::GetBiggestReferencedOffset(disc);

  if (size_known && size == DiscIO::MINI_DVD_SIZE)
    return DiscIO::MINI_DVD_SIZE;

  const bool mini_dvd_platform =
      disc.GetVolumeType() == DiscIO::Platform::GameCubeDisc || disc.IsDatelDisc();
  if (mini_dvd_platform && size <= DiscIO::MINI_DVD_SIZE)
    return DiscIO::MINI_DVD_SIZE;
  if (size <= DiscIO::SL_DVD_R_SIZE)
    return DiscIO::SL_DVD_SIZE;
  return DiscIO::DL_DVD_SIZE;
}

// Status bits for one interrupt source: the flag and, one bit below it, its
// enable mask. CVRINT lives in DICVR, the others in DISR.
constexpr u32 FlagBit(DIInterruptType type)
{
  switch (type)
  {
  case DIInterruptType::DEINT:
    return 1u << 2;
  case DIInterruptType::TCINT:
    return 1u << 4;
  case DIInterruptType::BRKINT:
    return 1u << 6;
  case DIInterruptType::CVRINT:
    return 1u << 2;
  }
  return 0;
}
}  // namespace

DVDInterface::DVDInterface(Core::System& system) : m_system(system)
{
}

DVDInterface::~DVDInterface() = default;

void DVDInterface::DoState(PointerWrap& p)
{
  // Field order is the savestate format; `save` handles its arguments left to
  // right.
  const auto save = [&p](auto&... fields) { (p.Do(fields), ...); };

  // Registers.
  save(m_DISR, m_DICVR);
  p.DoArray(m_DICMDBUF);
  save(m_DIMAR, m_DILENGTH, m_DICR, m_DIIMMBUF, m_DICFG);

  // Audio streaming.
  save(m_audio.playing, m_audio.stop_at_track_end, m_audio.position, m_audio.current_start,
       m_audio.current_length, m_audio.next_start, m_audio.next_length, m_audio.pending_blocks,
       m_audio.enabled, m_audio.buffer_length);

  // Drive status, read-ahead window and any pending disc swap.
  save(m_drive_state, m_error_code);
  save(m_read_ahead.start_time, m_read_ahead.end_time, m_read_ahead.start_offset,
       m_read_ahead.end_offset);
  save(m_disc_path_to_insert);

  m_system.GetDVDThread().DoState(p);
  m_adpcm_decoder.DoState(p);

  if (m_system.IsTriforce())
  {
    AMMediaboard::DoState(p);
    p.DoMarker("AMMediaboard");
  }
}

void DVDInterface::Init()
{
  ASSERT(!IsDiscInside());

  m_system.GetDVDThread().Start();

  m_DISR.Hex = 0;
  // Lid reads as open while the drive is empty (the Disc Channel checks it).
  m_DICVR.Hex = 1;
  m_DICMDBUF.fill(0);
  m_DIMAR = 0;
  m_DILENGTH = 0;
  m_DICR.Hex = 0;
  m_DIIMMBUF = 0;
  // CONFIG = 1: the boot ROM descrambler is off.
  m_DICFG.Hex = 1;

  ResetDrive(false);

  auto& core_timing = m_system.GetCoreTiming();
  m_auto_change_disc = core_timing.RegisterEvent("AutoChangeDisc", AutoChangeDiscCallback);
  m_eject_disc = core_timing.RegisterEvent("EjectDisc", EjectDiscCallback);
  m_insert_disc = core_timing.RegisterEvent("InsertDisc", InsertDiscCallback);
  m_finish_executing_command =
      core_timing.RegisterEvent("FinishExecutingCommand", FinishExecutingCommandCallback);

  // Kick off the audio streaming loop; it reschedules itself from then on.
  core_timing.ScheduleEvent(0, m_finish_executing_command,
                            EncodeCompletion(ReplyType::DTK, DIInterruptType::TCINT));

  if (m_system.IsTriforce())
  {
    AMMediaboard::Init();
    // The Triforce IPL wants the lid closed, and bit 3 of DICFG tells it the
    // board has the smaller memory configuration.
    m_DICVR.Hex = 0;
    m_DICFG.Hex |= 8;
  }
}

// Resets the drive's own controller (streaming, error, read-ahead), not the
// DI registers and not the disc.
void DVDInterface::ResetDrive(bool spinup)
{
  m_audio = AudioStream{};

  // Without a disc, the drive reports an open lid. (NoMediumPresent is for a
  // closed lid with an unreadable medium, which isn't emulated.)
  if (!IsDiscInside())
    SetDriveState(DriveState::CoverOpened);
  else if (spinup)
    SetDriveState(DriveState::DiscIdNotRead);
  else
    SetDriveState(DriveState::DiscChangeDetected);

  SetDriveError(DriveError::None);

  m_read_ahead = ReadAhead{};
}

void DVDInterface::Shutdown()
{
  m_system.GetDVDThread().Stop();

  if (m_system.IsTriforce())
    AMMediaboard::Shutdown();
}

void DVDInterface::SetDisc(std::unique_ptr<DiscIO::VolumeDisc> disc,
                           std::optional<std::vector<std::string>> auto_disc_change_paths)
{
  const bool had_disc = IsDiscInside();
  const bool has_disc = disc != nullptr;

  if (has_disc)
  {
    m_disc_end_offset = ComputeDiscEndOffset(*disc);
    if (disc->GetDataSizeType() != DiscIO::DataSizeType::Accurate)
      WARN_LOG_FMT(DVDINTERFACE, "Disc size not known exactly, assuming {} bytes",
                   m_disc_end_offset);

    const DiscIO::BlobReader& blob = disc->GetBlobReader();

    // A DirectoryBlob (also used for Riivolution) can be bigger than a real
    // Wii disc, which would trip the game's Error #001 check, so that check
    // is answered with a forced failure instead.
    Config::SetCurrent(Config::SESSION_SHOULD_FAKE_ERROR_001,
                       m_system.IsWii() && blob.GetBlobType() == DiscIO::BlobType::DIRECTORY);

    if (blob.GetBlockSize() > 0x200000 && !blob.HasFastRandomAccessInBlock())
    {
      OSD::AddMessage("You are running a disc image with a very large block size.", 60000);
      OSD::AddMessage("This will likely lead to performance problems.", 60000);
      OSD::AddMessage("You can use Dolphin's convert feature to reduce the block size.", 60000);
    }
  }

  if (auto_disc_change_paths.has_value())
  {
    ASSERT_MSG(DISCIO, auto_disc_change_paths->size() != 1,
               "Cannot automatically change between one disc");
    m_auto_disc_change_paths = std::move(*auto_disc_change_paths);
    m_auto_disc_change_index = 0;
  }

  // Going from a disc to a different disc always passes through "no disc".
  if (had_disc != has_disc)
    ExpansionInterface::g_rtc_flags[ExpansionInterface::RTCFlag::DiscChanged] = true;

  m_system.GetDVDThread().SetDisc(std::move(disc));
  UpdateLid();
  ResetDrive(false);
}

bool DVDInterface::IsDiscInside() const
{
  return m_system.GetDVDThread().HasDisc();
}

void DVDInterface::AutoChangeDiscCallback(Core::System& system, u64, s64)
{
  system.GetDVDInterface().AutoChangeDisc(Core::CPUThreadGuard{system});
}

void DVDInterface::EjectDiscCallback(Core::System& system, u64, s64)
{
  system.GetDVDInterface().SetDisc(nullptr, {});
}

void DVDInterface::InsertDiscCallback(Core::System& system, u64, s64)
{
  DVDInterface& di = system.GetDVDInterface();

  std::unique_ptr<DiscIO::VolumeDisc> disc = DiscIO::CreateDiscForCore(di.m_disc_path_to_insert);
  if (disc == nullptr)
  {
    PanicAlertFmtT("The disc that was about to be inserted couldn't be found.");
  }
  else
  {
    AchievementManager::GetInstance().ChangeDisc(disc.get());
    di.SetDisc(std::move(disc), {});
  }

  di.m_disc_path_to_insert.clear();
}

// CPU thread only.
void DVDInterface::EjectDisc(const Core::CPUThreadGuard&, EjectCause cause)
{
  m_system.GetCoreTiming().ScheduleEvent(0, m_eject_disc);
  if (cause == EjectCause::User)
    ExpansionInterface::g_rtc_flags[ExpansionInterface::RTCFlag::EjectButton] = true;
}

// CPU thread only.
void DVDInterface::ChangeDisc(const Core::CPUThreadGuard& guard,
                              const std::vector<std::string>& paths)
{
  ASSERT_MSG(DISCIO, !paths.empty(), "Trying to insert an empty list of discs");

  if (paths.size() > 1)
  {
    m_auto_disc_change_paths = paths;
    m_auto_disc_change_index = 0;
  }

  ChangeDisc(guard, paths.front());
}

// CPU thread only. Ejects now and inserts the new disc one emulated second later.
void DVDInterface::ChangeDisc(const Core::CPUThreadGuard& guard, const std::string& new_path)
{
  const bool swap_pending = !m_disc_path_to_insert.empty();
  if (swap_pending)
  {
    PanicAlertFmtT("A disc is already about to be inserted.");
    return;
  }

  EjectDisc(guard, EjectCause::User);
  m_disc_path_to_insert = new_path;

  const auto one_second = m_system.GetSystemTimers().GetTicksPerSecond();
  m_system.GetCoreTiming().ScheduleEvent(one_second, m_insert_disc);
  m_system.GetMovie().SignalDiscChange(new_path);

  // Keep the multi-disc list only if the new disc belongs to it.
  const auto in_list = std::ranges::find(m_auto_disc_change_paths, new_path);
  if (in_list == m_auto_disc_change_paths.end())
    m_auto_disc_change_paths.clear();
  else
    m_auto_disc_change_index = static_cast<size_t>(in_list - m_auto_disc_change_paths.begin());
}

// CPU thread only.
bool DVDInterface::AutoChangeDisc(const Core::CPUThreadGuard& guard)
{
  if (m_auto_disc_change_paths.empty())
    return false;

  const size_t next = (m_auto_disc_change_index + 1) % m_auto_disc_change_paths.size();
  m_auto_disc_change_index = next;
  ChangeDisc(guard, m_auto_disc_change_paths[next]);
  return true;
}

void DVDInterface::UpdateLid()
{
  const u32 open = IsDiscInside() ? 0 : 1;
  if (m_DICVR.CVR == open)
    return;
  m_DICVR.CVR = open;
  RaiseInterrupt(DIInterruptType::CVRINT);
}

bool DVDInterface::UpdateRunningGameMetadata(std::optional<u64> title_id)
{
  DVDThread& thread = m_system.GetDVDThread();
  if (!thread.HasDisc())
    return false;
  return thread.UpdateRunningGameMetadata(IOS::HLE::DIDevice::GetCurrentPartition(), title_id);
}

void DVDInterface::RegisterMMIO(MMIO::Mapping* mmio, u32 base, bool is_wii)
{
  // DISR: the mask and BREAK bits are written as given; a 1 written to an
  // interrupt flag acknowledges it.
  mmio->Register(base | REG_DISR, MMIO::DirectRead<u32>(&m_DISR.Hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   DVDInterface& di = system.GetDVDInterface();
                   const UDISR written(val);
                   constexpr u32 writable = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 5);
                   constexpr u32 acks = (1u << 2) | (1u << 4) | (1u << 6);

                   di.m_DISR.Hex = (di.m_DISR.Hex & ~writable) | (written.Hex & writable);
                   di.m_DISR.Hex &= ~(written.Hex & acks);

                   // Aborting a transfer isn't emulated.
                   if (di.m_DISR.BREAK)
                     DEBUG_ASSERT(false);

                   di.UpdateInterrupts();
                 }));

  mmio->Register(base | REG_DICVR, MMIO::DirectRead<u32>(&m_DICVR.Hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   DVDInterface& di = system.GetDVDInterface();
                   const UDICVR written(val);
                   di.m_DICVR.CVRINTMASK = written.CVRINTMASK.Value();
                   if (written.CVRINT)
                     di.m_DICVR.CVRINT = 0;
                   di.UpdateInterrupts();
                 }));

  // The command buffer is plain storage.
  for (u32 i = 0; i < 3; ++i)
  {
    mmio->Register(base | (REG_DICMDBUF0 + i * 4), MMIO::DirectRead<u32>(&m_DICMDBUF[i]),
                   MMIO::DirectWrite<u32>(&m_DICMDBUF[i]));
  }

  // DMA address: 32-byte aligned. On the GameCube register the top six bits
  // are not stored either; the Wii register keeps them because MEM2 lives up
  // there. RegisterMMIO is called for both blocks in Wii mode, so this follows
  // the block being written rather than the console mode.
  const u32 address_mask = is_wii ? ~0x1Fu : ~0xFC00001Fu;
  mmio->Register(base | REG_DIMAR, MMIO::DirectRead<u32>(&m_DIMAR),
                 MMIO::DirectWrite<u32>(&m_DIMAR, address_mask));
  mmio->Register(base | REG_DILENGTH, MMIO::DirectRead<u32>(&m_DILENGTH),
                 MMIO::DirectWrite<u32>(&m_DILENGTH, ~0x1Fu));

  // Writing TSTART runs the command.
  mmio->Register(base | REG_DICR, MMIO::DirectRead<u32>(&m_DICR.Hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   DVDInterface& di = system.GetDVDInterface();
                   di.m_DICR.Hex = val & 7;
                   if (di.m_DICR.TSTART)
                     di.ExecuteCommand(ReplyType::Interrupt);
                 }));

  mmio->Register(base | REG_DIIMMBUF, MMIO::DirectRead<u32>(&m_DIIMMBUF),
                 MMIO::DirectWrite<u32>(&m_DIIMMBUF));

  mmio->Register(base | REG_DICFG, MMIO::DirectRead<u32>(&m_DICFG.Hex),
                 MMIO::InvalidWrite<u32>());
}

void DVDInterface::UpdateInterrupts()
{
  // Each flag sits one bit above its enable mask.
  constexpr u32 disr_flags = FlagBit(DIInterruptType::DEINT) | FlagBit(DIInterruptType::TCINT) |
                             FlagBit(DIInterruptType::BRKINT);
  constexpr u32 dicvr_flags = FlagBit(DIInterruptType::CVRINT);

  const bool pending = (m_DISR.Hex & (m_DISR.Hex << 1) & disr_flags) != 0 ||
                       (m_DICVR.Hex & (m_DICVR.Hex << 1) & dicvr_flags) != 0;

  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_DI, pending);

  // Some games (Summoner: A Goddess Reborn) need the interrupt to be taken
  // promptly.
  m_system.GetCoreTiming().ForceExceptionCheck(50);
}

void DVDInterface::RaiseInterrupt(DIInterruptType type)
{
  u32& reg = type == DIInterruptType::CVRINT ? m_DICVR.Hex : m_DISR.Hex;
  reg |= FlagBit(type);
  UpdateInterrupts();
}

void DVDInterface::SetInterruptEnabled(DIInterruptType interrupt, bool enabled)
{
  u32& reg = interrupt == DIInterruptType::CVRINT ? m_DICVR.Hex : m_DISR.Hex;
  const u32 mask_bit = FlagBit(interrupt) >> 1;
  reg = enabled ? (reg | mask_bit) : (reg & ~mask_bit);
}

void DVDInterface::ClearInterrupt(DIInterruptType interrupt)
{
  u32& reg = interrupt == DIInterruptType::CVRINT ? m_DICVR.Hex : m_DISR.Hex;
  reg &= ~FlagBit(interrupt);
}

// Checks whether the drive is in a state where it can read. On failure the
// error code is set and the caller reports DEINT.
bool DVDInterface::DriveCanRead()
{
  DriveError error = DriveError::None;

  if (!IsDiscInside())
    error = DriveError::MediumNotPresent;
  else if (m_drive_state == DriveState::DiscChangeDetected)
    error = DriveError::MediumChanged;
  else if (m_drive_state == DriveState::MotorStopped)
    error = DriveError::MotorStopped;
  // The Triforce boot program never reads the disc ID first.
  else if (m_drive_state == DriveState::DiscIdNotRead && !m_system.IsTriforce())
    error = DriveError::NoDiscID;

  if (error == DriveError::None)
    return true;

  ERROR_LOG_FMT(DVDINTERFACE, "Drive cannot read (state {}, error {:#07x})",
                static_cast<int>(m_drive_state), static_cast<u32>(error));
  SetDriveError(error);
  return false;
}

// Returns true iff the read was handed to DVDThread (which then reports
// completion); otherwise the caller must schedule the completion itself.
bool DVDInterface::StartDiscRead(u64 dvd_offset, u32 output_address, u32 dvd_length,
                                 u32 output_length, const DiscIO::Partition& partition,
                                 ReplyType reply_type, DIInterruptType* interrupt_type)
{
  if (!DriveCanRead())
  {
    *interrupt_type = DIInterruptType::DEINT;
    return false;
  }
  *interrupt_type = DIInterruptType::TCINT;

  if (dvd_length > output_length)
  {
    WARN_LOG_FMT(DVDINTERFACE, "Disc read of {:#x} bytes into a {:#x} byte buffer, truncating",
                 dvd_length, output_length);
    dvd_length = output_length;
  }

  // Reads past the end of a pressed disc must fail: many Wii games read just
  // beyond it (still inside a DVD-R) and refuse to boot with Error #001 if
  // that succeeds.
  if (dvd_offset + dvd_length > m_disc_end_offset)
  {
    SetDriveError(DriveError::BlockOOB);
    *interrupt_type = DIInterruptType::DEINT;
    return false;
  }

  ScheduleReads(dvd_offset, dvd_length, partition, output_address, reply_type);
  return true;
}

void DVDInterface::Fail(DriveError error, CommandResult& result)
{
  SetDriveError(error);
  result.interrupt = DIInterruptType::DEINT;
}

void DVDInterface::ScheduleCommandCompletion(ReplyType reply_type, DIInterruptType interrupt)
{
  const u64 ticks_per_us = m_system.GetSystemTimers().GetTicksPerSecond() / 1000000;
  m_system.GetCoreTiming().ScheduleEvent(static_cast<s64>(COMMAND_LATENCY_US * ticks_per_us),
                                         m_finish_executing_command,
                                         EncodeCompletion(reply_type, interrupt));
}

void DVDInterface::ExecuteCommand(ReplyType reply_type)
{
  if (m_system.IsTriforce())
  {
    // The media board handles most commands itself. When it does, the
    // transfer completes at once.
    if (!AMMediaboard::ExecuteCommand(m_DICMDBUF, &m_DIIMMBUF, m_DIMAR, m_DILENGTH))
    {
      m_DICR.TSTART = 0;
      m_DIMAR += m_DILENGTH;
      m_DILENGTH = 0;
      RaiseInterrupt(DIInterruptType::TCINT);
      m_error_code = DriveError::None;
      return;
    }
    // Otherwise it left a plain drive opcode in the low byte.
    m_DICMDBUF[0] <<= 24;
  }

  CommandResult result;
  RunDriveCommand(reply_type, result);

  if (!result.queued_on_thread)
    ScheduleCommandCompletion(reply_type, result.interrupt);
}

void DVDInterface::RunDriveCommand(ReplyType reply_type, CommandResult& result)
{
  const u32 cmd0 = m_DICMDBUF[0];
  const u32 cmd1 = m_DICMDBUF[1];
  const u32 cmd2 = m_DICMDBUF[2];
  const auto command = static_cast<DICommand>(cmd0 >> 24);

  // Every command but RequestError starts by clearing the last error, since
  // RequestError is how software reads it.
  if (command != DICommand::RequestError)
    SetDriveError(DriveError::None);

  const auto unsupported = [&](const char* name, std::optional<GameQuirk> quirk = {}) {
    ERROR_LOG_FMT(DVDINTERFACE, "{} is not supported by retail drives ({:#010x})", name, cmd0);
    if (quirk)
      DolphinAnalytics::Instance().ReportGameQuirk(*quirk);
    Fail(DriveError::InvalidCommand, result);
  };

  switch (command)
  {
  case DICommand::Inquiry:
  {
    // Drive identification as returned by a retail Wii drive.
    auto& memory = m_system.GetMemory();
    memory.Write_U32(0x00000002, m_DIMAR);      // revision / device code
    memory.Write_U32(0x20060526, m_DIMAR + 4);  // firmware date
    memory.Write_U32(0x41000000, m_DIMAR + 8);  // version
    INFO_LOG_FMT(DVDINTERFACE, "Inquiry into {:#010x} ({:#x} bytes)", m_DIMAR, m_DILENGTH);
    break;
  }

  // Commands of modified GameCube drive firmware (libogc uses them to probe).
  case DICommand::Unknown55:
  case DICommand::UnknownEE:
    INFO_LOG_FMT(DVDINTERFACE, "Modchip command {:#010x} rejected", cmd0);
    Fail(DriveError::InvalidCommand, result);
    break;

  // Only works on development drives; retail games use it to detect them.
  case DICommand::ReportKey:
    INFO_LOG_FMT(DVDINTERFACE, "ReportKey rejected");
    Fail(DriveError::InvalidCommand, result);
    break;

  case DICommand::Read:
  {
    const u32 subcommand = cmd0 & 0xFF;
    if (subcommand == 0x00)
    {
      const u64 dvd_offset = u64{cmd1} << 2;
      INFO_LOG_FMT(DVDINTERFACE, "Read {:#x} bytes at {:#011x} into {:#010x} (DMA {:#x})", cmd2,
                   dvd_offset, m_DIMAR, m_DILENGTH);
      if (m_drive_state == DriveState::ReadyNoReadsMade)
        SetDriveState(DriveState::Ready);
      result.queued_on_thread =
          StartDiscRead(dvd_offset, m_DIMAR, cmd2, m_DILENGTH, DiscIO::PARTITION_NONE, reply_type,
                        &result.interrupt);
    }
    else if (subcommand == 0x40)
    {
      // Disc ID. The first one moves the drive to ReadyNoReadsMade, the window
      // in which AudioBufferConfig is allowed; a second one closes it.
      INFO_LOG_FMT(DVDINTERFACE, "Read disc ID into {:#010x}", m_DIMAR);
      if (m_drive_state == DriveState::DiscIdNotRead)
        SetDriveState(DriveState::ReadyNoReadsMade);
      else if (m_drive_state == DriveState::ReadyNoReadsMade)
        SetDriveState(DriveState::Ready);
      result.queued_on_thread = StartDiscRead(0, m_DIMAR, 0x20, m_DILENGTH,
                                              DiscIO::PARTITION_NONE, reply_type,
                                              &result.interrupt);
    }
    else
    {
      ERROR_LOG_FMT(DVDINTERFACE, "Read with unknown subcommand {:#010x}", cmd0);
    }
    break;
  }

  case DICommand::Seek:
    // Not modelled; reads account for head movement themselves.
    INFO_LOG_FMT(DVDINTERFACE, "Seek to {:#011x} ignored", u64{cmd1} << 2);
    break;

  case DICommand::ReadDVDMetadata:
  {
    static constexpr std::array<const char*, 3> kinds{"physical", "copyright", "disc key"};
    const u32 kind = (cmd0 >> 16) & 0xFF;
    ERROR_LOG_FMT(DVDINTERFACE, "ReadDVDMetadata ({}) rejected",
                  kind < kinds.size() ? kinds[kind] : "unknown");
    Fail(DriveError::InvalidCommand, result);
    break;
  }
  case DICommand::ReadDVD:
    unsupported("ReadDVD");
    break;
  case DICommand::ReadDVDConfig:
    unsupported("ReadDVDConfig");
    break;
  case DICommand::StopLaser:
    unsupported("StopLaser", GameQuirk::UsesDVDLowStopLaser);
    break;
  case DICommand::Offset:
    unsupported("Offset", GameQuirk::UsesDVDLowOffset);
    break;
  case DICommand::RequestDiscStatus:
    unsupported("RequestDiscStatus", GameQuirk::UsesDVDLowRequestDiscStatus);
    break;
  case DICommand::RequestRetryNumber:
    unsupported("RequestRetryNumber", GameQuirk::UsesDVDLowRequestRetryNumber);
    break;
  case DICommand::SetMaximumRotation:
    unsupported("SetMaximumRotation");
    break;
  case DICommand::SerMeasControl:
    unsupported("SerMeasControl", GameQuirk::UsesDVDLowSerMeasControl);
    break;
  case DICommand::Debug:
    unsupported("Drive debug command");
    break;

  case DICommand::ReadBCA:
  {
    // No real BCA is available. New Super Mario Bros. Wii only checks that
    // byte 0x33 is 1 and everything before it is 0, so supply exactly that.
    WARN_LOG_FMT(DVDINTERFACE, "ReadBCA: returning placeholder data");
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesDVDLowReadDiskBCA);
    auto& memory = m_system.GetMemory();
    memory.Memset(m_DIMAR, 0, 0x40);
    memory.Write_U8(1, m_DIMAR + 0x33);
    break;
  }

  case DICommand::RequestError:
  {
    const u32 state_code = m_drive_state == DriveState::Ready ?
                               0 :
                               static_cast<u32>(m_drive_state) - 1;
    m_DIIMMBUF = (state_code << 24) | static_cast<u32>(m_error_code);
    INFO_LOG_FMT(DVDINTERFACE, "RequestError -> {:#010x}", m_DIIMMBUF);
    SetDriveError(DriveError::None);
    break;
  }

  case DICommand::AudioStream:
    RunAudioStreamCommand(result);
    break;

  case DICommand::RequestAudioStatus:
    RunAudioStatusCommand(result);
    break;

  case DICommand::StopMotor:
    RunStopMotorCommand();
    break;

  case DICommand::AudioBufferConfig:
    RunAudioBufferConfigCommand(result);
    break;

  case DICommand::DebugUnlock:
  {
    // The two-step unlock of the Matsushita drive ("MATSHITA", "DVD-GAME").
    // Nothing is unlocked; the sequence is only recognised for the log.
    const bool step1 = cmd0 == 0xFF014D41 && cmd1 == 0x54534849 && cmd2 == 0x54410200;
    const bool step2 = cmd0 == 0xFF004456 && cmd1 == 0x442D4741 && cmd2 == 0x4D450300;
    INFO_LOG_FMT(DVDINTERFACE, "Drive unlock: {}",
                 step1 ? "step 1" : (step2 ? "step 2" : "not recognised"));
    break;
  }

  default:
    ERROR_LOG_FMT(DVDINTERFACE, "Unknown drive command {:#010x} (buffer {:#010x}, {:#x} bytes)",
                  cmd0, m_DIMAR, m_DILENGTH);
    PanicAlertFmtT("Unknown DVD command {0:08x} - fatal error", cmd0);
    Fail(DriveError::InvalidCommand, result);
    break;
  }
}

// DICMDBUF0 bits 16-23 select start (0) or stop (1); DICMDBUF1 << 2 is the
// track offset and DICMDBUF2 its length.
void DVDInterface::RunAudioStreamCommand(CommandResult& result)
{
  if (!DriveCanRead())
  {
    result.interrupt = DIInterruptType::DEINT;
    return;
  }
  if (!m_audio.enabled)
  {
    ERROR_LOG_FMT(DVDINTERFACE, "Audio stream command while streaming is disabled");
    Fail(DriveError::NoAudioBuf, result);
    return;
  }

  if (m_drive_state == DriveState::ReadyNoReadsMade)
    SetDriveState(DriveState::Ready);

  switch ((m_DICMDBUF[0] >> 16) & 0xFF)
  {
  case 0x00:
  {
    const u64 start = u64{m_DICMDBUF[1]} << 2;
    const u32 length = m_DICMDBUF[2];
    INFO_LOG_FMT(DVDINTERFACE, "Audio stream: queue {:#x} bytes at {:#x}", length, start);

    // Zero offset and length means "finish the current track, then stop".
    // Once that is requested, further tracks are ignored.
    if (start == 0 && length == 0)
    {
      m_audio.stop_at_track_end = true;
    }
    else if (!m_audio.stop_at_track_end)
    {
      m_audio.next_start = start;
      m_audio.next_length = length;
      if (!m_audio.playing)
      {
        m_audio.current_start = start;
        m_audio.current_length = length;
        m_audio.position = start;
        m_adpcm_decoder.ResetFilter();
        m_audio.playing = true;
      }
    }
    break;
  }
  case 0x01:
    INFO_LOG_FMT(DVDINTERFACE, "Audio stream: stop");
    m_audio.stop_at_track_end = false;
    m_audio.playing = false;
    break;
  default:
    ERROR_LOG_FMT(DVDINTERFACE, "Unknown audio stream command {:08x} {:08x} {:08x}",
                  m_DICMDBUF[0], m_DICMDBUF[1], m_DICMDBUF[2]);
    Fail(DriveError::InvalidAudioCommand, result);
    break;
  }
}

void DVDInterface::RunAudioStatusCommand(CommandResult& result)
{
  if (!DriveCanRead())
  {
    result.interrupt = DIInterruptType::DEINT;
    return;
  }
  if (!m_audio.enabled)
  {
    ERROR_LOG_FMT(DVDINTERFACE, "Audio status request while streaming is disabled");
    Fail(DriveError::NoAudioBuf, result);
    return;
  }

  switch ((m_DICMDBUF[0] >> 16) & 0xFF)
  {
  case 0x00:  // playing?
    m_DIIMMBUF = m_audio.playing ? 1 : 0;
    break;
  case 0x01:  // current position, in 32 KiB units, as a word address
    m_DIIMMBUF = static_cast<u32>((m_audio.position & ~u64{0x7FFF}) >> 2);
    break;
  case 0x02:  // track start, as a word address
    m_DIIMMBUF = static_cast<u32>(m_audio.current_start >> 2);
    break;
  case 0x03:  // track length
    m_DIIMMBUF = m_audio.current_length;
    break;
  default:
    ERROR_LOG_FMT(DVDINTERFACE, "Unknown audio status request {:08x} {:08x} {:08x}",
                  m_DICMDBUF[0], m_DICMDBUF[1], m_DICMDBUF[2]);
    Fail(DriveError::InvalidAudioCommand, result);
    return;
  }
  INFO_LOG_FMT(DVDINTERFACE, "Audio status {} -> {:#x} (pos {:#x}, track {:#x}+{:#x})",
               (m_DICMDBUF[0] >> 16) & 0xFF, m_DIIMMBUF, m_audio.position,
               m_audio.current_start, m_audio.current_length);
}

void DVDInterface::RunStopMotorCommand()
{
  const u32 cmd0 = m_DICMDBUF[0];
  const bool eject = (cmd0 & (1u << 17)) != 0;
  const bool kill = (cmd0 & (1u << 20)) != 0;
  INFO_LOG_FMT(DVDINTERFACE, "StopMotor (eject={}, kill={})", eject, kill);

  switch (m_drive_state)
  {
  case DriveState::Ready:
  case DriveState::ReadyNoReadsMade:
  case DriveState::DiscIdNotRead:
    SetDriveState(DriveState::MotorStopped);
    break;
  default:
    break;
  }

  const bool software_eject = eject && !kill;

  // Multi-disc games stop the motor when asking for the next disc; with the
  // auto-change option that is the cue to swap discs.
  const bool auto_change = Config::Get(Config::MAIN_AUTO_DISC_CHANGE) &&
                           !m_system.GetMovie().IsPlayingInput() &&
                           m_system.GetDVDThread().IsInsertedDiscRunning() &&
                           !m_auto_disc_change_paths.empty();
  if (auto_change)
  {
    const u64 delay = software_eject ? 0 : m_system.GetSystemTimers().GetTicksPerSecond() / 2;
    m_system.GetCoreTiming().ScheduleEvent(static_cast<s64>(delay), m_auto_change_disc);
    OSD::AddMessage("Changing discs automatically...", OSD::Duration::NORMAL);
  }
  else if (software_eject)
  {
    EjectDisc(Core::CPUThreadGuard{m_system}, EjectCause::Software);
  }
}

// Enables or disables streamed audio. The IPL sends this based on the
// streaming flag in the disc header. It is only accepted between the first
// disc ID read and the first real read.
void DVDInterface::RunAudioBufferConfigCommand(CommandResult& result)
{
  if (!DriveCanRead())
  {
    result.interrupt = DIInterruptType::DEINT;
    return;
  }
  if (m_drive_state == DriveState::Ready)
  {
    ERROR_LOG_FMT(DVDINTERFACE, "AudioBufferConfig after the first read");
    Fail(DriveError::InvalidPeriod, result);
    return;
  }

  // Can be repeated; the drive stays in ReadyNoReadsMade.
  AudioBufferConfig(((m_DICMDBUF[0] >> 16) & 1) != 0, m_DICMDBUF[0] & 0xF);
}

void DVDInterface::PerformDecryptingRead(u32 position, u32 length, u32 output_address,
                                         const DiscIO::Partition& partition, ReplyType reply_type)
{
  if (m_drive_state == DriveState::ReadyNoReadsMade)
    SetDriveState(DriveState::Ready);

  DIInterruptType interrupt = DIInterruptType::TCINT;
  const bool queued = StartDiscRead(u64{position} << 2, output_address, length, length, partition,
                                    reply_type, &interrupt);
  if (!queued)
    ScheduleCommandCompletion(reply_type, interrupt);
}

void DVDInterface::ForceOutOfBoundsRead(ReplyType reply_type)
{
  INFO_LOG_FMT(DVDINTERFACE, "Answering a read with a forced out-of-bounds error");

  if (m_drive_state == DriveState::ReadyNoReadsMade)
    SetDriveState(DriveState::Ready);

  SetDriveError(DriveError::BlockOOB);
  ScheduleCommandCompletion(reply_type, DIInterruptType::DEINT);
}

void DVDInterface::AudioBufferConfig(bool enable_dtk, u8 dtk_buffer_length)
{
  m_audio.enabled = enable_dtk;
  m_audio.buffer_length = dtk_buffer_length;
  INFO_LOG_FMT(DVDINTERFACE, "Streamed audio {} (buffer setting {})",
               enable_dtk ? "enabled" : "disabled", dtk_buffer_length);
}

void DVDInterface::SetDriveState(DriveState state)
{
  m_drive_state = state;
}

void DVDInterface::SetDriveError(DriveError error)
{
  m_error_code = error;
}

void DVDInterface::FinishExecutingCommandCallback(Core::System& system, u64 userdata,
                                                  s64 cycles_late)
{
  const auto reply_type = static_cast<ReplyType>(userdata >> 32);
  const auto interrupt = static_cast<DIInterruptType>(static_cast<u32>(userdata));
  system.GetDVDInterface().FinishExecutingCommand(reply_type, interrupt, cycles_late);
}

// `data` is only non-empty when DVDThread completes a read. NoReply and DTK
// replies only ever come from DVDThread.
void DVDInterface::FinishExecutingCommand(ReplyType reply_type, DIInterruptType interrupt_type,
                                          s64 cycles_late, std::span<const u8> data)
{
  if (interrupt_type == DIInterruptType::TCINT)
  {
    u32 transferred = 0;
    switch (reply_type)
    {
    case ReplyType::NoReply:
      transferred = static_cast<u32>(data.size());
      break;
    case ReplyType::Interrupt:
    case ReplyType::IOS:
      transferred = m_DILENGTH;
      break;
    case ReplyType::DTK:
      break;
    }
    m_DIMAR += transferred;
    m_DILENGTH -= transferred;
  }

  switch (reply_type)
  {
  case ReplyType::NoReply:
    break;
  case ReplyType::Interrupt:
    if (m_DICR.TSTART)
    {
      m_DICR.TSTART = 0;
      RaiseInterrupt(interrupt_type);
    }
    break;
  case ReplyType::IOS:
    IOS::HLE::DIDevice::InterruptFromDVDInterface(interrupt_type);
    break;
  case ReplyType::DTK:
    StreamAudioBlocks(interrupt_type, data, cycles_late);
    break;
  }
}

// Moves the stream position forward by up to max_blocks ADPCM blocks,
// following the queued next track at the end of the current one. Returns
// the number of bytes to read and leaves the block count in pending_blocks.
u32 DVDInterface::AdvanceStreamPosition(u32 max_blocks)
{
  u32 blocks = 0;
  while (blocks < max_blocks)
  {
    if (m_audio.position >= m_audio.current_start + m_audio.current_length)
    {
      DEBUG_LOG_FMT(DVDINTERFACE, "Audio track end at {:#x}, next track {:#x}+{:#x}",
                    m_audio.position, m_audio.next_start, m_audio.next_length);
      m_audio.position = m_audio.next_start;
      m_audio.current_start = m_audio.next_start;
      m_audio.current_length = m_audio.next_length;

      if (m_audio.stop_at_track_end)
      {
        m_audio.stop_at_track_end = false;
        m_audio.playing = false;
        break;
      }
      m_adpcm_decoder.ResetFilter();
    }

    m_audio.position += StreamADPCM::ONE_BLOCK_SIZE;
    ++blocks;
  }

  m_audio.pending_blocks = blocks;
  return blocks * StreamADPCM::ONE_BLOCK_SIZE;
}

// The streaming loop: each round hands the blocks read last time to the
// mixer, picks the next blocks and schedules their read so that it completes
// when those samples are due.
void DVDInterface::StreamAudioBlocks(DIInterruptType interrupt_type,
                                     std::span<const u8> audio_data, s64 cycles_late)
{
  auto& ai = m_system.GetAudioInterface();
  const u32 rate_divisor = ai.GetAISSampleRateDivisor();
  // About 3.5 ms of audio per round.
  const u32 blocks_per_round = ai.GetAISSampleRate() == AudioInterface::SampleRate::AI32KHz ? 4 : 6;
  constexpr u32 MAX_BLOCKS = 6;
  constexpr u32 SAMPLES = StreamADPCM::SAMPLES_PER_BLOCK;

  u64 read_offset = 0;
  u32 read_length = 0;
  bool reading = false;

  if (interrupt_type == DIInterruptType::TCINT)
  {
    ASSERT(m_audio.pending_blocks <= MAX_BLOCKS);
    const u32 blocks = std::min(m_audio.pending_blocks, MAX_BLOCKS);
    const size_t blocks_available = audio_data.size() / StreamADPCM::ONE_BLOCK_SIZE;
    const size_t blocks_to_decode = std::min<size_t>(blocks, blocks_available);

    // Stereo, two s16 per sample. The mixer takes big-endian samples.
    std::array<s16, MAX_BLOCKS * SAMPLES * 2> pcm{};
    for (size_t b = 0; b < blocks_to_decode; ++b)
    {
      s16* out = &pcm[b * SAMPLES * 2];
      m_adpcm_decoder.DecodeBlock(out, &audio_data[b * StreamADPCM::ONE_BLOCK_SIZE]);
      for (u32 s = 0; s < SAMPLES * 2; ++s)
        out[s] = static_cast<s16>(Common::swap16(static_cast<u16>(out[s])));
    }
    m_system.GetSoundStream()->GetMixer()->PushStreamingSamples(pcm.data(), blocks * SAMPLES);

    if (m_audio.playing && ai.IsPlaying())
    {
      read_offset = m_audio.position;
      read_length = AdvanceStreamPosition(blocks_per_round);
      reading = read_length > 0;
    }
    else
    {
      m_audio.pending_blocks = blocks_per_round;
    }
  }
  else
  {
    m_audio.pending_blocks = blocks_per_round;
  }

  // The time the pending samples take to play.
  const u64 play_ticks = u64{m_system.GetSystemTimers().GetTicksPerSecond()} *
                         m_audio.pending_blocks * SAMPLES * rate_divisor /
                         Mixer::FIXED_SAMPLE_RATE_DIVIDEND;
  const s64 delay = static_cast<s64>(play_ticks) - cycles_late;

  if (reading)
  {
    m_system.GetDVDThread().StartRead(read_offset, read_length, DiscIO::PARTITION_NONE,
                                      ReplyType::DTK, delay);
  }
  else
  {
    m_system.GetCoreTiming().ScheduleEvent(delay, m_finish_executing_command,
                                           EncodeCompletion(ReplyType::DTK, DIInterruptType::TCINT));
  }
}

// Splits a read into chunks and works out when each one completes, taking
// the drive's read-ahead buffer, seeks and rotation into account. Each chunk
// becomes one DVDThread request; only the last one carries the reply.
//
// Timing matters to some games (Metroid Prime's doors, Pitfall's loading
// screens), so these numbers follow hardware measurements closely.
void DVDInterface::ScheduleReads(u64 offset, u32 length, const DiscIO::Partition& partition,
                                 u32 output_address, ReplyType reply_type)
{
  auto& core_timing = m_system.GetCoreTiming();
  DVDThread& thread = m_system.GetDVDThread();
  const u64 now = core_timing.GetTicks();
  const u32 tps = m_system.GetSystemTimers().GetTicksPerSecond();
  const bool wii_disc = thread.GetDiscType() == DiscIO::Platform::WiiDisc;

  // The drive works on whole ECC blocks at raw (non-partition) offsets.
  u64 block = Common::AlignDown(thread.PartitionOffsetToRawOffset(offset, partition),
                                ECC_BLOCK_SIZE);
  const u64 first_block = block;

  // What the read-ahead buffer holds right now, [cached_begin, cached_end),
  // and where the head is.
  u64 cached_begin = 0;
  u64 cached_end = 0;
  u64 head = 0;

  if (Config::Get(Config::MAIN_FAST_DISC_SPEED))
  {
    // "Speed up disc transfer rate": pretend everything is buffered.
    cached_begin = std::numeric_limits<u64>::min();
    cached_end = std::numeric_limits<u64>::max();
  }
  else if (m_read_ahead.start_time != m_read_ahead.end_time)
  {
    cached_begin = m_read_ahead.end_offset > READ_AHEAD_SIZE ?
                       m_read_ahead.end_offset - READ_AHEAD_SIZE :
                       0;

    if (now >= m_read_ahead.end_time)
    {
      cached_end = m_read_ahead.end_offset;
    }
    else
    {
      // Partially filled: interpolate linearly, in whole ECC blocks.
      const u64 span = m_read_ahead.end_offset - m_read_ahead.start_offset;
      const u64 duration = m_read_ahead.end_time - m_read_ahead.start_time;
      const u64 filled = (now - m_read_ahead.start_time) * span / duration;
      cached_end = m_read_ahead.start_offset + Common::AlignDown(filled, ECC_BLOCK_SIZE);
    }
    head = cached_end;

    // A read before the buffer throws the buffer away (the head stays put).
    if (block < cached_begin)
      cached_begin = cached_end = 0;
  }

  DEBUG_LOG_FMT(DVDINTERFACE, "Read-ahead holds [{:#x}, {:#x}); request {:#x} bytes at {:#x}",
                cached_begin, cached_end, length, offset);

  s64 ticks = READ_START_LATENCY_US * (tps / 1000000);
  bool seeked = false;
  u32 hits = 0;
  u32 misses = 0;

  // Chunks follow the decrypted 0x7C00-byte blocks for hashed Wii
  // partitions and ECC blocks otherwise.
  const u32 chunk_size = partition != DiscIO::PARTITION_NONE && thread.HasWiiHashes() ?
                             DiscIO::VolumeWii::BLOCK_DATA_SIZE :
                             static_cast<u32>(ECC_BLOCK_SIZE);

  do
  {
    // Up to the next chunk boundary (a full chunk if already aligned).
    const u32 to_boundary = static_cast<u32>(Common::AlignUp(offset + 1, chunk_size) - offset);
    const u32 chunk = std::min(to_boundary, length);

    if (block >= cached_begin && block < cached_end)
    {
      // Buffer hit: just the transfer to memory.
      ticks += u64{chunk} * tps / BUFFERED_BYTES_PER_SECOND;
      ++hits;
    }
    else
    {
      if (block == head)
      {
        // Head already in place: stream one ECC block.
        ticks += static_cast<u64>(tps *
                                  DVDMath::CalculateRawDiscReadTime(block, ECC_BLOCK_SIZE, wii_disc));
      }
      else
      {
        // Seek (which includes reading one ECC block), then wait for the
        // block to come round.
        seeked = true;
        ticks += static_cast<u64>(tps * DVDMath::CalculateSeekTime(head, block));

        // Whole seconds only: the tick count is divided as an integer.
        const double seconds_after_seek =
            static_cast<double>((core_timing.GetTicks() + static_cast<u64>(ticks)) / tps);
        ticks = static_cast<s64>(
            static_cast<double>(ticks) +
            tps * DVDMath::CalculateRotationalLatency(block, seconds_after_seek, wii_disc));
        DEBUG_LOG_FMT(DVDINTERFACE, "Seek for {:#x} bytes at {:#x}, now {} ticks", chunk, offset,
                      ticks);
      }
      ++misses;
      head = block + ECC_BLOCK_SIZE;
    }

    const ReplyType chunk_reply = chunk == length ? reply_type : ReplyType::NoReply;
    thread.StartReadToEmulatedRAM(output_address, offset, chunk, partition, chunk_reply, ticks);

    output_address += chunk;
    offset += chunk;
    length -= chunk;
    block += ECC_BLOCK_SIZE;
  } while (length > 0);

  // Update the read-ahead window. The drive keeps the last block, the one
  // before it and everything after, and prefetches from where it stopped.
  // Re-reading within the first two blocks of a live buffer changes nothing.
  const u64 last_block = block;
  const bool within_buffer_start =
      last_block - cached_begin <= READ_AHEAD_KEEP_BEHIND && cached_begin != cached_end;
  if (!within_buffer_start)
  {
    // start_offset marks where prefetching resumes, not the buffer's start
    // (which is always end_offset - READ_AHEAD_SIZE).
    m_read_ahead.start_offset = last_block >= cached_end ? last_block : cached_end;

    m_read_ahead.end_offset = last_block + READ_AHEAD_SIZE - READ_AHEAD_KEEP_BEHIND;
    // After a seek, the block before the first one read was never buffered.
    if (seeked)
      m_read_ahead.end_offset = std::max(m_read_ahead.end_offset, first_block + READ_AHEAD_SIZE);

    m_read_ahead.start_time = now + static_cast<u64>(ticks);
    const double fill_seconds = DVDMath::CalculateRawDiscReadTime(
        m_read_ahead.start_offset, m_read_ahead.end_offset - m_read_ahead.start_offset, wii_disc);
    m_read_ahead.end_time = m_read_ahead.start_time + static_cast<u64>(tps * fill_seconds);
  }

  DEBUG_LOG_FMT(DVDINTERFACE, "Read scheduled: {} blocks from disc, {} from buffer, {} ticks",
                misses, hits, ticks);
}

}  // namespace DVD
