// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Bluetooth/BTEmu.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/SysConf.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace IOS::HLE
{
namespace HCI = BluetoothHCI;
namespace Op = BluetoothHCI::Opcode;

namespace
{
// The SYSCONF BT.DINF section: the remotes paired with the console and the ones in each slot.
// Addresses are stored most significant byte first.
constexpr std::size_t DINF_MAX_REGISTERED = 10;
constexpr std::size_t DINF_NAME_SIZE = 0x40;

#pragma pack(push, 1)
struct DinfDevice
{
  u8 address[6];
  char name[DINF_NAME_SIZE];
};

struct DinfSection
{
  u8 num_registered;
  DinfDevice registered[DINF_MAX_REGISTERED];
  DinfDevice active[MAX_BBMOTES];
  DinfDevice unknown;
};
#pragma pack(pop)
static_assert(sizeof(DinfSection) == 1121);

void FillDinfDevice(DinfDevice* device, const bdaddr_t& address, std::string_view name)
{
  for (std::size_t i = 0; i < address.size(); ++i)
    device->address[i] = address[address.size() - 1 - i];
  std::memcpy(device->name, name.data(), std::min(name.size(), DINF_NAME_SIZE));
}

// Builds one event in a queue entry. The parameter length is filled in by Finish.
class EventWriter
{
public:
  explicit EventWriter(u8 code, u16 connection_handle = 0)
  {
    m_event.connection_handle = connection_handle;
    m_event.buffer[0] = code;
  }

  EventWriter& U8(u8 value)
  {
    Put(&value, 1);
    return *this;
  }
  EventWriter& U16(u16 value)
  {
    const u8 bytes[2] = {static_cast<u8>(value), static_cast<u8>(value >> 8)};
    Put(bytes, sizeof(bytes));
    return *this;
  }
  EventWriter& Bytes(std::span<const u8> bytes)
  {
    Put(bytes.data(), bytes.size());
    return *this;
  }

  // Skips bytes that stay zero.
  EventWriter& Zeros(std::size_t count)
  {
    m_length += std::min(count, sizeof(m_event.buffer) - m_length);
    return *this;
  }

  u8* Cursor() { return m_event.buffer + m_length; }

  const SQueuedEvent& Finish()
  {
    m_event.size = static_cast<u32>(m_length);
    m_event.buffer[1] = static_cast<u8>(m_length - 2);
    return m_event;
  }

private:
  void Put(const u8* bytes, std::size_t count)
  {
    const std::size_t room = sizeof(m_event.buffer) - m_length;
    const std::size_t n = std::min(count, room);
    std::memcpy(m_event.buffer + m_length, bytes, n);
    m_length += n;
  }

  SQueuedEvent m_event;
  std::size_t m_length = 2;
};

// The parameters of an HCI command, copied out of emulated memory.
class CommandParams
{
public:
  CommandParams(const Memory::MemoryManager& memory, u32 address, std::size_t size)
      : m_size(std::min(size, m_bytes.size()))
  {
    memory.CopyFromEmu(m_bytes.data(), address, m_size);
  }

  u8 U8(std::size_t offset) const { return m_bytes[offset]; }
  u16 U16(std::size_t offset) const
  {
    return static_cast<u16>(m_bytes[offset] | (m_bytes[offset + 1] << 8));
  }
  bdaddr_t Address(std::size_t offset) const
  {
    bdaddr_t address;
    std::copy_n(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), address.size(),
                address.begin());
    return address;
  }

private:
  std::array<u8, HCI::NAME_SIZE> m_bytes{};
  std::size_t m_size;
};

std::string FormatAddress(const bdaddr_t& address)
{
  return fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", address[0], address[1],
                     address[2], address[3], address[4], address[5]);
}

// The wire bytes of a packed structure.
template <typename T>
std::span<const u8> AsBytes(const T& value)
{
  return {reinterpret_cast<const u8*>(&value), sizeof(T)};
}

// Return parameters that are a single success status.
constexpr std::array<u8, 1> STATUS_OK{0x00};
}  // namespace

SQueuedEvent::SQueuedEvent(u32 size_, u16 handle) : size(size_), connection_handle(handle)
{
  if (size > sizeof(buffer))
    PanicAlertFmt("SQueuedEvent: The size is too large.");
}

BluetoothEmuDevice::BluetoothEmuDevice(EmulationKernel& ios, const std::string& device_name)
    : BluetoothBaseDevice(ios, device_name)
{
  SysConf sysconf{ios.GetFS()};
  // The user's own pairings are kept in a backup and restored when emulation ends.
  if (!Core::WantsDeterminism())
    BackUpBTInfoSection(&sysconf);

  DinfSection dinf{};
  dinf.num_registered = MAX_BBMOTES;

  for (u8 slot = 0; slot < MAX_BBMOTES; ++slot)
  {
    // The last address byte is the slot: connection handles and remote numbers rely on it.
    const bdaddr_t address{0x11, 0x02, 0x19, 0x79, 0x00, slot};
    const std::string_view name{slot == WIIMOTE_BALANCE_BOARD ? "Nintendo RVL-WBC-01" :
                                                                "Nintendo RVL-CNT-01"};
    FillDinfDevice(&dinf.registered[slot], address, name);
    FillDinfDevice(&dinf.active[slot], address, name);

    DEBUG_LOG_FMT(IOS_WIIMOTE, "Remote slot {} has address {}", slot, FormatAddress(address));

    const unsigned int source =
        NetPlay::IsNetPlayRunning() ? NetPlay::NetPlay_GetLocalWiimoteForSlot(slot) : slot;
    m_wiimotes[slot] = std::make_unique<WiimoteDevice>(this, address, source);
  }

  // Games read the list of remotes from SYSCONF, so it has to be written right away.
  auto& bytes = sysconf.GetOrAddEntry("BT.DINF", SysConf::Entry::Type::BigArray)->bytes;
  bytes.resize(sizeof(dinf));
  std::memcpy(bytes.data(), &dinf, sizeof(dinf));
  if (!sysconf.Save())
    PanicAlertFmtT("Failed to write BT.DINF to SYSCONF");
}

BluetoothEmuDevice::~BluetoothEmuDevice() = default;

void BluetoothEmuDevice::DoState(PointerWrap& p)
{
  // Marks the state as one of emulated Bluetooth; passthrough states store true here.
  bool is_passthrough_state = false;
  p.Do(is_passthrough_state);
  if (is_passthrough_state && p.IsReadMode())
  {
    Core::DisplayMessage("State needs Bluetooth passthrough to be enabled. Aborting load.", 4000);
    p.SetVerifyMode();
    return;
  }

  // The order below is part of the savestate format.
  Device::DoState(p);
  p.Do(m_controller_bd);
  // The game's pending HCI event and ACL reads.
  DoStateForMessage(GetEmulationKernel(), p, m_hci_endpoint);
  DoStateForMessage(GetEmulationKernel(), p, m_acl_endpoint);
  p.Do(m_last_ticks);
  p.DoArray(m_packet_count);
  const auto save = [&p](auto&... fields) { (p.Do(fields), ...); };
  save(m_scan_enable, m_event_queue, m_pending_acl);
  for (auto& wiimote : m_wiimotes)
    wiimote->DoState(p);
}

bool BluetoothEmuDevice::RemoteConnect(WiimoteDevice& wiimote)
{
  if ((m_scan_enable & HCI::SCAN_PAGE) == 0)
    return false;
  SendConnectionRequest(wiimote);
  return true;
}

bool BluetoothEmuDevice::RemoteDisconnect(const bdaddr_t& address)
{
  // 0x13: the remote user terminated the connection.
  return SendDisconnectionComplete(GetConnectionHandle(address), 0x13);
}

std::optional<IPCReply> BluetoothEmuDevice::Close(u32 fd)
{
  m_scan_enable = 0;
  m_last_ticks = 0;
  std::ranges::fill(m_packet_count, 0u);
  m_hci_endpoint.reset();
  m_acl_endpoint.reset();
  return Device::Close(fd);
}

std::optional<IPCReply> BluetoothEmuDevice::IOCtlV(const IOCtlVRequest& request)
{
  auto& ios = GetEmulationKernel();

  if (request.request == USB::IOCTLV_USBV0_CTRLMSG)
  {
    // An HCI command. It is answered once its events are queued.
    ExecuteHCICommandMessage(USB::V0CtrlMessage(ios, request));
    return std::nullopt;
  }

  if (request.request == USB::IOCTLV_USBV0_BLKMSG)
  {
    const USB::V0BulkMessage message{ios, request};
    if (message.endpoint == ACL_DATA_OUT)
    {
      HandleOutgoingACL(message);
      return IPCReply(IPC_SUCCESS);
    }
    if (message.endpoint == ACL_DATA_IN)
    {
      // A buffer for ACL data from the remotes. It is answered when there is some.
      m_acl_endpoint = std::make_unique<USB::V0BulkMessage>(ios, request);
      DEBUG_LOG_FMT(IOS_WIIMOTE, "ACL buffer at {:#010x}", request.address);
      return std::nullopt;
    }
    DEBUG_ASSERT_MSG(IOS_WIIMOTE, 0, "Bulk message on unknown endpoint {:#x}", message.endpoint);
    return IPCReply(IPC_SUCCESS);
  }

  if (request.request == USB::IOCTLV_USBV0_INTRMSG)
  {
    const USB::V0IntrMessage message{ios, request};
    if (message.endpoint == HCI_EVENT)
    {
      // A buffer for the next event.
      m_hci_endpoint = std::make_unique<USB::V0IntrMessage>(ios, request);
      DEBUG_LOG_FMT(IOS_WIIMOTE, "Event buffer at {:#010x}", request.address);
      return std::nullopt;
    }
    DEBUG_ASSERT_MSG(IOS_WIIMOTE, 0, "Interrupt message on unknown endpoint {:#x}",
                     message.endpoint);
    return IPCReply(IPC_SUCCESS);
  }

  request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_WIIMOTE);
  return IPCReply(IPC_SUCCESS);
}

// ACL data from the software to a remote.
void BluetoothEmuDevice::HandleOutgoingACL(const USB::V0BulkMessage& message)
{
  auto& memory = GetSystem().GetMemory();

  std::array<u8, sizeof(HCI::ACLHeader)> header{};
  memory.CopyFromEmu(header.data(), message.data_address, header.size());
  const u16 handle_field = static_cast<u16>(header[0] | (header[1] << 8));
  const u16 length = static_cast<u16>(header[2] | (header[3] << 8));

  // Nintendo's stack never fragments and never broadcasts.
  DEBUG_ASSERT(HCI::ACLBroadcastFlag(handle_field) == HCI::ACL_POINT_TO_POINT);
  DEBUG_ASSERT(HCI::ACLBoundaryFlag(handle_field) == HCI::ACL_FIRST_FLUSHABLE);

  const u16 connection_handle = HCI::ACLConnectionHandle(handle_field);
  u8* const payload = memory.GetPointerForRange(message.data_address + 4, length);

  WiimoteDevice* const wiimote = AccessWiimote(connection_handle);
  if (wiimote == nullptr || payload == nullptr)
    return;

  DEBUG_LOG_FMT(IOS_WIIMOTE, "ACL data for connection {:#06x}", connection_handle);
  ++m_packet_count[GetWiimoteNumberFromConnectionHandle(connection_handle)];
  wiimote->ExecuteL2capCmd(payload, length);
}

void BluetoothEmuDevice::WriteACL(const USB::V0BulkMessage& endpoint, u16 connection_handle,
                                  const u8* data, u16 size)
{
  const u16 handle_field =
      HCI::MakeACLHandle(connection_handle, HCI::ACL_FIRST_FLUSHABLE, HCI::ACL_POINT_TO_POINT);
  const u8 header[4] = {static_cast<u8>(handle_field), static_cast<u8>(handle_field >> 8),
                        static_cast<u8>(size), static_cast<u8>(size >> 8)};

  auto& memory = GetSystem().GetMemory();
  memory.CopyToEmu(endpoint.data_address, header, sizeof(header));
  memory.CopyToEmu(endpoint.data_address + sizeof(header), data, size);
}

void BluetoothEmuDevice::QueueACL(const u8* data, u16 size, u16 connection_handle)
{
  // Busy exchanges can pile up packets; past 100 they are thrown away.
  if (m_pending_acl.size() >= 100)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Too many pending ACL packets; dropping one");
    return;
  }
  if (size > ACL_PKT_SIZE)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "ACL packet of {} bytes does not fit a buffer; dropping it", size);
    return;
  }

  PendingACL& packet = m_pending_acl.emplace_back(PendingACL{});
  std::copy_n(data, size, packet.data);
  packet.size = size;
  packet.conn_handle = connection_handle;
}

void BluetoothEmuDevice::SendACLPacket(const bdaddr_t& source, const u8* data, u32 size)
{
  const u16 connection_handle = GetConnectionHandle(source);

  // Data goes straight into the ACL buffer only when no event is waiting to be delivered and
  // the software is not waiting for one either; otherwise it is kept for a later update.
  if (m_acl_endpoint == nullptr || m_hci_endpoint != nullptr || !m_event_queue.empty())
  {
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Keeping ACL data for connection {:#06x}", connection_handle);
    QueueACL(data, static_cast<u16>(size), connection_handle);
    return;
  }

  WriteACL(*m_acl_endpoint, connection_handle, data, static_cast<u16>(size));
  GetEmulationKernel().EnqueueIPCReply(m_acl_endpoint->ios_request,
                                       static_cast<s32>(sizeof(HCI::ACLHeader) + size));
  m_acl_endpoint.reset();
}

// Hands an event to the event buffer the software gave us, which is then used up.
void BluetoothEmuDevice::DeliverEvent(const SQueuedEvent& event)
{
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Event {:#04x} to {:#010x}", event.buffer[0],
                m_hci_endpoint->ios_request.address);
  m_hci_endpoint->FillBuffer(event.buffer, event.size);
  GetEmulationKernel().EnqueueIPCReply(m_hci_endpoint->ios_request, static_cast<s32>(event.size));
  m_hci_endpoint.reset();
}

void BluetoothEmuDevice::AddEventToQueue(const SQueuedEvent& event)
{
  if (m_hci_endpoint == nullptr)
  {
    m_event_queue.push_back(event);
    return;
  }

  if (m_event_queue.empty())
  {
    DeliverEvent(event);
    return;
  }

  // Earlier events go first.
  m_event_queue.push_back(event);
  DeliverEvent(m_event_queue.front());
  m_event_queue.pop_front();
}

void BluetoothEmuDevice::Update()
{
  if (m_hci_endpoint != nullptr && !m_event_queue.empty())
  {
    DeliverEvent(m_event_queue.front());
    m_event_queue.pop_front();
  }

  if (m_acl_endpoint != nullptr && m_event_queue.empty() && !m_pending_acl.empty())
  {
    const PendingACL& packet = m_pending_acl.front();
    WriteACL(*m_acl_endpoint, packet.conn_handle, packet.data, packet.size);
    const s32 reply_size = static_cast<s32>(sizeof(HCI::ACLHeader) + packet.size);
    m_pending_acl.pop_front();
    GetEmulationKernel().EnqueueIPCReply(m_acl_endpoint->ios_request, reply_size);
    m_acl_endpoint.reset();
  }

  for (auto& wiimote : m_wiimotes)
    wiimote->Update();

  auto& system = GetSystem();
  const u64 ticks_per_report = system.GetSystemTimers().GetTicksPerSecond() / Wiimote::UPDATE_FREQ;
  const u64 now = system.GetCoreTiming().GetTicks();
  if (now - m_last_ticks > ticks_per_report)
    UpdateInputs(now);

  SendCompletedPackets();
}

// Reads the input of every remote once per report period.
void BluetoothEmuDevice::UpdateInputs(u64 now)
{
  auto& system = GetSystem();

  // Throttling first keeps the input as fresh as possible.
  system.GetCoreTiming().Throttle(static_cast<s64>(now));
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Bluetooth);
  g_controller_interface.UpdateInput();

  std::array<WiimoteEmu::DesiredWiimoteState, MAX_BBMOTES> states;
  std::array<WiimoteDevice::NextUpdateInputCall, MAX_BBMOTES> calls;
  for (std::size_t slot = 0; slot < MAX_BBMOTES; ++slot)
    calls[slot] = m_wiimotes[slot]->PrepareInput(&states[slot]);

  const auto wants_input = [&](std::size_t slot) {
    return calls[slot] != WiimoteDevice::NextUpdateInputCall::None;
  };

  // Netplay swaps the states of the four remotes (not the balance board) for the shared ones.
  if (NetPlay::IsNetPlayRunning())
  {
    std::array<WiimoteEmu::SerializedWiimoteState, MAX_WIIMOTES> serialized;
    std::array<NetPlay::NetPlayClient::WiimoteDataBatchEntry, MAX_WIIMOTES> entries;
    std::size_t entry_count = 0;
    for (std::size_t slot = 0; slot < MAX_WIIMOTES; ++slot)
    {
      if (!wants_input(slot))
        continue;
      serialized[slot] = WiimoteEmu::SerializeDesiredState(states[slot]);
      entries[entry_count].wiimote = static_cast<int>(slot);
      entries[entry_count].state = &serialized[slot];
      ++entry_count;
    }

    if (entry_count != 0)
    {
      NetPlay::NetPlay_GetWiimoteData(
          std::span<NetPlay::NetPlayClient::WiimoteDataBatchEntry>(entries.data(), entry_count));
      for (std::size_t i = 0; i < entry_count; ++i)
      {
        const auto slot = static_cast<std::size_t>(entries[i].wiimote);
        if (!WiimoteEmu::DeserializeDesiredState(&states[slot], serialized[slot]))
          PanicAlertFmtT("Received invalid Wii Remote data from Netplay.");
      }
    }
  }

  // Movies record and replay the four remotes.
  auto& movie = system.GetMovie();
  for (int slot = 0; slot < MAX_WIIMOTES; ++slot)
  {
    const auto index = static_cast<std::size_t>(slot);
    if (!wants_input(index))
      continue;
    movie.PlayWiimote(slot, &states[index]);
    movie.CheckWiimoteStatus(slot, states[index]);
  }

  for (std::size_t slot = 0; slot < MAX_BBMOTES; ++slot)
    m_wiimotes[slot]->UpdateInput(calls[slot], states[slot]);

  m_last_ticks = now;
}

void BluetoothEmuDevice::SendCommandStatus(u16 opcode)
{
  EventWriter event{HCI::Event::COMMAND_STATUS};
  event.U8(0x00).U8(1).U16(opcode);
  INFO_LOG_FMT(IOS_WIIMOTE, "Command status for {:#06x}", opcode);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendCommandComplete(u16 opcode, std::span<const u8> return_params)
{
  DEBUG_ASSERT(return_params.size() + 3 < 256);
  EventWriter event{HCI::Event::COMMAND_COMPLETE};
  event.U8(1).U16(opcode).Bytes(return_params);
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Command complete for {:#06x}", opcode);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendInquiryComplete(u8 num_responses)
{
  EventWriter event{HCI::Event::INQUIRY_COMPLETE};
  event.U8(0x00).U8(num_responses);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendInquiryResult()
{
  // Only the first discoverable remote answers an inquiry. Software asks players to press 1+2
  // in turn, and each later inquiry then picks up the next remote in that order.
  const auto found = std::ranges::find_if(m_wiimotes, &WiimoteDevice::IsInquiryScanEnabled);
  if (found == m_wiimotes.end())
  {
    SendInquiryComplete(0);
    return;
  }

  const WiimoteDevice& wiimote = **found;
  EventWriter event{HCI::Event::INQUIRY_RESULT};
  event.U8(1);
  event.Bytes(wiimote.GetBD());
  // Page scan repetition mode R1, period mode P0, mandatory page scan mode.
  event.U8(1).U8(0).U8(0);
  event.Bytes(wiimote.GetClass());
  event.U16(0x3818);  // clock offset
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Inquiry result: {}", FormatAddress(wiimote.GetBD()));
  AddEventToQueue(event.Finish());

  SendInquiryComplete(1);
}

void BluetoothEmuDevice::SendConnectionComplete(const bdaddr_t& bd, u8 status)
{
  EventWriter event{HCI::Event::CONNECTION_COMPLETE};
  event.U8(status).U16(GetConnectionHandle(bd)).Bytes(bd);
  // An ACL link without encryption.
  event.U8(HCI::LINK_TYPE_ACL).U8(0x00);
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Connection complete: {} status {:#04x}", FormatAddress(bd), status);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendConnectionRequest(const WiimoteDevice& wiimote)
{
  EventWriter event{HCI::Event::CONNECTION_REQUEST};
  event.Bytes(wiimote.GetBD()).Bytes(wiimote.GetClass()).U8(HCI::LINK_TYPE_ACL);
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Connection request: {}", FormatAddress(wiimote.GetBD()));
  AddEventToQueue(event.Finish());
}

bool BluetoothEmuDevice::SendDisconnectionComplete(u16 connection_handle, u8 reason)
{
  if (AccessWiimote(connection_handle) == nullptr)
    return false;

  EventWriter event{HCI::Event::DISCONNECTION_COMPLETE, connection_handle};
  event.U8(0x00).U16(connection_handle).U8(reason);
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Disconnection of {:#06x}, reason {:#04x}", connection_handle,
                reason);
  AddEventToQueue(event.Finish());
  return true;
}

void BluetoothEmuDevice::SendAuthenticationComplete(u16 connection_handle)
{
  if (AccessWiimote(connection_handle) == nullptr)
    return;

  EventWriter event{HCI::Event::AUTHENTICATION_COMPLETE, connection_handle};
  event.U8(0x00).U16(connection_handle);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendRemoteName(const bdaddr_t& bd)
{
  const WiimoteDevice* wiimote = AccessWiimote(bd);
  if (wiimote == nullptr)
    return;

  EventWriter event{HCI::Event::REMOTE_NAME_REQUEST_COMPLETE};
  event.U8(0x00).Bytes(bd);
  // The name field is NUL-padded to its full size.
  const std::string_view name{wiimote->GetName()};
  const std::size_t name_length = std::min(name.size(), HCI::NAME_SIZE - 1);
  std::memcpy(event.Cursor(), name.data(), name_length);
  event.Zeros(HCI::NAME_SIZE);
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Remote name of {}: {}", FormatAddress(bd), name);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendRemoteFeatures(u16 connection_handle)
{
  const WiimoteDevice* wiimote = AccessWiimote(connection_handle);
  if (wiimote == nullptr)
    return;

  EventWriter event{HCI::Event::READ_REMOTE_FEATURES_COMPLETE, connection_handle};
  event.U8(0x00).U16(connection_handle).Bytes(wiimote->GetFeatures());
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendRemoteVersion(u16 connection_handle)
{
  const WiimoteDevice* wiimote = AccessWiimote(connection_handle);
  if (wiimote == nullptr)
    return;

  EventWriter event{HCI::Event::READ_REMOTE_VERSION_COMPLETE, connection_handle};
  event.U8(0x00).U16(connection_handle);
  event.U8(wiimote->GetLMPVersion()).U16(wiimote->GetManufactorID());
  event.U16(wiimote->GetLMPSubVersion());
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendRoleChange(const bdaddr_t& bd, bool master)
{
  if (AccessWiimote(bd) == nullptr)
    return;

  EventWriter event{HCI::Event::ROLE_CHANGE};
  event.U8(0x00).Bytes(bd).U8(master ? 0x00 : 0x01);
  AddEventToQueue(event.Finish());
}

// Tells the software how many ACL packets each remote consumed, which frees its buffers.
void BluetoothEmuDevice::SendCompletedPackets()
{
  EventWriter event{HCI::Event::NUMBER_OF_COMPLETED_PACKETS};
  event.U8(static_cast<u8>(m_wiimotes.size()));

  u32 total = 0;
  for (std::size_t slot = 0; slot < m_wiimotes.size(); ++slot)
  {
    const u16 count = static_cast<u16>(m_packet_count[slot]);
    event.U16(GetConnectionHandle(m_wiimotes[slot]->GetBD())).U16(count);
    total += count;
    m_packet_count[slot] = 0;
  }

  // Nothing to report: no event.
  if (total != 0)
    AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendModeChange(u16 connection_handle, u8 mode, u16 interval)
{
  if (AccessWiimote(connection_handle) == nullptr)
    return;

  EventWriter event{HCI::Event::MODE_CHANGE, connection_handle};
  event.U8(0x00).U16(connection_handle).U8(mode).U16(interval);
  AddEventToQueue(event.Finish());
}

// Return Link Keys: the stored keys of the first `count` remotes.
void BluetoothEmuDevice::SendReturnLinkKeys(u8 count)
{
  EventWriter event{HCI::Event::RETURN_LINK_KEYS};
  event.U8(count);
  for (std::size_t slot = 0; slot < count; ++slot)
  {
    event.Bytes(m_wiimotes[slot]->GetBD()).Bytes(m_wiimotes[slot]->GetLinkKey());
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Link key of {}", FormatAddress(m_wiimotes[slot]->GetBD()));
  }
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendClockOffset(u16 connection_handle)
{
  if (AccessWiimote(connection_handle) == nullptr)
    return;

  EventWriter event{HCI::Event::READ_CLOCK_OFFSET_COMPLETE, connection_handle};
  event.U8(0x00).U16(connection_handle).U16(0x3818);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::SendPacketTypeChanged(u16 connection_handle, u16 packet_type)
{
  if (AccessWiimote(connection_handle) == nullptr)
    return;

  EventWriter event{HCI::Event::PACKET_TYPE_CHANGED, connection_handle};
  event.U8(0x00).U16(connection_handle).U16(packet_type);
  AddEventToQueue(event.Finish());
}

void BluetoothEmuDevice::ExecuteHCICommandMessage(const USB::V0CtrlMessage& ctrl_message)
{
  auto& memory = GetSystem().GetMemory();

  std::array<u8, sizeof(HCI::CommandHeader)> header{};
  memory.CopyFromEmu(header.data(), ctrl_message.data_address, header.size());
  const u16 opcode = static_cast<u16>(header[0] | (header[1] << 8));
  const u8 param_length = header[2];
  const u32 params_address = ctrl_message.data_address + static_cast<u32>(header.size());

  // Copies the first `size` parameter bytes.
  const auto params = [&](std::size_t size) {
    return CommandParams{memory, params_address, size};
  };

  DEBUG_LOG_FMT(IOS_WIIMOTE, "HCI command {:#06x} (group {:#04x}, command {:#05x})", opcode,
                HCI::OpcodeGroup(opcode), HCI::OpcodeCommand(opcode));

  switch (opcode)
  {
  // Controller settings that only need a success status. Their parameters are only logged.
  case Op::RESET:
    // Connections are not torn down; Nintendo's stack only resets during setup.
    SendCommandComplete(opcode, STATUS_OK);
    break;

  case Op::WRITE_CLASS_OF_DEVICE:
  {
    const auto p = params(3);
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Class of device {:02x}{:02x}{:02x}", p.U8(2), p.U8(1), p.U8(0));
    SendCommandComplete(opcode, STATUS_OK);
    break;
  }

  case Op::WRITE_LOCAL_NAME:
    params(HCI::NAME_SIZE);
    SendCommandComplete(opcode, STATUS_OK);
    break;

  case Op::WRITE_PIN_TYPE:
  case Op::WRITE_INQUIRY_SCAN_TYPE:
  case Op::WRITE_INQUIRY_MODE:
  case Op::WRITE_PAGE_SCAN_TYPE:
  {
    // A one-byte setting. Inquiries are always answered with standard results, whatever mode
    // is asked for.
    const auto p = params(1);
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Setting {:#06x} = {}", opcode, p.U8(0));
    SendCommandComplete(opcode, STATUS_OK);
    break;
  }

  case Op::WRITE_PAGE_TIMEOUT:
  {
    const auto p = params(2);
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Page timeout {}", p.U16(0));
    SendCommandComplete(opcode, STATUS_OK);
    break;
  }

  case Op::HOST_BUFFER_SIZE:
  {
    const auto p = params(7);
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Host buffers: ACL {} x {}, SCO {} x {}", p.U16(3), p.U16(0),
                  p.U16(5), p.U8(2));
    SendCommandComplete(opcode, STATUS_OK);
    break;
  }

  case Op::SET_EVENT_FILTER:
  {
    // Software only sets an inquiry filter for new devices. Inquiries are faked anyway, and
    // controllers may leave that filter out.
    const auto p = params(2);
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Event filter type {} condition {}", p.U8(0), p.U8(1));
    SendCommandComplete(opcode, STATUS_OK);
    break;
  }

  case Op::WRITE_SCAN_ENABLE:
  {
    const auto p = params(1);
    m_scan_enable = p.U8(0);
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Scan enable {:#04x}", m_scan_enable);
    SendCommandComplete(opcode, STATUS_OK);
    break;
  }

  case Op::WRITE_LINK_SUPERVISION_TIMEOUT:
  {
    // A timeout of 0 disables it. Connections never time out here anyway.
    const auto p = params(4);
    const u16 handle = p.U16(0);
    const std::array<u8, 3> reply{0x00, static_cast<u8>(handle), static_cast<u8>(handle >> 8)};
    SendCommandComplete(opcode, reply);
    break;
  }

  // Informational commands: the answers of the Wii's own module.
  case Op::READ_BUFFER_SIZE:
  {
    // The stack must never be told about more than 10 ACL buffers (its counter is a u8).
    HCI::ReadBufferSizeReply reply{};
    reply.acl_packet_length = ACL_PKT_SIZE;
    reply.sco_packet_length = SCO_PKT_SIZE;
    reply.acl_packet_count = ACL_PKT_NUM;
    reply.sco_packet_count = SCO_PKT_NUM;
    SendCommandComplete(opcode, AsBytes(reply));
    break;
  }

  case Op::READ_LOCAL_VERSION:
  {
    // HCI and LMP 1.1.
    HCI::ReadLocalVersionReply reply{};
    reply.hci_version = 0x03;
    reply.hci_revision = 0x40a7;
    reply.lmp_version = 0x03;
    reply.manufacturer = 0x000f;
    reply.lmp_subversion = 0x430e;
    SendCommandComplete(opcode, AsBytes(reply));
    break;
  }

  case Op::READ_LOCAL_FEATURES:
  {
    static constexpr std::array<u8, 1 + HCI::FEATURES_SIZE> reply{0x00, 0xff, 0xff, 0x8d, 0xfe,
                                                                  0x9b, 0xf9, 0x00, 0x80};
    SendCommandComplete(opcode, reply);
    break;
  }

  case Op::READ_BD_ADDR:
  {
    std::array<u8, 7> reply{};
    std::ranges::copy(m_controller_bd, reply.begin() + 1);
    SendCommandComplete(opcode, reply);
    break;
  }

  // Link keys.
  case Op::READ_STORED_LINK_KEY:
  {
    const auto p = params(7);
    // Every remote has a key; only "read all" is answered with them.
    u8 count = 0;
    if (p.U8(6) == 1)
      count = static_cast<u8>(m_wiimotes.size());
    else
      ERROR_LOG_FMT(IOS_WIIMOTE, "Read Stored Link Key for one device is not supported");

    SendReturnLinkKeys(count);
    // Status, then the maximum number of keys (255) and the number read.
    const std::array<u8, 5> reply{0x00, 0xff, 0x00, count, 0x00};
    SendCommandComplete(opcode, reply);
    break;
  }

  case Op::DELETE_STORED_LINK_KEY:
  {
    const auto p = params(sizeof(HCI::DeleteStoredLinkKeyParams));
    if (AccessWiimote(p.Address(0)) == nullptr)
      break;
    // Nothing is deleted.
    const std::array<u8, 3> reply{0x00, 0x00, 0x00};
    SendCommandComplete(opcode, reply);
    ERROR_LOG_FMT(IOS_WIIMOTE, "Software deleted a stored link key; pairing probably failed "
                               "(is SYSCONF loaded correctly?)");
    break;
  }

  case Op::LINK_KEY_REPLY:
  case Op::LINK_KEY_NEGATIVE_REPLY:
  {
    const auto p = params(opcode == Op::LINK_KEY_REPLY ? 6 + HCI::LINK_KEY_SIZE : 6);
    std::array<u8, 7> reply{};
    const bdaddr_t address = p.Address(0);
    std::ranges::copy(address, reply.begin() + 1);
    SendCommandComplete(opcode, reply);
    break;
  }

  // Baseband commands answered by the remotes.
  case Op::INQUIRY:
    params(5);
    SendCommandStatus(opcode);
    SendInquiryResult();
    break;

  case Op::INQUIRY_CANCEL:
    SendCommandComplete(opcode, STATUS_OK);
    break;

  case Op::REMOTE_NAME_REQUEST:
  {
    const auto p = params(10);
    SendCommandStatus(opcode);
    SendRemoteName(p.Address(0));
    break;
  }

  case Op::CREATE_CONNECTION:
  {
    const auto p = params(13);
    const bdaddr_t address = p.Address(0);
    SendCommandStatus(opcode);
    WiimoteDevice* const wiimote = AccessWiimote(address);
    const bool accepted = wiimote != nullptr && wiimote->EventConnectionRequest();
    // 0x08: connection timeout.
    SendConnectionComplete(address, accepted ? 0x00 : 0x08);
    break;
  }

  case Op::ACCEPT_CONNECTION:
  {
    const auto p = params(7);
    const bdaddr_t address = p.Address(0);
    const u8 role = p.U8(6);
    SendCommandStatus(opcode);
    WiimoteDevice* const wiimote = AccessWiimote(address);
    if (wiimote == nullptr || !wiimote->EventConnectionAccept())
    {
      // The remote gave up on this connection.
      SendConnectionComplete(address, 0x08);
      break;
    }
    // Role 0: the host wants to be master, so the controller switches roles first.
    if (role == 0)
      SendRoleChange(address, true);
    SendConnectionComplete(address, 0x00);
    break;
  }

  case Op::DISCONNECT:
  {
    const auto p = params(3);
    const u16 handle = p.U16(0);
    const u8 reason = p.U8(2);
    SendCommandStatus(opcode);
    SendDisconnectionComplete(handle, reason);
    if (WiimoteDevice* const wiimote = AccessWiimote(handle))
      wiimote->EventDisconnect(reason);
    break;
  }

  case Op::CHANGE_PACKET_TYPE:
  {
    // Nintendo's stack asks for DM1/DH1/DM5/DH5 (0xcc18); the packet type is simply accepted.
    const auto p = params(4);
    SendCommandStatus(opcode);
    SendPacketTypeChanged(p.U16(0), p.U16(2));
    break;
  }

  case Op::AUTHENTICATION_REQUESTED:
  {
    const auto p = params(2);
    SendCommandStatus(opcode);
    SendAuthenticationComplete(p.U16(0));
    break;
  }

  case Op::READ_REMOTE_FEATURES:
  {
    const auto p = params(2);
    SendCommandStatus(opcode);
    SendRemoteFeatures(p.U16(0));
    break;
  }

  case Op::READ_REMOTE_VERSION:
  {
    const auto p = params(2);
    SendCommandStatus(opcode);
    SendRemoteVersion(p.U16(0));
    break;
  }

  case Op::READ_CLOCK_OFFSET:
  {
    const auto p = params(2);
    SendCommandStatus(opcode);
    SendClockOffset(p.U16(0));
    break;
  }

  case Op::SNIFF_MODE:
  {
    // The remote goes into sniff mode (0x02) at the longest interval allowed.
    const auto p = params(10);
    SendCommandStatus(opcode);
    SendModeChange(p.U16(0), 0x02, p.U16(2));
    break;
  }

  case Op::WRITE_LINK_POLICY:
    params(4);
    SendCommandStatus(opcode);
    break;

  // Broadcom patch commands the stack sends while starting up (WUDiRemovePatch and friends).
  case Op::VENDOR_FC4C:
  case Op::VENDOR_FC4F:
  {
    const u32 size = ctrl_message.length - 3;
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Vendor command {:#06x}, {:#x} bytes", opcode, size);
    Dolphin_Debugger::PrintDataBuffer(GetSystem(), Common::Log::LogType::IOS_WIIMOTE,
                                      params_address, size, "Data: ");
    SendCommandComplete(opcode, STATUS_OK);
    break;
  }

  default:
    // Anything else is acknowledged without return parameters.
    SendCommandComplete(opcode, {});
    if (HCI::OpcodeGroup(opcode) == HCI::Group::VENDOR)
    {
      std::string bytes;
      for (u32 i = 0; i < param_length; ++i)
        bytes += fmt::format(" {:02x}", memory.Read_U8(params_address + i));
      ERROR_LOG_FMT(IOS_WIIMOTE, "Unknown vendor command {:#06x}:{}", opcode, bytes);
    }
    else
    {
      DEBUG_ASSERT_MSG(IOS_WIIMOTE, 0, "Unknown HCI command {:#06x}", opcode);
    }
    break;
  }

  GetEmulationKernel().EnqueueIPCReply(ctrl_message.ios_request,
                                       static_cast<s32>(ctrl_message.length));
}

WiimoteDevice* BluetoothEmuDevice::AccessWiimoteByIndex(std::size_t index)
{
  return index < m_wiimotes.size() ? m_wiimotes[index].get() : nullptr;
}

u16 BluetoothEmuDevice::GetConnectionHandle(const bdaddr_t& address)
{
  return static_cast<u16>(0x100 + address.back());
}

u32 BluetoothEmuDevice::GetWiimoteNumberFromConnectionHandle(u16 connection_handle)
{
  return connection_handle & 0xffu;
}

WiimoteDevice* BluetoothEmuDevice::AccessWiimote(const bdaddr_t& address)
{
  WiimoteDevice* const wiimote = AccessWiimoteByIndex(address.back());
  return wiimote != nullptr && wiimote->GetBD() == address ? wiimote : nullptr;
}

WiimoteDevice* BluetoothEmuDevice::AccessWiimote(u16 connection_handle)
{
  WiimoteDevice* const wiimote =
      AccessWiimoteByIndex(GetWiimoteNumberFromConnectionHandle(connection_handle));
  if (wiimote == nullptr)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "No remote has connection handle {:#06x}", connection_handle);
    PanicAlertFmtT("Can't find Wii Remote by connection handle {0:02x}", connection_handle);
  }
  return wiimote;
}
}  // namespace IOS::HLE
