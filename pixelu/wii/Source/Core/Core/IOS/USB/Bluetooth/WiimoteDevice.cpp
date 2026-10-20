// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"

#include <algorithm>
#include <functional>
#include <vector>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Core.h"
#include "Core/HW/WII_IPC.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/Host.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteHIDAttr.h"
#include "Core/IOS/USB/Bluetooth/l2cap.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace L2CAP = BluetoothL2CAP;

namespace
{
// OSD time of the connect/disconnect messages, in milliseconds.
constexpr u32 OSD_TIME = 3000;

// MTU a real remote asks for when it configures a channel.
constexpr u16 REMOTE_REQUESTED_MTU = 185;

// L2CAP fields are little endian, SDP fields big endian.
u16 GetLE16(const u8* p)
{
  return static_cast<u16>(p[0] | (p[1] << 8));
}
u16 GetBE16(const u8* p)
{
  return static_cast<u16>((p[0] << 8) | p[1]);
}
u32 GetBE32(const u8* p)
{
  return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

// Appends fields to a packet under construction.
class PacketWriter
{
public:
  void U8(u8 value) { m_bytes.push_back(value); }
  void LE16(u16 value)
  {
    U8(static_cast<u8>(value));
    U8(static_cast<u8>(value >> 8));
  }
  void BE16(u16 value)
  {
    U8(static_cast<u8>(value >> 8));
    U8(static_cast<u8>(value));
  }
  void BE32(u32 value)
  {
    BE16(static_cast<u16>(value >> 16));
    BE16(static_cast<u16>(value));
  }
  void Bytes(const u8* data, std::size_t count) { m_bytes.insert(m_bytes.end(), data, data + count); }

  std::span<const u8> View() const { return m_bytes; }

private:
  std::vector<u8> m_bytes;
};

// SDP PDU IDs and the data element types used in requests.
constexpr u8 SDP_SERVICE_SEARCH_REQUEST = 0x02;
constexpr u8 SDP_SERVICE_SEARCH_RESPONSE = 0x03;
constexpr u8 SDP_SERVICE_ATTRIBUTE_REQUEST = 0x04;
constexpr u8 SDP_SERVICE_ATTRIBUTE_RESPONSE = 0x05;
constexpr u8 SDP_ELEMENT_UINT32 = 0x0a;

// The remote has a single searchable service record: HID.
constexpr u32 SDP_HID_RECORD = 0x10000;

// Reads a ServiceAttributeRequest continuation state: 0 for none, or a two-byte offset.
u32 ReadContinuationState(const u8* state)
{
  if (state[0] == 0x02)
    return GetBE16(state + 1);
  if (state[0] != 0x00)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "SDP: unexpected continuation state length {}", state[0]);
    PanicAlertFmt("ParseCont: wrong cont: {}", state[0]);
  }
  return 0;
}

// Skips the AttributeIDList of a ServiceAttributeRequest (a sequence holding one range or one
// ID) and returns its size in bytes.
u32 SkipAttributeIDList(const u8* list)
{
  // list[0] and list[1] are the sequence header, list[2] the type of the single element.
  if (list[2] == SDP_ELEMENT_UINT32)
    return 7;

  WARN_LOG_FMT(IOS_WIIMOTE, "SDP: request for a single attribute");
  PanicAlertFmt("Read just a single attrib - not tested");
  return 5;
}
}  // namespace

bool WiimoteDevice::Channel::IsOpen() const
{
  return remote_cid != L2CAP::CID_NULL && remote_mtu != 0 &&
         configuration == Configuration::Accepted;
}

WiimoteDevice::WiimoteDevice(BluetoothEmuDevice* host, bdaddr_t bd, unsigned int hid_source_number)
    : m_host(host), m_bd(bd)
{
  // The last address byte is the slot number (see BluetoothEmuDevice).
  const u32 number = GetNumber();
  m_name = number == WIIMOTE_BALANCE_BOARD ? "Nintendo RVL-WBC-01" : "Nintendo RVL-CNT-01";
  INFO_LOG_FMT(IOS_WIIMOTE, "Wiimote: #{} Constructed", number);

  // What a real remote reports about itself.
  m_link_key.fill(static_cast<u8>(0xa0 + number));
  m_class = {0x00, 0x04, 0x48};
  m_features = {0xbc, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
  m_lmp_version = 0x2;
  m_lmp_subversion = 0x229;

  WiimoteCommon::HIDWiimote* const source =
      WiimoteCommon::GetHIDWiimoteSource(hid_source_number);
  if (source != nullptr)
  {
    source->SetWiimoteDeviceIndex(static_cast<u8>(number));
    // Already asking for a connection, so that SetSource below shows no "connected" message.
    SetLinkState(LinkState::RequestConnection);
  }
  SetSource(source);
}

WiimoteDevice::~WiimoteDevice() = default;

void WiimoteDevice::DoState(PointerWrap& p)
{
  p.Do(m_link_state);
  p.Do(m_hid_linking);
  p.Do(m_bd);
  p.Do(m_class);
  p.Do(m_features);
  p.Do(m_lmp_version);
  p.Do(m_lmp_subversion);
  p.Do(m_link_key);
  p.Do(m_name);
  p.Do(m_channels);
  p.Do(m_reconnect_delay);
}

u32 WiimoteDevice::GetNumber() const
{
  return m_bd.back();
}

bool WiimoteDevice::IsSourceValid() const
{
  return m_hid_source != nullptr;
}

bool WiimoteDevice::IsConnected() const
{
  return m_link_state == LinkState::Complete;
}

// An enabled remote without a connection is both discoverable and connectable.
bool WiimoteDevice::IsInquiryScanEnabled() const
{
  return IsSourceValid() && !IsConnected();
}

bool WiimoteDevice::IsPageScanEnabled() const
{
  return IsSourceValid() && !IsConnected();
}

void WiimoteDevice::SetLinkState(LinkState state)
{
  // Any change holds off button-triggered reconnection for a second.
  m_reconnect_delay = static_cast<u8>(::Wiimote::UPDATE_FREQ);

  const bool before = IsConnected();
  m_link_state = state;
  const bool after = IsConnected();

  // The UI shows which remotes are connected.
  if (before != after)
    Host_UpdateDisasmDialog();

  if (m_hid_source == nullptr || before == after)
    return;

  if (after)
    m_hid_source->EventLinked();
  else
    m_hid_source->EventUnlinked();
}

void WiimoteDevice::Reset()
{
  SetLinkState(LinkState::Inactive);
  m_hid_linking = HIDLinking::Idle;
  m_channels = {};
}

void WiimoteDevice::SetSource(WiimoteCommon::HIDWiimote* hid_source)
{
  if (m_hid_source != nullptr && IsConnected())
    Activate(false);

  m_hid_source = hid_source;
  if (m_hid_source == nullptr)
    return;

  m_hid_source->SetInterruptCallback(
      std::bind_front(&WiimoteDevice::InterruptDataInputCallback, this));
  Activate(true);
}

void WiimoteDevice::Activate(bool connect)
{
  const u32 shown_number = GetNumber() + 1;

  if (connect)
  {
    if (m_link_state != LinkState::Inactive)
      return;
    SetLinkState(LinkState::RequestConnection);
    Core::DisplayMessage(fmt::format("Wii Remote {} connected", shown_number), OSD_TIME);
    return;
  }

  if (!IsConnected())
    return;

  // The channels are dropped without an L2CAP disconnection; software copes with that.
  Reset();
  m_host->RemoteDisconnect(m_bd);
  Core::DisplayMessage(fmt::format("Wii Remote {} disconnected", shown_number), OSD_TIME);
}

bool WiimoteDevice::EventConnectionRequest()
{
  if (!IsPageScanEnabled())
    return false;

  Core::DisplayMessage(
      fmt::format("Wii Remote {} connected from emulated software", GetNumber() + 1), OSD_TIME);
  SetLinkState(LinkState::Complete);
  return true;
}

bool WiimoteDevice::EventConnectionAccept()
{
  if (!IsPageScanEnabled())
    return false;

  SetLinkState(LinkState::Complete);
  // The remote asked for this connection, so it is the one that opens the HID channels.
  m_hid_linking = HIDLinking::Opening;
  return true;
}

void WiimoteDevice::EventDisconnect(u8)
{
  // Software always gives reason 0x13 (remote user terminated the connection).
  Core::DisplayMessage(
      fmt::format("Wii Remote {} disconnected by emulated software", GetNumber() + 1), OSD_TIME);
  Reset();
}

WiimoteDevice::Channel* WiimoteDevice::FindChannel(u16 psm)
{
  const auto it = std::ranges::find_if(m_channels, [psm](const auto& entry) {
    return entry.second.psm == psm;
  });
  return it != m_channels.end() ? &it->second : nullptr;
}

const WiimoteDevice::Channel* WiimoteDevice::FindChannel(u16 psm) const
{
  return const_cast<WiimoteDevice*>(this)->FindChannel(psm);
}

u16 WiimoteDevice::FreeChannelID() const
{
  u16 cid = L2CAP::CID_FIRST_DYNAMIC;
  while (m_channels.contains(cid))
    ++cid;
  return cid;
}

bool WiimoteDevice::OpenChannel(u16 psm)
{
  const Channel* channel = FindChannel(psm);
  if (channel != nullptr)
    return channel->IsOpen();

  RequestConnection(psm);
  return false;
}

void WiimoteDevice::Update()
{
  if (m_link_state == LinkState::RequestConnection && m_host->RemoteConnect(*this))
  {
    // The request is now with the Bluetooth module; the remote waits for the outcome.
    SetLinkState(LinkState::Inactive);
  }

  if (!IsConnected())
    return;

  // Configure the channels the other side has just accepted or opened.
  for (auto& [cid, channel] : m_channels)
  {
    if (channel.remote_cid == L2CAP::CID_NULL ||
        channel.configuration != Channel::Configuration::NotRequested)
    {
      continue;
    }
    channel.configuration = Channel::Configuration::Requested;
    RequestConfiguration(channel.remote_cid, REMOTE_REQUESTED_MTU, L2CAP::DEFAULT_FLUSH_TIMEOUT);
  }

  // The HID control channel is opened first, then the interrupt channel.
  if (m_hid_linking == HIDLinking::Opening && OpenChannel(L2CAP::PSM_HID_CONTROL) &&
      OpenChannel(L2CAP::PSM_HID_INTERRUPT))
  {
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Wii Remote {}: HID channels are open", GetNumber());
    m_hid_linking = HIDLinking::Idle;
  }
}

WiimoteDevice::NextUpdateInputCall
WiimoteDevice::PrepareInput(WiimoteEmu::DesiredWiimoteState* wiimote_state)
{
  if (m_reconnect_delay != 0)
    --m_reconnect_delay;

  if (m_hid_source == nullptr)
    return NextUpdateInputCall::None;

  if (m_link_state == LinkState::Inactive)
  {
    // Without a connection, pressing a button asks for one (once the delay has passed).
    if (m_reconnect_delay != 0)
      return NextUpdateInputCall::None;
    wiimote_state->buttons = m_hid_source->GetCurrentlyPressedButtons();
    return NextUpdateInputCall::Activate;
  }

  // Reports can only be sent over an open interrupt channel.
  const Channel* interrupt = FindChannel(L2CAP::PSM_HID_INTERRUPT);
  if (interrupt == nullptr || !interrupt->IsOpen())
    return NextUpdateInputCall::None;

  const auto gpio_out = m_host->GetSystem().GetWiiIPC().GetGPIOOutFlags();
  const bool sensor_bar_on = static_cast<bool>(gpio_out[IOS::GPIO::SENSOR_BAR]);
  m_hid_source->PrepareInput(wiimote_state,
                             sensor_bar_on ? WiimoteCommon::HIDWiimote::SensorBarState::Enabled :
                                             WiimoteCommon::HIDWiimote::SensorBarState::Disabled);
  return NextUpdateInputCall::Update;
}

void WiimoteDevice::UpdateInput(NextUpdateInputCall next_call,
                                const WiimoteEmu::DesiredWiimoteState& wiimote_state)
{
  if (next_call == NextUpdateInputCall::Activate)
  {
    if ((wiimote_state.buttons.hex & WiimoteCommon::ButtonData::BUTTON_MASK) != 0)
      Activate(true);
  }
  else if (next_call == NextUpdateInputCall::Update)
  {
    m_hid_source->Update(wiimote_state);
  }
}

void WiimoteDevice::ExecuteL2capCmd(u8* ptr, u32 size)
{
  const u16 length = GetLE16(ptr);
  const u16 cid = GetLE16(ptr + 2);
  const u8* const payload = ptr + sizeof(L2CAP::PacketHeader);
  const u32 payload_size = size - u32{sizeof(L2CAP::PacketHeader)};

  if (length != payload_size)
  {
    INFO_LOG_FMT(IOS_WIIMOTE, "L2CAP: dropping a packet whose length field is wrong");
    return;
  }

  if (cid == L2CAP::CID_SIGNALING)
  {
    HandleSignaling(payload, payload_size);
    return;
  }

  const auto it = m_channels.find(cid);
  if (it == m_channels.end())
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "L2CAP: packet for unknown channel {:#x}", cid);
    return;
  }
  const Channel& channel = it->second;

  constexpr u8 SET_REPORT_OUTPUT =
      (WiimoteCommon::HID_TYPE_SET_REPORT << 4) | WiimoteCommon::HID_PARAM_OUTPUT;
  constexpr u8 DATA_OUTPUT = (WiimoteCommon::HID_TYPE_DATA << 4) | WiimoteCommon::HID_PARAM_OUTPUT;

  switch (channel.psm)
  {
  case L2CAP::PSM_SDP:
    HandleSDP(cid, payload);
    break;

  case L2CAP::PSM_HID_CONTROL:
    // Original remotes also take output reports as SET_REPORT on the control channel. Games
    // don't do this, but some homebrew does.
    if (payload[0] == SET_REPORT_OUTPUT)
    {
      const u8 handshake = WiimoteCommon::HID_HANDSHAKE_SUCCESS;
      SendL2CAPPacket(channel.remote_cid, std::span(&handshake, 1));
      m_hid_source->InterruptDataOutput(payload + 1, payload_size - 1);
    }
    else
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "HID control channel: unhandled header {:#x}", payload[0]);
    }
    break;

  case L2CAP::PSM_HID_INTERRUPT:
    if (payload[0] == DATA_OUTPUT)
      m_hid_source->InterruptDataOutput(payload + 1, payload_size - 1);
    else
      ERROR_LOG_FMT(IOS_WIIMOTE, "HID interrupt channel: unhandled header {:#x}", payload[0]);
    break;

  default:
    ERROR_LOG_FMT(IOS_WIIMOTE, "L2CAP: channel {:#x} has unknown PSM {:#x}", cid, channel.psm);
    break;
  }
}

void WiimoteDevice::SendL2CAPPacket(u16 cid, std::span<const u8> payload)
{
  PacketWriter packet;
  packet.LE16(static_cast<u16>(payload.size()));
  packet.LE16(cid);
  packet.Bytes(payload.data(), payload.size());
  const auto bytes = packet.View();
  m_host->SendACLPacket(m_bd, bytes.data(), static_cast<u32>(bytes.size()));
}

void WiimoteDevice::SendSignal(u8 identifier, u8 code, std::span<const u8> params)
{
  // The parameter length is one byte here, like the signaling commands the remote sends.
  const u8 length = static_cast<u8>(params.size());

  PacketWriter command;
  command.U8(code);
  command.U8(identifier);
  command.LE16(length);
  command.Bytes(params.data(), length);
  SendL2CAPPacket(L2CAP::CID_SIGNALING, command.View());
}

void WiimoteDevice::HandleSignaling(const u8* data, u32 size)
{
  // One packet can hold several commands. The remaining size is unsigned and is not checked
  // against the command lengths.
  while (size >= sizeof(L2CAP::SignalHeader))
  {
    const u8 code = data[0];
    const u8 identifier = data[1];
    const u16 length = GetLE16(data + 2);
    const u8* const params = data + sizeof(L2CAP::SignalHeader);
    size -= u32{sizeof(L2CAP::SignalHeader)} + length;

    switch (code)
    {
    case L2CAP::Signal::COMMAND_REJECT:
      ERROR_LOG_FMT(IOS_WIIMOTE, "L2CAP: a command was rejected. A damaged SYSCONF can cause "
                                 "this; try a fresh copy.");
      break;
    case L2CAP::Signal::CONNECTION_REQUEST:
      OnConnectionRequest(identifier, params);
      break;
    case L2CAP::Signal::CONNECTION_RESPONSE:
      OnConnectionResponse(params);
      break;
    case L2CAP::Signal::CONFIGURE_REQUEST:
      OnConfigureRequest(identifier, params, length);
      break;
    case L2CAP::Signal::CONFIGURE_RESPONSE:
      OnConfigureResponse(params);
      break;
    case L2CAP::Signal::DISCONNECTION_REQUEST:
      OnDisconnectionRequest(identifier, params);
      break;
    default:
      ERROR_LOG_FMT(IOS_WIIMOTE, "L2CAP: unknown signaling command {:#04x}", code);
      return;
    }

    data = params + length;
  }
}

void WiimoteDevice::OnConnectionRequest(u8 identifier, const u8* params)
{
  const u16 psm = GetLE16(params);
  const u16 source_cid = GetLE16(params + 2);

  u16 local_cid = L2CAP::CID_NULL;
  u16 result = L2CAP::RESULT_SUCCESS;

  if (FindChannel(psm) != nullptr)
  {
    // A remote has at most one channel per PSM and refuses a second one.
    ERROR_LOG_FMT(IOS_WIIMOTE, "L2CAP: refusing a second channel for PSM {:#x}", psm);
    result = L2CAP::RESULT_NO_RESOURCES;
  }
  else
  {
    if (psm != L2CAP::PSM_SDP && psm != L2CAP::PSM_HID_CONTROL && psm != L2CAP::PSM_HID_INTERRUPT)
      WARN_LOG_FMT(IOS_WIIMOTE, "L2CAP: connection for unexpected PSM {:#x}", psm);

    local_cid = FreeChannelID();
    Channel& channel = m_channels[local_cid];
    channel.psm = psm;
    channel.remote_cid = source_cid;
  }

  PacketWriter response;
  response.LE16(local_cid);
  response.LE16(source_cid);
  response.LE16(result);
  response.LE16(L2CAP::STATUS_NONE);
  SendSignal(identifier, L2CAP::Signal::CONNECTION_RESPONSE, response.View());
}

void WiimoteDevice::OnConnectionResponse(const u8* params)
{
  // Software accepts every channel the remote opens.
  const u16 destination_cid = GetLE16(params);
  const u16 source_cid = GetLE16(params + 2);
  m_channels[source_cid].remote_cid = destination_cid;
}

void WiimoteDevice::OnConfigureRequest(u8 identifier, const u8* params, u32 length)
{
  const u16 local_cid = GetLE16(params);
  Channel& channel = m_channels[local_cid];

  // Every option is accepted and echoed back unchanged.
  PacketWriter response;
  response.LE16(channel.remote_cid);
  response.LE16(0);  // flags
  response.LE16(L2CAP::RESULT_SUCCESS);

  u16 mtu = L2CAP::DEFAULT_MTU;
  u32 offset = 4;  // after the channel ID and the flags
  while (offset < length)
  {
    const u8* const option = params + offset;
    const u8 option_length = option[1];
    // The flush timeout does not matter: packets are never dropped.
    if (option[0] == L2CAP::OPTION_MTU)
      mtu = GetLE16(option + 2);
    else if (option[0] != L2CAP::OPTION_FLUSH_TIMEOUT)
      DEBUG_LOG_FMT(IOS_WIIMOTE, "L2CAP: unknown configuration option {:#x}", option[0]);

    const u32 option_size = u32{sizeof(L2CAP::OptionHeader)} + option_length;
    response.Bytes(option, option_size);
    offset += option_size;
  }

  SendSignal(identifier, L2CAP::Signal::CONFIGURE_RESPONSE, response.View());
  channel.remote_mtu = mtu;
}

void WiimoteDevice::OnConfigureResponse(const u8* params)
{
  m_channels[GetLE16(params)].configuration = Channel::Configuration::Accepted;
}

void WiimoteDevice::OnDisconnectionRequest(u8 identifier, const u8* params)
{
  const u16 destination_cid = GetLE16(params);
  const u16 source_cid = GetLE16(params + 2);
  m_channels.erase(destination_cid);

  PacketWriter response;
  response.LE16(destination_cid);
  response.LE16(source_cid);
  SendSignal(identifier, L2CAP::Signal::DISCONNECTION_RESPONSE, response.View());
}

void WiimoteDevice::RequestConnection(u16 psm)
{
  const u16 local_cid = FreeChannelID();
  m_channels[local_cid].psm = psm;

  PacketWriter request;
  request.LE16(psm);
  request.LE16(local_cid);
  // The remote uses the command code as the identifier.
  SendSignal(L2CAP::Signal::CONNECTION_REQUEST, L2CAP::Signal::CONNECTION_REQUEST, request.View());
}

void WiimoteDevice::RequestConfiguration(u16 remote_cid, u16 mtu, u16 flush_timeout)
{
  PacketWriter request;
  request.LE16(remote_cid);
  request.LE16(0);  // flags
  // Options are only sent when they differ from the defaults.
  if (mtu != L2CAP::DEFAULT_MTU)
  {
    request.U8(L2CAP::OPTION_MTU);
    request.U8(L2CAP::OPTION_MTU_LENGTH);
    request.LE16(mtu);
  }
  if (flush_timeout != L2CAP::DEFAULT_FLUSH_TIMEOUT)
  {
    request.U8(L2CAP::OPTION_FLUSH_TIMEOUT);
    request.U8(L2CAP::OPTION_FLUSH_TIMEOUT_LENGTH);
    request.LE16(flush_timeout);
  }
  SendSignal(L2CAP::Signal::CONFIGURE_REQUEST, L2CAP::Signal::CONFIGURE_REQUEST, request.View());
}

void WiimoteDevice::HandleSDP(u16 cid, const u8* data)
{
  // The answer goes out on the channel's local ID, as it always has.
  PacketWriter response;

  switch (data[0])
  {
  case SDP_SERVICE_SEARCH_REQUEST:
  {
    // Every search is answered with the HID record.
    const u16 transaction = GetBE16(data + 1);
    WARN_LOG_FMT(IOS_WIIMOTE, "SDP: service search (transaction {:#x})", transaction);

    response.U8(SDP_SERVICE_SEARCH_RESPONSE);
    response.BE16(transaction);
    response.BE16(9);  // parameter length
    response.BE16(1);  // total record count
    response.BE16(1);  // records in this response
    response.BE32(SDP_HID_RECORD);
    response.U8(0);  // no continuation
    break;
  }

  case SDP_SERVICE_ATTRIBUTE_REQUEST:
  {
    // Layout: transaction ID, parameter length, record handle, maximum byte count, attribute
    // ID list, continuation state. Every request gets the whole record, fragment by fragment.
    const u16 transaction = GetBE16(data + 1);
    const u32 record = GetBE32(data + 5);
    constexpr u32 ID_LIST_OFFSET = 11;
    const u32 continuation =
        ReadContinuationState(data + ID_LIST_OFFSET + SkipAttributeIDList(data + ID_LIST_OFFSET));
    WARN_LOG_FMT(IOS_WIIMOTE, "SDP: attribute request (record {:#x}, offset {:#x})", record,
                 continuation);

    if (record != SDP_HID_RECORD)
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "SDP: unknown record {:#x}", record);
      PanicAlertFmt("Unknown service handle {:x}", record);
    }

    u32 fragment_size = 0;
    const u8* const fragment = GetAttribPacket(record, continuation, fragment_size);

    response.U8(SDP_SERVICE_ATTRIBUTE_RESPONSE);
    response.BE16(transaction);
    if (fragment != nullptr)
      response.Bytes(fragment, fragment_size);
    break;
  }

  default:
    ERROR_LOG_FMT(IOS_WIIMOTE, "SDP: unknown PDU {:#x}", data[0]);
    PanicAlertFmt("WIIMOTE: Unknown SDP command {:x}", data[0]);
    return;
  }

  SendL2CAPPacket(cid, response.View());
}

void WiimoteDevice::InterruptDataInputCallback(u8 hid_type, const u8* data, u32 size)
{
  const Channel* interrupt = FindChannel(L2CAP::PSM_HID_INTERRUPT);
  if (interrupt == nullptr)
  {
    WARN_LOG_FMT(IOS_WIIMOTE, "Wii Remote {}: input report without an interrupt channel",
                 GetNumber());
    return;
  }

  // Header byte and report. Reports are at most MAX_PAYLOAD bytes with the header.
  std::array<u8, WiimoteCommon::MAX_PAYLOAD> report{};
  report[0] = hid_type;
  std::copy_n(data, size, report.begin() + 1);
  const u16 report_size = static_cast<u16>(1 + size);
  SendL2CAPPacket(interrupt->remote_cid, std::span(report.data(), report_size));
}
}  // namespace IOS::HLE
