// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceGBA.h"
#include "Core/HW/SystemTimers.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace SerialInterface
{
namespace
{
// Register offsets inside the SI block. The three per-port words repeat
// every 12 bytes starting at 0x00.
constexpr u32 PORT_STRIDE = 0x0C;
constexpr u32 REG_PORT_OUT = 0x00;
constexpr u32 REG_PORT_IN_HI = 0x04;
constexpr u32 REG_PORT_IN_LO = 0x08;
constexpr u32 REG_POLL = 0x30;
constexpr u32 REG_COMCSR = 0x34;
constexpr u32 REG_STATUS = 0x38;
constexpr u32 REG_EXI_CLOCK = 0x3C;
constexpr u32 REG_BUFFER = 0x80;

// SICOMCSR fields.
constexpr u32 CSR_TSTART = 1u << 0;
constexpr u32 CSR_CHANNEL_SHIFT = 1;
constexpr u32 CSR_CHANNEL_MASK = 0x3u << CSR_CHANNEL_SHIFT;
constexpr u32 CSR_INLEN_SHIFT = 8;
constexpr u32 CSR_INLEN_MASK = 0x7Fu << CSR_INLEN_SHIFT;
constexpr u32 CSR_OUTLEN_SHIFT = 16;
constexpr u32 CSR_OUTLEN_MASK = 0x7Fu << CSR_OUTLEN_SHIFT;
constexpr u32 CSR_RDSTINTMSK = 1u << 27;
constexpr u32 CSR_RDSTINT = 1u << 28;
constexpr u32 CSR_COMERR = 1u << 29;
constexpr u32 CSR_TCINTMSK = 1u << 30;
constexpr u32 CSR_TCINT = 1u << 31;
// The fields a write to SICOMCSR simply stores.
constexpr u32 CSR_WRITABLE = CSR_CHANNEL_MASK | CSR_INLEN_MASK | CSR_OUTLEN_MASK |
                             CSR_RDSTINTMSK | CSR_TCINTMSK;

// SISR: one byte per port, port 0 in the top byte. Within that byte bit 0 is
// UNRUN, 1 OVRUN, 2 COLL, 3 NOREP (all write-1-to-clear), 4 WRST and 5 RDST.
constexpr u32 SR_ERROR_BITS = 0x0F0F0F0F;
constexpr u32 SR_WRST_BITS = 0x10101010;
constexpr u32 SR_RDST_BITS = 0x20202020;
constexpr u32 SR_WR = 1u << 31;

constexpr u32 PortByteShift(u32 port)
{
  return 24 - 8 * port;
}
constexpr u32 NoResponseBit(u32 port)
{
  return 0x08u << PortByteShift(port);
}
constexpr u32 ReadStatusBit(u32 port)
{
  return 0x20u << PortByteShift(port);
}

// SIPOLL: EN0..EN3 live in bits 7..4, X (lines between polls) in bits 16..25.
constexpr u32 POLL_X_SHIFT = 16;
constexpr u32 POLL_X_MASK = 0x3FFu;
constexpr u32 POLL_X_DEFAULT = 492;
constexpr u8 PollEnabled(u32 poll, u32 port)
{
  return static_cast<u8>((poll >> (7 - port)) & 1);
}

// SICxINBUFH flags.
constexpr u32 IN_ERRSTAT = 1u << 31;
constexpr u32 IN_ERRLATCH = 1u << 30;

// A length field of 0 means 128 bytes.
constexpr s32 TransferLength(u32 field)
{
  const u32 length = field & 0x7F;
  return length == 0 ? 128 : static_cast<s32>(length);
}

template <typename T>
T LoadBigEndian(const u8* src)
{
  T value;
  std::memcpy(&value, src, sizeof(T));
  return Common::FromBigEndian(value);
}

template <typename T>
void StoreBigEndian(u8* dst, T value)
{
  const T swapped = Common::FromBigEndian(value);
  std::memcpy(dst, &swapped, sizeof(T));
}
}  // namespace

SerialInterfaceManager::SerialInterfaceManager(Core::System& system) : m_system(system)
{
}

SerialInterfaceManager::~SerialInterfaceManager() = default;

void SerialInterfaceManager::FlagNoResponse(u32 port)
{
  if (port < MAX_SI_CHANNELS)
    m_status |= NoResponseBit(port);
}

void SerialInterfaceManager::RefreshInterrupt()
{
  if ((m_status & SR_RDST_BITS) != 0)
    m_comcsr |= CSR_RDSTINT;
  else
    m_comcsr &= ~CSR_RDSTINT;

  const bool read_pending = (m_comcsr & CSR_RDSTINT) && (m_comcsr & CSR_RDSTINTMSK);
  const bool transfer_done = (m_comcsr & CSR_TCINT) && (m_comcsr & CSR_TCINTMSK);
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_SI,
                                                read_pending || transfer_done);
}

void SerialInterfaceManager::RaiseTransferComplete()
{
  m_comcsr |= CSR_TCINT;
  RefreshInterrupt();
}

void SerialInterfaceManager::OnTransferEvent(Core::System& system, u64, s64 cycles_late)
{
  system.GetSerialInterface().ProcessTransfer(cycles_late);
}

void SerialInterfaceManager::OnCooldownEvent(Core::System& system, u64 userdata, s64)
{
  system.GetSerialInterface().m_ports[userdata].unplug_cooldown = false;
}

void SerialInterfaceManager::ProcessTransfer(s64 cycles_late)
{
  if ((m_comcsr & CSR_TSTART) == 0)
    return;

  const u32 port = (m_comcsr & CSR_CHANNEL_MASK) >> CSR_CHANNEL_SHIFT;
  const s32 out_length = TransferLength((m_comcsr & CSR_OUTLEN_MASK) >> CSR_OUTLEN_SHIFT);
  const s32 in_length = TransferLength((m_comcsr & CSR_INLEN_MASK) >> CSR_INLEN_SHIFT);

  ISIDevice* const device = m_ports[port].device.get();
  const s32 reply_length = device->RunBuffer(m_buffer.data(), out_length);

  DEBUG_LOG_FMT(SERIALINTERFACE, "SI transfer on port {}: sent {} bytes, wanted {}, got {}", port,
                out_length, in_length, reply_length);
  if (reply_length > 0 && reply_length != in_length)
  {
    WARN_LOG_FMT(SERIALINTERFACE, "SI transfer on port {}: game expected {} reply bytes, device gave {}",
                 port, in_length, reply_length);
  }

  if (reply_length == 0)
  {
    // The device is still busy; poll it again after its transfer interval.
    m_system.GetCoreTiming().ScheduleEvent(device->TransferInterval() - cycles_late,
                                           m_transfer_event);
    return;
  }

  m_comcsr &= ~CSR_TSTART;
  if (reply_length < 0)
  {
    m_comcsr |= CSR_COMERR;
    FlagNoResponse(port);
  }
  else
  {
    m_comcsr &= ~CSR_COMERR;
  }
  RaiseTransferComplete();
}

void SerialInterfaceManager::DoState(PointerWrap& p)
{
  for (int i = 0; i < MAX_SI_CHANNELS; ++i)
  {
    Port& port = m_ports[static_cast<std::size_t>(i)];
    p.Do(port.in_hi);
    p.Do(port.in_lo);
    p.Do(port.out);
    p.Do(port.unplug_cooldown);

    SIDevices saved_type = port.device->GetDeviceType();
    p.Do(saved_type);
    if (saved_type != port.device->GetDeviceType())
      AddDevice(SIDevice_Create(m_system, saved_type, i));

    port.device->DoState(p);
  }

  p.Do(m_poll);
  p.Do(m_comcsr);
  p.Do(m_status);
  p.Do(m_exi_clock);
  p.Do(m_buffer);
}

void SerialInterfaceManager::ScheduleEvent(int device_number, s64 cycles_into_future, u64 userdata)
{
  m_system.GetCoreTiming().ScheduleEvent(cycles_into_future, m_port_events[static_cast<std::size_t>(device_number)],
                                         userdata);
}

void SerialInterfaceManager::RemoveEvent(int device_number)
{
  m_system.GetCoreTiming().RemoveEvent(m_port_events[static_cast<std::size_t>(device_number)]);
}

void SerialInterfaceManager::Init()
{
  auto& timing = m_system.GetCoreTiming();
  m_cooldown_event = timing.RegisterEvent("ChangeSIDevice", OnCooldownEvent);
  m_transfer_event = timing.RegisterEvent("SITransferPending", OnTransferEvent);
  for (u32 i = 0; i < MAX_SI_CHANNELS; ++i)
  {
    Port* const port = &m_ports[i];
    m_port_events[i] = timing.RegisterEvent(
        fmt::format("SIEventChannel{}", i),
        [port](Core::System&, u64 userdata, s64 cycles_late) {
          port->device->OnEvent(userdata, cycles_late);
        });
  }

  for (int i = 0; i < MAX_SI_CHANNELS; ++i)
  {
    Port& port = m_ports[static_cast<std::size_t>(i)];
    port.out = 0;
    port.in_hi = 0;
    port.in_lo = 0;
    port.unplug_cooldown = false;
    AddDevice(Config::Get(Config::GetInfoForSIDevice(i)), i);
  }

  m_poll = POLL_X_DEFAULT << POLL_X_SHIFT;
  m_comcsr = 0;
  m_status = 0;
  // The EXI clock lock bit is documented as set on reset, but real consoles
  // read it back as clear, so it starts at zero.
  m_exi_clock = 0;
  m_buffer.fill(0);
}

void SerialInterfaceManager::Shutdown()
{
  for (int i = 0; i < MAX_SI_CHANNELS; ++i)
    RemoveDevice(i);
  GBAConnectionWaiter_Shutdown();
}

u32 SerialInterfaceManager::ReadInputWord(u32 port, bool high)
{
  // Reading either input word acknowledges the port's new-data flag.
  m_status &= ~ReadStatusBit(port);
  RefreshInterrupt();
  return high ? m_ports[port].in_hi : m_ports[port].in_lo;
}

void SerialInterfaceManager::WriteComCsr(u32 value)
{
  m_comcsr = (m_comcsr & ~CSR_WRITABLE) | (value & CSR_WRITABLE);

  // The two interrupt flags are cleared by writing 1.
  if (value & CSR_RDSTINT)
    m_comcsr &= ~CSR_RDSTINT;
  if (value & CSR_TCINT)
    m_comcsr &= ~CSR_TCINT;

  if (value & CSR_TSTART)
  {
    // Starting a transfer while one is pending restarts it.
    if (m_comcsr & CSR_TSTART)
      m_system.GetCoreTiming().RemoveEvent(m_transfer_event);
    m_comcsr |= CSR_TSTART;
    ProcessTransfer(0);
  }

  if ((m_comcsr & CSR_TSTART) == 0)
    RefreshInterrupt();
}

void SerialInterfaceManager::WriteStatus(u32 value)
{
  m_status &= ~(value & SR_ERROR_BITS);

  if ((value & SR_WR) == 0)
    return;

  // WR copies every port's output word to its device.
  for (u32 i = 0; i < MAX_SI_CHANNELS; ++i)
    m_ports[i].device->SendCommand(m_ports[i].out, PollEnabled(m_poll, i));
  m_status &= ~(SR_WR | SR_WRST_BITS);
}

void SerialInterfaceManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  const u32 buffer_base = base | REG_BUFFER;

  // The communication buffer is big-endian and can be accessed in 32-bit or
  // 16-bit units.
  for (u32 offset = 0; offset < BUFFER_SIZE; offset += 4)
  {
    mmio->Register(base | (buffer_base + offset),
                   MMIO::ComplexRead<u32>([offset](Core::System& system, u32) {
                     return LoadBigEndian<u32>(&system.GetSerialInterface().m_buffer[offset]);
                   }),
                   MMIO::ComplexWrite<u32>([offset](Core::System& system, u32, u32 value) {
                     StoreBigEndian<u32>(&system.GetSerialInterface().m_buffer[offset], value);
                   }));
  }
  for (u32 offset = 0; offset < BUFFER_SIZE; offset += 2)
  {
    mmio->Register(base | (buffer_base + offset),
                   MMIO::ComplexRead<u16>([offset](Core::System& system, u32) {
                     return LoadBigEndian<u16>(&system.GetSerialInterface().m_buffer[offset]);
                   }),
                   MMIO::ComplexWrite<u16>([offset](Core::System& system, u32, u16 value) {
                     StoreBigEndian<u16>(&system.GetSerialInterface().m_buffer[offset], value);
                   }));
  }

  for (u32 port = 0; port < MAX_SI_CHANNELS; ++port)
  {
    const u32 port_base = base | (port * PORT_STRIDE);
    Port& state = m_ports[port];

    mmio->Register(port_base + REG_PORT_OUT, MMIO::DirectRead<u32>(&state.out),
                   MMIO::DirectWrite<u32>(&state.out));
    mmio->Register(port_base + REG_PORT_IN_HI,
                   MMIO::ComplexRead<u32>([port](Core::System& system, u32) {
                     return system.GetSerialInterface().ReadInputWord(port, true);
                   }),
                   MMIO::DirectWrite<u32>(&state.in_hi));
    mmio->Register(port_base + REG_PORT_IN_LO,
                   MMIO::ComplexRead<u32>([port](Core::System& system, u32) {
                     return system.GetSerialInterface().ReadInputWord(port, false);
                   }),
                   MMIO::DirectWrite<u32>(&state.in_lo));
  }

  mmio->Register(base | REG_POLL, MMIO::DirectRead<u32>(&m_poll), MMIO::DirectWrite<u32>(&m_poll));
  mmio->Register(base | REG_COMCSR, MMIO::DirectRead<u32>(&m_comcsr),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 value) {
                   system.GetSerialInterface().WriteComCsr(value);
                 }));
  mmio->Register(base | REG_STATUS, MMIO::DirectRead<u32>(&m_status),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 value) {
                   system.GetSerialInterface().WriteStatus(value);
                 }));
  mmio->Register(base | REG_EXI_CLOCK, MMIO::DirectRead<u32>(&m_exi_clock),
                 MMIO::DirectWrite<u32>(&m_exi_clock));
}

void SerialInterfaceManager::RemoveDevice(int device_number)
{
  m_ports.at(static_cast<std::size_t>(device_number)).device.reset();
}

void SerialInterfaceManager::AddDevice(std::unique_ptr<ISIDevice> device)
{
  // The new device already exists at this point; the old one is destroyed
  // only now, so its destructor runs after the new device's constructor.
  Port& port = m_ports.at(static_cast<std::size_t>(device->GetDeviceNumber()));
  port.device.reset();
  port.device = std::move(device);
}

void SerialInterfaceManager::AddDevice(const SIDevices device, int device_number)
{
  AddDevice(SIDevice_Create(m_system, device, device_number));
}

void SerialInterfaceManager::SwapDevice(SIDevices device, int port_number)
{
  if (port_number < 0 || port_number >= MAX_SI_CHANNELS)
    return;
  Port& port = m_ports[static_cast<std::size_t>(port_number)];
  if (port.unplug_cooldown)
    return;

  // A change always goes through "nothing plugged in" first, so a port with
  // a device is emptied now and gets the new device once the cooldown ends.
  if (GetDeviceType(port_number) != SIDEVICE_NONE)
    device = SIDEVICE_NONE;

  port.out = 0;
  port.in_hi = 0;
  port.in_lo = 0;
  AddDevice(device, port_number);

  if (device != SIDEVICE_NONE)
    return;
  port.unplug_cooldown = true;
  m_system.GetCoreTiming().ScheduleEvent(m_system.GetSystemTimers().GetTicksPerSecond(),
                                         m_cooldown_event, static_cast<u64>(port_number));
}

void SerialInterfaceManager::UpdateDevices()
{
  for (int i = 0; i < MAX_SI_CHANNELS; ++i)
  {
    const SIDevices wanted = Config::Get(Config::GetInfoForSIDevice(i));
    if (GetDeviceType(i) != wanted)
      SwapDevice(wanted, i);
  }

  // All four ports are polled back to back; NetPlay can batch them.
  NetPlay::SetSIPollBatching(true);

  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  g_controller_interface.UpdateInput();

  for (u32 i = 0; i < MAX_SI_CHANNELS; ++i)
  {
    Port& port = m_ports[i];
    // The device may overwrite the whole word, but the error latch survives.
    const u32 latch_before = port.in_hi & IN_ERRLATCH;
    bool failed = false;

    switch (port.device->GetData(port.in_hi, port.in_lo))
    {
    case DataResponse::Success:
      m_status |= ReadStatusBit(i);
      break;
    case DataResponse::ErrorNoResponse:
      FlagNoResponse(i);
      failed = true;
      break;
    case DataResponse::NoData:
      failed = true;
      break;
    }

    if (failed)
      port.in_hi = (port.in_hi & ~IN_ERRLATCH) | IN_ERRSTAT | IN_ERRLATCH;
    else
      port.in_hi = (port.in_hi & ~IN_ERRLATCH) | latch_before;
  }

  RefreshInterrupt();

  NetPlay::SetSIPollBatching(false);
}

SIDevices SerialInterfaceManager::GetDeviceType(int channel) const
{
  if (channel < 0 || channel >= MAX_SI_CHANNELS)
    return SIDEVICE_NONE;
  const auto& device = m_ports[static_cast<std::size_t>(channel)].device;
  return device ? device->GetDeviceType() : SIDEVICE_NONE;
}

u32 SerialInterfaceManager::GetPollXLines()
{
  return (m_poll >> POLL_X_SHIFT) & POLL_X_MASK;
}

}  // namespace SerialInterface
