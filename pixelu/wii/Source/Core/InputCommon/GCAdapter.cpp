// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/GCAdapter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"
#include "InputCommon/GCAdapterUSB.h"
#include "InputCommon/GCPadStatus.h"

// The official Wii U / Switch GameCube controller adapter (and compatible ones), driven through
// our own USB code (GCAdapterUSB). Three threads take part: a scanning thread that looks for the
// adapter twice a second, a read thread that polls the four ports, and a write thread that sends
// rumble changes.

namespace GCAdapter
{
namespace
{
using namespace std::chrono_literals;

constexpr u16 ADAPTER_VENDOR_ID = 0x057e;
constexpr u16 ADAPTER_PRODUCT_ID = 0x0337;

constexpr unsigned int TRANSFER_TIMEOUT_MS = 100;

// Adapter protocol: the host sends 0x13 once to start the adapter reporting, and 0x11 followed
// by one motor byte per port to set rumble. The adapter reports a 0x21 (HID descriptor type)
// byte followed by 9 bytes per port.
constexpr u8 COMMAND_START_POLLING = 0x13;
constexpr u8 COMMAND_RUMBLE = 0x11;
constexpr std::size_t INPUT_REPORT_SIZE = 37;
constexpr std::size_t PORT_REPORT_SIZE = 9;
constexpr std::size_t RUMBLE_COMMAND_SIZE = 5;
// The input report's first byte (the HID descriptor type).
constexpr u8 REPORT_TYPE = 0x21;

// Reads failing for longer than this mean the adapter is gone.
constexpr auto READ_FAILURE_LIMIT = 500ms;
// Reads per poll rate measurement.
constexpr int POLL_RATE_SAMPLES = 50;

constexpr int PORT_COUNT = SerialInterface::MAX_SI_CHANNELS;

enum class AdapterStatus
{
  NotDetected,
  Detected,
  Error,
};

enum class ControllerType : u8
{
  None = 0,
  Wired = 1,
  Wireless = 2,
};

struct Port
{
  // What a newly connected pad reports first (its resting values, with PAD_GET_ORIGIN).
  GCPadStatus origin = {};
  GCPadStatus status = {};
  ControllerType type = ControllerType::None;
  bool origin_pending = false;
};

// Which bit of which report byte (1 or 2 of a port's 9) is which button.
struct ButtonBit
{
  u8 byte;
  u8 mask;
  u16 button;
};
constexpr std::array<ButtonBit, 12> BUTTON_BITS = {{
    {1, 0x01, PAD_BUTTON_A},
    {1, 0x02, PAD_BUTTON_B},
    {1, 0x04, PAD_BUTTON_X},
    {1, 0x08, PAD_BUTTON_Y},
    {1, 0x10, PAD_BUTTON_LEFT},
    {1, 0x20, PAD_BUTTON_RIGHT},
    {1, 0x40, PAD_BUTTON_DOWN},
    {1, 0x80, PAD_BUTTON_UP},
    {2, 0x01, PAD_BUTTON_START},
    {2, 0x02, PAD_TRIGGER_Z},
    {2, 0x04, PAD_TRIGGER_R},
    {2, 0x08, PAD_TRIGGER_L},
}};

// --- Adapter connection ---
std::unique_ptr<GCAdapterUSB::Device> s_device;
std::atomic<AdapterStatus> s_status = AdapterStatus::NotDetected;
// Why the adapter could not be opened (shown by IsDetected), guarded by s_error_mutex.
std::string s_last_error;
std::mutex s_error_mutex;
std::function<void()> s_status_callback;
// Held while connecting or disconnecting.
std::mutex s_connection_mutex;

// --- Ports (guarded by s_ports_mutex) ---
std::array<Port, PORT_COUNT> s_ports;
std::mutex s_ports_mutex;
// The motor state last sent for each port.
std::array<u8, PORT_COUNT> s_motors;

// --- Read thread ---
std::thread s_reader;
Common::Flag s_reader_running;
Common::Flag s_reader_started;
Common::Flag s_reads_failing;
std::atomic<double> s_poll_rate{};

// --- Write thread ---
std::thread s_writer;
Common::Flag s_writer_running;
Common::Event s_write_requested;
std::array<u8, RUMBLE_COMMAND_SIZE> s_pending_write;
std::atomic<int> s_pending_write_size{0};

// --- Scanning thread ---
std::thread s_scanner;
Common::Flag s_scanner_running;
Common::Event s_scan_wakeup;

// --- Configuration ---
std::optional<Config::ConfigChangedCallbackID> s_config_callback;
std::atomic_bool s_adapter_wanted = false;
std::array<std::atomic_bool, PORT_COUNT> s_rumble_allowed{};
// CoreTiming ticks of the last Init while a game was running.
u64 s_last_init_ticks = 0;

void Disconnect();
void ReaderMain();

void NotifyStatusChanged()
{
  if (s_status_callback != nullptr)
    s_status_callback();
}

std::array<u8, RUMBLE_COMMAND_SIZE> MakeRumbleCommand()
{
  return {COMMAND_RUMBLE, s_motors[0], s_motors[1], s_motors[2], s_motors[3]};
}

ControllerType ReadControllerType(u8 flags)
{
  if ((flags & 0x10) != 0)
    return ControllerType::Wired;
  if ((flags & 0x20) != 0)
    return ControllerType::Wireless;
  return ControllerType::None;
}

// Turns every motor off. The caller holds s_connection_mutex.
void StopAllMotors()
{
  if (s_device == nullptr || s_status != AdapterStatus::Detected ||
      !s_adapter_wanted.load(std::memory_order_relaxed))
  {
    return;
  }

  s_motors.fill(0);
  std::array<u8, RUMBLE_COMMAND_SIZE> command = MakeRumbleCommand();

  if (s_device->WriteInterrupt(command.data(), static_cast<int>(command.size()),
                               TRANSFER_TIMEOUT_MS) < 0)
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "ResetRumbleLockNeeded: {}", s_device->LastError());
  }
  INFO_LOG_FMT(CONTROLLERINTERFACE, "Rumble state reset");
}

void HandleReport(const u8* report, std::size_t size)
{
  if (size != INPUT_REPORT_SIZE || report[0] != REPORT_TYPE)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "error reading payload (size: {}, type: {:02x})", size,
                  report[0]);
    return;
  }

  std::lock_guard lock(s_ports_mutex);
  for (int index = 0; index != PORT_COUNT; ++index)
  {
    const u8* const bytes = report + 1 + PORT_REPORT_SIZE * static_cast<std::size_t>(index);
    Port& port = s_ports[static_cast<std::size_t>(index)];
    const ControllerType type = ReadControllerType(bytes[0]);

    GCPadStatus pad = {};
    if (type == ControllerType::None)
    {
      // Keep empty ports looking empty, except where that would make the input depend on what
      // is plugged in on this machine.
      if (!Core::WantsDeterminism())
        pad.button = PAD_ERR_STATUS;
    }
    else
    {
      for (const ButtonBit& bit : BUTTON_BITS)
      {
        if ((bytes[bit.byte] & bit.mask) != 0)
          pad.button |= bit.button;
      }
      pad.stickX = bytes[3];
      pad.stickY = bytes[4];
      pad.substickX = bytes[5];
      pad.substickY = bytes[6];
      pad.triggerLeft = bytes[7];
      pad.triggerRight = bytes[8];

      if (port.type == ControllerType::None)
      {
        NOTICE_LOG_FMT(CONTROLLERINTERFACE, "New device connected to Port {} of Type: {:02x}",
                       index + 1, bytes[0]);
        pad.button |= PAD_GET_ORIGIN;
        port.origin = pad;
        port.origin_pending = true;
      }
    }

    port.type = type;
    port.status = pad;
  }
}

void WriterMain()
{
  Common::SetCurrentThreadName("GCAdapter Write Thread");
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GCAdapter write thread started");

  while (s_writer_running.IsSet())
  {
    s_write_requested.Wait();

    const int size = s_pending_write_size.load();
    if (size != 0 && s_device->WriteInterrupt(s_pending_write.data(), size, TRANSFER_TIMEOUT_MS) < 0)
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "Write: {}", s_device->LastError());

    Common::YieldCPU();
  }

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GCAdapter write thread stopped");
}

void ReaderMain()
{
  Common::SetCurrentThreadName("GCAdapter Read Thread");
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GCAdapter read thread started");

  s_writer_running.Set(true);
  s_writer = std::thread(WriterMain);

  auto last_good_read = std::chrono::steady_clock::now();
  s_reads_failing.Clear();
  s_reader_started.Set();

  ResetRumble();

  auto measurement_start = Clock::now();
  int measured_reads = 0;
  bool previous_read_failed = false;

  while (s_reader_running.IsSet() && !s_reads_failing.IsSet())
  {
    std::array<u8, INPUT_REPORT_SIZE> report;
    const int received =
        s_device->ReadInterrupt(report.data(), static_cast<int>(report.size()), TRANSFER_TIMEOUT_MS);

    if (received >= 0)
    {
      HandleReport(report.data(), static_cast<std::size_t>(received));
      last_good_read = std::chrono::steady_clock::now();
      previous_read_failed = false;
    }
    else
    {
      if (!previous_read_failed)
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "Read newly failed: {}", s_device->LastError());
      }
      else
      {
        INFO_LOG_FMT(CONTROLLERINTERFACE, "Read failed: {}", s_device->LastError());
        // Failing transfers can return at once (say, after unplugging); don't spin on them.
        Common::SleepCurrentThread(1);
      }
      previous_read_failed = true;

      if (s_device->IsGone() ||
          std::chrono::steady_clock::now() - last_good_read > READ_FAILURE_LIMIT)
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "Read: continuously failing transfers, resetting.");
        s_reads_failing.Set();
      }
    }

    ++measured_reads;
    if (measured_reads == POLL_RATE_SAMPLES)
    {
      const auto now = Clock::now();
      const double seconds = DT_s(now - measurement_start).count();
      s_poll_rate.store(POLL_RATE_SAMPLES / seconds, std::memory_order_relaxed);
      measurement_start = now;
      measured_reads = 0;
    }
  }

  // Take the write thread down with this one.
  if (s_writer_running.TestAndClear())
  {
    s_pending_write_size.store(0);
    s_write_requested.Set();
    s_writer.join();
  }

  s_poll_rate.store(0.0, std::memory_order_relaxed);
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GCAdapter read thread stopped");
}

// Opens the adapter if one is plugged in. On failure after finding one, the adapter is put in
// the error state with the reason.
bool OpenAdapter()
{
  bool found = false;
  std::string error;
  s_device = GCAdapterUSB::Device::Open(ADAPTER_VENDOR_ID, ADAPTER_PRODUCT_ID, &found, &error);
  if (s_device != nullptr)
  {
    NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Found GC Adapter [VID:{:04X}, PID:{:04X}]",
                   ADAPTER_VENDOR_ID, ADAPTER_PRODUCT_ID);
    // HID SET_IDLE (some adapter clones need it).
    if (!s_device->ControlOut(0x21, 11, 0x0001, 0, 1000))
      WARN_LOG_FMT(CONTROLLERINTERFACE, "SET_IDLE failed: {}", s_device->LastError());
    return true;
  }
  if (found)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Found a GC Adapter but couldn't use it: {}", error);
    {
      std::lock_guard lock(s_error_mutex);
      s_last_error = std::move(error);
    }
    s_status = AdapterStatus::Error;
  }
  return false;
}

// Starts an opened adapter and the read thread. The caller holds s_connection_mutex.
void StartAdapter()
{
  const std::array<u8, 1> start_command = {COMMAND_START_POLLING};
  if (s_device->WriteInterrupt(start_command.data(), static_cast<int>(start_command.size()),
                               TRANSFER_TIMEOUT_MS) < 0)
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "AddGCAdapter: {}", s_device->LastError());
  }

  s_reader_running.Set(true);
  s_reader = std::thread(ReaderMain);

  s_status = AdapterStatus::Detected;
  NotifyStatusChanged();
  StopAllMotors();
}

// One look for an adapter. The caller holds s_connection_mutex.
void Connect()
{
  const AdapterStatus status_before = s_status;
  // An earlier failure is retried.
  if (s_status == AdapterStatus::Error)
    s_status = AdapterStatus::NotDetected;

  s_ports.fill({});
  s_motors.fill(0);

  if (OpenAdapter())
    StartAdapter();

  // A successful connection has already reported itself.
  if (s_status != AdapterStatus::Detected && s_status != status_before)
    NotifyStatusChanged();
}

void Disconnect()
{
  // Skip if another thread is connecting or disconnecting right now.
  std::unique_lock lock(s_connection_mutex, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  if (s_status != AdapterStatus::Detected)
    return;

  if (s_reader_running.TestAndClear())
    s_reader.join();
  s_reader_started.Clear();

  s_ports.fill({});
  s_status = AdapterStatus::NotDetected;

  s_device.reset();
  NotifyStatusChanged();

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter detached");
}

void ScannerMain()
{
  Common::SetCurrentThreadName("GC Adapter Scanning Thread");
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter scanning thread started");

  while (s_scanner_running.IsSet())
  {
    if (s_device == nullptr)
    {
      std::lock_guard lock(s_connection_mutex);
      Connect();
    }

    // Look twice a second (StopScanning wakes the thread early).
    s_scan_wakeup.WaitFor(500ms);

    if (s_reader_started.IsSet() && s_reads_failing.IsSet())
      Disconnect();
  }

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter scanning thread stopped");
}

void StartScanning()
{
  if (s_scanner_running.IsSet())
    return;
  s_scanner_running.Set(true);
  s_scanner = std::thread(ScannerMain);
}

void StopScanning()
{
  if (!s_scanner_running.TestAndClear())
    return;
  s_scan_wakeup.Set();
  s_scanner.join();
}

// The adapter is only looked for while some port is set to use it.
void OnConfigChanged()
{
  bool wanted = false;
  for (int index = 0; index < PORT_COUNT; ++index)
  {
    if (Config::Get(Config::GetInfoForSIDevice(index)) ==
        SerialInterface::SIDevices::SIDEVICE_WIIU_ADAPTER)
    {
      wanted = true;
    }
    s_rumble_allowed[static_cast<std::size_t>(index)].store(
        Config::Get(Config::GetInfoForAdapterRumble(index)), std::memory_order_relaxed);
  }
  s_adapter_wanted.store(wanted, std::memory_order_relaxed);

  if (wanted)
    StartScanning();
  else
    StopScanning();
}
}  // namespace

void Init()
{
  if (s_device != nullptr)
    return;

  // While a game runs, at most one attempt per emulated second.
  Core::System& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  if (state != Core::State::Uninitialized && state != Core::State::Starting)
  {
    const u64 now = system.GetCoreTiming().GetTicks();
    if ((now - s_last_init_ticks) < system.GetSystemTimers().GetTicksPerSecond())
      return;
    s_last_init_ticks = now;
  }

  s_status = AdapterStatus::NotDetected;

  if (!s_config_callback)
    s_config_callback = Config::AddConfigChangedCallback(OnConfigChanged);
  OnConfigChanged();
}

void Shutdown()
{
  StopScanning();
  Disconnect();

  s_status = AdapterStatus::NotDetected;

  if (s_config_callback)
  {
    Config::RemoveConfigChangedCallback(*s_config_callback);
    s_config_callback.reset();
  }
}

void SetAdapterCallback(std::function<void(void)> func)
{
  s_status_callback = std::move(func);
}

void ResetRumble()
{
  std::unique_lock lock(s_connection_mutex, std::try_to_lock);
  if (lock.owns_lock())
    StopAllMotors();
}

GCPadStatus Input(int chan)
{
  if (!s_adapter_wanted.load(std::memory_order_relaxed))
    return {};
  if (s_device == nullptr || s_status != AdapterStatus::Detected)
    return {};

  std::lock_guard lock(s_ports_mutex);
  Port& port = s_ports[static_cast<std::size_t>(chan)];
  // The first read after a connection gets the origin.
  if (port.origin_pending)
  {
    port.origin_pending = false;
    return port.origin;
  }
  return port.status;
}

void Output(int chan, u8 rumble_command)
{
  const std::size_t index = static_cast<std::size_t>(chan);
  if (!s_adapter_wanted.load(std::memory_order_relaxed) ||
      !s_rumble_allowed[index].load(std::memory_order_relaxed))
  {
    return;
  }
  if (s_device == nullptr)
    return;

  // Wireless (WaveBird) pads have no motor.
  if (rumble_command == s_motors[index] || s_ports[index].type == ControllerType::Wireless)
    return;

  s_motors[index] = rumble_command;
  s_pending_write = MakeRumbleCommand();
  s_pending_write_size.store(static_cast<int>(RUMBLE_COMMAND_SIZE));
  s_write_requested.Set();
}

bool IsDetected(const char** error_message)
{
  const AdapterStatus status = s_status;
  if (status == AdapterStatus::Error)
  {
    if (error_message != nullptr)
    {
      // The text stays valid until the next failed attempt to open the adapter.
      static std::string shown;
      std::lock_guard lock(s_error_mutex);
      shown = s_last_error;
      *error_message = shown.c_str();
    }
    return false;
  }

  if (error_message != nullptr)
    *error_message = nullptr;
  return status == AdapterStatus::Detected;
}

bool DeviceConnected(int chan)
{
  std::lock_guard lock(s_ports_mutex);
  return s_ports[static_cast<std::size_t>(chan)].type != ControllerType::None;
}

void ResetDeviceType(int chan)
{
  std::lock_guard lock(s_ports_mutex);
  s_ports[static_cast<std::size_t>(chan)].type = ControllerType::None;
}

double GetCurrentPollRate()
{
  return s_poll_rate.load(std::memory_order_relaxed);
}
}  // namespace GCAdapter
