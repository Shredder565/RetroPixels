// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <span>
#include <string>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "Common/SPSCQueue.h"
#include "Common/Timer.h"
#include "Common/WorkQueueThread.h"

#include "Core/IOS/USB/Bluetooth/hci.h"
#include "Core/LibusbUtils.h"

struct libusb_device;
struct libusb_device_handle;
struct libusb_device_descriptor;
struct libusb_transfer;

// A USB Bluetooth adapter driven through libusb for Bluetooth passthrough. HCI events and ACL
// data are read continuously into queues; commands and ACL data are sent from a worker thread
// at the time the emulated console sent them, and commands obey the controller's flow control
// (Num_HCI_Command_Packets).
class LibUSBBluetoothAdapter
{
public:
  using BufferType = Common::UniqueBuffer<u8>;

  // Whether the user picked this adapter (by VID/PID) in the settings.
  static bool IsConfiguredBluetoothDevice(u16 vid, u16 pid);
  static bool IsBluetoothDevice(const libusb_device_descriptor& descriptor);
  static bool HasConfiguredBluetoothDevice();

  // Everything below is meant to be called from a single thread.

  LibUSBBluetoothAdapter();
  ~LibUSBBluetoothAdapter();

  LibUSBBluetoothAdapter(const LibUSBBluetoothAdapter&) = delete;
  LibUSBBluetoothAdapter& operator=(const LibUSBBluetoothAdapter&) = delete;

  // The oldest received packet, or an empty buffer. ReceiveHCIEvent also does the flow control
  // bookkeeping, so it must be called regularly.
  [[nodiscard]] BufferType ReceiveHCIEvent();
  [[nodiscard]] BufferType ReceiveACLData();

  // Sends the data at `target_time`.
  void ScheduleBulkTransfer(u8 endpoint, std::span<const u8> data, TimePoint target_time);
  void ScheduleControlTransfer(u8 type, u8 request, u8 value, u8 index, std::span<const u8> data,
                               TimePoint target_time);

  // Sends an HCI command as soon as possible.
  void SendControlTransfer(std::span<const u8> data);

  // Sends an HCI command and waits (eating other events) for its Command Complete event. The
  // return parameters are copied to `response`. Returns false on error or timeout.
  bool SendBlockingCommand(std::span<const u8> data, std::span<u8> response);

  // Whether the adapter is a Wii's own Bluetooth module (057e:0305).
  bool IsWiiBTModule() const;

private:
  struct TimedTransfer
  {
    libusb_transfer* transfer{};
    TimePoint target_time{};
  };

  // A command sent to the controller that has not been acknowledged yet.
  struct SentCommand
  {
    u16 opcode{};
    TimePoint submit_time{};
  };

  static constexpr std::size_t MAX_INPUT_QUEUE_SIZE = 100;
  static constexpr u8 INTERFACE = 0x00;

  LibusbUtils::Context m_context;
  libusb_device_handle* m_handle = nullptr;
  u16 m_device_vid = 0;
  u16 m_device_pid = 0;
  std::string m_last_open_error;

  // Every transfer and its buffer, until the transfer is freed.
  std::map<libusb_transfer*, BufferType> m_transfer_buffers;
  // Finished transfers, handed back by the callbacks (and the worker) to be freed here.
  Common::WaitableSPSCQueue<libusb_transfer*> m_transfers_to_free;
  // Guards m_keep_reading and pushes to m_transfers_to_free.
  std::mutex m_transfers_mutex;
  bool m_keep_reading = true;

  // Filled by the input transfer callbacks.
  Common::SPSCQueue<BufferType> m_hci_events;
  Common::SPSCQueue<BufferType> m_acl_data;

  Common::WorkQueueThreadSP<TimedTransfer> m_output_worker;
  Common::PrecisionTimer m_precision_timer;

  // Commands the controller can take right now (the last Num_HCI_Command_Packets it sent).
  u8 m_command_credits = 1;
  std::queue<TimedTransfer> m_waiting_commands;
  std::deque<SentCommand> m_sent_commands;

  // Limits repeated warnings.
  bool m_reported_transfer_failure = false;
  bool m_reported_full_queue = false;

  bool OpenDevice(const libusb_device_descriptor& descriptor, libusb_device* device);
  void StartInputTransfers();

  bool CanSendCommand() const;
  bool HasCommandsInFlight() const;
  void SendCommand(TimedTransfer transfer);
  void OnCommandAcknowledged(u16 opcode, u8 credits);
  void ProcessFlowControl();

  libusb_transfer* AllocateTransfer(std::size_t buffer_size);
  void FreeFinishedTransfers();
  void ReleaseTransfer(libusb_transfer* transfer);

  // Run on the worker thread.
  void SubmitWhenDue(TimedTransfer transfer);
  void Submit(libusb_transfer* transfer);

  // Run on the libusb event thread.
  void OnOutputDone(libusb_transfer* transfer);
  void OnInputDone(libusb_transfer* transfer);
  void ReportTransferStatus(const libusb_transfer* transfer);
};
