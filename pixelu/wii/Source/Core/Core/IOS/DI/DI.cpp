// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// /dev/di. Requests are translated into drive commands written to the DI registers, and the DVD
// interface runs them. Replies are sent when the drive raises its interrupt (or, for requests
// that never reach the drive, after the IPC overhead).

#include "Core/IOS/DI/DI.h"

#include <array>
#include <memory>
#include <string_view>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/SessionSettings.h"
#include "Core/CoreTiming.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/System.h"
#include "DiscIO/Volume.h"

namespace IOS::HLE
{
namespace
{
// Hollywood registers touched by the driver.
constexpr u32 DI_REGS = 0x0D806000;
constexpr u32 REG_DISR = DI_REGS + 0x00;
constexpr u32 REG_DICVR = DI_REGS + 0x04;
constexpr u32 REG_DICMDBUF0 = DI_REGS + 0x08;
constexpr u32 REG_DICMDBUF1 = DI_REGS + 0x0C;
constexpr u32 REG_DICMDBUF2 = DI_REGS + 0x10;
constexpr u32 REG_DIMAR = DI_REGS + 0x14;
constexpr u32 REG_DILENGTH = DI_REGS + 0x18;
constexpr u32 REG_DICR = DI_REGS + 0x1C;
constexpr u32 REG_DIIMMBUF = DI_REGS + 0x20;
constexpr u32 REG_HW_GPIO_OUT = 0x0D8000E0;
constexpr u32 REG_HW_RESETS = 0x0D800194;
constexpr u32 RESETS_DI_BIT = 1u << 10;

// The three command words. Words that are not given are left as they are in the registers.
struct DriveCommand
{
  u32 word0;
  std::optional<u32> word1;
  std::optional<u32> word2;
};

class Registers
{
public:
  explicit Registers(Core::System& system)
      : m_system(system), m_mmio(*system.GetMemory().GetMMIOMapping())
  {
  }

  u32 Read(u32 address) const { return m_mmio.Read<u32>(m_system, address); }
  void Write(u32 address, u32 value) const { m_mmio.Write<u32>(m_system, address, value); }

  void Send(const DriveCommand& command) const
  {
    Write(REG_DICMDBUF0, command.word0);
    if (command.word1)
      Write(REG_DICMDBUF1, *command.word1);
    if (command.word2)
      Write(REG_DICMDBUF2, *command.word2);
  }

private:
  Core::System& m_system;
  MMIO::Mapping& m_mmio;
};

// Places the low bit (or bits under `mask`) of a request byte at `shift` in a command word.
constexpr u32 Field(u8 value, u32 mask, int shift)
{
  return (static_cast<u32>(value) & mask) << shift;
}

// Disc areas (in 32-bit offsets) that DVDLowUnencryptedRead accepts. Later IOS versions added
// the two small ranges past the end of a single-layer disc to detect drives that don't fail
// out-of-bounds reads (the error #001 check).
struct ReadableRange
{
  u32 first;
  u32 last;
  bool error_001_probe;
};
constexpr std::array<ReadableRange, 3> UNENCRYPTED_READ_RANGES{{
    {0x00000000, 0x00014000, false},
    {0x460A0000, 0x460A0008, true},
    {0x7ED40000, 0x7ED40008, true},
}};

std::shared_ptr<DIDevice> FindDIDevice()
{
  auto* ios = Core::System::GetInstance().GetIOS();
  if (ios == nullptr)
    return nullptr;
  return std::static_pointer_cast<DIDevice>(ios->GetDeviceByName("/dev/di"));
}

void ReportQuirk(GameQuirk quirk)
{
  DolphinAnalytics::Instance().ReportGameQuirk(quirk);
}
}  // namespace

CoreTiming::EventType* DIDevice::s_finish_executing_di_command;

DIDevice::DIDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

void DIDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);
  const auto save = [&p](auto&... fields) { (p.Do(fields), ...); };
  save(m_commands_to_execute, m_executing_command, m_current_partition, m_has_initialized,
       m_last_length);
}

std::optional<IPCReply> DIDevice::Open(const OpenRequest& request)
{
  InitializeIfFirstTime();
  return Device::Open(request);
}

std::optional<IPCReply> DIDevice::IOCtl(const IOCtlRequest& request)
{
  InitializeIfFirstTime();

  // The first byte of the input normally repeats the request number; the request number wins.
  const u8 buffer_command = GetSystem().GetMemory().Read_U8(request.buffer_in);
  if (buffer_command != request.request)
  {
    WARN_LOG_FMT(IOS_DI, "IOCtl {:#04x} has command byte {:#04x} in its buffer; using the former",
                 request.request, buffer_command);
  }

  // The drive takes one command at a time. Everything is replied to later, from FinishDICommand.
  const bool drive_idle = !m_executing_command.has_value();
  m_commands_to_execute.push_back(request.address);
  if (drive_idle)
    ProcessQueuedIOCtl();
  return std::nullopt;
}

void DIDevice::ProcessQueuedIOCtl()
{
  if (m_commands_to_execute.empty())
  {
    PanicAlertFmt("IOS::HLE::DIDevice: ProcessQueuedIOCtl called with an empty queue");
    return;
  }

  const u32 request_address = m_commands_to_execute.front();
  m_commands_to_execute.pop_front();
  m_executing_command = ExecutingCommandInfo{request_address};

  Core::System& system = GetSystem();
  const std::optional<DIResult> immediate = StartIOCtl(IOCtlRequest{system, request_address});
  if (!immediate)
    return;  // The drive is busy with it.

  system.GetCoreTiming().ScheduleEvent(IPC_OVERHEAD_TICKS, s_finish_executing_di_command,
                                       static_cast<u64>(*immediate));
}

std::optional<DIDevice::DIResult> DIDevice::WriteIfFits(const IOCtlRequest& request, u32 value)
{
  if (request.buffer_out_size < sizeof(u32))
  {
    WARN_LOG_FMT(IOS_DI, "No room for a 32-bit result ({} bytes); replying with a security error",
                 request.buffer_out_size);
    return DIResult::SecurityError;
  }
  GetSystem().GetMemory().Write_U32(value, request.buffer_out);
  return DIResult::Success;
}

std::optional<DIDevice::DIResult> DIDevice::StartIOCtl(const IOCtlRequest& request)
{
  if (request.buffer_in_size != 0x20)
  {
    ERROR_LOG_FMT(IOS_DI, "IOCtl: input buffer is {:#x} bytes instead of 0x20",
                  request.buffer_in_size);
    return DIResult::SecurityError;
  }

  Core::System& system = GetSystem();
  Memory::MemoryManager& memory = system.GetMemory();
  DVD::DVDInterface& dvd = system.GetDVDInterface();
  const Registers regs{system};

  // Request parameters live at fixed offsets in the 0x20-byte input block.
  const auto byte_at = [&](u32 offset) { return memory.Read_U8(request.buffer_in + offset); };
  const auto word_at = [&](u32 offset) { return memory.Read_U32(request.buffer_in + offset); };

  // Commands that move data into the output buffer, and commands answered through DIIMMBUF.
  const auto dma = [&](const DriveCommand& command, u32 length) {
    regs.Send(command);
    return StartDMATransfer(length, request);
  };
  const auto immediate = [&](const DriveCommand& command, bool copy_result) {
    regs.Send(command);
    return StartImmediateTransfer(request, copy_result);
  };
  const auto reject_partition_ioctlv = [&](std::string_view name) -> std::optional<DIResult> {
    ERROR_LOG_FMT(IOS_DI, "{} is an ioctlv; rejecting it as an ioctl", name);
    ReportQuirk(GameQuirk::UsesDifferentPartitionCommand);
    return DIResult::SecurityError;
  };

  switch (static_cast<DIIoctl>(request.request))
  {
  // Plain drive commands.
  case DIIoctl::DVDLowInquiry:
    INFO_LOG_FMT(IOS_DI, "DVDLowInquiry");
    return dma({0x12000000, 0, {}}, 0x20);

  case DIIoctl::DVDLowReadDiskID:
    // Hardware also reads the encryption flags of Wii discs here, which is not emulated.
    INFO_LOG_FMT(IOS_DI, "DVDLowReadDiskID");
    return dma({0xA8000040, 0, 0x20}, 0x20);

  case DIIoctl::DVDLowReadDvdPhysical:
  case DIIoctl::DVDLowReadDvdCopyright:
  case DIIoctl::DVDLowReadDvdDiscKey:
  {
    // 0xAD with a sub-command: 0 physical format, 1 copyright, 2 disc key.
    const u32 kind = request.request - static_cast<u32>(DIIoctl::DVDLowReadDvdPhysical);
    const u8 position = byte_at(7);
    INFO_LOG_FMT(IOS_DI, "DVDLowReadDvd structure {}: position {:#04x}", kind, position);
    const DriveCommand command{0xAD000000 | (kind << 16) | (u32{position} << 8), 0, 0};
    if (kind == 1)
      return immediate(command, true);
    return dma(command, 0x800);
  }

  case DIIoctl::DVDLowReportKey:
  {
    const u8 key_type = byte_at(7);
    const u32 argument = word_at(8);
    INFO_LOG_FMT(IOS_DI, "DVDLowReportKey: {:#04x}, {:#08x}", key_type, argument);
    return dma({0xA4000000 | (u32{key_type} << 16), argument & 0xFFFFFF, 0}, 0x20);
  }

  case DIIoctl::DVDLowSeek:
  {
    // The offset is passed on as given; no partition translation.
    const u32 offset = word_at(4);
    INFO_LOG_FMT(IOS_DI, "DVDLowSeek: {:#010x}", offset);
    return immediate({0xAB000000, offset, {}}, false);
  }

  case DIIoctl::DVDLowReadDvd:
  {
    const u8 flag_a = byte_at(7);
    const u8 flag_b = byte_at(11);
    const u32 sectors = word_at(12);
    const u32 sector = word_at(16);
    INFO_LOG_FMT(IOS_DI, "DVDLowReadDvd({}, {}): sector {:#08x}, count {:#08x}", flag_a, flag_b,
                 sector, sectors);
    const u32 word0 = 0xD0000000 | Field(flag_a, 1, 7) | Field(flag_b, 1, 6);
    return dma({word0, sector & 0xFFFFFF, sectors & 0xFFFFFF}, sectors * 0x800);
  }

  case DIIoctl::DVDLowReadDvdConfig:
  {
    const u8 flag = byte_at(7);
    const u8 selector = byte_at(11);
    const u32 position = word_at(12);
    INFO_LOG_FMT(IOS_DI, "DVDLowReadDvdConfig({}, {}): {:#08x}", flag, selector, position);
    return immediate({0xD1000000 | Field(flag, 1, 16) | selector, position & 0xFFFFFF, 0}, true);
  }

  case DIIoctl::DVDLowStopLaser:
    INFO_LOG_FMT(IOS_DI, "DVDLowStopLaser");
    return immediate({0xD2000000, {}, {}}, true);

  case DIIoctl::DVDLowOffset:
  {
    const u8 flag = byte_at(7);
    const u32 offset = word_at(8);
    INFO_LOG_FMT(IOS_DI, "DVDLowOffset({}): {:#010x}", flag, offset);
    return immediate({0xD9000000 | Field(flag, 1, 16), offset, {}}, true);
  }

  case DIIoctl::DVDLowReadDiskBca:
    INFO_LOG_FMT(IOS_DI, "DVDLowReadDiskBca");
    return dma({0xDA000000, {}, {}}, 0x40);

  case DIIoctl::DVDLowRequestDiscStatus:
    INFO_LOG_FMT(IOS_DI, "DVDLowRequestDiscStatus");
    return immediate({0xDB000000, {}, {}}, true);

  case DIIoctl::DVDLowRequestRetryNumber:
    INFO_LOG_FMT(IOS_DI, "DVDLowRequestRetryNumber");
    return immediate({0xDC000000, {}, {}}, true);

  case DIIoctl::DVDLowSetMaximumRotation:
  {
    const u8 speed = byte_at(7);
    INFO_LOG_FMT(IOS_DI, "DVDLowSetMaximumRotation: {}", speed);
    return immediate({0xDD000000 | Field(speed, 3, 16), {}, {}}, false);
  }

  case DIIoctl::DVDLowSerMeasControl:
  {
    const u8 flag_a = byte_at(7);
    const u8 flag_b = byte_at(11);
    INFO_LOG_FMT(IOS_DI, "DVDLowSerMeasControl({}, {})", flag_a, flag_b);
    return dma({0xDF000000 | Field(flag_a, 1, 17) | Field(flag_b, 1, 16), {}, {}}, 0x20);
  }

  case DIIoctl::DVDLowRequestError:
    INFO_LOG_FMT(IOS_DI, "DVDLowRequestError");
    return immediate({0xE0000000, {}, {}}, true);

  case DIIoctl::DVDLowAudioStream:
  {
    const u8 mode = byte_at(7);
    const u32 length = word_at(8);
    const u32 offset = word_at(12);
    INFO_LOG_FMT(IOS_DI, "DVDLowAudioStream({}): offset {:#010x}, length {:#x}", mode, offset,
                 length);
    return immediate({0xE1000000 | Field(mode, 3, 16), offset, length}, false);
  }

  case DIIoctl::DVDLowRequestAudioStatus:
  {
    // DIIMMBUF is not copied back for this one, so callers need DVDLowGetImmBuf afterwards.
    const u8 mode = byte_at(7);
    INFO_LOG_FMT(IOS_DI, "DVDLowRequestAudioStatus({})", mode);
    return immediate({0xE2000000 | Field(mode, 3, 16), 0, {}}, false);
  }

  case DIIoctl::DVDLowStopMotor:
  {
    const u8 eject = byte_at(7);
    const u8 kill = byte_at(11);
    INFO_LOG_FMT(IOS_DI, "DVDLowStopMotor(eject {}, kill {})", eject, kill);
    return immediate({0xE3000000 | Field(eject, 1, 17) | Field(kill, 1, 20), 0, {}}, true);
  }

  case DIIoctl::DVDLowAudioBufferConfig:
  {
    // DIIMMBUF is copied back, though the drive never sets it for this command.
    const u8 enable = byte_at(7);
    const u8 buffer_size = byte_at(11);
    INFO_LOG_FMT(IOS_DI, "DVDLowAudioBufferConfig: enable {}, size {}", enable, buffer_size);
    return immediate({0xE4000000 | Field(enable, 1, 16) | Field(buffer_size, 0xf, 0), 0, {}}, true);
  }

  // Reads.
  case DIIoctl::DVDLowRead:
  {
    const u32 length = word_at(4);
    const u32 offset = word_at(8);
    INFO_LOG_FMT(IOS_DI, "DVDLowRead: {:#x} bytes at offset {:#010x}", length, offset);
    if (m_current_partition == DiscIO::PARTITION_NONE)
    {
      ERROR_LOG_FMT(IOS_DI, "DVDLowRead without an open partition");
      return DIResult::SecurityError;
    }
    if (request.buffer_out_size < length)
    {
      WARN_LOG_FMT(IOS_DI, "DVDLowRead: output buffer of {} bytes is too small for {} bytes",
                   request.buffer_out_size, length);
      return DIResult::SecurityError;
    }
    // IOS stores the offset here rather than the length.
    m_last_length = offset;
    dvd.PerformDecryptingRead(offset, length, request.buffer_out, m_current_partition,
                              DVD::ReplyType::IOS);
    return std::nullopt;
  }

  case DIIoctl::DVDLowUnencryptedRead:
  {
    const u32 length = word_at(4);
    const u32 first = word_at(8);
    const u32 last = first + (length >> 2);
    INFO_LOG_FMT(IOS_DI, "DVDLowUnencryptedRead: {:#x} bytes at offset {:#010x}", length, first);
    for (const ReadableRange& range : UNENCRYPTED_READ_RANGES)
    {
      const auto inside = [&range](u32 x) { return x >= range.first && x <= range.last; };
      if (!inside(first) || !inside(last))
        continue;

      regs.Send({0xA8000000, first, length});
      if (!range.error_001_probe || !Config::Get(Config::SESSION_SHOULD_FAKE_ERROR_001))
        return StartDMATransfer(length, request);

      // Act like a drive that fails reads past the end of the disc.
      regs.Write(REG_DIMAR, request.buffer_out);
      m_last_length = length;
      regs.Write(REG_DILENGTH, length);
      dvd.ForceOutOfBoundsRead(DVD::ReplyType::IOS);
      return std::nullopt;
    }
    WARN_LOG_FMT(IOS_DI, "DVDLowUnencryptedRead outside the allowed areas");
    return DIResult::SecurityError;
  }

  // Requests handled by the driver itself.
  case DIIoctl::DVDLowWaitForCoverClose:
    // This waits for the cover to close, so pretend that just happened.
    INFO_LOG_FMT(IOS_DI, "DVDLowWaitForCoverClose: replying at once");
    return DIResult::CoverClosed;

  case DIIoctl::DVDLowGetCoverRegister:
  {
    const u32 value = regs.Read(REG_DICVR);
    DEBUG_LOG_FMT(IOS_DI, "DVDLowGetCoverRegister: {:#010x}", value);
    return WriteIfFits(request, value);
  }

  case DIIoctl::DVDLowGetStatusRegister:
  {
    const u32 value = regs.Read(REG_DISR);
    INFO_LOG_FMT(IOS_DI, "DVDLowGetStatusRegister: {:#010x}", value);
    return WriteIfFits(request, value);
  }

  case DIIoctl::DVDLowGetControlRegister:
  {
    const u32 value = regs.Read(REG_DICR);
    INFO_LOG_FMT(IOS_DI, "DVDLowGetControlRegister: {:#010x}", value);
    return WriteIfFits(request, value);
  }

  case DIIoctl::DVDLowGetImmBuf:
  {
    const u32 value = regs.Read(REG_DIIMMBUF);
    INFO_LOG_FMT(IOS_DI, "DVDLowGetImmBuf: {:#010x}", value);
    return WriteIfFits(request, value);
  }

  case DIIoctl::DVDLowGetLength:
    INFO_LOG_FMT(IOS_DI, "DVDLowGetLength: {:#010x}", m_last_length);
    return WriteIfFits(request, m_last_length);

  case DIIoctl::DVDLowGetCoverStatus:
  {
    // 1: no disc, 2: disc present.
    const bool disc = dvd.IsDiscInside();
    INFO_LOG_FMT(IOS_DI, "DVDLowGetCoverStatus: {}", disc ? "disc present" : "no disc");
    return WriteIfFits(request, disc ? 2 : 1);
  }

  case DIIoctl::DVDLowNotifyReset:
    INFO_LOG_FMT(IOS_DI, "DVDLowNotifyReset");
    ResetDIRegisters();
    return DIResult::Success;

  case DIIoctl::DVDLowReset:
  {
    const bool spin_up = word_at(4) != 0;
    INFO_LOG_FMT(IOS_DI, "DVDLowReset (spin up: {})", spin_up);

    // A set GPIO bit keeps the drive from spinning up.
    const u32 gpio = regs.Read(REG_HW_GPIO_OUT);
    const u32 spin_bit = static_cast<u32>(GPIO::DI_SPIN);
    regs.Write(REG_HW_GPIO_OUT, spin_up ? (gpio & ~spin_bit) : (gpio | spin_bit));

    // If the drive is held in reset, release it; otherwise pulse the reset line.
    const bool held_in_reset = (regs.Read(REG_HW_RESETS) & RESETS_DI_BIT) == 0;
    if (!held_in_reset)
      regs.Write(REG_HW_RESETS, regs.Read(REG_HW_RESETS) & ~RESETS_DI_BIT);
    regs.Write(REG_HW_RESETS, regs.Read(REG_HW_RESETS) | RESETS_DI_BIT);

    ResetDIRegisters();
    return DIResult::Success;
  }

  case DIIoctl::DVDLowMaskCoverInterrupt:
  case DIIoctl::DVDLowUnmaskCoverInterrupt:
  {
    const bool unmask = request.request == static_cast<u32>(DIIoctl::DVDLowUnmaskCoverInterrupt);
    INFO_LOG_FMT(IOS_DI, "DVDLow{}CoverInterrupt", unmask ? "Unmask" : "Mask");
    dvd.SetInterruptEnabled(DVD::DIInterruptType::CVRINT, unmask);
    ReportQuirk(GameQuirk::UsesDIInterruptMaskCommand);
    return DIResult::Success;
  }

  case DIIoctl::DVDLowUnmaskStatusInterrupts:
    // Does nothing in IOS.
    INFO_LOG_FMT(IOS_DI, "DVDLowUnmaskStatusInterrupts");
    ReportQuirk(GameQuirk::UsesDIInterruptMaskCommand);
    return DIResult::Success;

  case DIIoctl::DVDLowClearCoverInterrupt:
    DEBUG_LOG_FMT(IOS_DI, "DVDLowClearCoverInterrupt");
    dvd.ClearInterrupt(DVD::DIInterruptType::CVRINT);
    return DIResult::Success;

  case DIIoctl::DVDLowClosePartition:
    INFO_LOG_FMT(IOS_DI, "DVDLowClosePartition");
    ChangePartition(DiscIO::PARTITION_NONE);
    return DIResult::Success;

  // Requests that are refused.
  case DIIoctl::DVDLowSetSpinupFlag:
    ERROR_LOG_FMT(IOS_DI, "DVDLowSetSpinupFlag is not accepted");
    return DIResult::BadArgument;

  case DIIoctl::DVDLowEnableDvdVideo:
    ERROR_LOG_FMT(IOS_DI, "DVDLowEnableDvdVideo is not accepted");
    return DIResult::SecurityError;

  // The partition requests are ioctlvs. IOS also lets them through as ioctls, but no game is
  // expected to do that.
  case DIIoctl::DVDLowOpenPartition:
    return reject_partition_ioctlv("DVDLowOpenPartition");
  case DIIoctl::DVDLowGetNoDiscOpenPartitionParams:
    return reject_partition_ioctlv("DVDLowGetNoDiscOpenPartitionParams");
  case DIIoctl::DVDLowNoDiscOpenPartition:
    return reject_partition_ioctlv("DVDLowNoDiscOpenPartition");
  case DIIoctl::DVDLowGetNoDiscBufferSizes:
    return reject_partition_ioctlv("DVDLowGetNoDiscBufferSizes");
  case DIIoctl::DVDLowOpenPartitionWithTmdAndTicket:
    return reject_partition_ioctlv("DVDLowOpenPartitionWithTmdAndTicket");
  case DIIoctl::DVDLowOpenPartitionWithTmdAndTicketView:
    return reject_partition_ioctlv("DVDLowOpenPartitionWithTmdAndTicketView");

  default:
    ERROR_LOG_FMT(IOS_DI, "Unknown ioctl {:#04x}", request.request);
    return DIResult::SecurityError;
  }
}

std::optional<DIDevice::DIResult> DIDevice::StartDMATransfer(u32 command_length,
                                                             const IOCtlRequest& request)
{
  // Real IOS sends the command anyway and gives up after a 15 second timeout; skip the wait.
  if (request.buffer_out_size < command_length)
  {
    WARN_LOG_FMT(IOS_DI, "Output buffer of {} bytes is smaller than the {} byte transfer",
                 request.buffer_out_size, command_length);
    return DIResult::ReadTimedOut;
  }

  // IOS mostly hangs on misaligned transfers; refuse them instead.
  constexpr u32 ALIGNMENT_MASK = 0x1f;
  if (((command_length | request.buffer_out) & ALIGNMENT_MASK) != 0)
  {
    WARN_LOG_FMT(IOS_DI, "Misaligned transfer: buffer {:#010x} ({:#x} bytes), length {:#x}",
                 request.buffer_out, request.buffer_out_size, command_length);
    return DIResult::BadArgument;
  }

  Core::System& system = GetSystem();
  const Registers regs{system};
  regs.Write(REG_DIMAR, request.buffer_out);
  m_last_length = command_length;
  regs.Write(REG_DILENGTH, command_length);
  system.GetDVDInterface().ExecuteCommand(DVD::ReplyType::IOS);
  return std::nullopt;
}

std::optional<DIDevice::DIResult> DIDevice::StartImmediateTransfer(const IOCtlRequest& request,
                                                                   bool write_to_buf)
{
  if (write_to_buf && request.buffer_out_size < sizeof(u32))
  {
    WARN_LOG_FMT(IOS_DI,
                 "Output buffer of {} bytes cannot hold DIIMMBUF; sending the command anyway",
                 request.buffer_out_size);
  }
  m_executing_command->m_copy_diimmbuf = write_to_buf;
  GetSystem().GetDVDInterface().ExecuteCommand(DVD::ReplyType::IOS);
  return std::nullopt;
}

void DIDevice::InterruptFromDVDInterface(DVD::DIInterruptType interrupt_type)
{
  DIResult result = DIResult::DriveError;
  if (interrupt_type == DVD::DIInterruptType::TCINT)
  {
    result = DIResult::Success;
  }
  else if (interrupt_type != DVD::DIInterruptType::DEINT)
  {
    PanicAlertFmt("IOS::HLE::DIDevice: unexpected DI interrupt {}",
                  static_cast<int>(interrupt_type));
  }

  if (const std::shared_ptr<DIDevice> di = FindDIDevice())
    di->FinishDICommand(result);
  else
    PanicAlertFmt("IOS::HLE::DIDevice: DI interrupt without a /dev/di device");
}

void DIDevice::FinishDICommandCallback(Core::System& /*system*/, u64 userdata, s64 /*ticksbehind*/)
{
  if (const std::shared_ptr<DIDevice> di = FindDIDevice())
    di->FinishDICommand(static_cast<DIResult>(userdata));
  else
    PanicAlertFmt("IOS::HLE::DIDevice: DI command finished without a /dev/di device");
}

void DIDevice::FinishDICommand(DIResult result)
{
  if (!m_executing_command)
  {
    PanicAlertFmt("IOS::HLE::DIDevice: FinishDICommand called with no command running");
    return;
  }

  Core::System& system = GetSystem();
  const IOCtlRequest request{system, m_executing_command->m_request_address};
  if (m_executing_command->m_copy_diimmbuf)
  {
    const u32 immbuf = Registers{system}.Read(REG_DIIMMBUF);
    system.GetMemory().Write_U32(immbuf, request.buffer_out);
  }
  GetEmulationKernel().EnqueueIPCReply(request, static_cast<s32>(result));
  m_executing_command.reset();

  // The drive is free again.
  if (!m_commands_to_execute.empty())
    ProcessQueuedIOCtl();
}

std::optional<IPCReply> DIDevice::IOCtlV(const IOCtlVRequest& request)
{
  // Unlike ioctls these are answered at once, without waiting for the drive (so one can run while
  // an ioctl is in progress).
  InitializeIfFirstTime();

  const IOCtlVRequest::IOVector& command_block = request.in_vectors[0];
  if (command_block.size != 0x20)
  {
    ERROR_LOG_FMT(IOS_DI, "IOCtlV: input buffer is {:#x} bytes instead of 0x20",
                  command_block.size);
    return IPCReply{static_cast<s32>(DIResult::BadArgument)};
  }

  Core::System& system = GetSystem();
  Memory::MemoryManager& memory = system.GetMemory();
  const u8 buffer_command = memory.Read_U8(command_block.address);
  if (buffer_command != request.request)
  {
    WARN_LOG_FMT(IOS_DI, "IOCtlV {:#04x} has command byte {:#04x} in its buffer; using the former",
                 request.request, buffer_command);
  }

  const auto dump = [&] {
    request.DumpUnknown(system, GetDeviceName(), Common::Log::LogType::IOS_DI);
  };

  DIResult result = DIResult::BadArgument;
  switch (static_cast<DIIoctl>(request.request))
  {
  case DIIoctl::DVDLowOpenPartition:
  {
    // in: command block, ticket, certificates; io: TMD, ES result.
    if (request.in_vectors.size() != 3 || request.io_vectors.size() != 2)
    {
      ERROR_LOG_FMT(IOS_DI, "DVDLowOpenPartition: expected 3 in/2 io vectors, got {}/{}",
                    request.in_vectors.size(), request.io_vectors.size());
      break;
    }
    // A caller-supplied ticket or certificate chain would be used by IOS; here the ones from the
    // disc always are.
    if (request.in_vectors[1].address != 0)
    {
      ERROR_LOG_FMT(IOS_DI, "DVDLowOpenPartition: ignoring the caller's ticket");
      ReportQuirk(GameQuirk::UsesDifferentPartitionCommand);
    }
    if (request.in_vectors[2].address != 0)
    {
      ERROR_LOG_FMT(IOS_DI, "DVDLowOpenPartition: ignoring the caller's certificates");
      ReportQuirk(GameQuirk::UsesDifferentPartitionCommand);
    }

    const u64 offset = u64{memory.Read_U32(command_block.address + 4)} << 2;
    ChangePartition(DiscIO::Partition(offset));
    INFO_LOG_FMT(IOS_DI, "DVDLowOpenPartition: partition at {:#011x}", offset);

    DVD::DVDThread& dvd_thread = system.GetDVDThread();
    const ES::TMDReader tmd = dvd_thread.GetTMD(m_current_partition);
    const std::vector<u8>& tmd_bytes = tmd.GetBytes();
    memory.CopyToEmu(request.io_vectors[0].address, tmd_bytes.data(), tmd_bytes.size());

    const ReturnCode verified = GetEmulationKernel().GetESDevice()->DIVerify(
        tmd, dvd_thread.GetTicket(m_current_partition));
    memory.Write_U32(static_cast<u32>(verified), request.io_vectors[1].address);
    result = DIResult::Success;
    break;
  }

  case DIIoctl::DVDLowGetNoDiscOpenPartitionParams:
  case DIIoctl::DVDLowNoDiscOpenPartition:
  case DIIoctl::DVDLowGetNoDiscBufferSizes:
    // These do nothing in IOS.
    ERROR_LOG_FMT(IOS_DI, "Ignoring no-disc partition ioctlv {:#04x}", request.request);
    ReportQuirk(GameQuirk::UsesDifferentPartitionCommand);
    dump();
    break;

  case DIIoctl::DVDLowOpenPartitionWithTmdAndTicket:
  case DIIoctl::DVDLowOpenPartitionWithTmdAndTicketView:
    ERROR_LOG_FMT(IOS_DI, "Partition ioctlv {:#04x} is not implemented", request.request);
    ReportQuirk(GameQuirk::UsesDifferentPartitionCommand);
    break;

  default:
    ERROR_LOG_FMT(IOS_DI, "Unknown ioctlv {:#04x}", request.request);
    dump();
    break;
  }
  return IPCReply{static_cast<s32>(result)};
}

void DIDevice::ChangePartition(const DiscIO::Partition partition)
{
  m_current_partition = partition;
}

DiscIO::Partition DIDevice::GetCurrentPartition()
{
  // Also called for GameCube games, where there is no /dev/di.
  const std::shared_ptr<DIDevice> di = FindDIDevice();
  return di ? di->m_current_partition : DiscIO::PARTITION_NONE;
}

void DIDevice::InitializeIfFirstTime()
{
  // IOS sets up the DI registers on the first open/ioctl/ioctlv (not when the module starts),
  // and titles can see the difference, so this is not done in the constructor. IOS skips this
  // when the drive is held in reset, which is not emulated.
  if (m_has_initialized)
    return;
  ResetDIRegisters();
  m_has_initialized = true;
}

void DIDevice::ResetDIRegisters()
{
  // Acknowledge pending transfer/error interrupts, enable them, and mask the cover interrupt.
  DVD::DVDInterface& dvd = GetSystem().GetDVDInterface();
  for (const auto type : {DVD::DIInterruptType::TCINT, DVD::DIInterruptType::DEINT})
    dvd.ClearInterrupt(type);
  for (const auto type : {DVD::DIInterruptType::TCINT, DVD::DIInterruptType::DEINT})
    dvd.SetInterruptEnabled(type, true);
  dvd.SetInterruptEnabled(DVD::DIInterruptType::CVRINT, false);
  ChangePartition(DiscIO::PARTITION_NONE);
}
}  // namespace IOS::HLE
