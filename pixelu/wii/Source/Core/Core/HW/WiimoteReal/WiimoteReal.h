// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/SPSCQueue.h"
#include "Common/WorkQueueThread.h"

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/USBUtils.h"

class PointerWrap;

// Real Wii Remotes connected to the PC: platform I/O backends find them, each connected remote
// runs its own reader and writer threads, and remotes are handed to the emulated Bluetooth
// slots, to ControllerInterface, or kept briefly in a pool.

namespace WiimoteReal
{
using WiimoteCommon::MAX_PAYLOAD;

// A raw HID report, starting with the HID transaction header byte.
using Report = std::vector<u8>;
constexpr int REPORT_HID_HEADER_SIZE = 1;

constexpr u32 WIIMOTE_DEFAULT_TIMEOUT = 1000;

// In units of 1.28 s; Wii games use 3.
constexpr u8 BLUETOOTH_INQUIRY_LENGTH = 3;

// Upper nibble of the header byte of outgoing reports: SET_REPORT (0x50) for the control channel,
// used on Windows, or DATA (0xA0) for the interrupt channel elsewhere.
#ifdef _WIN32
constexpr u8 WR_SET_REPORT = 0x50;
#else
constexpr u8 WR_SET_REPORT = 0xA0;
#endif

// Lower nibble: the report type.
constexpr u8 BT_INPUT = 0x01;
constexpr u8 BT_OUTPUT = 0x02;

// A real remote. Platform backends derive from this and provide the raw I/O.
class Wiimote : public WiimoteCommon::HIDWiimote
{
public:
  Wiimote(const Wiimote&) = delete;
  Wiimote& operator=(const Wiimote&) = delete;
  Wiimote(Wiimote&&) = delete;
  Wiimote& operator=(Wiimote&&) = delete;

  ~Wiimote() override;

  // Identifies the physical device (e.g. its device path), to avoid connecting it twice.
  virtual std::string GetId() const = 0;

  // Takes the oldest report received from the remote.
  bool GetNextReport(Report* report);

  // Connects and asks the remote whether it carries a balance board extension.
  bool IsBalanceBoard();

  void InterruptDataOutput(const u8* data, const u32 size) override;

  u8 GetWiimoteDeviceIndex() const override;
  void SetWiimoteDeviceIndex(u8 index) override;

  void PrepareInput(WiimoteEmu::DesiredWiimoteState* target_state,
                    SensorBarState sensor_bar_state) override;
  void Update(const WiimoteEmu::DesiredWiimoteState& target_state) override;
  void EventLinked() override;
  void EventUnlinked() override;
  WiimoteCommon::ButtonData GetCurrentlyPressedButtons() override;

  void EmuStop();

  void EmuResume();
  void EmuPause();

  // Starts the I/O threads (connecting the device) and reports whether that worked. `index` is
  // the slot, used for the balance board check and logging.
  bool Connect(int index);

  // Sends the initial reports of a newly assigned remote.
  void Prepare();

  virtual bool IsConnected() const = 0;

  void QueueReport(WiimoteCommon::OutputReportID rpt_id, const void* data, unsigned int size);

  template <typename T>
  void QueueReport(const T& report)
  {
    QueueReport(report.REPORT_ID, &report, sizeof(report));
  }

  int GetIndex() const;

protected:
  // Derived classes must call this from their destructor, while their I/O still works.
  void Shutdown();

  Wiimote();

  int m_index = 0;
  Report m_last_input_report;

  // Whether unlinking the remote closes the connection. Otherwise only reporting is reset, so a
  // button press can link it again (connecting is cumbersome on some platforms).
  bool m_really_disconnect = false;

  u8 m_bt_device_index = 0;

private:
  // An outgoing report and when to send it.
  struct TimedReport
  {
    TimePoint time;
    Report report;
  };

  bool Read();
  bool Write(const TimedReport& timed_report);

  void StartThread();
  void StopThread();

  void ResetDataReporting();

  virtual void EnablePowerAssertionInternal() {}
  virtual void DisablePowerAssertionInternal() {}

  virtual bool ConnectInternal() = 0;
  virtual void DisconnectInternal() = 0;

  Report& ProcessReadQueue(bool repeat_last_data_report);
  void ClearReadQueue();
  void WriteReport(Report rpt);

  // Platform I/O. IORead returns the length read, 0 on failure or -1 when nothing was read.
  virtual int IORead(u8* buf) = 0;
  virtual int IOWrite(u8 const* buf, size_t len) = 0;

  // Makes a blocked IORead return.
  virtual void IOWakeup() = 0;

  void ReadThreadFunc();
  void WriteThreadFunc();

  void RefreshConfig();

  std::atomic<bool> m_is_linked = false;

  // Speaker state as last set by the game; speaker data that cannot play is turned into
  // (shorter) rumble reports.
  bool m_speaker_enable = false;
  bool m_speaker_mute = false;

  // Last rumble state sent, to drop rumble reports that change nothing.
  bool m_rumble_state = false;

  // The writer thread (which also connects and runs the reader thread).
  std::thread m_write_thread;
  Common::Flag m_run_thread;
  // Set once the writer thread has tried to connect.
  Common::Event m_thread_ready_event;

  Common::SPSCQueue<Report> m_read_reports;
  Common::SPSCQueue<TimedReport> m_write_reports;
  // Wakes the writer thread.
  Common::Event m_write_event;

  bool m_speaker_enabled_in_dolphin_config = false;
  int m_balance_board_dump_port = 0;

  Config::ConfigChangedCallbackID m_config_changed_callback_id;
};

// A platform's way of finding remotes.
class WiimoteScannerBackend
{
public:
  virtual ~WiimoteScannerBackend() = default;

  // Called on each scanner iteration.
  virtual void Update() = 0;
  // Asks a blocking search to return early.
  virtual void RequestStopSearching() = 0;

  struct FindResults
  {
    std::vector<std::unique_ptr<Wiimote>> wii_remotes;
    std::vector<std::unique_ptr<Wiimote>> balance_boards;
  };

  // Searches for new remotes (with a Bluetooth inquiry where the platform can). Returns only
  // remotes not already in use.
  virtual FindResults FindNewWiimotes() { return FindAttachedWiimotes(); }

  // Returns remotes that are already connected to the PC but not in use (DolphinBar
  // interfaces, for instance).
  virtual FindResults FindAttachedWiimotes() { return {}; }
};

enum class WiimoteScanMode
{
  DO_NOT_SCAN,
  CONTINUOUSLY_SCAN,
  SCAN_ONCE
};

// Runs the backends on a scanning thread, plus a thread that maintains the pool.
class WiimoteScanner
{
public:
  WiimoteScanner() = default;
  void StartThread();
  void StopThread();
  void SetScanMode(WiimoteScanMode scan_mode);
  void PopulateDevices();

private:
  void ThreadFunc();
  void PoolThreadFunc();

  std::vector<std::unique_ptr<WiimoteScannerBackend>> m_backends;
  mutable std::mutex m_backends_mutex;

  std::thread m_scan_thread;
  Common::Flag m_scan_thread_running;
  Common::Flag m_populate_devices;
  Common::Event m_scan_mode_changed_or_population_event;
  std::atomic<WiimoteScanMode> m_scan_mode{WiimoteScanMode::DO_NOT_SCAN};
};

// Recursive because ControllerInterface may call AddWiimoteToPool from ProcessWiimotePool.
extern std::recursive_mutex g_wiimotes_mutex;
// The real remotes assigned to the emulated slots.
extern std::unique_ptr<Wiimote> g_wiimotes[MAX_BBMOTES];

void AddWiimoteToPool(std::unique_ptr<Wiimote>);

bool IsValidDeviceName(std::string_view name);
bool IsWiimoteName(std::string_view name);
bool IsBalanceBoardName(std::string_view name);

bool IsNewWiimote(const std::string& identifier);

bool IsKnownDeviceId(const USBUtils::DeviceInfo&);

void HandleWiimoteSourceChange(unsigned int wiimote_number);

#ifdef ANDROID
void InitAdapterClass();
#endif

void HandleWiimotesInControllerInterfaceSettingChange();
void PopulateDevices();
void ProcessWiimotePool();
}  // namespace WiimoteReal
