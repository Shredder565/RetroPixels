// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"

#include "Core/HW/StreamADPCM.h"

class PointerWrap;
namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core
namespace CoreTiming
{
struct EventType;
}
namespace DiscIO
{
class VolumeDisc;
struct Partition;
}  // namespace DiscIO
namespace MMIO
{
class Mapping;
}

namespace DVD
{
// Drive command opcodes (top byte of DICMDBUF0).
enum class DICommand : u8
{
  Inquiry = 0x12,
  ReportKey = 0xa4,
  Read = 0xa8,
  Seek = 0xab,
  ReadDVDMetadata = 0xad,
  ReadDVD = 0xd0,
  ReadDVDConfig = 0xd1,
  StopLaser = 0xd2,
  Offset = 0xd9,
  ReadBCA = 0xda,
  RequestDiscStatus = 0xdb,
  RequestRetryNumber = 0xdc,
  SetMaximumRotation = 0xdd,
  SerMeasControl = 0xdf,
  RequestError = 0xe0,
  AudioStream = 0xe1,
  RequestAudioStatus = 0xe2,
  StopMotor = 0xe3,
  AudioBufferConfig = 0xe4,
  Debug = 0xfe,
  DebugUnlock = 0xff,
  Unknown55 = 0x55,
  UnknownEE = 0xee,
};

// State of the drive itself. In the RequestError reply, Ready and
// ReadyNoReadsMade both read as 0 and every later state as (value - 1).
enum class DriveState : u8
{
  Ready = 0,
  ReadyNoReadsMade = 1,
  CoverOpened = 2,
  DiscChangeDetected = 3,
  NoMediumPresent = 4,
  MotorStopped = 5,
  DiscIdNotRead = 6
};

// Low 24 bits of the RequestError reply: a SCSI-style sense key nibble
// followed by the additional sense code and qualifier.
enum class DriveError : u32
{
  None = 0x00000,
  MotorStopped = 0x20400,
  NoDiscID = 0x20401,
  MediumNotPresent = 0x23a00,  // also reported while the lid is open
  SeekNotDone = 0x30200,
  ReadError = 0x31100,
  ProtocolError = 0x40800,
  InvalidCommand = 0x52000,
  NoAudioBuf = 0x52001,  // audio streaming has not been enabled
  BlockOOB = 0x52100,
  InvalidField = 0x52400,
  InvalidAudioCommand = 0x52401,
  InvalidPeriod = 0x52402,  // command no longer allowed at this point
  EndOfUserArea = 0x56300,
  MediumChanged = 0x62800,
  MediumRemovalRequest = 0xb5a01,
};

enum class DIInterruptType : int
{
  DEINT = 0,
  TCINT = 1,
  BRKINT = 2,
  CVRINT = 3,
};

// Who gets told when a command finishes.
enum class ReplyType : u32
{
  NoReply,
  Interrupt,
  IOS,
  DTK,
};

enum class EjectCause
{
  User,
  Software,
};

class DVDInterface
{
public:
  explicit DVDInterface(Core::System& system);
  DVDInterface(const DVDInterface&) = delete;
  DVDInterface(DVDInterface&&) = delete;
  DVDInterface& operator=(const DVDInterface&) = delete;
  DVDInterface& operator=(DVDInterface&&) = delete;
  ~DVDInterface();

  void Init();
  void ResetDrive(bool spinup);
  void Shutdown();
  void DoState(PointerWrap& p);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base, bool is_wii);

  void SetDisc(std::unique_ptr<DiscIO::VolumeDisc> disc,
               std::optional<std::vector<std::string>> auto_disc_change_paths);
  bool IsDiscInside() const;
  void EjectDisc(const Core::CPUThreadGuard& guard, EjectCause cause);
  void ChangeDisc(const Core::CPUThreadGuard& guard, const std::vector<std::string>& paths);
  void ChangeDisc(const Core::CPUThreadGuard& guard, const std::string& new_path);
  bool AutoChangeDisc(const Core::CPUThreadGuard& guard);

  // Calls SConfig::SetRunningGameMetadata(Volume&, Partition&) and returns true
  // when a disc is inserted and either no title_id is given or it equals the
  // disc's title ID. Returns false otherwise.
  bool UpdateRunningGameMetadata(std::optional<u64> title_id = {});

  // Entry points for the IOS /dev/di emulation, which talks to the drive
  // directly instead of through the MMIO registers (and so can skip the
  // disc encryption).
  void ExecuteCommand(ReplyType reply_type);
  void PerformDecryptingRead(u32 position, u32 length, u32 output_address,
                             const DiscIO::Partition& partition, ReplyType reply_type);

  // Makes the next read fail with BlockOOB. Used so that DirectoryBlobs, which
  // can contain data beyond a real disc's end, still pass the Error #001 check.
  void ForceOutOfBoundsRead(ReplyType reply_type);

  // For the emulated IPL. Bypasses the drive state checks.
  void AudioBufferConfig(bool enable_dtk, u8 dtk_buffer_length);

  void SetDriveState(DriveState state);
  void SetDriveError(DriveError error);

  // Called by DVDThread when a read is due.
  void FinishExecutingCommand(ReplyType reply_type, DIInterruptType interrupt_type, s64 cycles_late,
                              std::span<const u8> data = {});

  // For IOS HLE.
  void SetInterruptEnabled(DIInterruptType interrupt, bool enabled);
  void ClearInterrupt(DIInterruptType interrupt);

private:
  // Result of decoding one drive command.
  struct CommandResult
  {
    DIInterruptType interrupt = DIInterruptType::TCINT;
    // True when DVDThread was given the work and will report completion
    // itself; otherwise a fixed-latency completion event is scheduled.
    bool queued_on_thread = false;
  };

  void RunDriveCommand(ReplyType reply_type, CommandResult& result);
  void RunAudioStreamCommand(CommandResult& result);
  void RunAudioStatusCommand(CommandResult& result);
  void RunStopMotorCommand();
  void RunAudioBufferConfigCommand(CommandResult& result);
  void Fail(DriveError error, CommandResult& result);
  void ScheduleCommandCompletion(ReplyType reply_type, DIInterruptType interrupt);

  void StreamAudioBlocks(DIInterruptType interrupt_type, std::span<const u8> audio_data,
                         s64 cycles_late);
  u32 AdvanceStreamPosition(u32 max_blocks);

  void UpdateLid();
  void UpdateInterrupts();
  void RaiseInterrupt(DIInterruptType type);

  bool DriveCanRead();
  bool StartDiscRead(u64 dvd_offset, u32 output_address, u32 dvd_length, u32 output_length,
                     const DiscIO::Partition& partition, ReplyType reply_type,
                     DIInterruptType* interrupt_type);
  void ScheduleReads(u64 offset, u32 length, const DiscIO::Partition& partition, u32 output_address,
                     ReplyType reply_type);

  static void AutoChangeDiscCallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void EjectDiscCallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void InsertDiscCallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void FinishExecutingCommandCallback(Core::System& system, u64 userdata, s64 cycles_late);

  // DISR, the status register (layout as documented in YAGCD).
  union UDISR
  {
    u32 Hex = 0;

    BitField<0, 1, u32> BREAK;
    BitField<1, 1, u32> DEINTMASK;
    BitField<2, 1, u32> DEINT;
    BitField<3, 1, u32> TCINTMASK;
    BitField<4, 1, u32> TCINT;
    BitField<5, 1, u32> BRKINTMASK;
    BitField<6, 1, u32> BRKINT;
    BitField<7, 25, u32> reserved;

    UDISR() = default;
    explicit UDISR(u32 hex) : Hex{hex} {}
  };

  // DICVR, the lid register.
  union UDICVR
  {
    u32 Hex = 0;

    BitField<0, 1, u32> CVR;  // 1 = lid open
    BitField<1, 1, u32> CVRINTMASK;
    BitField<2, 1, u32> CVRINT;
    BitField<3, 29, u32> reserved;

    UDICVR() = default;
    explicit UDICVR(u32 hex) : Hex{hex} {}
  };

  // DICR, the transfer control register.
  union UDICR
  {
    u32 Hex = 0;

    BitField<0, 1, u32> TSTART;  // set to start, reads 0 when done
    BitField<1, 1, u32> DMA;     // 0 = immediate (result in DIIMMBUF)
    BitField<2, 1, u32> RW;      // 0 = disc to memory
    BitField<3, 29, u32> reserved;
  };

  // DICFG, read-only configuration.
  union UDICFG
  {
    u32 Hex = 0;

    BitField<0, 8, u32> CONFIG;
    BitField<8, 24, u32> reserved;

    UDICFG() = default;
    explicit UDICFG(u32 hex) : Hex{hex} {}
  };

  // MMIO registers
  UDISR m_DISR;
  UDICVR m_DICVR;
  std::array<u32, 3> m_DICMDBUF{};
  u32 m_DIMAR = 0;
  u32 m_DILENGTH = 0;
  UDICR m_DICR;
  u32 m_DIIMMBUF = 0;
  UDICFG m_DICFG;

  StreamADPCM::ADPCMDecoder m_adpcm_decoder;

  // Streamed disc audio (DTK)
  struct AudioStream
  {
    bool playing = false;
    bool stop_at_track_end = false;
    u64 position = 0;
    u64 current_start = 0;
    u32 current_length = 0;
    u64 next_start = 0;
    u32 next_length = 0;
    u32 pending_blocks = 0;
    bool enabled = false;
    u8 buffer_length = 0;  // stored, but has no modelled effect
  };
  AudioStream m_audio;

  DriveState m_drive_state = DriveState::Ready;
  DriveError m_error_code = DriveError::None;
  u64 m_disc_end_offset = 0;

  // The drive's read-ahead buffer: it keeps reading past the last request,
  // filling [start_offset, end_offset) between start_time and end_time.
  struct ReadAhead
  {
    u64 start_time = 0;
    u64 end_time = 0;
    u64 start_offset = 0;
    u64 end_offset = 0;
  };
  ReadAhead m_read_ahead;

  std::string m_disc_path_to_insert;
  std::vector<std::string> m_auto_disc_change_paths;
  size_t m_auto_disc_change_index = 0;

  CoreTiming::EventType* m_finish_executing_command = nullptr;
  CoreTiming::EventType* m_auto_change_disc = nullptr;
  CoreTiming::EventType* m_eject_disc = nullptr;
  CoreTiming::EventType* m_insert_disc = nullptr;

  Core::System& m_system;
};
}  // namespace DVD
