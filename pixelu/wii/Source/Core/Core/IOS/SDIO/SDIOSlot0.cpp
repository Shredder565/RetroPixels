// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The front SD slot. IOS exposes a thin layer over the SD host controller: titles send raw SD
// commands, and block reads/writes go to a card image on the host.

#include "Core/IOS/SDIO/SDIOSlot0.h"

#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/SDCardUtil.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/VersionInfo.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Card image created when none exists yet.
constexpr u64 DEFAULT_CARD_SIZE_MIB = 128;

// Responses for commands whose answer does not depend on the card.
constexpr u32 R1_TRANSFER_STATE = 0x900;
constexpr u32 R1_STANDBY_STATE = 0x700;
constexpr u32 R1_APP_CMD = 0x920;
constexpr u32 RELATIVE_CARD_ADDRESS = 0x9f62;
// The CID as IOS hands it out: four words, last word first.
constexpr std::array<u32, 4> CARD_ID{0xAC68006B, 0x4E430403, 0x4C504849, 0x00D0444F};

// Writes a 128-bit register (as four words, least significant first) the way IOS returns it:
// the most significant word at the lowest address.
void WriteResponse128(Memory::MemoryManager& memory, u32 address, const std::array<u32, 4>& words)
{
  for (u32 i = 0; i < 4; ++i)
    memory.Write_U32(words[i], address + 12 - 4 * i);
}

bool SDWritesAllowed()
{
  return Config::Get(Config::MAIN_ALLOW_SD_WRITES);
}
}  // namespace

SDIOSlot0Device::SDIOSlot0Device(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name),
      m_sdhc_supported(HasFeature(ios.GetVersion(), Feature::SDv2))
{
  if (!SDWritesAllowed())
    INFO_LOG_FMT(IOS_SD, "SD card writes are disabled in the settings");

  m_config_callback_id =
      CPUThreadConfigCallback::AddConfigChangedCallback([this] { RefreshConfig(); });
  m_sd_card_inserted = Config::Get(Config::MAIN_WII_SD_CARD);
}

SDIOSlot0Device::~SDIOSlot0Device()
{
  CPUThreadConfigCallback::RemoveConfigChangedCallback(m_config_callback_id);
}

void SDIOSlot0Device::RefreshConfig()
{
  // Inserting or removing the card in the settings may answer a pending event request.
  const bool inserted = Config::Get(Config::MAIN_WII_SD_CARD);
  if (inserted == m_sd_card_inserted)
    return;
  m_sd_card_inserted = inserted;
  EventNotify();
}

void SDIOSlot0Device::DoState(PointerWrap& p)
{
  Device::DoState(p);
  // The card image is not part of the state; reopen it on load.
  if (p.IsReadMode())
    OpenInternal();
  const auto save = [&p](auto&... fields) { (p.Do(fields), ...); };
  save(m_status, m_block_length, m_bus_width, m_registers, m_protocol, m_sdhc_supported);
}

void SDIOSlot0Device::EventNotify()
{
  if (!m_event)
    return;

  const EventType wanted = m_sd_card_inserted ? EVENT_INSERT : EVENT_REMOVE;
  if (m_event->type != wanted)
    return;

  INFO_LOG_FMT(IOS_SD, "Telling the PPC that the SD card was {}",
               m_sd_card_inserted ? "inserted" : "removed");
  GetEmulationKernel().EnqueueIPCReply(m_event->request, m_event->type);
  m_event.reset();
}

void SDIOSlot0Device::OpenInternal()
{
  const std::string path = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  m_card.Open(path, "r+b");
  if (m_card)
    return;

  WARN_LOG_FMT(IOS_SD, "Cannot open the SD card image {}; creating a {} MiB one", path,
               DEFAULT_CARD_SIZE_MIB);
  if (Common::SDCardCreate(DEFAULT_CARD_SIZE_MIB, path))
  {
    INFO_LOG_FMT(IOS_SD, "Created {}", path);
    m_card.Open(path, "r+b");
  }
  if (!m_card)
    ERROR_LOG_FMT(IOS_SD, "No usable SD card image (is the user directory read-only?)");
}

std::optional<IPCReply> SDIOSlot0Device::Open(const OpenRequest& /*request*/)
{
  OpenInternal();
  m_registers.fill(0);
  m_is_active = true;
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> SDIOSlot0Device::Close(u32 fd)
{
  m_card.Close();
  m_block_length = 0;
  m_bus_width = 0;
  return Device::Close(fd);
}

std::optional<IPCReply> SDIOSlot0Device::IOCtl(const IOCtlRequest& request)
{
  // Every ioctl starts from a cleared output buffer.
  GetSystem().GetMemory().Memset(request.buffer_out, 0, request.buffer_out_size);

  // SENDCMD may answer later, so it is handled apart from the others.
  if (request.request == IOCTL_SENDCMD)
    return SendCommand(request);

  using Handler = IPCReply (SDIOSlot0Device::*)(const IOCtlRequest&);
  static constexpr std::array<std::pair<u32, Handler>, 6> HANDLERS{{
      {IOCTL_WRITEHCR, &SDIOSlot0Device::WriteHCRegister},
      {IOCTL_READHCR, &SDIOSlot0Device::ReadHCRegister},
      {IOCTL_RESETCARD, &SDIOSlot0Device::ResetCard},
      {IOCTL_SETCLK, &SDIOSlot0Device::SetClk},
      {IOCTL_GETSTATUS, &SDIOSlot0Device::GetStatus},
      {IOCTL_GETOCR, &SDIOSlot0Device::GetOCRegister},
  }};
  for (const auto& [number, handler] : HANDLERS)
  {
    if (number == request.request)
      return (this->*handler)(request);
  }

  ERROR_LOG_FMT(IOS_SD, "Unhandled ioctl {:#010x}", request.request);
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> SDIOSlot0Device::IOCtlV(const IOCtlVRequest& request)
{
  if (request.request == IOCTLV_SENDCMD)
    return SendCommand(request);
  ERROR_LOG_FMT(IOS_SD, "Unhandled ioctlv {:#010x}", request.request);
  return IPCReply(IPC_SUCCESS);
}

s32 SDIOSlot0Device::ExecuteCommand(const Request& request, u32 buffer_in, u32 /*buffer_in_size*/,
                                    u32 /*rw_buffer*/, u32 rw_buffer_size, u32 buffer_out,
                                    u32 /*buffer_out_size*/)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();

  // The command block: nine words, of which the last is padding. `data_address` is where block
  // transfers read from or write to.
  std::array<u32, 9> block;
  for (u32 i = 0; i < block.size(); ++i)
    block[i] = memory.Read_U32(buffer_in + 4 * i);
  const u32 command = block[0];
  const u32 argument = block[3];
  const u32 block_count = block[4];
  const u32 block_size = block[5];
  const u32 data_address = block[6];
  const bool dma = block[7] != 0;

  const auto respond = [&](u32 value) { memory.Write_U32(value, buffer_out); };

  switch (command)
  {
  case GO_IDLE_STATE:
    INFO_LOG_FMT(IOS_SD, "GO_IDLE_STATE");
    respond(0);
    return RET_OK;

  case SEND_RELATIVE_ADDR:
    respond(RELATIVE_CARD_ADDRESS);
    return RET_OK;

  case SELECT_CARD:
    // An RCA in the upper half selects the card; zero deselects it.
    respond((argument >> 16) != 0 ? R1_STANDBY_STATE : R1_TRANSFER_STATE);
    return RET_OK;

  case SEND_IF_COND:
    // Only v2 cards answer this, echoing the voltage and check pattern.
    INFO_LOG_FMT(IOS_SD, "SEND_IF_COND");
    InitSDHC();
    respond(argument);
    return RET_OK;

  case SEND_CSD:
    WriteResponse128(memory, buffer_out, m_protocol == SDProtocol::V1 ? GetCSDv1() : GetCSDv2());
    return RET_OK;

  case ALL_SEND_CID:
  case SEND_CID:
    INFO_LOG_FMT(IOS_SD, "SEND_CID");
    for (u32 i = 0; i < CARD_ID.size(); ++i)
      memory.Write_U32(CARD_ID[i], buffer_out + 4 * i);
    return RET_OK;

  case SET_BLOCKLEN:
    m_block_length = argument;
    respond(R1_TRANSFER_STATE);
    return RET_OK;

  case APP_CMD_NEXT:
    respond(R1_APP_CMD);
    return RET_OK;

  case ACMD_SETBUSWIDTH:
    // 0: 1-bit bus, 2: 4-bit bus.
    m_bus_width = argument & 3;
    respond(R1_APP_CMD);
    return RET_OK;

  case ACMD_SENDOPCOND:
    respond(GetOCRegister());
    return RET_OK;

  case READ_MULTIPLE_BLOCK:
  case WRITE_MULTIPLE_BLOCK:
  {
    const bool write = command == WRITE_MULTIPLE_BLOCK;
    INFO_LOG_FMT(IOS_SD, "{}{} {} block(s) of {} bytes, card offset {:#010x}, memory {:#010x}",
                 dma ? "DMA " : "", write ? "write" : "read", block_count, block_size, argument,
                 data_address);

    s32 result = RET_OK;
    if (write && !SDWritesAllowed())
    {
      ERROR_LOG_FMT(IOS_SD, "Refusing to write to a locked card");
      result = RET_LOCKED;
    }
    else
    {
      const u32 length = block_size * block_count;
      const auto offset = static_cast<s64>(GetAddressFromRequest(argument));
      if (!m_card.Seek(offset, File::SeekOrigin::Begin))
        ERROR_LOG_FMT(IOS_SD, "Seeking in the card image failed");

      u8* data = memory.GetPointerForRange(data_address, length);
      const bool ok = write ? m_card.WriteBytes(data, length) : m_card.ReadBytes(data, length);
      if (!ok)
      {
        ERROR_LOG_FMT(IOS_SD, "Card image {} failed (error {}, eof {})", write ? "write" : "read",
                      std::ferror(m_card.GetHandle()), std::feof(m_card.GetHandle()));
        result = RET_FAIL;
      }
      else if (!write)
      {
        DEBUG_LOG_FMT(IOS_SD, "Read {} bytes (buffer {} bytes)", length, rw_buffer_size);
      }
    }
    respond(R1_TRANSFER_STATE);
    return result;
  }

  case EVENT_REGISTER:
    // Answered later, by EventNotify.
    INFO_LOG_FMT(IOS_SD, "Event request {:#x}", argument);
    m_event = std::make_unique<Event>(static_cast<EventType>(argument), request);
    return RET_EVENT_REGISTER;

  case EVENT_UNREGISTER:
    INFO_LOG_FMT(IOS_SD, "Cancelling event request {:#x}", argument);
    if (!m_event)
      return IPC_EINVAL;
    // The cancelled request is answered before this one, unlike on hardware.
    GetEmulationKernel().EnqueueIPCReply(m_event->request, EVENT_INVALID);
    m_event.reset();
    return RET_OK;

  default:
    ERROR_LOG_FMT(IOS_SD, "Unhandled SD command {:#010x}", command);
    return RET_OK;
  }
}

IPCReply SDIOSlot0Device::WriteHCRegister(const IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 reg = memory.Read_U32(request.buffer_in);
  const u32 value = memory.Read_U32(request.buffer_in + 16);
  INFO_LOG_FMT(IOS_SD, "Write host controller register {:#010x} = {:#010x}", reg, value);

  if (reg >= m_registers.size())
  {
    WARN_LOG_FMT(IOS_SD, "Host controller register {:#x} does not exist", reg);
    return IPCReply(IPC_SUCCESS);
  }

  u32 stored = value;
  if (reg == HCR_CLOCKCONTROL && (value & 1) != 0)
    stored |= 2;  // internal clock enabled -> report it as stable
  else if (reg == HCR_SOFTWARERESET && value != 0)
    stored = 0;  // a reset completes at once
  m_registers[reg] = stored;
  return IPCReply(IPC_SUCCESS);
}

IPCReply SDIOSlot0Device::ReadHCRegister(const IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 reg = memory.Read_U32(request.buffer_in);
  if (reg >= m_registers.size())
  {
    WARN_LOG_FMT(IOS_SD, "Host controller register {:#x} does not exist", reg);
    return IPCReply(IPC_SUCCESS);
  }

  INFO_LOG_FMT(IOS_SD, "Read host controller register {:#010x} = {:#010x}", reg, m_registers[reg]);
  memory.Write_U32(m_registers[reg], request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply SDIOSlot0Device::ResetCard(const IOCtlRequest& request)
{
  // The reply is the RCA in the upper half and a zero (success) lower half; m_status is used.
  INFO_LOG_FMT(IOS_SD, "IOCTL_RESETCARD");
  GetSystem().GetMemory().Write_U32(m_status, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply SDIOSlot0Device::SetClk(const IOCtlRequest& request)
{
  // The argument is half the SD clock divisor. libogc always passes 1.
  const u32 divisor = GetSystem().GetMemory().Read_U32(request.buffer_in);
  INFO_LOG_FMT(IOS_SD, "IOCTL_SETCLK {}", divisor);
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> SDIOSlot0Device::SendCommand(const IOCtlRequest& request)
{
  INFO_LOG_FMT(IOS_SD, "IOCTL_SENDCMD {:#x} (request {:08x})",
               GetSystem().GetMemory().Read_U32(request.buffer_in), request.address);

  const s32 result = ExecuteCommand(request, request.buffer_in, request.buffer_in_size, 0, 0,
                                    request.buffer_out, request.buffer_out_size);
  if (result != RET_EVENT_REGISTER)
    return IPCReply(IPC_SUCCESS);

  // The card may already be in the state the caller is waiting for.
  EventNotify();
  return std::nullopt;
}

IPCReply SDIOSlot0Device::GetStatus(const IOCtlRequest& request)
{
  // IOS initialises SDSC cards by itself. SDHC cards only on versions that support them;
  // elsewhere the title has to do it (SEND_IF_COND).
  if (m_card.GetSize() > SDSC_MAX_SIZE)
  {
    if (m_sdhc_supported)
      InitSDHC();
    m_status |= CARD_SDHC;
  }
  else
  {
    m_status |= CARD_INITIALIZED;
  }

  // Insertion and write protection come from the settings and are not stored in m_status.
  u32 status = CARD_NOT_EXIST;
  if (Config::Get(Config::MAIN_WII_SD_CARD))
  {
    status = m_status | CARD_INSERTED;
    if (!SDWritesAllowed())
      status |= CARD_LOCKED;
  }

  INFO_LOG_FMT(IOS_SD, "IOCTL_GETSTATUS: {} card, {}{}{}",
               (status & CARD_SDHC) != 0 ? "SDHC" : "SD",
               (status & CARD_INSERTED) != 0 ? "inserted" : "not inserted",
               (status & CARD_LOCKED) != 0 ? ", locked" : "",
               (status & CARD_INITIALIZED) != 0 ? ", initialized" : "");
  GetSystem().GetMemory().Write_U32(status, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply SDIOSlot0Device::GetOCRegister(const IOCtlRequest& request)
{
  const u32 ocr = GetOCRegister();
  INFO_LOG_FMT(IOS_SD, "IOCTL_GETOCR: {:#x}", ocr);
  GetSystem().GetMemory().Write_U32(ocr, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

IPCReply SDIOSlot0Device::SendCommand(const IOCtlVRequest& request)
{
  // in: command block, data buffer; io: response.
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const IOCtlVRequest::IOVector& command_block = request.in_vectors[0];
  const IOCtlVRequest::IOVector& data = request.in_vectors[1];
  const IOCtlVRequest::IOVector& response = request.io_vectors[0];

  DEBUG_LOG_FMT(IOS_SD, "IOCTLV_SENDCMD {:#010x}", memory.Read_U32(command_block.address));
  memory.Memset(response.address, 0, response.size);

  // Event requests are not supported through here: the internal code is replied as is.
  return IPCReply(ExecuteCommand(request, command_block.address, command_block.size, data.address,
                                 data.size, response.address, response.size));
}

u32 SDIOSlot0Device::GetOCRegister() const
{
  // 2.7-3.6 V, plus the power-up done and capacity bits.
  constexpr u32 VOLTAGE_WINDOW = 0x00ff8000;
  constexpr u32 POWERED_UP = 0x80000000;
  constexpr u32 HIGH_CAPACITY = 0x40000000;
  u32 ocr = VOLTAGE_WINDOW;
  if ((m_status & CARD_INITIALIZED) != 0)
    ocr |= POWERED_UP;
  if ((m_status & CARD_SDHC) != 0)
    ocr |= HIGH_CAPACITY;
  return ocr;
}

// CSD register layouts (SD physical layer spec, CSD versions 1.0 and 2.0). The words are listed
// least significant first and, as IOS returns them, shifted right by eight bits (no CRC byte).
// Fixed fields are in the constants; the size fields are added below.
//
// Version 1.0: TAAC 80 ms, TRAN_SPEED 25 Mbit/s, CCC 0x5b5, READ_BL_PARTIAL, 100 mA currents,
// ERASE_BLK_EN, SECTOR_SIZE 128 blocks, R2W_FACTOR 2, COPY.
std::array<u32, 4> SDIOSlot0Device::GetCSDv1() const
{
  // capacity = (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN, with C_SIZE < 4096,
  // C_SIZE_MULT <= 7 and READ_BL_LEN starting at 9 (512-byte blocks).
  u64 units = m_card.GetSize();
  u32 block_len_log2 = 9;
  u32 shift = 0;
  bool inexact = false;
  while (units > 4096)
  {
    if ((units & 1) != 0)
      inexact = true;
    units >>= 1;
    ++shift;
    if (shift <= 2 + 7 + block_len_log2)
      continue;
    // C_SIZE_MULT is full: use bigger blocks instead.
    ++block_len_log2;
    if (block_len_log2 <= 15)
      continue;
    ERROR_LOG_FMT(IOS_SD, "The SD card image is too large for an SDSC card");
    units = 4096;
    shift = 2 + 7 + block_len_log2;
  }
  const u32 size_mult = shift - (2 + block_len_log2);
  const auto c_size = static_cast<u32>(units - 1);

  if (inexact)
  {
    WARN_LOG_FMT(IOS_SD, "The SD card image size cannot be expressed exactly in the CSD");
  }
  else
  {
    INFO_LOG_FMT(IOS_SD, "CSD: C_SIZE {}, C_SIZE_MULT {}, READ_BL_LEN {}", c_size, size_mult,
                 block_len_log2);
  }

  constexpr std::array<u32, 4> FIXED{0x000007f0, 0x035b5080, 0x003ffc7f, 0x80040040};
  return {{
      FIXED[0],
      FIXED[1] | (block_len_log2 << 8) | (c_size >> 10),
      FIXED[2] | (c_size << 22) | (size_mult << 7),
      FIXED[3] | (block_len_log2 << 18),
  }};
}

// Version 2.0: TAAC 1 ms, TRAN_SPEED 50 Mbit/s, CCC 0x5f5, READ_BL_LEN 9, ERASE_BLK_EN,
// SECTOR_SIZE 128 blocks, R2W_FACTOR 4, WRITE_BL_LEN 9. C_SIZE counts 512 KiB units.
std::array<u32, 4> SDIOSlot0Device::GetCSDv2() const
{
  constexpr u64 UNIT = 512 * 1024;
  const u64 size = m_card.GetSize();
  if (size % UNIT != 0)
    WARN_LOG_FMT(IOS_SD, "The SDHC card image size is not a multiple of 512 KiB");
  const auto c_size = static_cast<u32>(size / UNIT - 1);

  constexpr std::array<u32, 4> FIXED{0x00400e00, 0x5a5f5900, 0x0000007f, 0x800a4000};
  return {{FIXED[0], FIXED[1], FIXED[2] | (c_size << 8), FIXED[3]}};
}

u64 SDIOSlot0Device::GetAddressFromRequest(u32 arg) const
{
  // SDSC cards are addressed in bytes, SDHC cards in 512-byte blocks.
  const u64 address = arg;
  return (m_status & CARD_SDHC) != 0 ? address * 512 : address;
}

void SDIOSlot0Device::InitSDHC()
{
  m_protocol = SDProtocol::V2;
  m_status |= CARD_INITIALIZED;
}
}  // namespace IOS::HLE
