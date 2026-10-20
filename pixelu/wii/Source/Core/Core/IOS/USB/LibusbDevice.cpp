// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/LibusbDevice.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <libusb.h>

#include "Common/Assert.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
namespace
{
// libusb's descriptor structs start with exactly the fields of the USB descriptors, in the same
// order and with the same sizes, so the leading bytes can be copied over as they are.
template <typename Ours, typename Theirs>
Ours CopyDescriptor(const Theirs& source)
{
  static_assert(sizeof(Theirs) >= sizeof(Ours));
  Ours result;
  std::memcpy(&result, &source, sizeof(result));
  return result;
}

const char* TransferTypeName(u8 type)
{
  switch (type)
  {
  case LIBUSB_TRANSFER_TYPE_CONTROL:
    return "Control";
  case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
    return "Isochronous";
  case LIBUSB_TRANSFER_TYPE_BULK:
    return "Bulk";
  case LIBUSB_TRANSFER_TYPE_INTERRUPT:
    return "Interrupt";
  default:
    return "Unknown";
  }
}

// IOS reply values for transfers that did not complete.
constexpr s32 USB_STALL_RESULT = -7004;
constexpr s32 USB_FAILED_RESULT = -5;

// The Rock Band 3 LED request (SET_REPORT on interface 0, 8 bytes) is sent by every RB3 title.
bool IsRockBand3LedRequest(const CtrlMessage& message)
{
  return message.request_type == 0x21 && message.request == 0x09 && message.length == 8;
}
}  // namespace

LibusbDevice::LibusbDevice(libusb_device* device, const libusb_device_descriptor& descriptor)
    : m_device(device)
{
  libusb_ref_device(m_device);
  m_vid = descriptor.idVendor;
  m_pid = descriptor.idProduct;
  m_reported_vid = m_vid;
  m_reported_pid = m_pid;

  const u64 bus = libusb_get_bus_number(device);
  const u64 address = libusb_get_device_address(device);
  m_id = (u64{m_vid} << 32) | (u64{m_pid} << 16) | (bus << 8) | address;

  for (u8 config = 0; config < descriptor.bNumConfigurations; ++config)
  {
    auto [result, config_descriptor] = LibusbUtils::MakeConfigDescriptor(m_device, config);
    if (result != LIBUSB_SUCCESS || !config_descriptor)
    {
      WARN_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Could not get configuration descriptor {}: {}",
                   m_vid, m_pid, config, LibusbUtils::ErrorWrap(result));
    }
    // A failed entry is kept (empty) so that indices still match configuration numbers.
    m_config_descriptors.push_back(std::move(config_descriptor));
  }

  if (Config::Get(Config::MAIN_USB_PASSTHROUGH_DISGUISE_PLAYSTATION_AS_WII))
    ApplyPlayStationDisguise();
}

LibusbDevice::~LibusbDevice()
{
  if (m_handle != nullptr)
  {
    ReleaseInterfacesOfActiveConfig();
    libusb_close(m_handle);
  }
  libusb_unref_device(m_device);
}

DeviceDescriptor LibusbDevice::GetDeviceDescriptor() const
{
  libusb_device_descriptor host_descriptor;
  libusb_get_device_descriptor(m_device, &host_descriptor);
  DeviceDescriptor descriptor = CopyDescriptor<DeviceDescriptor>(host_descriptor);
  descriptor.idVendor = m_reported_vid;
  descriptor.idProduct = m_reported_pid;
  return descriptor;
}

std::vector<ConfigDescriptor> LibusbDevice::GetConfigurations() const
{
  std::vector<ConfigDescriptor> configurations;
  for (const LibusbUtils::ConfigDescriptor& config : m_config_descriptors)
  {
    if (config)
    {
      configurations.push_back(CopyDescriptor<ConfigDescriptor>(*config));
      continue;
    }
    ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Skipping a configuration that could not be read", m_vid,
                  m_pid);
  }
  return configurations;
}

std::vector<InterfaceDescriptor> LibusbDevice::GetInterfaces(const u8 config) const
{
  std::vector<InterfaceDescriptor> interfaces;
  if (config >= m_config_descriptors.size() || !m_config_descriptors[config])
  {
    ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] No configuration descriptor {}", m_vid, m_pid, config);
    return interfaces;
  }

  const libusb_config_descriptor& host_config = *m_config_descriptors[config];
  for (u8 number = 0; number < host_config.bNumInterfaces; ++number)
  {
    const libusb_interface& host_interface = host_config.interface[number];
    // Every alternate setting is listed as an interface descriptor of its own.
    for (u8 alt = 0; alt < host_interface.num_altsetting; ++alt)
      interfaces.push_back(CopyDescriptor<InterfaceDescriptor>(host_interface.altsetting[alt]));
  }
  return interfaces;
}

std::vector<EndpointDescriptor> LibusbDevice::GetEndpoints(const u8 config, const u8 iface,
                                                           const u8 alt) const
{
  std::vector<EndpointDescriptor> endpoints;
  if (config >= m_config_descriptors.size() || !m_config_descriptors[config])
  {
    ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] No configuration descriptor {}", m_vid, m_pid, config);
    return endpoints;
  }

  const libusb_config_descriptor& host_config = *m_config_descriptors[config];
  ASSERT(iface < host_config.bNumInterfaces);
  const libusb_interface& host_interface = host_config.interface[iface];
  ASSERT(alt < host_interface.num_altsetting);
  const libusb_interface_descriptor& setting = host_interface.altsetting[alt];
  for (u8 index = 0; index < setting.bNumEndpoints; ++index)
    endpoints.push_back(CopyDescriptor<EndpointDescriptor>(setting.endpoint[index]));
  return endpoints;
}

std::string LibusbDevice::GetErrorName(const int error_code) const
{
  return libusb_error_name(error_code);
}

bool LibusbDevice::Attach()
{
  if (m_device_attached)
    return true;

  if (m_handle == nullptr)
  {
    NOTICE_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Opening the device", m_vid, m_pid);
    const int result = libusb_open(m_device, &m_handle);
    if (result != LIBUSB_SUCCESS)
    {
      ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Could not open the device: {}", m_vid, m_pid,
                    LibusbUtils::ErrorWrap(result));
      m_handle = nullptr;
      return false;
    }
  }

  if (ClaimAllInterfaces(DEFAULT_CONFIG_NUM) < LIBUSB_SUCCESS)
    return false;

  m_device_attached = true;
  return true;
}

bool LibusbDevice::AttachAndChangeInterface(const u8 iface)
{
  if (!Attach())
    return false;
  if (iface == m_active_interface)
    return true;
  return ChangeInterface(iface) == LIBUSB_SUCCESS;
}

int LibusbDevice::CancelTransfer(const u8 endpoint)
{
  INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Cancelling the transfers of endpoint {:#x}", m_vid,
               m_pid, m_active_interface, endpoint);
  const auto found = m_pending.find(endpoint);
  if (found == m_pending.end())
    return IPC_ENOENT;
  found->second.CancelAll();
  return IPC_SUCCESS;
}

int LibusbDevice::ChangeInterface(const u8 iface)
{
  INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Active interface is now {}", m_vid, m_pid,
               m_active_interface, iface);
  // All interfaces were claimed by Attach, so this is only bookkeeping.
  m_active_interface = iface;
  return LIBUSB_SUCCESS;
}

int LibusbDevice::GetNumberOfAltSettings(const u8 iface)
{
  return m_config_descriptors[0]->interface[iface].num_altsetting;
}

int LibusbDevice::SetAltSetting(const u8 alt_setting)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Alternate setting {}", m_vid, m_pid,
               m_active_interface, alt_setting);
  return libusb_set_interface_alt_setting(m_handle, m_active_interface, alt_setting);
}

void LibusbDevice::ApplyPlayStationDisguise()
{
  // The PS3 Rock Band instruments are the Wii ones with Sony's IDs (VID 0x12ba). Reporting the
  // Wii IDs (Harmonix, VID 0x1bad) lets Wii titles use them. The PS3 Rock Band 1 and 2 guitars
  // and drums share one PID, so they are reported as the Rock Band 2 model, which has more
  // features (latency calibration, cymbals).
  //
  // The instruments that came with Rock Band 3 (keyboard, pro guitars, MIDI Pro Adapter modes)
  // only send their keys/frets on PS3 after a vendor request that the PS3 game sends and the Wii
  // game does not. Those also get m_needs_rb3_instrument_enable.
  struct Disguise
  {
    u16 ps3_pid;
    u16 wii_pid;
    bool needs_instrument_enable;
  };
  static constexpr std::array<Disguise, 9> disguises{{
      {0x0200, 0x3010, false},  // guitar (RB1/RB2)
      {0x0210, 0x3110, false},  // drums (RB1/RB2)
      {0x0218, 0x3138, false},  // MIDI Pro Adapter, drums mode
      {0x2330, 0x3330, true},   // RB3 keyboard
      {0x2338, 0x3338, true},   // MIDI Pro Adapter, keyboard mode
      {0x2430, 0x3430, true},   // Mustang pro guitar
      {0x2438, 0x3438, true},   // MIDI Pro Adapter, Mustang mode
      {0x2530, 0x3530, true},   // Squier pro guitar (supported by the game, never sold)
      {0x2538, 0x3538, true},   // MIDI Pro Adapter, Squier mode
  }};

  constexpr u16 SONY_VID = 0x12ba;
  constexpr u16 HARMONIX_VID = 0x1bad;
  if (m_vid != SONY_VID)
    return;

  const auto match = std::ranges::find(disguises, m_pid, &Disguise::ps3_pid);
  if (match == disguises.end())
    return;

  m_reported_pid = match->wii_pid;
  m_needs_rb3_instrument_enable = match->needs_instrument_enable;
  m_reported_vid = HARMONIX_VID;
}

int LibusbDevice::SendRockBand3InstrumentEnable()
{
  // HID SET_REPORT (output report 0) with the payload the PS3 game sends.
  static constexpr std::array<u8, 40> payload = {
      0xe9, 0x00, 0x89, 0x1b, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x89, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xe9, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  auto packet = std::make_unique<u8[]>(LIBUSB_CONTROL_SETUP_SIZE + payload.size());
  libusb_fill_control_setup(packet.get(), 0x21, 0x09, 0x0300, 0x00,
                            static_cast<u16>(payload.size()));
  std::ranges::copy(payload, packet.get() + LIBUSB_CONTROL_SETUP_SIZE);

  libusb_transfer* transfer = libusb_alloc_transfer(0);
  transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;
  // Nobody waits for this one, so it has no callback.
  libusb_fill_control_transfer(transfer, m_handle, packet.release(), nullptr, this, 0);
  return libusb_submit_transfer(transfer);
}

int LibusbDevice::Queue(libusb_transfer* transfer, std::unique_ptr<TransferCommand> command)
{
  transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;
  m_pending[transfer->endpoint].Add(transfer, std::move(command));
  return libusb_submit_transfer(transfer);
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<CtrlMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  DEBUG_LOG_FMT(IOS_USB,
                "[{:04x}:{:04x} {}] Control request: type={:02x} request={:02x} value={:04x}"
                " index={:04x} length={:04x}",
                m_vid, m_pid, m_active_interface, cmd->request_type, cmd->request, cmd->value,
                cmd->index, cmd->length);

  // Two standard requests change host-side state and must go through libusb's API instead of
  // being sent to the device. They are answered right away.
  const u16 setup = static_cast<u16>((cmd->request_type << 8) | cmd->request);
  if (setup == USBHDR(DIR_HOST2DEVICE, TYPE_STANDARD, REC_INTERFACE, REQUEST_SET_INTERFACE))
  {
    INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] SET_INTERFACE index={:04x} value={:04x}", m_vid,
                 m_pid, m_active_interface, cmd->index, cmd->value);
    const u8 iface = static_cast<u8>(cmd->index);
    if (iface != m_active_interface)
    {
      const int change_result = ChangeInterface(iface);
      if (change_result < LIBUSB_SUCCESS)
      {
        ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Could not switch to interface {}: {}", m_vid,
                      m_pid, m_active_interface, cmd->index, LibusbUtils::ErrorWrap(change_result));
        return change_result;
      }
    }
    const int result = SetAltSetting(static_cast<u8>(cmd->value));
    if (result == LIBUSB_SUCCESS)
      cmd->GetEmulationKernel().EnqueueIPCReply(cmd->ios_request, cmd->length);
    return result;
  }
  if (setup == USBHDR(DIR_HOST2DEVICE, TYPE_STANDARD, REC_DEVICE, REQUEST_SET_CONFIGURATION))
  {
    INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] SET_CONFIGURATION index={:04x} value={:04x}", m_vid,
                 m_pid, m_active_interface, cmd->index, cmd->value);
    ReleaseInterfacesOfActiveConfig();
    const int result = libusb_set_configuration(m_handle, cmd->value);
    if (result == LIBUSB_SUCCESS)
    {
      ClaimAllInterfaces(static_cast<u8>(cmd->value));
      cmd->GetEmulationKernel().EnqueueIPCReply(cmd->ios_request, cmd->length);
    }
    return result;
  }

  // Anything else is sent as it is: setup packet followed by the data stage.
  auto packet = std::make_unique<u8[]>(cmd->length + LIBUSB_CONTROL_SETUP_SIZE);
  libusb_fill_control_setup(packet.get(), cmd->request_type, cmd->request, cmd->value, cmd->index,
                            cmd->length);
  cmd->GetEmulationKernel().GetSystem().GetMemory().CopyFromEmu(
      packet.get() + LIBUSB_CONTROL_SETUP_SIZE, cmd->data_address, cmd->length);

  // The LED request is a good moment to also turn on the RB3 instrument inputs.
  const bool enable_rb3_inputs = m_needs_rb3_instrument_enable && IsRockBand3LedRequest(*cmd);

  libusb_transfer* transfer = libusb_alloc_transfer(0);
  libusb_fill_control_transfer(transfer, m_handle, packet.release(), OnControlTransferDone, this,
                               0);
  const int result = Queue(transfer, std::move(cmd));

  if (enable_rb3_inputs)
    SendRockBand3InstrumentEnable();
  return result;
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<BulkMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Bulk transfer: length={:04x} endpoint={:02x}", m_vid,
                m_pid, m_active_interface, cmd->length, cmd->endpoint);

  libusb_transfer* transfer = libusb_alloc_transfer(0);
  u8* data = cmd->MakeBuffer(cmd->length).release();
  libusb_fill_bulk_transfer(transfer, m_handle, cmd->endpoint, data, static_cast<int>(cmd->length),
                            OnTransferDone, this, 0);
  return Queue(transfer, std::move(cmd));
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<IntrMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Interrupt transfer: length={:04x} endpoint={:02x}",
                m_vid, m_pid, m_active_interface, cmd->length, cmd->endpoint);

  libusb_transfer* transfer = libusb_alloc_transfer(0);
  u8* data = cmd->MakeBuffer(cmd->length).release();
  libusb_fill_interrupt_transfer(transfer, m_handle, cmd->endpoint, data,
                                 static_cast<int>(cmd->length), OnTransferDone, this, 0);
  return Queue(transfer, std::move(cmd));
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<IsoMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  DEBUG_LOG_FMT(IOS_USB,
                "[{:04x}:{:04x} {}] Isochronous transfer: length={:04x} endpoint={:02x}"
                " packets={:02x}",
                m_vid, m_pid, m_active_interface, cmd->length, cmd->endpoint, cmd->num_packets);

  libusb_transfer* transfer = libusb_alloc_transfer(cmd->num_packets);
  transfer->dev_handle = m_handle;
  transfer->endpoint = cmd->endpoint;
  transfer->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
  transfer->timeout = 0;
  transfer->buffer = cmd->MakeBuffer(cmd->length).release();
  transfer->length = static_cast<int>(cmd->length);
  transfer->num_iso_packets = cmd->num_packets;
  for (size_t packet = 0; packet < cmd->num_packets; ++packet)
    transfer->iso_packet_desc[packet].length = cmd->packet_sizes[packet];
  transfer->callback = OnTransferDone;
  transfer->user_data = this;
  return Queue(transfer, std::move(cmd));
}

void LibusbDevice::OnControlTransferDone(libusb_transfer* transfer)
{
  auto* self = static_cast<LibusbDevice*>(transfer->user_data);
  self->m_pending[0].Finish(transfer, [transfer](const TransferCommand& command) {
    command.FillBuffer(libusb_control_transfer_get_data(transfer),
                       static_cast<size_t>(transfer->actual_length));
    // IOS reports the length of the whole transfer, setup packet included.
    return static_cast<s32>(transfer->length);
  });
}

void LibusbDevice::OnTransferDone(libusb_transfer* transfer)
{
  auto* self = static_cast<LibusbDevice*>(transfer->user_data);
  self->m_pending[transfer->endpoint].Finish(transfer, [transfer](const TransferCommand& command) {
    if (transfer->type != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
    {
      command.FillBuffer(transfer->buffer, static_cast<size_t>(transfer->actual_length));
      return static_cast<s32>(transfer->actual_length);
    }

    const auto& iso = static_cast<const IsoMessage&>(command);
    command.FillBuffer(transfer->buffer, iso.length);
    for (size_t packet = 0; packet < iso.num_packets; ++packet)
    {
      iso.SetPacketReturnValue(packet,
                               static_cast<u16>(transfer->iso_packet_desc[packet].actual_length));
    }
    // Isochronous transfers are answered with 0; any other value counts as an error.
    return static_cast<s32>(IPC_SUCCESS);
  });
}

void LibusbDevice::PendingTransfers::Add(libusb_transfer* transfer,
                                         std::unique_ptr<TransferCommand> command)
{
  std::lock_guard lock(m_lock);
  m_commands.emplace(transfer, std::move(command));
}

void LibusbDevice::PendingTransfers::Finish(
    libusb_transfer* transfer, const std::function<s32(const TransferCommand&)>& on_success)
{
  std::lock_guard lock(m_lock);
  const auto found = m_commands.find(transfer);
  if (found == m_commands.end())
  {
    ERROR_LOG_FMT(IOS_USB, "A transfer finished that was never queued");
    return;
  }

  // The data buffer was handed to libusb with release(); take it back so that it is freed.
  const std::unique_ptr<u8[]> data(transfer->buffer);
  const TransferCommand& command = *found->second;

  s32 reply = LIBUSB_SUCCESS;
  if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
  {
    reply = on_success(command);
  }
  else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
  {
    reply = IPC_ENOENT;
  }
  else if (transfer->status == LIBUSB_TRANSFER_ERROR ||
           transfer->status == LIBUSB_TRANSFER_CANCELLED ||
           transfer->status == LIBUSB_TRANSFER_TIMED_OUT ||
           transfer->status == LIBUSB_TRANSFER_OVERFLOW ||
           transfer->status == LIBUSB_TRANSFER_STALL)
  {
    const auto* owner = static_cast<const LibusbDevice*>(transfer->user_data);
    ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] {} transfer on endpoint {:#04x} failed: {}",
                  owner->m_vid, owner->m_pid, owner->m_active_interface,
                  TransferTypeName(transfer->type), transfer->endpoint,
                  libusb_error_name(transfer->status));
    reply = transfer->status == LIBUSB_TRANSFER_STALL ? USB_STALL_RESULT : USB_FAILED_RESULT;
  }

  command.OnTransferComplete(reply);
  m_commands.erase(found);
}

void LibusbDevice::PendingTransfers::CancelAll()
{
  std::lock_guard lock(m_lock);
  if (m_commands.empty())
    return;

  INFO_LOG_FMT(IOS_USB, "Cancelling {} transfer(s)", m_commands.size());
  // The callbacks run later, from libusb's event thread, and answer the requests.
  for (const auto& entry : m_commands)
    libusb_cancel_transfer(entry.first);
}

int LibusbDevice::ForEachInterface(const u8 config_num, const std::function<int(u8)>& action) const
{
  if (config_num >= m_config_descriptors.size() || !m_config_descriptors[config_num])
    return LIBUSB_ERROR_NOT_FOUND;

  // A configuration without interfaces also ends up as "not found".
  int result = LIBUSB_ERROR_NOT_FOUND;
  const u8 count = m_config_descriptors[config_num]->bNumInterfaces;
  for (u8 number = 0; number < count; ++number)
  {
    result = action(number);
    if (result < LIBUSB_SUCCESS)
      break;
  }
  return result;
}

int LibusbDevice::ClaimAllInterfaces(const u8 config_num) const
{
  const int result = ForEachInterface(config_num, [this](u8 number) {
    // Host drivers (HID, audio...) have to let go of the interface before it can be claimed.
    // NOT_FOUND means no driver was bound; NOT_SUPPORTED is what platforms without kernel
    // drivers (Windows) return.
    const int detach_result = libusb_detach_kernel_driver(m_handle, number);
    if (detach_result < LIBUSB_SUCCESS && detach_result != LIBUSB_ERROR_NOT_FOUND &&
        detach_result != LIBUSB_ERROR_NOT_SUPPORTED)
    {
      ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Could not detach the host driver: {}", m_vid, m_pid,
                    LibusbUtils::ErrorWrap(detach_result));
      return detach_result;
    }
    return libusb_claim_interface(m_handle, number);
  });

  if (result < LIBUSB_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Could not claim the interfaces of configuration {}",
                  m_vid, m_pid, config_num);
  }
  return result;
}

int LibusbDevice::ReleaseAllInterfaces(const u8 config_num) const
{
  const int result = ForEachInterface(
      config_num, [this](u8 number) { return libusb_release_interface(m_handle, number); });

  const bool expected_failure =
      result == LIBUSB_ERROR_NO_DEVICE || result == LIBUSB_ERROR_NOT_FOUND;
  if (result < LIBUSB_SUCCESS && !expected_failure)
  {
    ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Could not release the interfaces of configuration {}",
                  m_vid, m_pid, config_num);
  }
  return result;
}

int LibusbDevice::ReleaseInterfacesOfActiveConfig() const
{
  int active_config = 0;
  const int result = libusb_get_configuration(m_handle, &active_config);
  if (result < LIBUSB_SUCCESS)
    return result;
  return ReleaseAllInterfaces(static_cast<u8>(active_config));
}
}  // namespace IOS::HLE::USB
