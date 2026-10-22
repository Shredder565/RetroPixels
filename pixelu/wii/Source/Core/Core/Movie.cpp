// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Movie.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <locale>
#include <mbedtls/md.h>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Common/VariantUtil.h"
#include "Common/Version.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigLoaders/MovieConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DSP/DSPCore.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "InputCommon/GCPadStatus.h"

#include "VideoCommon/VideoBackendBase.h"

namespace Movie
{
using WiimoteEmu::Classic;
using WiimoteEmu::DesiredWiimoteState;
using WiimoteEmu::Nunchuk;
using WiimoteEmu::SerializedWiimoteState;

namespace
{
constexpr std::array<u8, 4> DTM_MAGIC{'D', 'T', 'M', 0x1A};
constexpr size_t HEADER_SIZE = sizeof(DTMHeader);
constexpr size_t PAD_RECORD_SIZE = sizeof(ControllerState);
constexpr size_t FIRST_WIIMOTE_LINE = 4;

// Name of the state that a recording started from a running game begins with.
std::string RecordingStartStatePath()
{
  return File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav";
}

bool WiimoteSourceEnabled(int index)
{
  return Config::Get(Config::GetInfoForWiimoteSource(index)) != WiimoteSource::None;
}

// The header stores the build's git hash as 20 raw bytes, two hex digits per
// byte. A revision string that is not pairs of hex digits is stored as text.
std::array<u8, 20> RevisionToHeaderBytes(const std::string& revision)
{
  std::array<u8, 20> out{};
  const bool is_hex_pairs =
      revision.size() % 2 == 0 && std::ranges::all_of(revision, Common::IsXDigit);
  if (!is_hex_pairs)
  {
    std::copy_n(revision.begin(), std::min(revision.size(), out.size()), out.begin());
    return out;
  }

  const auto nibble = [](char c) -> u8 {
    if (c >= '0' && c <= '9')
      return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<u8>(c - 'a' + 10);
    return static_cast<u8>(c - 'A' + 10);
  };
  const size_t count = std::min(revision.size() / 2, out.size());
  for (size_t n = 0; n < count; ++n)
    out[n] = static_cast<u8>((nibble(revision[2 * n]) << 4) | nibble(revision[2 * n + 1]));
  return out;
}

// A 2D analog value. When both axes sit at an edge or the centre, it is
// written as directions ("LEFT", "UP"...), otherwise as raw numbers.
std::string StickText(u32 x, u32 y, const std::string& prefix, u32 range = 255)
{
  const u32 center = range / 2 + 1;
  const auto is_extreme = [&](u32 v) { return v <= 1 || v == center || v >= range; };
  if (!is_extreme(x) || !is_extreme(y))
    return fmt::format("{}:{},{}", prefix, x, y);

  const char* const horizontal = x == center ? nullptr : (x < center ? "LEFT" : "RIGHT");
  const char* const vertical = y == center ? nullptr : (y < center ? "DOWN" : "UP");
  if (horizontal && vertical)
    return fmt::format("{}:{},{}", prefix, horizontal, vertical);
  if (horizontal)
    return fmt::format("{}:{}", prefix, horizontal);
  if (vertical)
    return fmt::format("{}:{}", prefix, vertical);
  return {};
}

// A 1D analog value: nothing at rest, the bare name when fully pressed.
std::string TriggerText(u32 value, const std::string& prefix, u32 range = 255)
{
  if (value == 0)
    return {};
  if (value == range)
    return prefix;
  return fmt::format("{}:{}", prefix, value);
}

void AppendPressed(std::string& out, std::initializer_list<std::pair<bool, const char*>> buttons)
{
  for (const auto& [pressed, label] : buttons)
  {
    if (pressed)
      out += label;
  }
}

std::string PadDisplayLine(const ControllerState& s, int port)
{
  std::string line = fmt::format("P{}:", port + 1);
  if (!s.is_connected)
    return line + " DISCONNECTED";

  AppendPressed(line, {{s.A, " A"},
                       {s.B, " B"},
                       {s.X, " X"},
                       {s.Y, " Y"},
                       {s.Z, " Z"},
                       {s.Start, " START"},
                       {s.DPadUp, " UP"},
                       {s.DPadDown, " DOWN"},
                       {s.DPadLeft, " LEFT"},
                       {s.DPadRight, " RIGHT"},
                       {s.reset, " RESET"}});

  // A trigger reads as pressed when its digital bit is set or it is all the way in.
  line += (s.L || s.TriggerL == 255) ? std::string(" L") : TriggerText(s.TriggerL, " L");
  line += (s.R || s.TriggerR == 255) ? std::string(" R") : TriggerText(s.TriggerR, " R");
  line += StickText(s.AnalogStickX, s.AnalogStickY, " ANA");
  line += StickText(s.CStickX, s.CStickY, " C");
  return line;
}

void AppendNunchuk(std::string& line, const Nunchuk::DesiredState& nunchuk)
{
  const u8 pressed = nunchuk.GetButtons();
  AppendPressed(line, {{(pressed & Nunchuk::BUTTON_C) != 0, " C"},
                       {(pressed & Nunchuk::BUTTON_Z) != 0, " Z"}});
  line += fmt::format(" N-ACC:{},{},{}", nunchuk.GetAccelX(), nunchuk.GetAccelY(),
                      nunchuk.GetAccelZ());
  line += StickText(nunchuk.jx, nunchuk.jy, " ANA");
}

void AppendClassic(std::string& line, const Classic::DesiredState& classic)
{
  const u16 pressed = classic.GetButtons();
  const auto has = [pressed](u16 mask) { return (pressed & mask) != 0; };
  AppendPressed(line, {{has(Classic::PAD_LEFT), " LEFT"},
                       {has(Classic::PAD_RIGHT), " RIGHT"},
                       {has(Classic::PAD_DOWN), " DOWN"},
                       {has(Classic::PAD_UP), " UP"},
                       {has(Classic::BUTTON_A), " A"},
                       {has(Classic::BUTTON_B), " B"},
                       {has(Classic::BUTTON_X), " X"},
                       {has(Classic::BUTTON_Y), " Y"},
                       {has(Classic::BUTTON_ZL), " ZL"},
                       {has(Classic::BUTTON_ZR), " ZR"},
                       {has(Classic::BUTTON_PLUS), " +"},
                       {has(Classic::BUTTON_MINUS), " -"},
                       {has(Classic::BUTTON_HOME), " HOME"}});

  constexpr u32 trigger_range = (1u << Classic::TRIGGER_BITS) - 1;
  constexpr u32 left_range = (1u << Classic::LEFT_STICK_BITS) - 1;
  constexpr u32 right_range = (1u << Classic::RIGHT_STICK_BITS) - 1;
  line += TriggerText(classic.GetLeftTrigger().value, " L", trigger_range);
  line += TriggerText(classic.GetRightTrigger().value, " R", trigger_range);
  const auto left = classic.GetLeftStick().value;
  line += StickText(left.x, left.y, " ANA", left_range);
  const auto right = classic.GetRightStick().value;
  line += StickText(right.x, right.y, " R-ANA", right_range);
}

std::string WiimoteDisplayLine(int index, const DesiredWiimoteState& state)
{
  std::string line = fmt::format("R{}:", index + 1);

  const auto& b = state.buttons;
  if ((b.hex & WiimoteCommon::ButtonData::BUTTON_MASK) != 0)
  {
    AppendPressed(line, {{b.left != 0, " LEFT"},
                         {b.right != 0, " RIGHT"},
                         {b.down != 0, " DOWN"},
                         {b.up != 0, " UP"},
                         {b.a != 0, " A"},
                         {b.b != 0, " B"},
                         {b.plus != 0, " +"},
                         {b.minus != 0, " -"},
                         {b.one != 0, " 1"},
                         {b.two != 0, " 2"},
                         {b.home != 0, " HOME"}});
  }

  if (state.acceleration != state.DEFAULT_ACCELERATION)
  {
    const auto& accel = state.acceleration.value;
    line += fmt::format(" ACC:{},{},{}", accel.x, accel.y, accel.z);
  }

  if (state.camera_points != state.DEFAULT_CAMERA)
  {
    std::string ir;
    for (const auto& point : state.camera_points)
    {
      if (ir.empty())
        ir = " IR:";
      else
        ir += ',';
      // A size of 0xff marks a point the camera does not see.
      ir += point.size == 0xff ? std::string("_") :
                                 fmt::format("{},{}", point.position.x, point.position.y);
    }
    line += ir;
  }

  if (state.extension.data.index() == static_cast<size_t>(WiimoteEmu::ExtensionNumber::NONE))
    return line;

  const auto name_only = [&line](const char* name) {
    line += ' ';
    line += name;
  };
  std::visit(overloaded{
                 [](const std::monostate&) {},
                 [&](const Nunchuk::DesiredState& s) { AppendNunchuk(line, s); },
                 [&](const Classic::DesiredState& s) { AppendClassic(line, s); },
                 [&](const WiimoteEmu::Guitar::DesiredState&) { name_only("Guitar"); },
                 [&](const WiimoteEmu::Drums::DesiredState&) { name_only("Drums"); },
                 [&](const WiimoteEmu::Turntable::DesiredState&) { name_only("Turntable"); },
                 [&](const WiimoteEmu::UDrawTablet::DesiredState&) { name_only("UDraw"); },
                 [&](const WiimoteEmu::DrawsomeTablet::DesiredState&) { name_only("Drawsome"); },
                 [&](const WiimoteEmu::TaTaCon::DesiredState&) { name_only("TaTaCon"); },
                 [&](const WiimoteEmu::Shinkansen::DesiredState&) { name_only("Shinkansen"); },
             },
             state.extension.data);
  return line;
}
}  // namespace

MovieManager::MovieManager(Core::System& system) : m_system(system)
{
}

MovieManager::~MovieManager() = default;

void MovieManager::SetInputDisplay(size_t slot, std::string text)
{
  std::lock_guard lock(m_input_display_lock);
  m_input_display[slot] = std::move(text);
}

std::string MovieManager::GetInputDisplay()
{
  // Without a movie, show whatever is currently plugged in.
  if (!IsMovieActive())
  {
    m_controllers = {};
    m_wiimotes = {};
    const auto& serial = m_system.GetSerialInterface();
    for (size_t port = 0; port < m_controllers.size(); ++port)
    {
      const int channel = static_cast<int>(port);
      const auto device = serial.GetDeviceType(channel);
      if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
        m_controllers[port] = ControllerType::GBA;
      else if (device != SerialInterface::SIDEVICE_NONE)
        m_controllers[port] = ControllerType::GC;
      else
        m_controllers[port] = ControllerType::None;
      m_wiimotes[port] = WiimoteSourceEnabled(channel);
    }
  }

  std::string text;
  std::lock_guard lock(m_input_display_lock);
  for (int port = 0; port < 4; ++port)
  {
    if (IsUsingPad(port))
      text += m_input_display[static_cast<size_t>(port)] + '\n';
  }
  for (int remote = 0; remote < 4; ++remote)
  {
    if (IsUsingWiimote(remote))
      text += m_input_display[FIRST_WIIMOTE_LINE + static_cast<size_t>(remote)] + '\n';
  }
  return text;
}

std::string MovieManager::GetRTCDisplay() const
{
  using ExpansionInterface::CEXIIPL;
  const time_t now = CEXIIPL::GetEmulatedTime(m_system, CEXIIPL::UNIX_EPOCH);
  // fmt ignores the locale unless given one; use the user's for %c.
  return fmt::format(std::locale{""}, "Date/Time: {:%c}", fmt::gmtime(now));
}

std::string MovieManager::GetRerecords() const
{
  if (!IsMovieActive())
    return "Rerecords: N/A";
  return fmt::format("Rerecords: {}", m_rerecords);
}

void MovieManager::FrameUpdate()
{
  ++m_current_frame;
  if (!m_polled)
    ++m_current_lag_count;

  // While recording, the totals track the current position.
  if (IsRecordingInput())
  {
    m_total_frames = m_current_frame;
    m_total_lag_count = m_current_lag_count;
  }
  m_polled = false;
}

void MovieManager::Init(const BootParameters& boot)
{
  const auto* const disc = std::get_if<BootParameters::Disc>(&boot.parameters);
  m_current_file_name = disc ? disc->path : std::string();

  m_polled = false;
  m_save_config = false;

  if (IsPlayingInput())
  {
    ReadHeader();
    std::thread(&MovieManager::CheckMD5, this).detach();

    const std::string running_id = SConfig::GetInstance().GetGameID();
    if (std::strncmp(m_temp_header.gameID.data(), running_id.c_str(), 6) != 0)
    {
      PanicAlertFmtT("The recorded game ({0}) is not the same as the selected game ({1})",
                     m_temp_header.GetGameID(), SConfig::GetInstance().GetGameID());
      EndPlayInput(false);
    }
  }

  if (IsRecordingInput())
  {
    GetSettings();
    std::thread(&MovieManager::GetMD5, this).detach();
    m_tick_count_at_last_input = 0;
  }

  m_pad_state = {};
  for (std::string& line : m_input_display)
    line.clear();

  if (IsMovieActive())
    return;

  m_recording_from_save_state = false;
  m_rerecords = 0;
  m_current_byte = 0;
  m_current_frame = 0;
  m_current_lag_count = 0;
  m_current_input_count = 0;
}

void MovieManager::InputUpdate()
{
  ++m_current_input_count;
  if (!IsRecordingInput())
    return;

  const auto& timing = m_system.GetCoreTiming();
  m_total_input_count = m_current_input_count;
  m_total_tick_count += timing.GetTicks() - m_tick_count_at_last_input;
  m_tick_count_at_last_input = timing.GetTicks();
}

void MovieManager::SetPolledDevice() { m_polled = true; }

void MovieManager::SetReadOnly(bool bEnabled)
{
  if (bEnabled != m_read_only)
    Core::DisplayMessage(bEnabled ? "Read-only mode." : "Read+Write mode.", 1000);
  m_read_only = bEnabled;
}

bool MovieManager::IsRecordingInput() const { return m_play_mode == PlayMode::Recording; }

bool MovieManager::IsRecordingInputFromSaveState() const { return m_recording_from_save_state; }

bool MovieManager::IsJustStartingRecordingInputFromSaveState() const
{
  return m_current_frame == 0 && IsRecordingInputFromSaveState();
}

bool MovieManager::IsJustStartingPlayingInputFromSaveState() const
{
  return m_current_frame == 1 && IsRecordingInputFromSaveState() && IsPlayingInput();
}

bool MovieManager::IsPlayingInput() const { return m_play_mode == PlayMode::Playing; }

bool MovieManager::IsMovieActive() const { return m_play_mode != PlayMode::None; }

bool MovieManager::IsReadOnly() const { return m_read_only; }

u64 MovieManager::GetRecordingStartTime() const { return m_recording_start_time; }

u64 MovieManager::GetCurrentFrame() const { return m_current_frame; }

u64 MovieManager::GetTotalFrames() const { return m_total_frames; }

u64 MovieManager::GetCurrentInputCount() const { return m_current_input_count; }

u64 MovieManager::GetTotalInputCount() const { return m_total_input_count; }

u64 MovieManager::GetCurrentLagCount() const { return m_current_lag_count; }

u64 MovieManager::GetTotalLagCount() const { return m_total_lag_count; }

void MovieManager::SetClearSave(bool enabled) { m_clear_save = enabled; }

void MovieManager::SignalDiscChange(const std::string& new_path)
{
  if (!IsRecordingInput())
    return;

  // Only the file name goes into the header (npos + 1 wraps to 0: no separator).
  const size_t name_start = new_path.find_last_of("/\\") + 1;
  std::string file_name = new_path.substr(name_start);
  if (file_name.length() > sizeof(DTMHeader::discChange))
  {
    PanicAlertFmtT("The disc change to \"{0}\" could not be saved in the .dtm "
                   "file.\nThe filename of the disc image must not be "
                   "longer than 40 characters.",
                   file_name);
  }
  m_disc_change_filename = std::move(file_name);
  m_has_disc_change = true;
}

void MovieManager::SetReset(bool reset) { m_reset = reset; }

bool MovieManager::IsUsingPad(int controller) const
{
  return m_controllers[static_cast<size_t>(controller)] != ControllerType::None;
}

bool MovieManager::IsUsingBongo(int controller) const { return (m_bongos >> controller & 1) != 0; }

bool MovieManager::IsUsingGBA(int controller) const
{
  return m_controllers[static_cast<size_t>(controller)] == ControllerType::GBA;
}

bool MovieManager::IsUsingWiimote(int wiimote) const
{
  return m_wiimotes[static_cast<size_t>(wiimote)];
}

bool MovieManager::IsConfigSaved() const { return m_save_config; }

bool MovieManager::IsStartingFromClearSave() const { return m_clear_save; }

bool MovieManager::IsUsingMemcard(ExpansionInterface::Slot slot) const
{
  if (slot == ExpansionInterface::Slot::A)
    return (m_memcards & 1) != 0;
  if (slot == ExpansionInterface::Slot::B)
    return (m_memcards & 2) != 0;
  return false;
}

bool MovieManager::IsNetPlayRecording() const { return m_net_play; }

// Both ChangePads functions only read the current device settings; the
// result is not stored anywhere, matching the established behaviour.
void MovieManager::ChangePads()
{
  if (!Core::IsRunning(m_system))
    return;

  [[maybe_unused]] ControllerTypeArray detected{};
  for (int channel = 0; channel < SerialInterface::MAX_SI_CHANNELS; ++channel)
  {
    const auto device = Config::Get(Config::GetInfoForSIDevice(channel));
    ControllerType type = ControllerType::None;
    if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      type = ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(device))
      type = ControllerType::GC;
    detected[static_cast<size_t>(channel)] = type;
  }
}

void MovieManager::ChangeWiiPads()
{
  [[maybe_unused]] WiimoteEnabledArray detected{};
  for (int index = 0; index < MAX_WIIMOTES; ++index)
    detected[static_cast<size_t>(index)] = WiimoteSourceEnabled(index);
}

bool MovieManager::BeginRecordingInput(const ControllerTypeArray& controllers,
                                       const WiimoteEnabledArray& wiimotes)
{
  if (m_play_mode != PlayMode::None)
    return false;
  if (controllers == ControllerTypeArray{} && wiimotes == WiimoteEnabledArray{})
    return false;

  Core::DisplayMessage("Starting movie recording", 2000);
  const Core::CPUThreadGuard guard{m_system};

  m_controllers = controllers;
  m_wiimotes = wiimotes;
  m_current_frame = 0;
  m_total_frames = 0;
  m_current_lag_count = 0;
  m_total_lag_count = 0;
  m_current_input_count = 0;
  m_total_input_count = 0;
  m_total_tick_count = 0;
  m_tick_count_at_last_input = 0;
  m_bongos = 0;
  m_memcards = 0;

  // The emulated RTC start: the NetPlay clock, a custom RTC or the host clock.
  if (NetPlay::IsNetPlayRunning())
  {
    m_net_play = true;
    m_recording_start_time = ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime();
  }
  else if (Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE))
  {
    m_recording_start_time = Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);
  }
  else
  {
    m_recording_start_time = Common::Timer::GetLocalTimeSinceJan1970();
  }

  m_rerecords = 0;

  for (int channel = 0; channel < SerialInterface::MAX_SI_CHANNELS; ++channel)
  {
    if (Config::Get(Config::GetInfoForSIDevice(channel)) == SerialInterface::SIDEVICE_GC_TARUKONGA)
      m_bongos = static_cast<u8>(m_bongos | (1 << channel));
  }

  // Recording a running game starts from a fresh savestate of it.
  if (Core::IsRunning(m_system))
  {
    const std::string state_path = RecordingStartStatePath();
    if (File::Exists(state_path))
      File::Delete(state_path);
    State::SaveAs(m_system, state_path);
    m_recording_from_save_state = true;

    std::thread(&MovieManager::GetMD5, this).detach();
    GetSettings();
  }

  // Wii Remotes must start from a reset state or playback desyncs. (For
  // GameCube games this does nothing harmful.)
  if (!Core::IsRunning(m_system))
    ::Wiimote::ResetAllWiimotes();

  m_play_mode = PlayMode::Recording;
  m_author = Config::Get(Config::MAIN_MOVIE_MOVIE_AUTHOR);
  m_temp_input.clear();
  m_current_byte = 0;

  // The SYSCONF code reads the Movie config layer during recording as well as
  // playback, so install one built from the current settings. Whether this is
  // a Wii game is unknown here, so assume it is; GameCube only uses the
  // language from it, which comes from the base settings anyway. The header
  // lives on because the layer keeps a pointer to it.
  static DTMHeader s_recording_layer_header = [] {
    DTMHeader header{};
    header.bWii = true;
    return header;
  }();
  ConfigLoaders::SaveToDTM(&s_recording_layer_header);
  Config::AddLayer(ConfigLoaders::GenerateMovieConfigLoader(&s_recording_layer_header));

  if (Core::IsRunning(m_system))
    Core::UpdateWantDeterminism(m_system);

  return true;
}

void MovieManager::CheckPadStatus(const GCPadStatus* PadStatus, int controllerID)
{
  const u16 held = PadStatus->button;
  const auto has = [held](u16 mask) { return (held & mask) != 0; };

  ControllerState& s = m_pad_state;
  s.Start = has(PAD_BUTTON_START);
  s.A = has(PAD_BUTTON_A);
  s.B = has(PAD_BUTTON_B);
  s.X = has(PAD_BUTTON_X);
  s.Y = has(PAD_BUTTON_Y);
  s.Z = has(PAD_TRIGGER_Z);
  s.DPadUp = has(PAD_BUTTON_UP);
  s.DPadDown = has(PAD_BUTTON_DOWN);
  s.DPadLeft = has(PAD_BUTTON_LEFT);
  s.DPadRight = has(PAD_BUTTON_RIGHT);
  s.L = has(PAD_TRIGGER_L);
  s.R = has(PAD_TRIGGER_R);
  s.get_origin = has(PAD_GET_ORIGIN);
  s.is_connected = PadStatus->isConnected;

  s.TriggerL = PadStatus->triggerLeft;
  s.TriggerR = PadStatus->triggerRight;
  s.AnalogStickX = PadStatus->stickX;
  s.AnalogStickY = PadStatus->stickY;
  s.CStickX = PadStatus->substickX;
  s.CStickY = PadStatus->substickY;

  // Disc swaps and resets are one-shot events attached to the next poll.
  s.disc = std::exchange(m_has_disc_change, false);
  s.reset = std::exchange(m_reset, false);

  SetInputDisplay(static_cast<size_t>(controllerID), PadDisplayLine(s, controllerID));
}

void MovieManager::RecordInput(const GCPadStatus* PadStatus, int controllerID)
{
  if (!IsRecordingInput() || !IsUsingPad(controllerID))
    return;

  CheckPadStatus(PadStatus, controllerID);

  m_temp_input.resize(m_current_byte + PAD_RECORD_SIZE);
  std::memcpy(m_temp_input.data() + m_current_byte, &m_pad_state, PAD_RECORD_SIZE);
  m_current_byte += PAD_RECORD_SIZE;
}

void MovieManager::CheckWiimoteStatus(int wiimote, const DesiredWiimoteState& desired_state)
{
  SetPolledDevice();
  SetInputDisplay(FIRST_WIIMOTE_LINE + static_cast<size_t>(wiimote),
                  WiimoteDisplayLine(wiimote, desired_state));

  if (IsRecordingInput())
    RecordWiimote(wiimote, WiimoteEmu::SerializeDesiredState(desired_state));
}

void MovieManager::RecordWiimote(int wiimote, const SerializedWiimoteState& serialized_state)
{
  if (!IsRecordingInput() || !IsUsingWiimote(wiimote))
    return;

  InputUpdate();

  // Each Wii Remote record is a length byte followed by that many bytes.
  const u8 length = serialized_state.length;
  m_temp_input.resize(m_current_byte + 1 + length);
  m_temp_input[m_current_byte] = length;
  ++m_current_byte;
  std::copy_n(serialized_state.data.begin(), length, m_temp_input.data() + m_current_byte);
  m_current_byte += length;
}

void MovieManager::ReadHeader()
{
  const DTMHeader& h = m_temp_header;
  for (size_t port = 0; port < 4; ++port)
  {
    const u8 bit = static_cast<u8>(1u << port);
    if ((h.GBAControllers & bit) != 0)
      m_controllers[port] = ControllerType::GBA;
    else if ((h.controllers & bit) != 0)
      m_controllers[port] = ControllerType::GC;
    else
      m_controllers[port] = ControllerType::None;
    m_wiimotes[port] = (h.controllers >> (port + 4) & 1) != 0;
  }

  m_recording_start_time = h.recordingStartTime;
  m_rerecords = std::max(m_rerecords, h.numRerecords);

  if (!h.bSaveConfig)
  {
    GetSettings();
  }
  else
  {
    m_save_config = true;
    Config::AddLayer(ConfigLoaders::GenerateMovieConfigLoader(&m_temp_header));
    m_clear_save = h.bClearSave;
    m_memcards = h.memcards;
    m_bongos = h.bongos;
    m_net_play = h.bNetPlay;
    m_revision = h.revision;
  }

  // Both strings keep the header's full fixed width, padding included.
  m_disc_change_filename.assign(h.discChange.begin(), h.discChange.end());
  m_author.assign(h.author.begin(), h.author.end());
  m_md5 = h.md5;
  m_dsp_irom_hash = h.DSPiromHash;
  m_dsp_coef_hash = h.DSPcoefHash;
}

bool MovieManager::PlayInput(const std::string& movie_path,
                             std::optional<std::string>* savestate_path)
{
  if (m_play_mode != PlayMode::None)
    return false;

  File::IOFile movie(movie_path, "rb");
  if (!movie.ReadArray(&m_temp_header, 1))
    return false;
  if (m_temp_header.filetype != DTM_MAGIC)
  {
    PanicAlertFmtT("Invalid recording file");
    return false;
  }

  ReadHeader();

  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    return false;

  // Playback starts at the beginning and runs to the recorded totals.
  m_current_frame = 0;
  m_current_input_count = 0;
  m_current_lag_count = 0;
  m_total_frames = m_temp_header.frameCount;
  m_total_input_count = m_temp_header.inputCount;
  m_total_lag_count = m_temp_header.lagCount;
  m_total_tick_count = m_temp_header.tickCount;
  m_play_mode = PlayMode::Playing;

  // Same reason as in BeginRecordingInput: reset Wii Remotes before boot.
  ::Wiimote::ResetAllWiimotes();
  Core::UpdateWantDeterminism(m_system);

  m_temp_input.resize(movie.GetSize() - HEADER_SIZE);
  movie.ReadBytes(m_temp_input.data(), m_temp_input.size());
  m_current_byte = 0;
  movie.Close();

  if (!m_temp_header.bFromSaveState || savestate_path == nullptr)
    return true;

  // The movie starts from <movie>.sav; the caller loads it.
  const std::string state_file = movie_path + ".sav";
  if (File::Exists(state_file))
  {
    *savestate_path = state_file;
  }
  else
  {
    PanicAlertFmtT("Movie {0} indicates that it starts from a savestate, but {1} "
                   "doesn't exist. The movie will likely not sync!",
                   movie_path, state_file);
  }
  m_recording_from_save_state = true;

#ifdef USE_RETRO_ACHIEVEMENTS
  // Keep hardcore mode from being turned back on before the movie starts.
  Config::SetBase(Config::RA_HARDCORE_ENABLED, false);
#endif  // USE_RETRO_ACHIEVEMENTS

  LoadInput(movie_path);
  return true;
}

void MovieManager::DoState(PointerWrap& p)
{
  // Saved whether or not a movie runs; the totals come back from the movie
  // file in LoadInput.
  p.Do(m_current_frame);
  p.Do(m_current_byte);
  p.Do(m_current_lag_count);
  p.Do(m_current_input_count);
  p.Do(m_polled);
  p.Do(m_tick_count_at_last_input);
}

void MovieManager::SwitchPlayMode(PlayMode mode, const char* message)
{
  if (m_play_mode == mode)
    return;
  m_play_mode = mode;
  Core::UpdateWantDeterminism(m_system);
  Core::DisplayMessage(message, 2000);
}

// Explains to the user where the movie in a loaded savestate first differs
// from the one in memory.
void MovieManager::ReportMismatch(const std::vector<u8>& saved_input, size_t index)
{
  if (IsUsingWiimote(0))
  {
    const size_t file_offset = index + HEADER_SIZE;
    PanicAlertFmtT("Warning: You loaded a save whose movie mismatches on byte {0} "
                   "({1:#x}). You should load another save before continuing, or "
                   "load this state with read-only mode off. Otherwise you'll "
                   "probably get a desync.",
                   file_offset, file_offset);
    std::ranges::copy(saved_input, m_temp_input.begin());
    return;
  }

  const size_t frame = index / PAD_RECORD_SIZE;
  ControllerState c;
  ControllerState m;
  std::memcpy(&c, m_temp_input.data() + frame * PAD_RECORD_SIZE, PAD_RECORD_SIZE);
  std::memcpy(&m, saved_input.data() + frame * PAD_RECORD_SIZE, PAD_RECORD_SIZE);
  // The savestate's "Connected" value repeats the current movie's bit; this
  // is the established output.
  PanicAlertFmtT("Warning: You loaded a save whose movie mismatches on frame "
                 "{0}. You should load another save before continuing, or load "
                 "this state with read-only mode off. Otherwise you'll probably "
                 "get a desync.\n\nMore information: The current movie is {1} "
                 "frames long and the savestate's movie is {2} frames long.\n\n"
                 "On frame {3}, the current movie presses:\nStart={4}, A={5}, "
                 "B={6}, X={7}, Y={8}, Z={9}, DUp={10}, DDown={11}, DLeft={12}, "
                 "DRight={13}, L={14}, R={15}, LT={16}, RT={17}, AnalogX={18}, "
                 "AnalogY={19}, CX={20}, CY={21}, Connected={22}\n\nOn frame "
                 "{23}, the savestate's movie presses:\nStart={24}, A={25}, "
                 "B={26}, X={27}, Y={28}, Z={29}, DUp={30}, DDown={31}, "
                 "DLeft={32}, DRight={33}, L={34}, R={35}, LT={36}, RT={37}, "
                 "AnalogX={38}, AnalogY={39}, CX={40}, CY={41}, Connected={42}",
                 frame, m_total_frames, m_temp_header.frameCount,  //
                 frame, c.Start, c.A, c.B, c.X, c.Y, c.Z, c.DPadUp, c.DPadDown, c.DPadLeft,
                 c.DPadRight, c.L, c.R, c.TriggerL, c.TriggerR, c.AnalogStickX, c.AnalogStickY,
                 c.CStickX, c.CStickY, c.is_connected,  //
                 frame, m.Start, m.A, m.B, m.X, m.Y, m.Z, m.DPadUp, m.DPadDown, m.DPadLeft,
                 m.DPadRight, m.L, m.R, m.TriggerL, m.TriggerR, m.AnalogStickX, m.AnalogStickY,
                 m.CStickX, m.CStickY, c.is_connected);
}

void MovieManager::LoadInput(const std::string& movie_path)
{
  File::IOFile movie;
  if (!movie.Open(movie_path, "r+b"))
  {
    PanicAlertFmtT("Failed to read {0}", movie_path);
    EndPlayInput(false);
    return;
  }

  movie.ReadArray(&m_temp_header, 1);
  if (m_temp_header.filetype != DTM_MAGIC)
  {
    PanicAlertFmtT("Savestate movie {0} is corrupted, movie recording stopping...", movie_path);
    EndPlayInput(false);
    return;
  }
  ReadHeader();

  // Loading a state in read+write mode is a rerecord; note it in the file.
  if (!m_read_only)
  {
    ++m_rerecords;
    m_temp_header.numRerecords = m_rerecords;
    movie.Seek(0, File::SeekOrigin::Begin);
    movie.WriteArray(&m_temp_header, 1);
  }

  ChangePads();
  if (m_system.IsWii())
    ChangeWiiPads();

  const u64 file_input_size = movie.GetSize() - HEADER_SIZE;
  bool past_end = false;

  // Only possible if the .dtm was cut short by hand.
  if (m_current_byte > file_input_size)
  {
    PanicAlertFmtT("Warning: You loaded a save whose movie ends before the current "
                   "frame in the save (byte {0} < {1}) (frame {2} < {3}). You "
                   "should load another save before continuing.",
                   file_input_size + HEADER_SIZE, m_current_byte + HEADER_SIZE,
                   m_temp_header.frameCount, m_current_frame);
    past_end = true;
  }

  if (!m_read_only || m_temp_input.empty())
  {
    // Take the whole movie from the file.
    m_total_frames = m_temp_header.frameCount;
    m_total_lag_count = m_temp_header.lagCount;
    m_total_input_count = m_temp_header.inputCount;
    m_total_tick_count = m_temp_header.tickCount;
    m_tick_count_at_last_input = m_temp_header.tickCount;

    m_temp_input.resize(static_cast<size_t>(file_input_size));
    movie.ReadBytes(m_temp_input.data(), m_temp_input.size());
  }
  else if (m_current_byte > 0 && m_current_byte <= file_input_size)
  {
    // Read-only with a movie in memory: the state's movie must agree with it
    // up to the state's position.
    if (m_current_byte > m_temp_input.size())
    {
      past_end = true;
      PanicAlertFmtT("Warning: You loaded a save that's after the end of the current "
                     "movie. (byte {0} > {1}) (input {2} > {3}). You should load "
                     "another save before continuing, or load this state with "
                     "read-only mode off.",
                     m_current_byte + HEADER_SIZE, m_temp_input.size() + HEADER_SIZE,
                     m_current_input_count, m_total_input_count);
    }
    else
    {
      std::vector<u8> saved_input(static_cast<size_t>(m_current_byte));
      movie.ReadArray(saved_input.data(), saved_input.size());
      const auto first_difference = std::ranges::mismatch(saved_input, m_temp_input).in1;
      if (first_difference != saved_input.end())
      {
        ReportMismatch(saved_input,
                       static_cast<size_t>(std::distance(saved_input.begin(), first_difference)));
      }
    }
  }
  movie.Close();

  m_save_config = m_temp_header.bSaveConfig;

  if (past_end)
    EndPlayInput(false);
  else if (m_read_only)
    SwitchPlayMode(PlayMode::Playing, "Switched to playback");
  else
    SwitchPlayMode(PlayMode::Recording, "Switched to recording");
}

void MovieManager::CheckInputEnd()
{
  const bool out_of_data = m_current_byte >= m_temp_input.size();
  // A movie from boot also ends once the recorded tick count has passed.
  if (out_of_data || (m_system.GetCoreTiming().GetTicks() > m_total_tick_count &&
                      !IsRecordingInputFromSaveState()))
  {
    EndPlayInput(!m_read_only);
  }
}

void MovieManager::PlayController(GCPadStatus* PadStatus, int controllerID)
{
  // Playback relies on the game polling pads in the same order as when it was
  // recorded.
  if (!IsPlayingInput() || !IsUsingPad(controllerID) || m_temp_input.empty())
    return;

  if (m_current_byte + PAD_RECORD_SIZE > m_temp_input.size())
  {
    PanicAlertFmtT("Premature movie end in PlayController. {0} + {1} > {2}", m_current_byte,
                   PAD_RECORD_SIZE, m_temp_input.size());
    EndPlayInput(!m_read_only);
    return;
  }

  std::memcpy(&m_pad_state, m_temp_input.data() + m_current_byte, PAD_RECORD_SIZE);
  m_current_byte += PAD_RECORD_SIZE;

  const ControllerState& s = m_pad_state;
  PadStatus->isConnected = s.is_connected;
  PadStatus->triggerLeft = s.TriggerL;
  PadStatus->triggerRight = s.TriggerR;
  PadStatus->stickX = s.AnalogStickX;
  PadStatus->stickY = s.AnalogStickY;
  PadStatus->substickX = s.CStickX;
  PadStatus->substickY = s.CStickY;

  u16 buttons = PAD_USE_ORIGIN;
  const auto set_if = [&buttons](bool pressed, u16 mask) {
    if (pressed)
      buttons |= mask;
  };
  set_if(s.A, PAD_BUTTON_A);
  set_if(s.B, PAD_BUTTON_B);
  set_if(s.X, PAD_BUTTON_X);
  set_if(s.Y, PAD_BUTTON_Y);
  set_if(s.Z, PAD_TRIGGER_Z);
  set_if(s.Start, PAD_BUTTON_START);
  set_if(s.DPadUp, PAD_BUTTON_UP);
  set_if(s.DPadDown, PAD_BUTTON_DOWN);
  set_if(s.DPadLeft, PAD_BUTTON_LEFT);
  set_if(s.DPadRight, PAD_BUTTON_RIGHT);
  set_if(s.L, PAD_TRIGGER_L);
  set_if(s.R, PAD_TRIGGER_R);
  set_if(s.get_origin, PAD_GET_ORIGIN);
  PadStatus->button = buttons;
  // A and B are also analog; a press is played back as fully in.
  if (s.A)
    PadStatus->analogA = 0xFF;
  if (s.B)
    PadStatus->analogB = 0xFF;

  if (s.disc)
  {
    const Core::CPUThreadGuard guard(m_system);
    if (!m_system.GetDVDInterface().AutoChangeDisc(guard))
    {
      m_system.GetCPU().Break();
      PanicAlertFmtT("Change the disc to {0}", m_disc_change_filename);
    }
  }

  if (s.reset)
    m_system.GetProcessorInterface().ResetButton_Tap();

  SetInputDisplay(static_cast<size_t>(controllerID), PadDisplayLine(s, controllerID));

  CheckInputEnd();
}

bool MovieManager::PlayWiimote(int wiimote, DesiredWiimoteState* desired_state)
{
  if (!IsPlayingInput() || !IsUsingWiimote(wiimote) || m_temp_input.empty())
    return false;

  // Failing any check below ends playback (or resumes recording).
  const auto give_up = [this] {
    EndPlayInput(!m_read_only);
    return false;
  };

  if (m_current_byte + 1 > m_temp_input.size())
  {
    PanicAlertFmtT("Premature movie end in PlayWiimote. {0} + 1 > {1}", m_current_byte,
                   m_temp_input.size());
    return give_up();
  }

  SerializedWiimoteState serialized;
  serialized.length = m_temp_input[m_current_byte];
  const size_t length = serialized.length;
  if (length > serialized.data.size())
  {
    PanicAlertFmtT("Invalid serialized length:{0} in PlayWiimote. byte:{1}",
                   static_cast<int>(serialized.length), m_current_byte);
    return give_up();
  }

  ++m_current_byte;
  if (m_current_byte + length > m_temp_input.size())
  {
    PanicAlertFmtT("Premature movie end in PlayWiimote. {0} + {1} > {2}", m_current_byte,
                   static_cast<int>(serialized.length), m_temp_input.size());
    return give_up();
  }

  std::copy_n(m_temp_input.data() + m_current_byte, length,
              serialized.data.begin());
  if (!WiimoteEmu::DeserializeDesiredState(desired_state, serialized))
  {
    // The hint is shown when the movie has no GameCube pads (established
    // behaviour, even though the advice reads as if it were the other case).
    const bool no_gc_pads = m_controllers == ControllerTypeArray{};
    PanicAlertFmtT("Aborting playback. Error in DeserializeDesiredState. byte:{0}{1}",
                   m_current_byte,
                   no_gc_pads ? " Try re-creating the recording with all GameCube "
                                "controllers disabled (in Configure > GameCube > "
                                "Device Settings)." :
                                "");
    return give_up();
  }

  m_current_byte += length;
  ++m_current_input_count;

  CheckInputEnd();
  return true;
}

void MovieManager::EndPlayInput(bool cont)
{
  if (cont)
  {
    // Going from None to Recording would need UpdateWantDeterminism.
    ASSERT(IsMovieActive());
    m_play_mode = PlayMode::Recording;
    Core::DisplayMessage("Reached movie end. Resuming recording.", 2000);
    return;
  }

  if (m_play_mode == PlayMode::None)
    return;

  // May run on the EmuThread while booting, before the CPU runs.
  auto& cpu = m_system.GetCPU();
  const bool cpu_was_running = Core::IsRunning(m_system) && !cpu.IsStepping();
  if (cpu_was_running && Config::Get(Config::MAIN_MOVIE_PAUSE_MOVIE))
    cpu.Break();

  m_rerecords = 0;
  m_current_byte = 0;
  m_play_mode = PlayMode::None;
  Core::DisplayMessage("Movie End.", 2000);
  m_recording_from_save_state = false;
  Config::RemoveLayer(Config::LayerType::Movie);
  // The totals and input data stay, so a later movie savestate can resume.

  Core::QueueHostJob([](Core::System& system) { Core::UpdateWantDeterminism(system); });
}

void MovieManager::SaveRecording(const std::string& filename)
{
  File::IOFile out(filename, "wb");

  DTMHeader header;
  std::memset(&header, 0, sizeof(header));
  header.filetype = DTM_MAGIC;
  std::strncpy(header.gameID.data(), SConfig::GetInstance().GetGameID().c_str(), 6);
  header.bWii = m_system.IsWii();
  header.bFromSaveState = m_recording_from_save_state;
  header.frameCount = m_total_frames;
  header.lagCount = m_total_lag_count;
  header.inputCount = m_total_input_count;
  header.numRerecords = m_rerecords;
  header.recordingStartTime = m_recording_start_time;
  header.bSaveConfig = true;

  // Fills the settings fields; the fields after it override what it wrote.
  ConfigLoaders::SaveToDTM(&header);

  header.memcards = m_memcards;
  header.bClearSave = m_clear_save;
  header.bNetPlay = m_net_play;
  std::strncpy(header.discChange.data(), m_disc_change_filename.c_str(),
               header.discChange.size());
  std::strncpy(header.author.data(), m_author.c_str(), header.author.size());
  header.md5 = m_md5;
  header.revision = m_revision;
  header.DSPiromHash = m_dsp_irom_hash;
  header.DSPcoefHash = m_dsp_coef_hash;
  header.tickCount = m_total_tick_count;
  header.uniqueID = 0;  // never implemented

  out.WriteArray(&header, 1);
  bool ok = out.WriteBytes(m_temp_input.data(), m_temp_input.size());

  // A movie that starts from a state carries that state alongside it.
  if (ok && m_recording_from_save_state)
    ok = File::CopyRegularFile(RecordingStartStatePath(), filename + ".sav");

  Core::DisplayMessage(ok ? fmt::format("DTM {} saved", filename) :
                            fmt::format("Failed to save {}", filename),
                       2000);
}

namespace
{
// Adler-32 of a DSP ROM file, read as big-endian u16 words. Missing words
// (short or absent file) stay zero.
u32 HashDSPRom(const std::string& user_file, const char* rom_name, size_t words, size_t bytes)
{
  std::string path = user_file;
  if (!File::Exists(path))
    path = File::GetSysDirectory() + GC_SYS_DIR DIR_SEP + rom_name;

  std::vector<u16> rom(words);
  File::IOFile file(path, "rb");
  file.ReadArray(rom.data(), rom.size());
  file.Close();
  std::ranges::transform(rom, rom.begin(), [](u16 word) { return Common::swap16(word); });
  return Common::HashAdler32(reinterpret_cast<const u8*>(rom.data()), bytes);
}
}  // namespace

void MovieManager::GetSettings()
{
  using ExpansionInterface::EXIDeviceType;
  using ExpansionInterface::Slot;

  const EXIDeviceType type_a = Config::Get(Config::MAIN_SLOT_A);
  const EXIDeviceType type_b = Config::Get(Config::MAIN_SLOT_B);
  const bool raw_card_a = type_a == EXIDeviceType::MemoryCard;
  const bool folder_a = type_a == EXIDeviceType::MemoryCardFolder;
  const bool raw_card_b = type_b == EXIDeviceType::MemoryCard;
  const bool folder_b = type_b == EXIDeviceType::MemoryCardFolder;

  m_save_config = true;
  m_net_play = NetPlay::IsNetPlayRunning();

  // "Clear save": nothing saved for this game yet, so playback should also
  // start without saves.
  if (m_system.IsWii())
  {
    const u64 title_id = SConfig::GetInstance().GetTitleID();
    const std::string banner =
        Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Session) + "/banner.bin";
    m_clear_save = !File::Exists(banner);
  }
  else
  {
    const auto card_file_exists = [](Slot slot) {
      return File::Exists(Config::GetMemcardPath(slot, SConfig::GetInstance().m_region));
    };
    const auto folder_has_files = [this](Slot slot) {
      const auto [folder, migrate] = ExpansionInterface::CEXIMemoryCard::GetGCIFolderPath(
          slot, ExpansionInterface::AllowMovieFolder::No, *this);
      return File::ScanDirectoryTree(folder, false).size > 0;
    };
    // Checked in this order, stopping at the first slot with saves.
    const bool any_saves = (raw_card_a && card_file_exists(Slot::A)) ||
                           (raw_card_b && card_file_exists(Slot::B)) ||
                           (folder_a && folder_has_files(Slot::A)) ||
                           (folder_b && folder_has_files(Slot::B));
    m_clear_save = !any_saves;
  }

  if (raw_card_a || folder_a)
    m_memcards |= 1;
  if (raw_card_b || folder_b)
    m_memcards |= 2;

  m_revision = RevisionToHeaderBytes(Common::GetScmRevGitStr());

  if (Config::Get(Config::MAIN_DSP_HLE))
  {
    m_dsp_irom_hash = 0;
    m_dsp_coef_hash = 0;
    return;
  }

  const std::string user_dir = File::GetUserPath(D_GCUSER_IDX);
  m_dsp_irom_hash =
      HashDSPRom(user_dir + DSP_IROM, DSP_IROM, DSP::DSP_IROM_SIZE, DSP::DSP_IROM_BYTE_SIZE);
  m_dsp_coef_hash =
      HashDSPRom(user_dir + DSP_COEF, DSP_COEF, DSP::DSP_COEF_SIZE, DSP::DSP_COEF_BYTE_SIZE);
}

void MovieManager::CheckMD5()
{
  if (m_current_file_name.empty())
    return;

  // Movies older than the MD5 field have zeros there; nothing to compare.
  if (m_temp_header.md5 == std::array<u8, 16>{})
    return;

  Core::DisplayMessage("Verifying checksum...", 2000);

  std::array<u8, 16> disc_md5;
  mbedtls_md_file(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), m_current_file_name.c_str(),
                  disc_md5.data());

  if (disc_md5 != m_md5)
    Core::DisplayMessage("Checksum of current game does not match the recorded game!", 3000);
  else
    Core::DisplayMessage("Checksum of current game matches the recorded game.", 2000);
}

void MovieManager::GetMD5()
{
  if (m_current_file_name.empty())
    return;

  Core::DisplayMessage("Calculating checksum of game file...", 2000);
  mbedtls_md_file(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), m_current_file_name.c_str(),
                  m_md5.data());
  Core::DisplayMessage("Finished calculating checksum.", 2000);
}

void MovieManager::Shutdown()
{
  m_current_input_count = 0;
  m_total_input_count = 0;
  m_total_frames = 0;
  m_tick_count_at_last_input = 0;
  m_temp_input.clear();
}
}  // namespace Movie
