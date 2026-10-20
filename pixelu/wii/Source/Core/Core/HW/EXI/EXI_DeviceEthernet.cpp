// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceEthernet.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace ExpansionInterface
{
namespace
{
// EXI-side registers of the adapter (addressed by 16-bit commands).
constexpr u16 ADAPTER_EXI_ID = 0x00;
constexpr u16 ADAPTER_REVISION = 0x01;
constexpr u16 ADAPTER_IRQ_MASK = 0x02;
constexpr u16 ADAPTER_IRQ_CAUSE = 0x03;
constexpr u16 ADAPTER_DEVICE_ID = 0x04;
constexpr u16 ADAPTER_AC_START = 0x05;

constexpr u32 BBA_EXI_ID = 0x04020200;
constexpr u32 BBA_DEVICE_ID = 0xD107;
constexpr u32 BBA_AC_START = 0x4E;

// The adapter's interrupt cause bit for "the controller wants attention".
constexpr u8 ADAPTER_IRQ_CONTROLLER = 0x80;

// MX98728EC registers used here.
constexpr u32 MX_NCRA = 0x00;
constexpr u32 MX_NCRB = 0x01;
constexpr u32 MX_LTPS = 0x04;
constexpr u32 MX_LRPS = 0x05;
constexpr u32 MX_IMR = 0x08;
constexpr u32 MX_IR = 0x09;
constexpr u32 MX_BP = 0x0a;
constexpr u32 MX_IOB = 0x10;
constexpr u32 MX_RWP = 0x16;
constexpr u32 MX_RRP = 0x18;
constexpr u32 MX_RHBP = 0x1a;
constexpr u32 MX_PAR = 0x20;
constexpr u32 MX_MAR = 0x26;
constexpr u32 MX_NWAYC = 0x30;
constexpr u32 MX_NWAYS = 0x31;
constexpr u32 MX_MISC1 = 0x3d;
constexpr u32 MX_TXFIFOCNT = 0x3e;
constexpr u32 MX_WRTXFIFOD = 0x48;
constexpr u32 MX_MISC2 = 0x50;

// NCRA bits.
constexpr u8 NCRA_RESET = 0x01;
constexpr u8 NCRA_ST0 = 0x02;
constexpr u8 NCRA_ST1 = 0x04;
constexpr u8 NCRA_SR = 0x08;
// NCRB bits.
constexpr u8 NCRB_PR = 0x01;
constexpr u8 NCRB_PM = 0x04;
constexpr u8 NCRB_AB = 0x10;
// Interrupt bits (IMR, IR).
constexpr u8 IRQ_RECEIVE = 0x02;
constexpr u8 IRQ_TRANSMIT = 0x04;
constexpr u8 IRQ_RX_BUFFER_FULL = 0x80;
// NWAYC / NWAYS bits.
constexpr u8 NWAYC_ANE = 0x04;
constexpr u8 NWAYC_LTE = 0x80;
constexpr u8 NWAYS_LS100 = 0x02;
constexpr u8 NWAYS_LPNWAY = 0x04;
constexpr u8 NWAYS_ANCLPT = 0x08;
constexpr u8 NWAYS_100TXF = 0x10;
// MISC1 / MISC2 bits.
constexpr u8 MISC1_TPF = 0x04;
constexpr u8 MISC1_TPH = 0x08;
constexpr u8 MISC1_TXF = 0x10;
constexpr u8 MISC1_TXH = 0x20;
constexpr u8 MISC2_AUTORCVR = 0x80;
// Receive descriptor status bits.
constexpr u32 RX_BUFFER_FULL = 0x01;
constexpr u32 RX_FIFO_OVERRUN = 0x08;
constexpr u32 RX_MULTICAST = 0x20;

constexpr u32 PAGE_SIZE = 0x100;
constexpr u32 DESCRIPTOR_SIZE = 4;

// The receive descriptor at the start of a frame's first page: status, length (descriptor
// included) and the page after the frame.
constexpr u32 MakeDescriptor(u32 next_page, u32 length, u32 status)
{
  return ((status & 0xff) << 24) | ((length & 0xfff) << 12) | (next_page & 0xfff);
}

// The 6-bit multicast hash: the top bits of the Ethernet CRC-32 of the address.
u32 MulticastHash(const u8* address)
{
  u32 crc = 0xffffffff;
  for (int index = 0; index < 6; ++index)
  {
    for (int bit = 0; bit < 8; ++bit)
    {
      const bool feedback = ((crc >> 31) ^ (address[index] >> bit)) & 1;
      crc <<= 1;
      if (feedback)
        crc ^= 0x04c11db7;
    }
  }
  return crc >> 26;
}
}  // namespace

CEXIETHERNET::CEXIETHERNET(Core::System& system, BBADeviceType type) : IEXIDevice(system)
{
  // The MAC address comes from the settings; a missing or unreadable one is replaced by a new
  // random address, which is saved.
  std::string mac_setting = Config::Get(Config::MAIN_BBA_MAC);
  std::optional<Common::MACAddress> mac = Common::StringToMacAddress(mac_setting);
  Common::ToLower(&mac_setting);
  if (!mac)
  {
    mac = Common::GenerateMacAddress(Common::MACConsumer::BBA);
    mac_setting = Common::MacAddressToString(*mac);
    Config::SetBaseOrCurrent(Config::MAIN_BBA_MAC, mac_setting);
    Config::Save();
  }

  switch (type)
  {
  case BBADeviceType::TAP:
    m_network_interface = std::make_unique<TAPNetworkInterface>(this);
    INFO_LOG_FMT(SP1, "Created TAP physical network interface.");
    break;

  case BBADeviceType::TAPSERVER:
    m_network_interface = std::make_unique<TAPServerNetworkInterface>(
        this, Config::Get(Config::MAIN_BBA_TAPSERVER_DESTINATION));
    INFO_LOG_FMT(SP1, "Created tapserver physical network interface.");
    break;

  case BBADeviceType::BuiltIn:
    m_network_interface = std::make_unique<BuiltInBBAInterface>(
        this, Config::Get(Config::MAIN_BBA_BUILTIN_DNS), Config::Get(Config::MAIN_BBA_BUILTIN_IP));
    INFO_LOG_FMT(SP1, "Created Built in network interface.");
    break;

  case BBADeviceType::IPC:
    // Every IPC adapter gets a fresh address so that several instances can share a channel.
    mac = Common::GenerateMacAddress(Common::MACConsumer::BBA);
    m_network_interface = std::make_unique<IPCBBAInterface>(this);
    INFO_LOG_FMT(SP1, "Created IPC-based network interface.");
    break;

  case BBADeviceType::XLINK:
    // XLink Kai only accepts the two Nintendo OUIs used for GameCube adapters. Warn, but carry
    // on.
    if (!mac_setting.starts_with("00:09:bf") && !mac_setting.starts_with("00:17:ab"))
    {
      PanicAlertFmtT(
          "BBA MAC address {0} invalid for XLink Kai. A valid Nintendo GameCube MAC address "
          "must be used. Generate a new MAC address starting with 00:09:bf or 00:17:ab.",
          mac_setting);
    }
    // The client needs a name that is unique per emulator; the MAC address gives one.
    m_network_interface = std::make_unique<XLinkNetworkInterface>(
        this, Config::Get(Config::MAIN_BBA_XLINK_IP), 34523,
        "dolphin" + Config::Get(Config::MAIN_BBA_MAC), Config::Get(Config::MAIN_BBA_XLINK_CHAT_OSD));
    INFO_LOG_FMT(SP1, "Created XLink Kai BBA network interface connection to {}:34523",
                 Config::Get(Config::MAIN_BBA_XLINK_IP));
    break;
  }

  m_tx_fifo = std::make_unique<u8[]>(TX_FIFO_SIZE);
  m_mem = std::make_unique<u8[]>(MEMORY_SIZE);
  m_rx_frame = std::make_unique<u8[]>(BBA_RECV_SIZE);

  // Power-on register values: promiscuous, autonegotiation and link test on, and the FIFO
  // thresholds. Then the station address, and a link that is always up at 100 Mbit full duplex.
  std::fill_n(m_mem.get(), MEMORY_SIZE, u8{0});
  Reg(MX_NCRB) = NCRB_PR;
  Reg(MX_NWAYC) = NWAYC_LTE | NWAYC_ANE;
  Reg(MX_MISC1) = MISC1_TPF | MISC1_TPH | MISC1_TXF | MISC1_TXH;
  std::copy(mac->begin(), mac->end(), &m_mem[MX_PAR]);
  Reg(MX_NWAYS) = NWAYS_LS100 | NWAYS_LPNWAY | NWAYS_100TXF | NWAYS_ANCLPT;
}

CEXIETHERNET::~CEXIETHERNET()
{
  m_network_interface->Deactivate();
}

u16 CEXIETHERNET::Reg16(u32 address) const
{
  return static_cast<u16>(m_mem[address & (MEMORY_SIZE - 1)] |
                          (m_mem[(address + 1) & (MEMORY_SIZE - 1)] << 8));
}

void CEXIETHERNET::SetReg16(u32 address, u16 value)
{
  Reg(address) = static_cast<u8>(value);
  Reg(address + 1) = static_cast<u8>(value >> 8);
}

Common::MACAddress CEXIETHERNET::MacAddress() const
{
  Common::MACAddress mac;
  std::copy_n(&m_mem[MX_PAR], mac.size(), mac.begin());
  return mac;
}

u32 CEXIETHERNET::ReceivePagesInUse() const
{
  // How far the write pointer is ahead of the read pointer, in pages, on the 16-page ring.
  u8 write_page = static_cast<u8>(Reg16(MX_RWP));
  const u8 read_page = static_cast<u8>(Reg16(MX_RRP));
  if (read_page > write_page)
    write_page = static_cast<u8>(write_page + 16);
  return static_cast<u32>(write_page - read_page);
}

void CEXIETHERNET::SetCS(int cs)
{
  // Every selection starts with a new command.
  if (cs != 0)
    m_transfer.valid = false;
}

bool CEXIETHERNET::IsPresent() const
{
  return true;
}

bool CEXIETHERNET::IsInterruptSet()
{
  return (m_irq_cause & m_irq_mask) != 0;
}

void CEXIETHERNET::RaiseInterrupt(u8 cause, bool schedule, CoreTiming::FromThread from)
{
  // The controller's cause is kept in IR when IMR enables it, and forwarded to the adapter.
  if ((Reg(MX_IMR) & cause) == 0)
    return;
  Reg(MX_IR) |= cause;
  m_irq_cause |= ADAPTER_IRQ_CONTROLLER;
  if (schedule)
    m_system.GetExpansionInterface().ScheduleUpdateInterrupts(from, 0);
}

void CEXIETHERNET::StartTransfer(u32 command)
{
  // Controller commands have bit 31 set, a 16-bit address in bits 8-23 and the write flag in
  // bit 30. Adapter commands are 16 bits wide: write flag in bit 14, address in bits 8-13.
  m_transfer.valid = true;
  if ((command & 0x80000000) != 0)
  {
    m_transfer.region = Region::Controller;
    m_transfer.address = static_cast<u16>(command >> 8);
    m_transfer.write = (command & 0x40000000) != 0;
  }
  else
  {
    m_transfer.region = Region::Adapter;
    m_transfer.address = static_cast<u16>(((command & ~u32{0xc000}) >> 8) & 0xff);
    m_transfer.write = (command & 0x4000) != 0;
  }

  DEBUG_LOG_FMT(SP1, "BBA {} {} at {:x}",
                m_transfer.region == Region::Controller ? "controller" : "adapter",
                m_transfer.write ? "write" : "read", m_transfer.address);

  if (m_transfer.region == Region::Controller && m_transfer.address == MX_IOB)
  {
    ERROR_LOG_FMT(SP1,
                  "Usage of BBA_IOB indicates that the rx packet descriptor has been corrupted.");
  }
}

void CEXIETHERNET::WriteAdapterRegister(u32 value)
{
  if (m_transfer.address == ADAPTER_IRQ_CAUSE)
  {
    // Writing 1 acknowledges a cause. Frames still waiting in the ring raise it again.
    m_irq_cause &= static_cast<u8>(value ^ 0xff);
    if (Reg16(MX_RRP) != Reg16(MX_RWP))
      RaiseInterrupt(IRQ_RECEIVE, false, CoreTiming::FromThread::CPU);
  }
  else if (m_transfer.address == ADAPTER_IRQ_MASK)
  {
    m_irq_mask = static_cast<u8>(value);
  }
  m_system.GetExpansionInterface().UpdateInterrupts();
}

void CEXIETHERNET::ImmWrite(u32 data, u32 size)
{
  // Right-align the bytes that were sent.
  data >>= (4 - size) * 8;

  if (!m_transfer.valid)
  {
    StartTransfer(data);
    return;
  }

  DEBUG_LOG_FMT(SP1, "BBA write {:x}", data);
  if (m_transfer.region == Region::Adapter)
    WriteAdapterRegister(data);
  else
    WriteController(data, size);
}

u32 CEXIETHERNET::ReadAdapterRegister(u32 size)
{
  u32 value = 0;
  switch (m_transfer.address)
  {
  case ADAPTER_EXI_ID:
    value = BBA_EXI_ID;
    break;
  case ADAPTER_REVISION:
    value = 0;
    break;
  case ADAPTER_DEVICE_ID:
    value = BBA_DEVICE_ID;
    break;
  case ADAPTER_AC_START:
    value = BBA_AC_START;
    break;
  case ADAPTER_IRQ_CAUSE:
    value = m_irq_cause;
    break;
  default:
    break;
  }
  m_transfer.address = static_cast<u16>(m_transfer.address + size);
  return value;
}

u32 CEXIETHERNET::ImmRead(u32 size)
{
  u32 value = 0;
  if (m_transfer.region == Region::Adapter)
  {
    value = ReadAdapterRegister(size);
  }
  else
  {
    // Controller memory is read big-endian, one byte per transferred byte.
    for (u32 index = 0; index < size; ++index)
    {
      value = (value << 8) | Reg(m_transfer.address);
      m_transfer.address = static_cast<u16>(m_transfer.address + 1);
    }
  }

  DEBUG_LOG_FMT(SP1, "BBA read of {} bytes: {:x}", size, value);
  return value << ((4 - size) * 8);
}

void CEXIETHERNET::DMAWrite(u32 addr, u32 size)
{
  // The only DMA the adapter takes is frame data for the transmit FIFO.
  const bool to_fifo = m_transfer.region == Region::Controller && m_transfer.write &&
                       m_transfer.address == MX_WRTXFIFOD;
  if (!to_fifo)
  {
    ERROR_LOG_FMT(SP1, "DMA write in {} {} mode - not implemented",
                  m_transfer.region == Region::Adapter ? "exi" : "mx",
                  m_transfer.write ? "write" : "read");
    return;
  }

  auto& memory = m_system.GetMemory();
  PushTransmitFifo(memory.GetPointerForRange(addr, size), size);
}

void CEXIETHERNET::DMARead(u32 addr, u32 size)
{
  // Reads come from controller memory (receive pages). The part past the end of it is left
  // alone.
  const u32 start = m_transfer.address;
  const u32 available = start < MEMORY_SIZE ? MEMORY_SIZE - start : 0;
  auto& memory = m_system.GetMemory();
  memory.CopyToEmu(addr, &m_mem[std::min(start, MEMORY_SIZE - 1)], std::min(size, available));
  m_transfer.address = static_cast<u16>(m_transfer.address + size);
}

void CEXIETHERNET::DoState(PointerWrap& p)
{
  p.DoArray(m_tx_fifo.get(), TX_FIFO_SIZE);
  p.DoArray(m_mem.get(), MEMORY_SIZE);
}

u32 CEXIETHERNET::WriteControlA(u32 value)
{
  if ((value & NCRA_RESET) != 0)
  {
    // A software reset is where games bring the network up.
    INFO_LOG_FMT(SP1, "Software reset");
    m_network_interface->Activate();
  }

  if (((Reg(MX_NCRA) ^ value) & NCRA_SR) != 0)
  {
    DEBUG_LOG_FMT(SP1, "{} rx", (value & NCRA_SR) ? "start" : "stop");
    if ((value & NCRA_SR) != 0)
      m_network_interface->RecvStart();
    else
      m_network_interface->RecvStop();
  }

  // A transmit can only start when none is running.
  if ((Reg(MX_NCRA) & (NCRA_ST0 | NCRA_ST1)) == 0)
  {
    if ((value & NCRA_ST0) != 0)
    {
      // Transmitting from the packet buffer (local DMA) is not supported.
      INFO_LOG_FMT(SP1, "start tx - local DMA");
      ERROR_LOG_FMT(SP1, "tx packet buffer not implemented.");
    }
    else if ((value & NCRA_ST1) != 0)
    {
      DEBUG_LOG_FMT(SP1, "start tx - direct FIFO");
      TransmitFifo();
      // The frame is gone by now, so the start bit does not stay set.
      value &= ~u32{NCRA_ST1};
    }
  }
  return value;
}

void CEXIETHERNET::WriteController(u32 data, u32 size)
{
  const u32 address = m_transfer.address;

  if (address == MX_WRTXFIFOD)
  {
    // FIFO data goes in as sent (most significant byte first); the address does not advance.
    std::array<u8, 4> bytes{};
    for (u32 index = 0; index < size; ++index)
      bytes[index] = static_cast<u8>(data >> (8 * (size - 1 - index)));
    PushTransmitFifo(bytes.data(), size);
    return;
  }

  if (address == MX_TXFIFOCNT || address == MX_TXFIFOCNT + 1)
  {
    // The FIFO count is read-only.
    m_transfer.address = static_cast<u16>(m_transfer.address + size);
    return;
  }

  if (address == MX_NCRA)
  {
    data = WriteControlA(data);
  }
  else if (address == MX_IR)
  {
    // Interrupt bits are cleared by writing; whatever is written, the register reads back 0.
    data = 0;
  }

  // Store the bytes big-endian from the current address on.
  for (u32 index = 0; index < size; ++index)
  {
    Reg(m_transfer.address) = static_cast<u8>(data >> (8 * (size - 1 - index)));
    m_transfer.address = static_cast<u16>(m_transfer.address + 1);
  }
}

void CEXIETHERNET::PushTransmitFifo(const u8* data, u32 size)
{
  // Direct FIFO mode: the controller builds the frame from the bytes as they arrive.
  const u16 count = Reg16(MX_TXFIFOCNT);
  if (data != nullptr && count < TX_FIFO_SIZE)
    std::memcpy(&m_tx_fifo[count], data, std::min<u32>(size, TX_FIFO_SIZE - count));

  // The count is kept to 12 bits; a real frame never comes close.
  SetReg16(MX_TXFIFOCNT, static_cast<u16>((count + size) & 0xfff));
}

void CEXIETHERNET::TransmitFifo()
{
  const u16 size = std::min<u16>(static_cast<u16>(TX_FIFO_SIZE), Reg16(MX_TXFIFOCNT));
  const u8* frame = m_tx_fifo.get();
  if (m_network_interface->SendFrame(frame, size))
    m_system.GetPowerPC().GetDebugInterface().NetworkLogger()->LogBBA(frame, size);
}

void CEXIETHERNET::SendComplete()
{
  Reg(MX_NCRA) &= static_cast<u8>(~(NCRA_ST0 | NCRA_ST1));
  SetReg16(MX_TXFIFOCNT, 0);
  RaiseInterrupt(IRQ_TRANSMIT, true, CoreTiming::FromThread::CPU);
  Reg(MX_LTPS) = 0;
}

bool CEXIETHERNET::AcceptsDestination() const
{
  const u8 filter = m_mem[MX_NCRB];
  if ((filter & NCRB_PR) != 0)
    return true;

  const u8* destination = m_rx_frame.get();
  if ((destination[0] & 1) == 0)
    return std::equal(destination, destination + 6, &m_mem[MX_PAR]);

  const bool broadcast = std::all_of(destination, destination + 6, [](u8 b) { return b == 0xff; });
  if (broadcast)
    return (filter & NCRB_AB) != 0;

  if ((filter & NCRB_PM) != 0)
    return true;

  const u32 hash = MulticastHash(destination);
  return (m_mem[MX_MAR + hash / 8] & (1u << (hash % 8))) != 0;
}

void CEXIETHERNET::ReceiveFrame(const u8* frame, u32 size)
{
  // An empty frame has no destination address to filter on; drop it.
  if (size == 0)
    return;
  std::memcpy(m_rx_frame.get(), frame, size);
  m_rx_length = size;
  StoreReceivedFrame();
}

// Runs on the backends' threads for every frame; keep it lean.
void CEXIETHERNET::StoreReceivedFrame()
{
  if (AcceptsDestination())
  {
    m_system.GetPowerPC().GetDebugInterface().NetworkLogger()->LogBBA(m_rx_frame.get(),
                                                                      m_rx_length);

    const u16 first_page = Reg16(MX_RWP);
    const u16 last_page = Reg16(MX_RHBP);
    const u16 ring_start = Reg16(MX_BP);
    const auto next_page = [&](u16 page) {
      return page == last_page ? ring_start : static_cast<u16>(page + 1);
    };

    // Copy the frame after the descriptor, page by page. Running into the page software has not
    // read yet stops the copy.
    u32 status = 0;
    u16 page = first_page;
    u32 offset = DESCRIPTOR_SIZE;
    for (u32 index = 0; index < m_rx_length; ++index)
    {
      Reg(page * PAGE_SIZE + offset) = m_rx_frame[index];
      if (++offset < PAGE_SIZE)
        continue;
      offset = 0;
      page = next_page(page);
      if (page == Reg16(MX_RRP))
      {
        status |= RX_FIFO_OVERRUN | RX_BUFFER_FULL;
        Reg(MX_IR) |= Reg(MX_IMR) & IRQ_RX_BUFFER_FULL;
        break;
      }
    }

    // The next frame starts on a fresh page.
    if ((m_rx_length + DESCRIPTOR_SIZE) % PAGE_SIZE != 0)
      page = next_page(page);

    if ((m_rx_frame[0] & 1) != 0)
      status |= RX_MULTICAST;

    if ((status & RX_BUFFER_FULL) != 0 && (Reg(MX_MISC2) & MISC2_AUTORCVR) == 0)
      ERROR_LOG_FMT(SP1, "RBF while AUTORCVR == 0!");

    const u32 descriptor = MakeDescriptor(page, DESCRIPTOR_SIZE + m_rx_length, status);
    for (u32 index = 0; index < DESCRIPTOR_SIZE; ++index)
      Reg(first_page * PAGE_SIZE + index) = static_cast<u8>(descriptor >> (8 * index));

    SetReg16(MX_RWP, page);
    Reg(MX_LRPS) = static_cast<u8>(status);

    if ((Reg(MX_IMR) & IRQ_RECEIVE) != 0)
      RaiseInterrupt(IRQ_RECEIVE, true, CoreTiming::FromThread::NON_CPU);
    else
      WARN_LOG_FMT(SP1, "NOT raising recv interrupt");
  }

  // Keep receiving while software has receiving enabled.
  if ((Reg(MX_NCRA) & NCRA_SR) != 0)
    m_network_interface->RecvStart();
}
}  // namespace ExpansionInterface
