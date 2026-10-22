// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Bluetooth/LibUSBBluetoothAdapter.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>

#include <libusb.h>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Thread.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/IOS/USB/Bluetooth/RealtekFirmwareLoader.h"
#include "Core/IOS/USB/Bluetooth/hci.h"

namespace
{
// Size of the buffers the input transfers read into.
constexpr int INPUT_BUFFER_SIZE = 1024;

// A command the controller has not acknowledged by then is given up on.
constexpr auto COMMAND_TIMEOUT = std::chrono::seconds{1};

// HCI commands are class requests to the device (Bluetooth USB transport).
constexpr u8 COMMAND_REQUEST_TYPE = static_cast<u8>(static_cast<u8>(LIBUSB_ENDPOINT_OUT) |
                                                     static_cast<u8>(LIBUSB_REQUEST_TYPE_CLASS) |
                                                     static_cast<u8>(LIBUSB_RECIPIENT_DEVICE));

constexpr u8 EVENT_ENDPOINT = 0x81;
constexpr u8 ACL_IN_ENDPOINT = 0x82;

u16 ReadLE16(const u8* bytes)
{
  return static_cast<u16>(bytes[0] | (bytes[1] << 8));
}

// libusb callbacks carry the adapter in user_data.
template <void (LibUSBBluetoothAdapter::*Handler)(libusb_transfer*)>
void LIBUSB_CALL ForwardToAdapter(libusb_transfer* transfer)
{
  (static_cast<LibUSBBluetoothAdapter*>(transfer->user_data)->*Handler)(transfer);
}
}  // namespace

bool LibUSBBluetoothAdapter::IsBluetoothDevice(const libusb_device_descriptor& descriptor)
{
  // Wireless controller class, RF subclass, Bluetooth programming interface.
  if (descriptor.bDeviceClass == LIBUSB_CLASS_WIRELESS && descriptor.bDeviceSubClass == 0x01 &&
      descriptor.bDeviceProtocol == 0x01)
  {
    return true;
  }
  // Some adapters report another class, so the user can name one, and known Realtek adapters
  // are accepted too.
  return IsConfiguredBluetoothDevice(descriptor.idVendor, descriptor.idProduct) ||
         IsKnownRealtekBluetoothDevice(descriptor.idVendor, descriptor.idProduct);
}

bool LibUSBBluetoothAdapter::HasConfiguredBluetoothDevice()
{
  return Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_VID) != -1 &&
         Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_PID) != -1;
}

bool LibUSBBluetoothAdapter::IsConfiguredBluetoothDevice(u16 vid, u16 pid)
{
  return Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_VID) == vid &&
         Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_PID) == pid;
}

bool LibUSBBluetoothAdapter::IsWiiBTModule() const
{
  return m_device_vid == 0x057e && m_device_pid == 0x0305;
}

LibUSBBluetoothAdapter::LibUSBBluetoothAdapter()
{
  if (!m_context.IsValid())
    return;

  m_last_open_error.clear();
  const bool only_configured = HasConfiguredBluetoothDevice();

  // Takes the first usable adapter.
  const int list_result = m_context.GetDeviceList([&](libusb_device* device) {
    libusb_device_descriptor descriptor;
    libusb_get_device_descriptor(device, &descriptor);

    const auto [config_result, config] = LibusbUtils::MakeConfigDescriptor(device);
    if (config_result != LIBUSB_SUCCESS || !config)
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "No config descriptor for {:04x}:{:04x}: {}", descriptor.idVendor,
                    descriptor.idProduct, LibusbUtils::ErrorWrap(config_result));
      return true;
    }

    if (only_configured && !IsConfiguredBluetoothDevice(descriptor.idVendor, descriptor.idProduct))
      return true;

    if (!IsBluetoothDevice(descriptor) || !OpenDevice(descriptor, device))
      return true;

    const auto describe = [this](u8 index) {
      return LibusbUtils::GetStringDescriptor(m_handle, index).value_or("?");
    };
    const std::string manufacturer = describe(descriptor.iManufacturer);
    const std::string product = describe(descriptor.iProduct);
    const std::string serial = describe(descriptor.iSerialNumber);
    NOTICE_LOG_FMT(IOS_WIIMOTE, "Bluetooth passthrough uses {:04x}:{:04x} (rev {:x}): {} {} {}",
                   descriptor.idVendor, descriptor.idProduct, descriptor.bcdDevice, manufacturer,
                   product, serial);
    m_device_vid = descriptor.idVendor;
    m_device_pid = descriptor.idProduct;
    return false;
  });

  if (list_result != LIBUSB_SUCCESS)
  {
    m_last_open_error =
        Common::FmtFormatT("GetDeviceList failed: {0}", LibusbUtils::ErrorWrap(list_result));
  }

  if (m_handle == nullptr)
  {
    // Without an adapter passthrough can't work: say why, then stop emulation.
    if (!m_last_open_error.empty())
    {
      CriticalAlertFmtT(
          "Could not find any usable Bluetooth USB adapter for Bluetooth Passthrough.\n"
          "The following error occurred when Dolphin tried to use an adapter:\n{0}\n\n"
          "The emulated console will now stop.",
          m_last_open_error);
    }
    else
    {
      CriticalAlertFmtT(
          "Could not find any usable Bluetooth USB adapter for Bluetooth Passthrough.\n\n"
          "The emulated console will now stop.");
    }
    Core::QueueHostJob(&Core::Stop);
    return;
  }

  StartInputTransfers();
  m_output_worker.Reset("Bluetooth Output",
                        [this](TimedTransfer transfer) { SubmitWhenDue(transfer); });

  // Many Realtek adapters do nothing useful until their firmware is uploaded.
  if (IsRealtekVID(m_device_vid) || IsKnownRealtekBluetoothDevice(m_device_vid, m_device_pid))
  {
    INFO_LOG_FMT(IOS_WIIMOTE, "Setting up Realtek adapter {:04x}:{:04x}", m_device_vid,
                 m_device_pid);
    if (!InitializeRealtekBluetoothDevice(*this))
    {
      PanicAlertFmtT("Failed to initialize Realtek Bluetooth device.\n\n"
                     "Bluetooth passthrough will probably not work.");
    }
  }
}

LibUSBBluetoothAdapter::~LibUSBBluetoothAdapter()
{
  if (m_handle == nullptr)
    return;

  // Let the commands still in flight finish (or time out).
  while (HasCommandsInFlight())
  {
    (void)ReceiveHCIEvent();
    Common::YieldCPU();
  }

  m_output_worker.Shutdown();

  {
    std::lock_guard lock{m_transfers_mutex};
    m_keep_reading = false;
  }

  for (const auto& entry : m_transfer_buffers)
    libusb_cancel_transfer(entry.first);

  // Every transfer comes back through its callback once cancelled.
  while (!m_transfer_buffers.empty())
  {
    m_transfers_to_free.WaitForData();
    FreeFinishedTransfers();
  }

  if (const int result = libusb_release_interface(m_handle, 0); result != LIBUSB_SUCCESS)
    WARN_LOG_FMT(IOS_WIIMOTE, "libusb_release_interface: {}", LibusbUtils::ErrorWrap(result));

  libusb_close(std::exchange(m_handle, nullptr));
}

bool LibUSBBluetoothAdapter::OpenDevice(const libusb_device_descriptor& descriptor,
                                        libusb_device* device)
{
  if (const int result = libusb_open(device, &m_handle); result != LIBUSB_SUCCESS)
  {
    m_last_open_error = Common::FmtFormatT("Failed to open Bluetooth device {:04x}:{:04x}: {}",
                                           descriptor.idVendor, descriptor.idProduct,
                                           LibusbUtils::ErrorWrap(result));
    return false;
  }

#ifndef __FreeBSD__
  // FreeBSD does not let regular users detach kernel drivers, so it is not even tried there.
  if (libusb_set_auto_detach_kernel_driver(m_handle, 1) != LIBUSB_SUCCESS)
  {
    const int result = libusb_detach_kernel_driver(m_handle, INTERFACE);
    const bool acceptable = result == LIBUSB_SUCCESS || result == LIBUSB_ERROR_NOT_FOUND ||
                            result == LIBUSB_ERROR_NOT_SUPPORTED;
    if (!acceptable)
    {
      m_last_open_error = Common::FmtFormatT(
          "Failed to detach kernel driver for BT passthrough: {0}", LibusbUtils::ErrorWrap(result));
      return false;
    }
  }
#endif

  if (const int result = libusb_claim_interface(m_handle, INTERFACE); result != LIBUSB_SUCCESS)
  {
    m_last_open_error = Common::FmtFormatT("Failed to claim interface for BT passthrough: {0}",
                                           LibusbUtils::ErrorWrap(result));
    return false;
  }
  return true;
}

void LibUSBBluetoothAdapter::StartInputTransfers()
{
  constexpr auto on_done = &ForwardToAdapter<&LibUSBBluetoothAdapter::OnInputDone>;

  libusb_transfer* const events = AllocateTransfer(INPUT_BUFFER_SIZE);
  libusb_fill_interrupt_transfer(events, m_handle, EVENT_ENDPOINT, events->buffer,
                                 INPUT_BUFFER_SIZE, on_done, this, 0);
  Submit(events);

  libusb_transfer* const acl = AllocateTransfer(INPUT_BUFFER_SIZE);
  libusb_fill_bulk_transfer(acl, m_handle, ACL_IN_ENDPOINT, acl->buffer, INPUT_BUFFER_SIZE,
                            on_done, this, 0);
  Submit(acl);
}

bool LibUSBBluetoothAdapter::CanSendCommand() const
{
  return m_sent_commands.size() < m_command_credits;
}

bool LibUSBBluetoothAdapter::HasCommandsInFlight() const
{
  return !m_waiting_commands.empty() || !m_sent_commands.empty();
}

void LibUSBBluetoothAdapter::SendCommand(TimedTransfer transfer)
{
  const u8* const command = libusb_control_transfer_get_data(transfer.transfer);
  // A command that waited in the queue starts its timeout now.
  const TimePoint submit_time = std::max(transfer.target_time, Clock::now());
  m_sent_commands.push_back(SentCommand{ReadLE16(command), submit_time});

  DEBUG_ASSERT(m_command_credits != 0);
  --m_command_credits;
  m_output_worker.EmplaceItem(transfer);
}

void LibUSBBluetoothAdapter::OnCommandAcknowledged(u16 opcode, u8 credits)
{
  const auto sent = std::ranges::find(m_sent_commands, opcode, &SentCommand::opcode);
  if (sent != m_sent_commands.end())
  {
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Controller acknowledged command {:#06x}", opcode);
    m_sent_commands.erase(sent);
  }
  else if (opcode != 0)
  {
    // Opcode 0 only hands out credits.
    WARN_LOG_FMT(IOS_WIIMOTE, "Controller acknowledged unknown command {:#06x}", opcode);
  }
  m_command_credits = credits;
}

void LibUSBBluetoothAdapter::ProcessFlowControl()
{
  const TimePoint deadline = Clock::now() - COMMAND_TIMEOUT;
  while (!m_sent_commands.empty() && m_sent_commands.front().submit_time < deadline)
  {
    WARN_LOG_FMT(IOS_WIIMOTE, "Command {:#06x} was never acknowledged",
                 m_sent_commands.front().opcode);
    m_sent_commands.pop_front();
  }

  // With nothing in flight, one command may always go.
  if (m_sent_commands.empty())
    m_command_credits = 1;

  if (!m_waiting_commands.empty() && CanSendCommand())
  {
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Sending a queued command");
    SendCommand(m_waiting_commands.front());
    m_waiting_commands.pop();
  }

  FreeFinishedTransfers();
}

auto LibUSBBluetoothAdapter::ReceiveHCIEvent() -> BufferType
{
  if (m_hci_events.Empty())
  {
    ProcessFlowControl();
    return {};
  }

  BufferType event = std::move(m_hci_events.Front());
  m_hci_events.Pop();

  // Command Complete (credits, opcode) and Command Status (status, credits, opcode) acknowledge
  // a command and say how many more the controller takes.
  const u8 code = event[0];
  if (code == BluetoothHCI::Event::COMMAND_COMPLETE || code == BluetoothHCI::Event::COMMAND_STATUS)
  {
    const std::size_t params = code == BluetoothHCI::Event::COMMAND_COMPLETE ?
                                   sizeof(BluetoothHCI::CommandCompleteParams) :
                                   sizeof(BluetoothHCI::CommandStatusParams);
    if (event.size() < sizeof(BluetoothHCI::EventHeader) + params) [[unlikely]]
    {
      WARN_LOG_FMT(IOS_WIIMOTE, "Truncated command acknowledgement");
    }
    else
    {
      const u8* const p = event.data() + sizeof(BluetoothHCI::EventHeader) + params;
      OnCommandAcknowledged(ReadLE16(p - 2), p[-3]);
    }
  }

  ProcessFlowControl();
  return event;
}

auto LibUSBBluetoothAdapter::ReceiveACLData() -> BufferType
{
  BufferType data;
  m_acl_data.Pop(data);
  return data;
}

libusb_transfer* LibUSBBluetoothAdapter::AllocateTransfer(std::size_t buffer_size)
{
  libusb_transfer* const transfer = libusb_alloc_transfer(0);
  auto& buffer = m_transfer_buffers.emplace(transfer, BufferType{buffer_size}).first->second;
  transfer->buffer = buffer.get();
  return transfer;
}

void LibUSBBluetoothAdapter::ReleaseTransfer(libusb_transfer* transfer)
{
  libusb_free_transfer(transfer);
  m_transfer_buffers.erase(transfer);
}

void LibUSBBluetoothAdapter::FreeFinishedTransfers()
{
  libusb_transfer* transfer = nullptr;
  while (m_transfers_to_free.Pop(transfer))
    ReleaseTransfer(transfer);
}

void LibUSBBluetoothAdapter::SubmitWhenDue(TimedTransfer transfer)
{
  if (Config::Get(Config::MAIN_PRECISION_FRAME_TIMING))
    m_precision_timer.SleepUntil(transfer.target_time);
  else
    std::this_thread::sleep_until(transfer.target_time);
  Submit(transfer.transfer);
}

void LibUSBBluetoothAdapter::Submit(libusb_transfer* transfer)
{
  const int result = libusb_submit_transfer(transfer);
  if (result == LIBUSB_SUCCESS)
    return;

  ERROR_LOG_FMT(IOS_WIIMOTE, "libusb_submit_transfer: {}", LibusbUtils::ErrorWrap(result));
  // No callback comes for a transfer that was never submitted.
  std::lock_guard lock{m_transfers_mutex};
  m_transfers_to_free.Emplace(transfer);
}

void LibUSBBluetoothAdapter::ScheduleBulkTransfer(u8 endpoint, std::span<const u8> data,
                                                  TimePoint target_time)
{
  libusb_transfer* const transfer = AllocateTransfer(data.size());
  libusb_fill_bulk_transfer(transfer, m_handle, endpoint, transfer->buffer,
                            static_cast<int>(data.size()),
                            &ForwardToAdapter<&LibUSBBluetoothAdapter::OnOutputDone>, this, 0);
  std::ranges::copy(data, transfer->buffer);
  m_output_worker.EmplaceItem(TimedTransfer{transfer, target_time});
}

void LibUSBBluetoothAdapter::ScheduleControlTransfer(u8 type, u8 request, u8 value, u8 index,
                                                     std::span<const u8> data,
                                                     TimePoint target_time)
{
  libusb_transfer* const transfer = AllocateTransfer(LIBUSB_CONTROL_SETUP_SIZE + data.size());
  libusb_fill_control_setup(transfer->buffer, type, request, value, index,
                            static_cast<u16>(data.size()));
  libusb_fill_control_transfer(transfer, m_handle, transfer->buffer,
                               &ForwardToAdapter<&LibUSBBluetoothAdapter::OnOutputDone>, this, 0);
  std::ranges::copy(data, transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE);

  const TimedTransfer timed{transfer, target_time};
  if (CanSendCommand())
  {
    SendCommand(timed);
    return;
  }
  DEBUG_LOG_FMT(IOS_WIIMOTE, "Controller is busy; queueing a command");
  m_waiting_commands.push(timed);
}

void LibUSBBluetoothAdapter::SendControlTransfer(std::span<const u8> data)
{
  ScheduleControlTransfer(COMMAND_REQUEST_TYPE, 0, 0, 0, data, Clock::now());
}

bool LibUSBBluetoothAdapter::SendBlockingCommand(std::span<const u8> data, std::span<u8> response)
{
  SendControlTransfer(data);
  const u16 opcode = ReadLE16(data.data());

  constexpr std::size_t PARAMS_OFFSET =
      sizeof(BluetoothHCI::EventHeader) + sizeof(BluetoothHCI::CommandCompleteParams);

  while (HasCommandsInFlight())
  {
    const BufferType event = ReceiveHCIEvent();
    if (event.empty())
    {
      Common::YieldCPU();
      continue;
    }
    if (event[0] != BluetoothHCI::Event::COMMAND_COMPLETE)
      continue;
    if (event.size() < PARAMS_OFFSET)
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Truncated Command Complete event");
      continue;
    }
    if (ReadLE16(event.data() + PARAMS_OFFSET - 2) != opcode)
      continue;

    if (event.size() < PARAMS_OFFSET + response.size())
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Command {:#06x} returned too few bytes", opcode);
      return false;
    }
    std::copy_n(event.data() + PARAMS_OFFSET, response.size(), response.data());
    return true;
  }
  return false;
}

void LibUSBBluetoothAdapter::OnOutputDone(libusb_transfer* transfer)
{
  ReportTransferStatus(transfer);
  std::lock_guard lock{m_transfers_mutex};
  m_transfers_to_free.Emplace(transfer);
}

void LibUSBBluetoothAdapter::OnInputDone(libusb_transfer* transfer)
{
  if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
  {
    const bool is_event = transfer->endpoint == EVENT_ENDPOINT;
    auto& queue = is_event ? m_hci_events : m_acl_data;
    if (queue.Size() >= MAX_INPUT_QUEUE_SIZE)
    {
      // Happens while emulation is paused.
      if (!std::exchange(m_reported_full_queue, true))
        WARN_LOG_FMT(IOS_WIIMOTE, "{} queue is full; dropping packets", is_event ? "HCI" : "ACL");
    }
    else
    {
      if (transfer->actual_length != 0)
      {
        const auto length = static_cast<std::size_t>(transfer->actual_length);
        BufferType packet{length};
        std::copy_n(transfer->buffer, length, packet.data());
        queue.Emplace(std::move(packet));
      }
      m_reported_full_queue = false;
    }
  }

  ReportTransferStatus(transfer);

  std::lock_guard lock{m_transfers_mutex};
  if (m_keep_reading)
  {
    // Input transfers are resubmitted for as long as the adapter is in use.
    const int result = libusb_submit_transfer(transfer);
    if (result == LIBUSB_SUCCESS)
      return;
    ERROR_LOG_FMT(IOS_WIIMOTE, "Resubmitting input transfer: {}", LibusbUtils::ErrorWrap(result));
  }
  m_transfers_to_free.Emplace(transfer);
}

void LibUSBBluetoothAdapter::ReportTransferStatus(const libusb_transfer* transfer)
{
  if (transfer->status == LIBUSB_TRANSFER_COMPLETED ||
      transfer->status == LIBUSB_TRANSFER_CANCELLED)
  {
    m_reported_transfer_failure = false;
    return;
  }

  ERROR_LOG_FMT(IOS_WIIMOTE, "libusb transfer failed: {}",
                LibusbUtils::ErrorWrap(transfer->status));
  if (!std::exchange(m_reported_transfer_failure, true))
  {
    Core::DisplayMessage("Failed to send a command to the Bluetooth adapter.", 10000);
    Core::DisplayMessage("It may not be compatible with passthrough mode.", 10000);
  }
}
