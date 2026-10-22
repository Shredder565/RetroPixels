// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteReal/WiimoteReal.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <unordered_set>

#include <SFML/Network/UdpSocket.hpp>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Swap.h"
#include "Common/Thread.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteReal/IOAndroid.h"
#include "Core/HW/WiimoteReal/IOLinux.h"
#include "Core/HW/WiimoteReal/IOWin.h"
#include "Core/HW/WiimoteReal/IOhidapi.h"
#include "Core/System.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Wiimote/WiimoteController.h"

namespace WiimoteReal
{
using namespace WiimoteCommon;

std::recursive_mutex g_wiimotes_mutex;

// Assigned remotes. Only changed on the CPU thread (CPUThreadGuard) with g_wiimotes_mutex held.
std::unique_ptr<Wiimote> g_wiimotes[MAX_BBMOTES];

namespace
{
bool s_real_wiimotes_initialized = false;

// Ids of the devices we have objects for, so that no device is connected twice.
std::unordered_set<std::string> s_known_ids;
std::mutex s_known_ids_mutex;

void RememberId(const std::string& id)
{
  std::lock_guard lock(s_known_ids_mutex);
  s_known_ids.insert(id);
}

// Connected remotes that no slot wants yet. They stay for a few seconds, flashing their LEDs,
// then get disconnected.
struct PoolEntry
{
  std::unique_ptr<Wiimote> wiimote;
  Clock::time_point added = Clock::now();

  bool HasExpired() const { return Clock::now() - added > std::chrono::seconds{5}; }
};
std::vector<PoolEntry> s_wiimote_pool;

WiimoteScanner s_wiimote_scanner;

// Slot number given to pooled remotes; only shows up in logs.
constexpr int POOL_INDEX = 99;

bool WantsRealWiimote(unsigned int slot)
{
  return Config::Get(Config::GetInfoForWiimoteSource(static_cast<int>(slot))) ==
         WiimoteSource::Real;
}

void HandleWiimoteDisconnect(unsigned int slot)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  // Keep the object alive until UpdateSource has detached it from the emulated Bluetooth
  // device, which still points at it.
  const std::unique_ptr<Wiimote> leaving = std::move(g_wiimotes[slot]);
  WiimoteCommon::UpdateSource(slot);
}

// Scanner thread, or the UI thread on a source change.
bool TryToConnectWiimoteToSlot(std::unique_ptr<Wiimote>& wiimote, unsigned int slot)
{
  if (!WantsRealWiimote(slot) || g_wiimotes[slot])
    return false;

  if (!wiimote->Connect(int(slot)))
  {
    ERROR_LOG_FMT(WIIMOTE, "Could not connect a real Wii Remote to slot {}", slot + 1);
    return false;
  }

  wiimote->Prepare();

  // Light the slot's LED (the balance board slot lights the first one).
  OutputReportLeds leds = {};
  leds.leds = u8(1 << (slot % WIIMOTE_BALANCE_BOARD));
  wiimote->QueueReport(leds);

  {
    const Core::CPUThreadGuard guard(Core::System::GetInstance());
    g_wiimotes[slot] = std::move(wiimote);
    WiimoteCommon::UpdateSource(slot);
  }

  NOTICE_LOG_FMT(WIIMOTE, "Real Wii Remote connected to slot {}", slot + 1);
  return true;
}

void TryToConnectBalanceBoard(std::unique_ptr<Wiimote> board)
{
  if (!TryToConnectWiimoteToSlot(board, WIIMOTE_BALANCE_BOARD))
    NOTICE_LOG_FMT(WIIMOTE, "No free slot for a real balance board");
}

// Fills an empty "Real" slot from the pool, taking a remote back from ControllerInterface if
// the pool is empty.
void TryToFillWiimoteSlot(u32 slot)
{
  std::lock_guard lock(g_wiimotes_mutex);

  if (g_wiimotes[slot] || !WantsRealWiimote(slot))
    return;

  if (s_wiimote_pool.empty())
    ciface::WiimoteController::ReleaseDevices(1);
  if (s_wiimote_pool.empty())
    return;

  if (TryToConnectWiimoteToSlot(s_wiimote_pool.front().wiimote, slot))
    s_wiimote_pool.erase(s_wiimote_pool.begin());
}

unsigned int CalculateWantedWiimotes()
{
  std::lock_guard lock(g_wiimotes_mutex);
  unsigned int wanted = 0;
  for (unsigned int slot = 0; slot != MAX_WIIMOTES; ++slot)
  {
    if (WantsRealWiimote(slot) && !g_wiimotes[slot])
      ++wanted;
  }
  return wanted;
}

unsigned int CalculateWantedBB()
{
  std::lock_guard lock(g_wiimotes_mutex);
  const bool wanted = Config::Get(Config::WIIMOTE_BB_SOURCE) == WiimoteSource::Real &&
                      !g_wiimotes[WIIMOTE_BALANCE_BOARD];
  return wanted ? 1 : 0;
}

void CheckForDisconnectedWiimotes()
{
  std::lock_guard lock(g_wiimotes_mutex);
  for (unsigned int slot = 0; slot != MAX_BBMOTES; ++slot)
  {
    if (g_wiimotes[slot] && !g_wiimotes[slot]->IsConnected())
      HandleWiimoteDisconnect(slot);
  }
}

// Data reports (0x30 and up) can be repeated or skipped; other reports must all be delivered.
bool IsDataReport(const Report& report)
{
  return report.size() >= 2 && report[1] >= u8(InputReportID::ReportCore);
}

// Sends a copy of a report to a local UDP port, for analysing balance board traffic.
void DumpToPort(const Report& report, int port)
{
  static sf::UdpSocket socket;
  (void)socket.send(report.data(), report.size(), sf::IpAddress::LocalHost,
                    static_cast<unsigned short>(port));
}

template <typename Func>
void ForEachConnected(Func&& func)
{
  for (auto& wiimote : g_wiimotes)
  {
    if (wiimote && wiimote->IsConnected())
      func(*wiimote);
  }
}

void RequestPopulateDevices()
{
  g_controller_interface.PlatformPopulateDevices([] { ProcessWiimotePool(); });
}
}  // namespace

void PopulateDevices()
{
  // Done on the scanner thread, which avoids a rare deadlock.
  s_wiimote_scanner.PopulateDevices();
}

// Fills the "Real" slots, then hands the rest of the pool to ControllerInterface (or takes
// remotes back from it) depending on the setting.
void ProcessWiimotePool()
{
  std::lock_guard lock(g_wiimotes_mutex);

  for (u32 slot = 0; slot != MAX_WIIMOTES; ++slot)
    TryToFillWiimoteSlot(slot);

  if (!Config::Get(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE))
  {
    ciface::WiimoteController::ReleaseDevices();
    return;
  }

  for (PoolEntry& entry : s_wiimote_pool)
    ciface::WiimoteController::AddDevice(std::move(entry.wiimote));
  s_wiimote_pool.clear();
}

void AddWiimoteToPool(std::unique_ptr<Wiimote> wiimote)
{
  if (!wiimote->Connect(POOL_INDEX))
  {
    ERROR_LOG_FMT(WIIMOTE, "Could not connect a real Wii Remote");
    return;
  }

  wiimote->EmuStop();

  std::lock_guard lock(g_wiimotes_mutex);
  s_wiimote_pool.emplace_back(PoolEntry{std::move(wiimote)});
}

Wiimote::Wiimote()
{
  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();
}

Wiimote::~Wiimote()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
}

void Wiimote::Shutdown()
{
  std::lock_guard lock(s_known_ids_mutex);
  s_known_ids.erase(GetId());

  StopThread();
  ClearReadQueue();
}

// CPU thread (or construction/destruction).
void Wiimote::WriteReport(Report rpt)
{
  // Track rumble and speaker state from every report with a payload.
  if (rpt.size() >= 3)
  {
    const bool rumble = (rpt[2] & 0x1) != 0;
    const auto id = OutputReportID(rpt[1]);

    // A rumble report that changes nothing is dropped.
    if (id == OutputReportID::Rumble && rumble == m_rumble_state)
      return;
    if (id == OutputReportID::SpeakerEnable)
      m_speaker_enable = (rpt[2] & 0x4) != 0;
    else if (id == OutputReportID::SpeakerMute)
      m_speaker_mute = (rpt[2] & 0x4) != 0;

    m_rumble_state = rumble;
  }

  // From the CPU thread a report goes out at the host time matching the emulated time (this
  // paces speaker data); from anywhere else, right away.
  auto& core_timing = Core::System::GetInstance().GetCoreTiming();
  const TimePoint when =
      Core::IsCPUThread() ? core_timing.GetTargetHostTime(core_timing.GetTicks()) : Clock::now();

  m_write_reports.Emplace(when, std::move(rpt));
  m_write_event.Set();
}

// CPU thread.
void Wiimote::QueueReport(WiimoteCommon::OutputReportID rpt_id, const void* data, unsigned int size)
{
  const auto* const payload = static_cast<const u8*>(data);

  Report rpt;
  rpt.reserve(size + 2);
  rpt.push_back(WR_SET_REPORT | BT_OUTPUT);
  rpt.push_back(u8(rpt_id));
  rpt.insert(rpt.end(), payload, payload + size);
  WriteReport(std::move(rpt));
}

void Wiimote::ResetDataReporting()
{
  m_last_input_report.clear();

  // Back to the power-on state: non-continuous core reports (this also stops rumble).
  OutputReportMode mode = {};
  mode.mode = InputReportID::ReportCore;
  mode.continuous = 0;
  QueueReport(mode);
}

void Wiimote::ClearReadQueue()
{
  // SPSCQueue::Clear is not thread safe, so drain it instead.
  Report discarded;
  while (m_read_reports.Pop(discarded))
  {
  }
}

void Wiimote::EventLinked()
{
  m_is_linked = true;

  // Drop whatever arrived while unlinked and start from plain core reports.
  ClearReadQueue();
  ResetDataReporting();
  EnablePowerAssertionInternal();
}

void Wiimote::EventUnlinked()
{
  m_really_disconnect ? DisconnectInternal() : EmuStop();
}

// The power assertion keeps the host from sleeping while a remote is in use.
void Wiimote::EmuResume()
{
  EnablePowerAssertionInternal();
}

void Wiimote::EmuPause()
{
  DisablePowerAssertionInternal();
}

void Wiimote::InterruptDataOutput(const u8* data, const u32 size)
{
  // Send as SET_REPORT rather than DATA: Nintendo's remotes accept both, third-party ones need
  // this.
  Report rpt(size + REPORT_HID_HEADER_SIZE);
  rpt[0] = WR_SET_REPORT | BT_OUTPUT;
  std::copy_n(data, size, rpt.begin() + REPORT_HID_HEADER_SIZE);

  const auto id = OutputReportID(rpt[1]);
  if (id == OutputReportID::LED)
  {
    // With every LED off a connected remote looks disconnected, so light them all instead.
    auto& leds = *reinterpret_cast<OutputReportLeds*>(&rpt[2]);
    if (leds.leds == 0)
      leds.leds = 0xf;
  }
  else if (id == OutputReportID::SpeakerData)
  {
    const bool speaker_plays =
        m_speaker_enabled_in_dolphin_config && m_speaker_enable && !m_speaker_mute;
    if (!speaker_plays)
    {
      // Keep only the rumble bit, as a rumble report.
      rpt.resize(3);
      rpt[1] = u8(OutputReportID::Rumble);
      rpt[2] &= 0x1;
    }
  }

  WriteReport(std::move(rpt));
}

bool Wiimote::Read()
{
  Report rpt(MAX_PAYLOAD);
  const int length = IORead(rpt.data());

  // Reports are only kept while linked.
  if (length > 0 && m_is_linked)
  {
    if (m_balance_board_dump_port > 0 && m_index == WIIMOTE_BALANCE_BOARD)
      DumpToPort(rpt, m_balance_board_dump_port);

    rpt.resize(static_cast<std::size_t>(length));
    m_read_reports.Push(std::move(rpt));
  }

  return length != 0;
}

bool Wiimote::Write(const TimedReport& timed_report)
{
  const Report& rpt = timed_report.report;

  if (m_balance_board_dump_port > 0 && m_index == WIIMOTE_BALANCE_BOARD)
    DumpToPort(rpt, m_balance_board_dump_port);

  // Mostly matters for speaker data (and helps little even there).
  std::this_thread::sleep_until(timed_report.time);

  return IOWrite(rpt.data(), rpt.size()) != 0;
}

bool Wiimote::IsBalanceBoard()
{
  if (!ConnectInternal())
    return false;

  constexpr u8 HEADER = WR_SET_REPORT | BT_OUTPUT;

  // Initialize the extension (write 0x55 to 0xa400f0, then 0x00 to 0xa400fb), as full size
  // zero padded reports.
  // TODO: Build these from the report structs.
  static const u8 init_step_1[MAX_PAYLOAD] = {
      HEADER, u8(OutputReportID::WriteData), 0x04, 0xa4, 0x00, 0xf0, 0x01, 0x55};
  static const u8 init_step_2[MAX_PAYLOAD] = {
      HEADER, u8(OutputReportID::WriteData), 0x04, 0xa4, 0x00, 0xfb, 0x01, 0x00};
  if (!IOWrite(init_step_1, sizeof(init_step_1)) || !IOWrite(init_step_2, sizeof(init_step_2)))
  {
    ERROR_LOG_FMT(WIIMOTE, "Balance board check: could not initialize the extension");
    return false;
  }

  // Ask for the status; once it shows an extension, read two identifier bytes at 0xa400fe.
  static const u8 request_status[] = {HEADER, u8(OutputReportID::RequestStatus), 0};
  static const u8 read_identifier[] = {
      HEADER, u8(OutputReportID::ReadData), 0x04, 0xa4, 0x00, 0xfe, 0x02, 0x00};

  int result = IOWrite(request_status, sizeof(request_status));
  u8 buf[MAX_PAYLOAD] = {};
  while (result != 0)
  {
    result = IORead(buf);
    if (result == -1)
      continue;

    const u8* const payload = &buf[2];
    switch (InputReportID(buf[1]))
    {
    case InputReportID::Status:
      // Only remotes with an extension can be a balance board.
      if (!reinterpret_cast<const InputReportStatus*>(payload)->extension)
        return false;
      result = IOWrite(read_identifier, sizeof(read_identifier));
      break;

    case InputReportID::ReadDataReply:
    {
      const auto* reply = reinterpret_cast<const InputReportReadDataReply*>(payload);
      const u16 address = Common::swap16(reply->address);
      if (address != 0x00fe)
      {
        ERROR_LOG_FMT(WIIMOTE, "Balance board check: unexpected read reply for {:X}", address);
        return false;
      }
      // The balance board identifies as 04 02.
      return reply->data[0] == 0x04 && reply->data[1] == 0x02;
    }

    case InputReportID::Ack:
    {
      const auto* ack = reinterpret_cast<const InputReportAck*>(payload);
      if (ack->rpt_id == OutputReportID::ReadData && ack->error_code != ErrorCode::Success)
      {
        WARN_LOG_FMT(WIIMOTE, "Balance board check: reading 0xa400fe failed, so not a board");
        return false;
      }
      break;
    }

    default:
      break;
    }
  }
  return false;
}

bool Wiimote::GetNextReport(Report* report)
{
  return m_read_reports.Pop(*report);
}

// Picks the report to deliver this update: everything up to and including the first non-data
// report, of which only the last is kept. Optionally the last data report is delivered again
// when nothing new arrived.
Report& Wiimote::ProcessReadQueue(bool repeat_last_data_report)
{
  const bool keep_last = repeat_last_data_report && IsDataReport(m_last_input_report);
  if (!keep_last)
    m_last_input_report.clear();

  while (GetNextReport(&m_last_input_report) && IsDataReport(m_last_input_report))
  {
  }

  return m_last_input_report;
}

u8 Wiimote::GetWiimoteDeviceIndex() const
{
  return m_bt_device_index;
}

void Wiimote::SetWiimoteDeviceIndex(u8 index)
{
  m_bt_device_index = index;
}

void Wiimote::PrepareInput(WiimoteEmu::DesiredWiimoteState*, SensorBarState)
{
  // A real remote produces its own input.
}

void Wiimote::Update(const WiimoteEmu::DesiredWiimoteState&)
{
  // Remotes report at 200 Hz only once the Bluetooth link is in sniff mode, which PC stacks do
  // not use, so they send 100 Hz. Games want close to 200 Hz before they send speaker data, and
  // some show input problems (e.g. Super Mario Galaxy's cursor flickering), so data reports can
  // be repeated to fake 200 Hz. That in turn upsets motion gesture detection in some games
  // (e.g. Sonic and the Secret Rings, Jett Rocket), hence the setting.
  const bool repeat = Config::Get(Config::MAIN_REAL_WII_REMOTE_REPEAT_REPORTS);

  const Report& rpt = ProcessReadQueue(repeat);
  if (rpt.empty())
    return;

  InterruptCallback(rpt.front(), rpt.data() + REPORT_HID_HEADER_SIZE,
                    u32(rpt.size() - REPORT_HID_HEADER_SIZE));
}

ButtonData Wiimote::GetCurrentlyPressedButtons()
{
  // Only data reports are looked at (other reports carry buttons too, but that is not used).
  const Report& rpt = m_last_input_report;
  if (rpt.size() < 4)
    return ButtonData{};

  const auto mode = InputReportID(rpt[1]);
  if (!DataReportBuilder::IsValidMode(mode))
    return ButtonData{};

  ButtonData buttons = {};
  MakeDataReportManipulator(mode, const_cast<u8*>(rpt.data()) + 2)->GetCoreData(&buttons);
  return buttons;
}

void Wiimote::Prepare()
{
  const TimePoint now = Clock::now();

  // Non-continuous core reports, with rumble on...
  m_write_reports.Emplace(now, Report{WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::ReportMode), 1,
                                      u8(InputReportID::ReportCore)});

  // ...and 200 ms later a status request, which turns rumble off again.
  m_write_reports.Emplace(now + std::chrono::milliseconds{200},
                          Report{WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::RequestStatus), 0});

  m_write_event.Set();
}

void Wiimote::EmuStop()
{
  // Incoming reports are dropped from here on.
  m_is_linked = false;

  ResetDataReporting();
  DisablePowerAssertionInternal();
}

void WiimoteScanner::StartThread()
{
  if (m_scan_thread_running.IsSet())
    return;

  m_scan_thread_running.Set();
  m_scan_thread = std::thread(&WiimoteScanner::ThreadFunc, this);
}

void WiimoteScanner::StopThread()
{
  if (!m_scan_thread_running.IsSet())
    return;

  SetScanMode(WiimoteScanMode::DO_NOT_SCAN);
  for (const auto& backend : m_backends)
    backend->RequestStopSearching();

  m_scan_thread_running.Clear();
  m_scan_thread.join();
}

void WiimoteScanner::SetScanMode(WiimoteScanMode scan_mode)
{
  m_scan_mode.store(scan_mode);
  m_scan_mode_changed_or_population_event.Set();
}

void WiimoteScanner::PoolThreadFunc()
{
  Common::SetCurrentThreadName("Wiimote Pool Thread");

  // Pooled remotes alternate between LEDs 1+3 and 2+4.
  u8 led_pattern = 0b1010;

  auto wake_time = std::chrono::steady_clock::now();
  while (m_scan_thread_running.IsSet())
  {
    std::this_thread::sleep_until(wake_time);
    wake_time += std::chrono::milliseconds(250);

    std::lock_guard lock(g_wiimotes_mutex);

    std::erase_if(s_wiimote_pool, [](const PoolEntry& entry) {
      if (!entry.wiimote->IsConnected())
      {
        INFO_LOG_FMT(WIIMOTE, "Dropping a disconnected Wii Remote from the pool");
        return true;
      }
      if (entry.HasExpired())
      {
        INFO_LOG_FMT(WIIMOTE, "Dropping an unused Wii Remote from the pool");
        return true;
      }
      return false;
    });

    for (PoolEntry& entry : s_wiimote_pool)
    {
      OutputReportLeds leds = {};
      leds.leds = led_pattern;
      entry.wiimote->QueueReport(leds);
    }

    led_pattern ^= 0b1111;
  }
}

void WiimoteScanner::PopulateDevices()
{
  m_populate_devices.Set();
  m_scan_mode_changed_or_population_event.Set();
}

void WiimoteScanner::ThreadFunc()
{
  std::thread pool_thread(&WiimoteScanner::PoolThreadFunc, this);

  Common::SetCurrentThreadName("Wiimote Scanning Thread");
  NOTICE_LOG_FMT(WIIMOTE, "Wii Remote scanner started");

  // The backends live entirely on this thread (macOS' hidapi crashes when its manager is
  // created and closed on different threads).
  {
    std::lock_guard lock(m_backends_mutex);
    m_backends.emplace_back(std::make_unique<WiimoteScannerLinux>());
    m_backends.emplace_back(std::make_unique<WiimoteScannerAndroid>());
    m_backends.emplace_back(std::make_unique<WiimoteScannerWindows>());
    m_backends.emplace_back(std::make_unique<WiimoteScannerHidapi>());
  }

  while (m_scan_thread_running.IsSet())
  {
    m_scan_mode_changed_or_population_event.WaitFor(std::chrono::milliseconds(500));

    if (m_populate_devices.TestAndClear())
      RequestPopulateDevices();

    for (const auto& backend : m_backends)
      backend->Update();

    CheckForDisconnectedWiimotes();

    // Remotes are only needed for ControllerInterface or for empty "Real" slots, and never
    // with Bluetooth passthrough or while a GameCube game runs.
    if (!Config::Get(Config::MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE))
    {
      auto& system = Core::System::GetInstance();
      const bool core_running = Core::GetState(system) != Core::State::Uninitialized;
      if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED) ||
          (core_running && !system.IsWii()))
      {
        continue;
      }

      if (CalculateWantedWiimotes() == 0 && CalculateWantedBB() == 0)
        continue;
    }

    // A one-time scan turns itself off. Without scanning, already attached remotes (Windows,
    // hidapi, DolphinBar) are still picked up.
    auto mode = WiimoteScanMode::SCAN_ONCE;
    m_scan_mode.compare_exchange_strong(mode, WiimoteScanMode::DO_NOT_SCAN);
    const bool inquiry = mode != WiimoteScanMode::DO_NOT_SCAN;

    for (const auto& backend : m_backends)
    {
      auto found = inquiry ? backend->FindNewWiimotes() : backend->FindAttachedWiimotes();

      std::unique_lock lock(g_wiimotes_mutex);

      for (auto& wiimote : found.wii_remotes)
      {
        RememberId(wiimote->GetId());
        AddWiimoteToPool(std::move(wiimote));
        RequestPopulateDevices();
      }

      for (auto& board : found.balance_boards)
      {
        RememberId(board->GetId());
        TryToConnectBalanceBoard(std::move(board));
      }
    }
  }

  {
    std::lock_guard lock(m_backends_mutex);
    m_backends.clear();
  }

  pool_thread.join();

  NOTICE_LOG_FMT(WIIMOTE, "Wii Remote scanner stopped");
}

bool Wiimote::Connect(int index)
{
  m_index = index;
  StartThread();
  return IsConnected();
}

void Wiimote::StartThread()
{
  if (m_write_thread.joinable())
    return;

  // The writer thread connects, then starts the reader thread.
  m_run_thread.Set();
  m_write_thread = std::thread(&Wiimote::WriteThreadFunc, this);

  m_thread_ready_event.Wait();
}

void Wiimote::StopThread()
{
  if (!m_write_thread.joinable())
    return;

  // The writer thread stops the reader thread on its way out.
  m_run_thread.Clear();
  m_write_event.Set();
  m_write_thread.join();
}

void Wiimote::ReadThreadFunc()
{
  Common::SetCurrentThreadName("Wiimote Read Thread");

  while (m_run_thread.IsSet())
  {
    if (Read())
      continue;

    WARN_LOG_FMT(WIIMOTE, "Reading from Wii Remote {} failed", m_index + 1);
    m_run_thread.Clear();
    m_write_event.Set();
    break;
  }
}

void Wiimote::WriteThreadFunc()
{
  Common::SetCurrentThreadName("Wiimote Write Thread");

  // A second attempt after a short pause, as connections can take a moment to settle.
  bool connected = ConnectInternal();
  if (!connected)
  {
    Common::SleepCurrentThread(100);
    connected = ConnectInternal();
  }

  m_thread_ready_event.Set();

  if (!connected)
    return;

  std::thread read_thread{&Wiimote::ReadThreadFunc, this};

  // Disconnects only show up when writing (on Windows and with the DolphinBar), so after a
  // second without writes a "rumble off" report is sent. It also keeps rumble from sticking.
  constexpr auto IDLE_WRITE_INTERVAL = std::chrono::milliseconds{1000};

  TimePoint last_write = Clock::now();

  while (m_run_thread.IsSet())
  {
    bool written;

    if (!m_write_reports.Empty())
    {
      written = Write(m_write_reports.Front());
      m_write_reports.Pop();
    }
    else if (Clock::now() - last_write >= IDLE_WRITE_INTERVAL)
    {
      DEBUG_LOG_FMT(WIIMOTE, "Idle write test on Wii Remote {}", m_index + 1);
      const u8 rumble_off[] = {WR_SET_REPORT | BT_OUTPUT, u8(OutputReportID::Rumble), 0x00};
      written = IOWrite(std::data(rumble_off), std::size(rumble_off)) > 0;
    }
    else
    {
      m_write_event.WaitFor(IDLE_WRITE_INTERVAL);
      continue;
    }

    last_write = Clock::now();

    if (!written)
    {
      WARN_LOG_FMT(WIIMOTE, "Writing to Wii Remote {} failed", m_index + 1);
      m_run_thread.Clear();
      break;
    }
  }

  IOWakeup();
  read_thread.join();

  NOTICE_LOG_FMT(WIIMOTE, "Disconnecting Wii Remote {}", m_index + 1);
  DisconnectInternal();
}

void Wiimote::RefreshConfig()
{
  m_speaker_enabled_in_dolphin_config = Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER);
  m_balance_board_dump_port = Config::Get(Config::MAIN_BB_DUMP_PORT);
}

int Wiimote::GetIndex() const
{
  return m_index;
}

// Also called by the config dialog when settings change.
void Initialize(::Wiimote::InitializeMode init_mode)
{
  if (!s_real_wiimotes_initialized)
    s_wiimote_scanner.StartThread();

  s_wiimote_scanner.SetScanMode(Config::Get(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING) ?
                                    WiimoteScanMode::CONTINUOUSLY_SCAN :
                                    WiimoteScanMode::DO_NOT_SCAN);

  // Savestates need the remotes present, so optionally wait (up to ten seconds) for them.
  if (init_mode == ::Wiimote::InitializeMode::DO_WAIT_FOR_WIIMOTES)
  {
    s_wiimote_scanner.SetScanMode(WiimoteScanMode::SCAN_ONCE);
    for (int tries = 100; tries != 0 && CalculateWantedWiimotes() != 0; --tries)
      Common::SleepCurrentThread(100);
  }

  if (s_real_wiimotes_initialized)
    return;

  NOTICE_LOG_FMT(WIIMOTE, "WiimoteReal::Initialize");
  s_real_wiimotes_initialized = true;
}

// Emulation stops.
void Stop()
{
  ForEachConnected([](Wiimote& wiimote) { wiimote.EmuStop(); });
}

// The application exits.
void Shutdown()
{
  s_real_wiimotes_initialized = false;
  s_wiimote_scanner.StopThread();

  NOTICE_LOG_FMT(WIIMOTE, "WiimoteReal::Shutdown");

  std::lock_guard lock(g_wiimotes_mutex);
  for (unsigned int slot = 0; slot != MAX_BBMOTES; ++slot)
    HandleWiimoteDisconnect(slot);

  // Also take the remotes back from ControllerInterface and drop the pool.
  ciface::WiimoteController::ReleaseDevices();
  s_wiimote_pool.clear();
}

void Resume()
{
  ForEachConnected([](Wiimote& wiimote) { wiimote.EmuResume(); });
}

void Pause()
{
  ForEachConnected([](Wiimote& wiimote) { wiimote.EmuPause(); });
}

// GUI thread.
void Refresh()
{
  if (!Config::Get(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING))
    s_wiimote_scanner.SetScanMode(WiimoteScanMode::SCAN_ONCE);
}

bool IsValidDeviceName(std::string_view name)
{
  return IsWiimoteName(name) || IsBalanceBoardName(name);
}

// Like the Wii, only the start of the Bluetooth name is checked.
bool IsWiimoteName(std::string_view name)
{
  return name.starts_with("Nintendo RVL-CNT");
}

bool IsBalanceBoardName(std::string_view name)
{
  return name.starts_with("Nintendo RVL-WBC");
}

// Scanner backends (on the scanner thread).
bool IsNewWiimote(const std::string& identifier)
{
  std::lock_guard lock(s_known_ids_mutex);
  return !s_known_ids.contains(identifier);
}

// Nintendo's USB vendor id with the Wii Remote and Wii Remote Plus product ids.
bool IsKnownDeviceId(const USBUtils::DeviceInfo& device_info)
{
  constexpr u16 NINTENDO_VID = 0x057e;
  return device_info.vid == NINTENDO_VID &&
         (device_info.pid == 0x0306 || device_info.pid == 0x0330);
}

// A slot stopped being "Real": its remote goes back to the pool (balance boards are not
// pooled).
void HandleWiimoteSourceChange(unsigned int index)
{
  std::lock_guard lock(g_wiimotes_mutex);

  {
    const Core::CPUThreadGuard guard(Core::System::GetInstance());
    std::unique_ptr<Wiimote> released = std::move(g_wiimotes[index]);
    if (released && index != WIIMOTE_BALANCE_BOARD)
      AddWiimoteToPool(std::move(released));
  }

  RequestPopulateDevices();
}

void HandleWiimotesInControllerInterfaceSettingChange()
{
  RequestPopulateDevices();
}

}  // namespace WiimoteReal
