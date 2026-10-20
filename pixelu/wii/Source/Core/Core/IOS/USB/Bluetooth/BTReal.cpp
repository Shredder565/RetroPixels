// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Bluetooth/BTReal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/USB/Host.h"
#include "Core/System.h"

#include "VideoCommon/OnScreenDisplay.h"

namespace IOS::HLE
{
namespace HCI = BluetoothHCI;

namespace
{
void PutLE16(u8* out, u16 value)
{
  out[0] = static_cast<u8>(value);
  out[1] = static_cast<u8>(value >> 8);
}

void PutLE32(u8* out, u32 value)
{
  PutLE16(out, static_cast<u16>(value));
  PutLE16(out + 2, static_cast<u16>(value >> 16));
}

u16 GetLE16(const u8* in)
{
  return static_cast<u16>(in[0] | (in[1] << 8));
}

// The vendor event the Wii's module sends for its sync button.
constexpr u8 SYNC_PRESSED = 0x08;
constexpr u8 SYNC_HELD = 0x09;

// Offsets in a Command Complete event: header (2), credits (1), opcode (2), return parameters.
constexpr std::size_t COMPLETE_OPCODE = 3;
constexpr std::size_t COMPLETE_PARAMS = 5;
}  // namespace

BluetoothRealDevice::BluetoothRealDevice(EmulationKernel& ios, const std::string& device_name)
    : BluetoothBaseDevice(ios, device_name)
{
  LoadLinkKeys();
}

BluetoothRealDevice::~BluetoothRealDevice()
{
  SaveLinkKeys();
}

std::optional<IPCReply> BluetoothRealDevice::Open(const OpenRequest& request)
{
  m_adapter = std::make_unique<LibUSBBluetoothAdapter>();
  return Device::Open(request);
}

std::optional<IPCReply> BluetoothRealDevice::Close(u32 fd)
{
  m_hci_endpoint.reset();
  m_acl_endpoint.reset();
  m_fake_events = {};

  // The adapter waits for this to complete when it is destroyed.
  SendReset();
  m_adapter.reset();

  return Device::Close(fd);
}

std::optional<IPCReply> BluetoothRealDevice::IOCtlV(const IOCtlVRequest& request)
{
  auto& ios = GetEmulationKernel();
  auto& memory = GetSystem().GetMemory();

  switch (request.request)
  {
  case USB::IOCTLV_USBV0_CTRLMSG:
  {
    const USB::V0CtrlMessage command{ios, request};
    HandleCommand(command);
    return IPCReply{command.length};
  }

  case USB::IOCTLV_USBV0_BLKMSG:
  {
    auto message = std::make_unique<USB::V0BulkMessage>(ios, request);
    if (message->endpoint == ACL_DATA_IN)
    {
      m_acl_endpoint = std::move(message);
      TryToFillACLEndpoint();
      return std::nullopt;
    }

    const auto memory_span = memory.GetSpanForAddress(message->data_address);
    const auto data = memory_span.first(std::min<std::size_t>(message->length, memory_span.size()));
    m_adapter->ScheduleBulkTransfer(message->endpoint, data, GetTargetTime());
    return IPCReply{IPC_SUCCESS};
  }

  case USB::IOCTLV_USBV0_INTRMSG:
  {
    auto message = std::make_unique<USB::V0IntrMessage>(ios, request);
    if (message->endpoint == HCI_EVENT)
    {
      m_hci_endpoint = std::move(message);
      TryToFillHCIEndpoint();
      return std::nullopt;
    }
    ERROR_LOG_FMT(IOS_WIIMOTE, "Interrupt message on unknown endpoint {:#04x}", message->endpoint);
    return IPCReply{IPC_SUCCESS};
  }

  default:
    ERROR_LOG_FMT(IOS_WIIMOTE, "Unknown ioctlv {:#010x}", request.request);
    return IPCReply{IPC_SUCCESS};
  }
}

// An HCI command from the software, sent on to the adapter.
void BluetoothRealDevice::HandleCommand(const USB::V0CtrlMessage& command)
{
  auto& memory = GetSystem().GetMemory();
  const u16 opcode = Common::swap16(memory.Read_U16(command.data_address));

  // Only the Wii's module knows the Broadcom patch commands, so others get a made-up success.
  if (!m_adapter->IsWiiBTModule() &&
      (opcode == HCI::Opcode::VENDOR_FC4C || opcode == HCI::Opcode::VENDOR_FC4F))
  {
    m_fake_events.push([this, opcode](USB::V0IntrMessage& endpoint) {
      FakeVendorCommandReply(opcode, endpoint);
    });
    return;
  }

  if (opcode == HCI::Opcode::DELETE_STORED_LINK_KEY)
  {
    // The software deletes keys, so they go from our copy too. Note that the parameters are
    // read from the start of the command (where the opcode is), not after its header; this is
    // how passthrough has always behaved, so it is kept.
    std::array<u8, sizeof(HCI::DeleteStoredLinkKeyParams)> params{};
    memory.CopyFromEmu(params.data(), command.data_address, params.size());
    bdaddr_t address;
    std::copy_n(params.begin(), address.size(), address.begin());
    INFO_LOG_FMT(IOS_WIIMOTE, "Software deletes stored link keys");
    if (params[6] != 0)
      m_link_keys.clear();
    else
      m_link_keys.erase(address);
  }
  else if (opcode == HCI::Opcode::SNIFF_MODE)
  {
    // Sniff mode is what makes remotes report at 200 Hz rather than 100 Hz.
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Software puts a connection into sniff mode");
  }

  const auto memory_span = memory.GetSpanForAddress(command.data_address);
  const auto data = memory_span.first(std::min<std::size_t>(command.length, memory_span.size()));
  m_adapter->ScheduleControlTransfer(command.request_type, command.request,
                                     static_cast<u8>(command.value),
                                     static_cast<u8>(command.index), data, GetTargetTime());

  // Right after the software resets the controller, our link keys are put back: some adapters
  // drop them on reset, and the adapter may have been used by the host or another instance.
  if (opcode == HCI::Opcode::RESET)
  {
    SendDeleteAllLinkKeys();
    SendStoredLinkKeys();
  }
}

void BluetoothRealDevice::Update()
{
  TryToFillHCIEndpoint();
  TryToFillACLEndpoint();
}

void BluetoothRealDevice::TryToFillHCIEndpoint()
{
  if (m_hci_endpoint == nullptr)
    return;

  // Sync button presses come first, then made-up replies, then the adapter's events.
  const SyncButtonState sync = m_sync_button_state;
  if (sync == SyncButtonState::Pressed)
  {
    Core::DisplayMessage("Scanning for Wii Remotes", 2000);
    FakeSyncButtonEvent(*m_hci_endpoint, SYNC_PRESSED);
  }
  else if (sync == SyncButtonState::LongPressed)
  {
    Core::DisplayMessage("Reset saved Wii Remote pairings", 2000);
    FakeSyncButtonEvent(*m_hci_endpoint, SYNC_HELD);
  }
  else if (!m_fake_events.empty())
  {
    const FakeEvent fake = std::move(m_fake_events.front());
    m_fake_events.pop();
    fake(*m_hci_endpoint);
  }
  else
  {
    const BufferType event = ProcessHCIEvent(m_adapter->ReceiveHCIEvent());
    if (event.empty())
      return;
    DEBUG_ASSERT(event.size() <= m_hci_endpoint->length);
    m_hci_endpoint->FillBuffer(event.data(), event.size());
    GetEmulationKernel().EnqueueIPCReply(m_hci_endpoint->ios_request,
                                         static_cast<s32>(event.size()));
  }

  m_hci_endpoint.reset();
}

auto BluetoothRealDevice::ProcessHCIEvent(BufferType event) -> BufferType
{
  if (event.empty())
    return event;

  const u8 code = event[0];

  // Keep every new link key (address at 2, key at 8).
  if (code == HCI::Event::LINK_KEY_NOTIFICATION && event.size() >= 2 + 6 + 16)
  {
    INFO_LOG_FMT(IOS_WIIMOTE, "Adapter reported a new link key");
    bdaddr_t address;
    std::copy_n(event.data() + 2, address.size(), address.begin());
    std::copy_n(event.data() + 8, 16, m_link_keys[address].begin());
  }

  if (m_adapter->IsWiiBTModule())
    return event;

  // Other adapters need some help to look like the Wii's module.
  if (code == HCI::Event::COMMAND_COMPLETE && event.size() >= COMPLETE_PARAMS + 8 &&
      GetLE16(event.data() + COMPLETE_OPCODE) == HCI::Opcode::READ_BUFFER_SIZE)
  {
    // Nintendo's stack keeps a u8 count of free controller buffers and breaks (disconnects,
    // "event mismatch") when told about more than the module's 10. The other sizes are forced
    // to the module's as well.
    u8* const reply = event.data() + COMPLETE_PARAMS;
    const u16 acl_count = GetLE16(reply + 4);
    if (reply[0] != 0x00)
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Read Buffer Size failed with status {:#04x}", reply[0]);
      reply[0] = 0x00;
      PutLE16(reply + 4, ACL_PKT_NUM);
    }
    else if (acl_count > ACL_PKT_NUM)
    {
      PutLE16(reply + 4, ACL_PKT_NUM);
    }
    else if (acl_count < ACL_PKT_NUM)
    {
      WARN_LOG_FMT(IOS_WIIMOTE, "Adapter has only {} ACL buffers (the Wii's module has {})",
                   acl_count, ACL_PKT_NUM);
    }
    PutLE16(reply + 1, ACL_PKT_SIZE);
    reply[3] = SCO_PKT_SIZE;
    PutLE16(reply + 6, SCO_PKT_NUM);
  }
  else if (code == HCI::Event::CONNECTION_COMPLETE && event.size() >= 5)
  {
    // Some adapters (e.g. the Sena UD100, 0a12:0001) use best effort service, which costs input
    // rate and drops input in some games. Every new connection asks for guaranteed service.
    INFO_LOG_FMT(IOS_WIIMOTE, "Asking for guaranteed service on the new connection");
    std::array<u8, sizeof(HCI::CommandHeader) + sizeof(HCI::QoSSetupParams)> qos{};
    PutLE16(&qos[0], HCI::Opcode::QOS_SETUP);
    qos[2] = sizeof(HCI::QoSSetupParams);
    u8* const params = &qos[3];
    std::copy_n(event.data() + 3, 2, params);  // connection handle
    params[3] = HCI::SERVICE_GUARANTEED;
    PutLE32(params + 4, 0xffffffff);  // token rate
    PutLE32(params + 8, 0xffffffff);  // peak bandwidth
    PutLE32(params + 12, 10000);      // latency (microseconds)
    PutLE32(params + 16, 0xffffffff);  // delay variation
    SendCommand(qos);
  }
  else if (code == HCI::Event::QOS_SETUP_COMPLETE && event.size() > 6)
  {
    const u8 service = event[6];
    if (service == HCI::SERVICE_GUARANTEED)
      INFO_LOG_FMT(IOS_WIIMOTE, "Adapter set up guaranteed service");
    else
      WARN_LOG_FMT(IOS_WIIMOTE, "Adapter set up service type {} instead", service);
  }

  return event;
}

void BluetoothRealDevice::TryToFillACLEndpoint()
{
  if (m_acl_endpoint == nullptr)
    return;

  // Catching up with real time first keeps input latency low.
  auto& core_timing = GetSystem().GetCoreTiming();
  core_timing.Throttle(static_cast<s64>(core_timing.GetTicks()));

  const BufferType data = m_adapter->ReceiveACLData();
  if (data.empty())
    return;

  DEBUG_ASSERT(data.size() <= m_acl_endpoint->length);
  m_acl_endpoint->FillBuffer(data.data(), data.size());
  GetEmulationKernel().EnqueueIPCReply(m_acl_endpoint->ios_request, static_cast<s32>(data.size()));
  m_acl_endpoint.reset();
}

TimePoint BluetoothRealDevice::GetTargetTime() const
{
  auto& core_timing = GetSystem().GetCoreTiming();
  return core_timing.GetTargetHostTime(static_cast<s64>(core_timing.GetTicks()));
}

void BluetoothRealDevice::DoState(PointerWrap& p)
{
  // Marks the state as a passthrough one; emulated Bluetooth stores false here.
  bool is_passthrough_state = true;
  p.Do(is_passthrough_state);
  if (!is_passthrough_state && p.IsReadMode())
  {
    Core::DisplayMessage("State needs Bluetooth passthrough to be disabled. Aborting load.", 4000);
    p.SetVerifyMode();
    return;
  }

  Device::DoState(p);
  DoStateForMessage(GetEmulationKernel(), p, m_hci_endpoint);
  DoStateForMessage(GetEmulationKernel(), p, m_acl_endpoint);

  // The real remotes' state is not part of the savestate.
  if (p.IsReadMode())
  {
    OSD::AddMessage("If the savestate does not load correctly, disconnect all Wii Remotes "
                    "and reload it.",
                    OSD::Duration::NORMAL);
  }

  static bool s_warned_about_saving = false;
  if (p.IsWriteMode() && !s_warned_about_saving)
  {
    OSD::AddMessage("Savestates may not work with Bluetooth passthrough in all cases.\n"
                    "They will only work if no remote is connected when restoring the state,\n"
                    "or no remote is disconnected after saving.",
                    OSD::Duration::VERY_LONG);
    s_warned_about_saving = true;
  }
}

void BluetoothRealDevice::UpdateSyncButtonState(bool is_held)
{
  if (m_sync_button_state == SyncButtonState::Unpressed && is_held)
  {
    m_sync_button_timer.Start();
    m_sync_button_state = SyncButtonState::Held;
  }

  if (m_sync_button_state == SyncButtonState::Held)
  {
    if (!is_held)
      m_sync_button_state = SyncButtonState::Pressed;
    else if (m_sync_button_timer.ElapsedMs() > SYNC_BUTTON_HOLD_MS_TO_RESET)
      m_sync_button_state = SyncButtonState::LongPressed;
  }

  // The release after a long press only re-arms the button.
  if (m_sync_button_state == SyncButtonState::Ignored && !is_held)
    m_sync_button_state = SyncButtonState::Unpressed;
}

void BluetoothRealDevice::TriggerSyncButtonPressedEvent()
{
  m_sync_button_state = SyncButtonState::Pressed;
}

void BluetoothRealDevice::TriggerSyncButtonHeldEvent()
{
  m_sync_button_state = SyncButtonState::LongPressed;
}

void BluetoothRealDevice::SendCommand(std::span<const u8> command)
{
  m_adapter->SendControlTransfer(command);
}

void BluetoothRealDevice::SendReset()
{
  INFO_LOG_FMT(IOS_WIIMOTE, "Resetting the adapter");
  std::array<u8, sizeof(HCI::CommandHeader)> reset{};
  PutLE16(&reset[0], HCI::Opcode::RESET);
  SendCommand(reset);
}

void BluetoothRealDevice::SendDeleteAllLinkKeys()
{
  INFO_LOG_FMT(IOS_WIIMOTE, "Deleting the adapter's link keys");
  std::array<u8, sizeof(HCI::CommandHeader) + sizeof(HCI::DeleteStoredLinkKeyParams)> command{};
  PutLE16(&command[0], HCI::Opcode::DELETE_STORED_LINK_KEY);
  command[2] = sizeof(HCI::DeleteStoredLinkKeyParams);
  // A zero address with "delete all" set.
  command.back() = 0x01;
  SendCommand(command);
}

void BluetoothRealDevice::SendStoredLinkKeys()
{
  if (m_link_keys.empty())
    return;

  // Write Stored Link Key takes 1 to 11 keys.
  constexpr std::size_t MAX_KEYS = 0x0b;
  constexpr std::size_t KEY_ENTRY = 6 + 16;
  const auto count = static_cast<u8>(std::min(m_link_keys.size(), MAX_KEYS));
  INFO_LOG_FMT(IOS_WIIMOTE, "Writing {} link keys to the adapter", count);

  std::array<u8, sizeof(HCI::CommandHeader) + 1 + MAX_KEYS * KEY_ENTRY> command{};
  PutLE16(&command[0], HCI::Opcode::WRITE_STORED_LINK_KEY);
  command[2] = static_cast<u8>(1 + count * KEY_ENTRY);
  command[3] = count;

  u8* entry = &command[4];
  std::size_t written = 0;
  for (const auto& [address, key] : m_link_keys)
  {
    if (written == count)
      break;
    entry = std::ranges::copy(address, entry).out;
    entry = std::ranges::copy(key, entry).out;
    ++written;
  }
  SendCommand(std::span{command}.first(4 + count * KEY_ENTRY));
}

void BluetoothRealDevice::WriteEvent(USB::V0IntrMessage& endpoint, u8 code,
                                     std::span<const u8> params)
{
  std::array<u8, 2 + 255> event{};
  const std::size_t length = std::min<std::size_t>(params.size(), 255);
  event[0] = code;
  event[1] = static_cast<u8>(length);
  std::copy_n(params.begin(), length, event.begin() + 2);

  DEBUG_ASSERT(2 + length <= endpoint.length);
  GetSystem().GetMemory().CopyToEmu(endpoint.data_address, event.data(), 2 + length);
  GetEmulationKernel().EnqueueIPCReply(endpoint.ios_request, static_cast<s32>(2 + length));
}

// A Command Complete without return parameters, giving one command credit.
void BluetoothRealDevice::FakeVendorCommandReply(u16 opcode, USB::V0IntrMessage& endpoint)
{
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Answering vendor command {:#06x} ourselves", opcode);
  const std::array<u8, 3> params{0x01, static_cast<u8>(opcode), static_cast<u8>(opcode >> 8)};
  WriteEvent(endpoint, HCI::Event::COMMAND_COMPLETE, params);
}

// Pressing the sync button makes the Wii's module send a vendor event (0x08), and the software
// then looks for remotes. Holding it for ten seconds sends 0x09, which clears the pairings.
void BluetoothRealDevice::FakeSyncButtonEvent(USB::V0IntrMessage& endpoint, u8 kind)
{
  NOTICE_LOG_FMT(IOS_WIIMOTE, "Sending a sync button event ({:#04x})", kind);
  const std::array<u8, 1> params{kind};
  WriteEvent(endpoint, HCI::Event::VENDOR, params);
  m_sync_button_state = SyncButtonState::Ignored;
}

// The setting is a list of "aa:bb:cc:dd:ee:ff=<32 hex digits>" separated by commas, with the
// address written most significant byte first.
void BluetoothRealDevice::LoadLinkKeys()
{
  const std::string setting = Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_LINK_KEYS);
  if (setting.empty())
    return;

  for (const std::string& entry : SplitString(setting, ','))
  {
    const std::string_view text{entry};
    const std::size_t separator = text.find('=');
    if (separator == std::string_view::npos)
      continue;

    const std::string_view address_text = text.substr(0, separator);
    std::optional<bdaddr_t> address = Common::StringToMacAddress(address_text);
    if (!address)
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Skipping the link key of malformed address {}", address_text);
      continue;
    }
    std::ranges::reverse(*address);

    // Two hex digits per byte; a byte that does not parse stays 0.
    const std::string_view key_text = text.substr(separator + 1);
    linkkey_t key{};
    for (std::size_t i = 0; i < key.size() && 2 * i < key_text.size(); ++i)
      TryParse(std::string{key_text.substr(2 * i, 2)}, &key[i], 16);

    m_link_keys[*address] = key;
  }
}

void BluetoothRealDevice::SaveLinkKeys()
{
  std::string setting;
  for (const auto& [address, key] : m_link_keys)
  {
    if (!setting.empty())
      setting += ',';
    bdaddr_t shown;
    std::ranges::reverse_copy(address, shown.begin());
    setting += Common::MacAddressToString(shown);
    setting += '=';
    for (const u8 byte : key)
      setting += fmt::format("{:02x}", byte);
  }
  Config::SetBase(Config::MAIN_BLUETOOTH_PASSTHROUGH_LINK_KEYS, setting);
}
}  // namespace IOS::HLE
