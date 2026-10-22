// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceBaseboard.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <numeric>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace ExpansionInterface
{

namespace
{
// Shared by every instance: the media board raises it without knowing which
// device object is plugged in.
bool s_interrupt = false;
u32 s_interrupt_polls = 0;
u32 s_interrupt_status = 0;

// IsInterruptSet() keeps reporting a raised interrupt for this many polls.
constexpr u32 INTERRUPT_HOLD = 12;

constexpr std::array<u8, 4> SERIAL_ID{0x06, 0x04, 0x10, 0x00};

enum Command : u8
{
  BackupOffsetSet = 0x01,
  BackupWrite = 0x02,
  BackupRead = 0x03,
  DMAOffsetLengthSet = 0x05,
  ReadISR = 0x82,
  WriteISR = 0x83,
  ReadIMR = 0x86,
  WriteIMR = 0x87,
  WriteLANCNT = 0xFF,
};

// Virtua Striker 4 and Gekitou Pro Yakyuu check two blocks of their backup
// memory; give both blocks the marker and checksum the games expect.
void FixBackupChecksums(File::IOFile& file)
{
  const u64 size = file.GetSize();
  if (size < 0x400)
    return;

  std::vector<u8> bytes(static_cast<std::size_t>(size));
  file.Seek(0, File::SeekOrigin::Begin);
  file.ReadBytes(bytes.data(), bytes.size());

  for (const std::size_t block : {std::size_t{0}, std::size_t{0x200}})
  {
    const u16 marker = 0x1703;
    std::memcpy(&bytes[block + 0x12], &marker, sizeof(marker));

    const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(block + 0x0C);
    const u32 sum = std::accumulate(first, first + 0x1F4, u32{0});
    bytes[block + 0x0A] = static_cast<u8>((sum >> 8) & 0xFF);
    bytes[block + 0x0B] = static_cast<u8>(sum & 0xFF);
  }

  file.Seek(0, File::SeekOrigin::Begin);
  file.WriteBytes(bytes.data(), bytes.size());
  file.Flush();
}
}  // namespace

void GenerateInterrupt(int flag)
{
  s_interrupt = true;
  s_interrupt_polls = 0;
  s_interrupt_status = static_cast<u32>(flag);
  Core::System::GetInstance().GetExpansionInterface().UpdateInterrupts();
}

CEXIBaseboard::CEXIBaseboard(Core::System& system) : IEXIDevice(system)
{
  const std::string& folder = File::GetUserPath(D_TRIUSER_IDX);
  const std::string game_id = SConfig::GetInstance().GetGameID();

  const std::string path = fmt::format("{}tribackup_{}.bin", folder, game_id);
  if (!m_backup.Open(path, "rb+"))
    m_backup.Open(path, "wb+");

  if (!m_backup.IsGood())
  {
    PanicAlertFmt("Failed to open {}\nFile might be in use.", path);
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    const std::string fallback = fmt::format("{}tribackup_tmp_{}{}.bin", folder, std::rand(), game_id);
    m_backup.Open(fallback, "wb+");
  }

  const u32 game_type = AMMediaboard::GetGameType();
  if (game_type == AMMediaboard::VirtuaStriker4 || game_type == AMMediaboard::GekitouProYakyuu)
    FixBackupChecksums(m_backup);
}

CEXIBaseboard::~CEXIBaseboard() = default;

void CEXIBaseboard::SetCS(int cs)
{
  DEBUG_LOG_FMT(SP1, "AM-BB: ChipSelect={}", cs);
  if (cs != 0)
    m_position = 0;
}

bool CEXIBaseboard::IsInterruptSet()
{
  if (!s_interrupt)
    return false;
  DEBUG_LOG_FMT(SP1, "AM-BB: IRQ");
  if (++s_interrupt_polls > INTERRUPT_HOLD)
    s_interrupt = false;
  return true;
}

bool CEXIBaseboard::IsPresent() const
{
  return true;
}

void CEXIBaseboard::DMAWrite(u32 addr, u32 size)
{
  auto& memory = m_system.GetMemory();
  const std::span<u8> span = memory.GetSpanForAddress(addr);
  if (span.size() < size)
  {
    ERROR_LOG_FMT(SP1, "AM-BB: Backup DMA Write overflow: address=0x{:08x}, length={}, span={}",
                  addr, size, span.size());
    return;
  }
  NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: Backup DMA Write: {:08x} {:x}", addr, size);
  m_backup.Seek(m_backup_offset, File::SeekOrigin::Begin);
  m_backup.WriteBytes(span.data(), size);
  m_backup.Flush();
}

void CEXIBaseboard::DMARead(u32 addr, u32 size)
{
  auto& memory = m_system.GetMemory();
  const std::span<u8> span = memory.GetSpanForAddress(addr);
  if (span.size() < size)
  {
    ERROR_LOG_FMT(SP1, "AM-BB: Backup DMA Read overflow: address=0x{:08x}, length={}, span={}",
                  addr, size, span.size());
    return;
  }
  NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: Backup DMA Read: {:08x} {:x}", addr, size);
  m_backup.Seek(m_backup_offset, File::SeekOrigin::Begin);
  m_backup.ReadBytes(span.data(), size);
}

// The answer to the fifth byte, once the whole command is in.
u8 CEXIBaseboard::RunCommand()
{
  switch (m_command[0])
  {
  case BackupOffsetSet:
    m_backup_offset = static_cast<u16>((m_command[1] << 8) | m_command[2]);
    DEBUG_LOG_FMT(SP1, "AM-BB: COMMAND: BackupOffsetSet:{:04x}", m_backup_offset);
    m_backup.Seek(m_backup_offset, File::SeekOrigin::Begin);
    return 0x01;
  case BackupWrite:
    DEBUG_LOG_FMT(SP1, "AM-BB: COMMAND: BackupWrite:{:04x}-{:02x}", m_backup_offset, m_command[1]);
    m_backup.WriteBytes(&m_command[1], 1);
    m_backup.Flush();
    return 0x01;
  case BackupRead:
    DEBUG_LOG_FMT(SP1, "AM-BB: COMMAND: BackupRead :{:04x}", m_backup_offset);
    return 0x01;
  case DMAOffsetLengthSet:
    m_dma_offset = static_cast<u32>((m_command[1] << 8) | m_command[2]);
    m_dma_length = m_command[3];
    NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: DMAOffsetLengthSet :{:04x} {:02x}", m_dma_offset,
                   m_dma_length);
    return 0x01;
  case ReadISR:
    NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: ReadISR  :{:02x} {:02x}:{:02x} {:02x}", m_command[1],
                   m_command[2], 0x04, s_interrupt_status & 0xFF);
    return 0x04;
  case WriteISR:
    s_interrupt_status &= static_cast<u32>(0xFF ^ m_command[2]) | 0xFFFFFF00u;
    NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: WriteISR :{:02x} {:02x}", m_command[1], m_command[2]);
    return 0x04;
  case ReadIMR:
    NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: ReadIMR  :{:02x} {:02x}", m_command[1], m_command[2]);
    return 0x04;
  case WriteIMR:
    NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: WriteIMR :{:02x} {:02x}", m_command[1], m_command[2]);
    return 0x04;
  case WriteLANCNT:
    NOTICE_LOG_FMT(SP1, "AM-BB: COMMAND: WriteLANCNT :{:02x} {:02x}", m_command[1],
                   m_command[2]);
    if (m_command[1] == 0 && m_command[2] == 0)
    {
      s_interrupt = true;
      s_interrupt_polls = 0;
      s_interrupt_status = 2;
    }
    if (m_command[1] == 2 && m_command[2] == 1)
      s_interrupt_status = 0;
    return 0x08;
  default:
    ERROR_LOG_FMT(SP1, "AM-BB: COMMAND: {:02x} {:02x} {:02x}", m_command[0], m_command[1],
                  m_command[2]);
    return 0x04;
  }
}

// The answer to bytes after the fifth.
u8 CEXIBaseboard::ContinueCommand(u8 byte)
{
  switch (m_command[0])
  {
  case BackupRead:
    m_backup.ReadBytes(&byte, 1);
    return byte;
  case DMAOffsetLengthSet:
    return 0x01;
  case ReadISR:
    if (m_position == 6)
    {
      s_interrupt = false;
      return static_cast<u8>(s_interrupt_status & 0xFF);
    }
    return 0x04;
  case ReadIMR:
    if (m_position == 5)
      return 0xFF;
    if (m_position == 6)
      return 0x81;
    return byte;
  default:
    ERROR_LOG_FMT(SP1, "Unknown AM-BB command: {:02x}", m_command[0]);
    return byte;
  }
}

void CEXIBaseboard::TransferByte(u8& byte)
{
  DEBUG_LOG_FMT(SP1, "AM-BB: > {:02x}", byte);

  if (m_position < m_command.size())
  {
    m_command[m_position] = byte;
    byte = 0xFF;
  }

  if (m_position >= 2 && m_command[0] == 0 && m_command[1] == 0)
  {
    // Serial ID read.
    byte = SERIAL_ID[(m_position - 2) & 3];
  }
  else if (m_position == 3)
  {
    DEBUG_LOG_FMT(SP1, "AM-BB: cs: {:02x}, w: {:02x}", m_command[0], m_command[1]);
  }
  else if (m_position == 4)
  {
    byte = RunCommand();
  }
  else if (m_position > 4)
  {
    byte = ContinueCommand(byte);
  }
  else
  {
    byte = 0xFF;
  }

  DEBUG_LOG_FMT(SP1, "AM-BB < {:02x}", byte);
  ++m_position;
}

void CEXIBaseboard::DoState(PointerWrap& p)
{
  p.Do(m_position);
  p.Do(s_interrupt);
  p.DoArray(m_command);
}

}  // namespace ExpansionInterface
