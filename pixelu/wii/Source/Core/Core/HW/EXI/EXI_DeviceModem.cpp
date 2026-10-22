// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceModem.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace ExpansionInterface
{

namespace
{
// Transfer descriptor fields.
constexpr u32 DESC_WRITE = 0x40000000;
constexpr u32 DESC_MODEM = 0x20000000;
constexpr u32 DESC_KIND_MASK = 0x0F000000;
constexpr u32 KIND_AT = 0x03000000;
constexpr u32 KIND_PACKET = 0x08000000;
constexpr u32 RESET_DESCRIPTOR = 0x00008000;

// Registers.
constexpr std::size_t REG_DEVICE_TYPE = 0x00;
constexpr std::size_t REG_INTERRUPT_MASK = 0x01;
constexpr std::size_t REG_PENDING_INTERRUPTS = 0x02;
constexpr std::size_t REG_AT_COMMAND_SIZE = 0x04;
constexpr std::size_t REG_AT_REPLY_SIZE = 0x05;
constexpr std::size_t REG_RX_AVAILABLE_HIGH = 0x0B;
constexpr std::size_t REG_RX_AVAILABLE_LOW = 0x0C;
constexpr std::size_t REG_RX_THRESHOLD_HIGH = 0x10;
constexpr std::size_t REG_RX_THRESHOLD_LOW = 0x11;
constexpr std::size_t REGISTER_COUNT = 0x20;

// Interrupt bits.
constexpr u8 INT_AT_REPLY = 0x02;
constexpr u8 INT_SEND_READY = 0x10;
constexpr u8 INT_RECEIVE_READY = 0x20;

constexpr std::size_t RX_AVAILABLE_LIMIT = 0x200;

bool IsWrite(u32 descriptor)
{
  return (descriptor & DESC_WRITE) != 0;
}

bool IsModem(u32 descriptor)
{
  return (descriptor & DESC_MODEM) != 0;
}

u32 RemainingSize(u32 descriptor)
{
  return (descriptor >> 8) & 0xFFFF;
}

std::size_t FirstRegister(u32 descriptor)
{
  return (descriptor >> 24) & 0x1F;
}
}  // namespace

CEXIModem::CEXIModem(Core::System& system, ModemDeviceType type) : IEXIDevice(system)
{
  switch (type)
  {
  case ModemDeviceType::TAPSERVER:
    m_network_interface = std::make_unique<TAPServerNetworkInterface>(
        this, Config::Get(Config::MAIN_MODEM_TAPSERVER_DESTINATION));
    INFO_LOG_FMT(SP1, "Created tapserver physical network interface.");
    break;
  }

  m_registers[REG_DEVICE_TYPE] = 0x02;
  m_registers[REG_INTERRUPT_MASK] = 0x02;
}

CEXIModem::~CEXIModem()
{
  m_network_interface->RecvStop();
  m_network_interface->Deactivate();
}

bool CEXIModem::IsPresent() const
{
  return true;
}

void CEXIModem::SetCS(int)
{
  m_descriptor = NO_TRANSFER;
}

bool CEXIModem::IsInterruptSet()
{
  return (m_registers[REG_INTERRUPT_MASK] & m_registers[REG_PENDING_INTERRUPTS]) != 0;
}

void CEXIModem::DoState(PointerWrap&)
{
  // The connection cannot be saved; nothing else is either.
}

void CEXIModem::SetInterrupt(u8 flag, bool on, bool from_cpu)
{
  u8& pending = m_registers[REG_PENDING_INTERRUPTS];
  if (on)
    pending |= flag;
  else
    pending &= static_cast<u8>(0xFF ^ flag);
  m_system.GetExpansionInterface().ScheduleUpdateInterrupts(
      from_cpu ? CoreTiming::FromThread::CPU : CoreTiming::FromThread::NON_CPU, 0);
}

// The receive buffer lock must be held.
void CEXIModem::UpdateReceiveState(bool from_cpu)
{
  const std::size_t available = std::min(m_receive_buffer.size(), RX_AVAILABLE_LIMIT);
  m_registers[REG_RX_AVAILABLE_HIGH] = static_cast<u8>(available >> 8);
  m_registers[REG_RX_AVAILABLE_LOW] = static_cast<u8>(available & 0xFF);

  const std::size_t threshold = (std::size_t{m_registers[REG_RX_THRESHOLD_HIGH]} << 8) |
                                m_registers[REG_RX_THRESHOLD_LOW];
  SetInterrupt(INT_RECEIVE_READY, m_receive_buffer.size() >= threshold, from_cpu);
}

void CEXIModem::AdvanceDescriptor(u32 remaining)
{
  if (remaining == 0)
    m_descriptor = NO_TRANSFER;
  else
    m_descriptor = (m_descriptor & 0xFF000000) | (remaining << 8);
}

void CEXIModem::SendComplete()
{
  SetInterrupt(INT_SEND_READY, true, true);
}

void CEXIModem::AddToReceiveBuffer(std::string&& data)
{
  std::lock_guard lock(m_receive_lock);
  m_receive_buffer += data;
  UpdateReceiveState(false);
}

void CEXIModem::ReplyAT(const std::string& text)
{
  m_at_reply += text;
  m_registers[REG_AT_REPLY_SIZE] = static_cast<u8>(m_at_reply.size());
  SetInterrupt(INT_AT_REPLY, !m_at_reply.empty(), true);
}

void CEXIModem::RunATCommands()
{
  while (true)
  {
    const std::size_t end = m_at_command.find_first_of("\r\n");
    if (end == std::string::npos)
      return;
    const std::string command = m_at_command.substr(0, end);
    m_at_command.erase(0, end + 1);

    INFO_LOG_FMT(SP1, "Received AT command: {}", command);
    if (command.starts_with("ATZ") || command == "ATH0")
    {
      // Hang up.
      m_network_interface->RecvStop();
      m_network_interface->Deactivate();
      ReplyAT("OK\r");
    }
    else if (command.starts_with("ATD"))
    {
      if (m_network_interface->Activate())
      {
        m_network_interface->RecvStart();
        ReplyAT("OK\rCONNECT 115200\r");
      }
      else
      {
        ReplyAT("OK\rNO ANSWER\r");
      }
    }
    else
    {
      ReplyAT("OK\r");
    }
  }
}

void CEXIModem::ReadBuffer(u8* destination, u32 size)
{
  const u32 requested = RemainingSize(m_descriptor);
  if (size > requested)
  {
    ERROR_LOG_FMT(SP1, "More bytes requested ({}) than originally requested for transfer {:x}",
                  size, m_descriptor);
    size = requested;
  }

  const u32 kind = m_descriptor & DESC_KIND_MASK;
  if (kind == KIND_AT)
  {
    const std::size_t count = std::min<std::size_t>(size, m_at_reply.size());
    if (destination != nullptr)
      std::memcpy(destination, m_at_reply.data(), count);
    m_at_reply.erase(0, count);
    m_registers[REG_AT_REPLY_SIZE] = static_cast<u8>(m_at_reply.size());
    SetInterrupt(INT_AT_REPLY, !m_at_reply.empty(), true);
  }
  else if (kind == KIND_PACKET)
  {
    std::lock_guard lock(m_receive_lock);
    const std::size_t count = std::min<std::size_t>(size, m_receive_buffer.size());
    if (destination != nullptr)
      std::memcpy(destination, m_receive_buffer.data(), count);
    m_receive_buffer.erase(0, count);
    UpdateReceiveState(true);
  }
  else
  {
    ERROR_LOG_FMT(SP1, "Invalid modem read transfer type {:x}", m_descriptor);
  }

  AdvanceDescriptor(requested - size);
}

void CEXIModem::WriteBuffer(const u8* source, u32 size)
{
  const u32 expected = RemainingSize(m_descriptor);
  if (size > expected)
  {
    ERROR_LOG_FMT(SP1, "More bytes received ({}) than expected for transfer {:x}", size,
                  m_descriptor);
    return;
  }

  const u32 kind = m_descriptor & DESC_KIND_MASK;
  const std::string_view bytes =
      source != nullptr ? std::string_view(reinterpret_cast<const char*>(source), size) :
                          std::string_view{};
  if (kind == KIND_AT)
  {
    m_at_command += bytes;
    RunATCommands();
    m_registers[REG_AT_COMMAND_SIZE] = static_cast<u8>(m_at_command.size());
  }
  else if (kind == KIND_PACKET)
  {
    m_send_buffer += bytes;
    SetInterrupt(INT_SEND_READY, true, true);
    m_network_interface->SendAndRemoveAllHDLCFrames(&m_send_buffer);
  }
  else
  {
    ERROR_LOG_FMT(SP1, "Invalid modem write transfer type {:x}", m_descriptor);
  }

  AdvanceDescriptor(expected - size);
}

void CEXIModem::ImmWrite(u32 data, u32 size)
{
  if (m_descriptor == NO_TRANSFER)
  {
    m_descriptor = data;
    if (data == RESET_DESCRIPTOR)
    {
      m_network_interface->RecvStop();
      m_network_interface->Deactivate();
      m_descriptor = NO_TRANSFER;
    }
    return;
  }

  if (!IsWrite(m_descriptor))
  {
    ERROR_LOG_FMT(SP1, "Received EXI IMM write {:x} ({} bytes) after read command {:x}", data,
                  size, m_descriptor);
    m_descriptor = NO_TRANSFER;
    return;
  }

  if (IsModem(m_descriptor))
  {
    std::array<u8, 4> bytes{};
    const u32 count = std::min<u32>(size, 4);
    for (u32 i = 0; i != count; ++i)
      bytes[i] = static_cast<u8>(data >> (24 - 8 * i));
    WriteBuffer(bytes.data(), count);
    return;
  }

  bool interrupts_touched = false;
  std::size_t reg = FirstRegister(m_descriptor);
  for (u32 left = size; left != 0 && reg < REGISTER_COUNT; --left, ++reg)
  {
    m_registers[reg] = static_cast<u8>(data >> 24);
    data <<= 8;
    if (reg == REG_INTERRUPT_MASK || reg == REG_PENDING_INTERRUPTS)
      interrupts_touched = true;
  }
  if (interrupts_touched)
    m_system.GetExpansionInterface().ScheduleUpdateInterrupts(CoreTiming::FromThread::CPU, 0);
  m_descriptor = NO_TRANSFER;
}

u32 CEXIModem::ImmRead(u32 size)
{
  if (m_descriptor == NO_TRANSFER)
  {
    ERROR_LOG_FMT(SP1, "Received EXI IMM read ({} bytes) with no pending transfer", size);
    return 0;
  }

  if (IsWrite(m_descriptor))
  {
    ERROR_LOG_FMT(SP1, "Received EXI IMM read ({} bytes) after write command {:x}", size,
                  m_descriptor);
    m_descriptor = NO_TRANSFER;
    return 0;
  }

  if (IsModem(m_descriptor))
  {
    std::array<u8, 4> bytes{};
    ReadBuffer(bytes.data(), std::min<u32>(size, 4));
    return (u32{bytes[0]} << 24) | (u32{bytes[1]} << 16) | (u32{bytes[2]} << 8) | bytes[3];
  }

  const std::size_t first = FirstRegister(m_descriptor);
  if (first == REG_DEVICE_TYPE)
    return EXI_DEVTYPE_MODEM;

  u32 result = 0;
  for (u32 z = 0; z < std::min<u32>(size, 4) && first + z < REGISTER_COUNT; ++z)
    result |= u32{m_registers[first + z]} << ((3 - z) * 8);
  m_descriptor = NO_TRANSFER;
  return result;
}

void CEXIModem::DMAWrite(u32 addr, u32 size)
{
  if (m_descriptor == NO_TRANSFER)
  {
    ERROR_LOG_FMT(SP1, "Received EXI DMA write {:x} ({} bytes) after read command {:x}", addr,
                  size, m_descriptor);
    return;
  }
  if (!IsWrite(m_descriptor))
  {
    ERROR_LOG_FMT(SP1, "Received EXI DMA write {:x} ({} bytes) after read command {:x}", addr,
                  size, m_descriptor);
    m_descriptor = NO_TRANSFER;
    return;
  }
  if (!IsModem(m_descriptor))
  {
    ERROR_LOG_FMT(SP1, "Received EXI DMA write {:x} ({} bytes) to registers {:x}", addr, size,
                  m_descriptor);
    m_descriptor = NO_TRANSFER;
    return;
  }

  auto& memory = m_system.GetMemory();
  WriteBuffer(memory.GetPointerForRange(addr, size), size);
}

void CEXIModem::DMARead(u32 addr, u32 size)
{
  if (m_descriptor == NO_TRANSFER)
  {
    ERROR_LOG_FMT(SP1, "Received EXI DMA read {:x} ({} bytes) with no pending transfer", addr,
                  size);
    return;
  }
  if (IsWrite(m_descriptor))
  {
    ERROR_LOG_FMT(SP1, "Received EXI DMA read {:x} ({} bytes) after write command {:x}", addr,
                  size, m_descriptor);
    m_descriptor = NO_TRANSFER;
    return;
  }
  if (!IsModem(m_descriptor))
  {
    ERROR_LOG_FMT(SP1, "Received EXI DMA read {:x} ({} bytes) to registers {:x}", addr, size,
                  m_descriptor);
    m_descriptor = NO_TRANSFER;
    return;
  }

  auto& memory = m_system.GetMemory();
  ReadBuffer(memory.GetPointerForRange(addr, size), size);
}

}  // namespace ExpansionInterface
