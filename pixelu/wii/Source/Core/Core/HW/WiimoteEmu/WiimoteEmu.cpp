// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"

#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/Extension/DrawsomeTablet.h"
#include "Core/HW/WiimoteEmu/Extension/Drums.h"
#include "Core/HW/WiimoteEmu/Extension/Guitar.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/Extension/Shinkansen.h"
#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"
#include "Core/HW/WiimoteEmu/Extension/Turntable.h"
#include "Core/HW/WiimoteEmu/Extension/UDrawTablet.h"

#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/Force.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUAccelerometer.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUCursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUGyroscope.h"
#include "InputCommon/ControllerEmu/ControlGroup/IRPassthrough.h"
#include "InputCommon/ControllerEmu/ControlGroup/ModifySettingsButton.h"
#include "InputCommon/ControllerEmu/ControlGroup/Tilt.h"

namespace WiimoteEmu
{
using namespace WiimoteCommon;

namespace
{
// Report bits of the Buttons group entries, in group order.
constexpr u16 CORE_BUTTON_BITS[] = {Wiimote::BUTTON_A,     Wiimote::BUTTON_B,
                                    Wiimote::BUTTON_ONE,   Wiimote::BUTTON_TWO,
                                    Wiimote::BUTTON_MINUS, Wiimote::BUTTON_PLUS,
                                    Wiimote::BUTTON_HOME};

// D-pad bits for up, down, left and right; held sideways the pad is turned a quarter.
constexpr u16 DPAD_BITS[] = {Wiimote::PAD_UP, Wiimote::PAD_DOWN, Wiimote::PAD_LEFT,
                             Wiimote::PAD_RIGHT};
constexpr u16 DPAD_SIDEWAYS_BITS[] = {Wiimote::PAD_RIGHT, Wiimote::PAD_LEFT, Wiimote::PAD_UP,
                                      Wiimote::PAD_DOWN};

constexpr float UPDATE_PERIOD = 1.f / ::Wiimote::UPDATE_FREQ;

// The default IR calibration block: four 10 bit points (the corners of the low/high rectangle)
// with the top two bits of each coordinate packed into a "mix" byte per pair of points, then a
// checksum.
std::array<u8, 11> MakeIRCalibration()
{
  struct Point
  {
    u16 x, y;
  };
  constexpr Point P1{Wiimote::IR_LOW_X, Wiimote::IR_LOW_Y};
  constexpr Point P2{Wiimote::IR_HIGH_X, Wiimote::IR_LOW_Y};
  constexpr Point P3{Wiimote::IR_HIGH_X, Wiimote::IR_HIGH_Y};
  constexpr Point P4{Wiimote::IR_LOW_X, Wiimote::IR_HIGH_Y};

  const auto low = [](u16 v) { return static_cast<u8>(v & 0xff); };
  const auto top = [](u16 v) { return (v >> 8) & 0b11; };
  const auto mix = [&](const Point& a, const Point& b) {
    return static_cast<u8>((top(a.y) << 6) | (top(a.x) << 4) | (top(b.y) << 2) | top(b.x));
  };

  std::array<u8, 11> block = {low(P1.x), low(P1.y), mix(P1, P2), low(P2.x),
                              low(P2.y), low(P3.x), low(P3.y),   mix(P3, P4),
                              low(P4.x), low(P4.y), 0};
  UpdateCalibrationDataChecksum(block, 1);
  return block;
}

// The default accelerometer calibration: zero-g and one-g per axis (each followed by a byte of
// low bits) and a checksum.
std::array<u8, 10> MakeAccelCalibration()
{
  constexpr u8 ZERO = Wiimote::ACCEL_ZERO_G;
  constexpr u8 ONE = Wiimote::ACCEL_ONE_G;
  std::array<u8, 10> block = {ZERO, ZERO, ZERO, 0, ONE, ONE, ONE, 0, 0, 0};
  UpdateCalibrationDataChecksum(block, 1);
  return block;
}

// Found at 0x16D0 on real remotes; meaning unknown.
constexpr std::array<u8, 24> EEPROM_16D0 = {0x00, 0x00, 0x00, 0xFF, 0x11, 0xEE, 0x00, 0x00,
                                            0x33, 0xCC, 0x44, 0xBB, 0x00, 0x00, 0x66, 0x99,
                                            0x77, 0x88, 0x00, 0x00, 0x2B, 0x01, 0xE8, 0x13};

void ReadFileInto(const std::string& path, u8* out, std::size_t size)
{
  std::ifstream file;
  File::OpenFStream(file, path, std::ios::binary | std::ios::in);
  file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
}

void WriteFileFrom(const std::string& path, const u8* in, std::size_t size)
{
  std::ofstream file;
  File::OpenFStream(file, path, std::ios::binary | std::ios::out);
  file.write(reinterpret_cast<const char*>(in), static_cast<std::streamsize>(size));
}

// Rounds value * scale to an integer in [0, limit].
long ScaleAndClamp(ControlState value, int limit)
{
  return std::clamp(std::lround(value * ControlState(limit)), 0l, long(limit));
}

// Camera points taken directly from the IR passthrough inputs (coordinates and size 0..1).
std::array<CameraPoint, CameraLogic::NUM_POINTS>
PassthroughCameraPoints(ControllerEmu::IRPassthrough* passthrough)
{
  std::array<CameraPoint, CameraLogic::NUM_POINTS> points;
  for (std::size_t i = 0; i != points.size(); ++i)
  {
    const ControlState size = passthrough->GetObjectSize(i);
    if (size <= 0.0f)
      continue;

    const ControlState x = passthrough->GetObjectPositionX(i);
    const ControlState y = passthrough->GetObjectPositionY(i);

    points[i].position.x = static_cast<u16>(ScaleAndClamp(x, CameraLogic::CAMERA_RES_X - 1));
    points[i].position.y = static_cast<u16>(ScaleAndClamp(y, CameraLogic::CAMERA_RES_Y - 1));
    points[i].size = static_cast<u8>(ScaleAndClamp(size, CameraLogic::MAX_POINT_SIZE));
  }
  return points;
}
}  // namespace

void Wiimote::Reset()
{
  const bool deterministic = Core::WantsDeterminism();

  SetRumble(false);

  // Power-on reporting: core buttons, only on change.
  m_reporting_mode = InputReportID::ReportCore;
  m_reporting_continuous = false;

  m_speaker_mute = false;

  // The EEPROM is kept in a file per remote in the session's Wii root. Changes from the previous
  // session are written out first, then the file is read back. In deterministic modes (netplay,
  // movies) the defaults are always used instead.
  // TODO: Saving and loading under different Wii roots or determinism modes is not handled.
  const std::string wii_root = File::GetUserPath(D_SESSION_WIIROOT_IDX);
  const std::string eeprom_path = wii_root + "/" + GetName() + ".bin";

  if (m_eeprom_dirty && !deterministic)
  {
    INFO_LOG_FMT(WIIMOTE, "Saving EEPROM of {}", GetName());
    WriteFileFrom(eeprom_path, m_eeprom.data.data(), EEPROM_FREE_SIZE);
    m_eeprom_dirty = false;
  }

  m_eeprom = {};

  if (!deterministic && File::Exists(eeprom_path))
  {
    ReadFileInto(eeprom_path, m_eeprom.data.data(), EEPROM_FREE_SIZE);
  }
  else
  {
    m_eeprom.ir_calibration_1 = m_eeprom.ir_calibration_2 = MakeIRCalibration();
    m_eeprom.accel_calibration_1 = m_eeprom.accel_calibration_2 = MakeAccelCalibration();
    m_eeprom.unk_2 = EEPROM_16D0;

    // Use the console's Mii data, when there is any.
    const std::string mii_path = wii_root + "/mii.bin";
    if (File::Exists(mii_path))
    {
      ReadFileInto(mii_path, m_eeprom.mii_data_1.data(), m_eeprom.mii_data_1.size());
      m_eeprom.mii_data_2 = m_eeprom.mii_data_1;
    }
  }

  m_read_request = {};

  // Speaker and camera are always on the bus.
  m_i2c_bus.Reset();
  m_i2c_bus.AddSlave(&m_speaker_logic);
  m_i2c_bus.AddSlave(&m_camera_logic);

  // Start with nothing plugged in.
  m_is_motion_plus_attached = false;
  m_active_extension = ExtensionNumber::NONE;
  m_extension_port.AttachExtension(GetNoneExtension());
  m_motion_plus.GetExtPort().AttachExtension(GetNoneExtension());

  // Plug in what the user selected (which also resets it). Deterministic sessions wait for the
  // first desired state instead.
  if (!deterministic)
  {
    HandleExtensionSwap(static_cast<ExtensionNumber>(m_attachments->GetSelectedAttachment()),
                        m_motion_plus_setting.GetValue());
  }

  m_speaker_logic.Reset();
  m_camera_logic.Reset();

  // A cleared status (extension bit 0 included) makes the first update send a status report
  // when an extension is plugged in, as a real remote does.
  m_status = {};
  m_status.extension = 0;

  m_swing_state = {};
  m_tilt_state = {};
  m_point_state = {};
  m_shake_state = {};
  m_imu_cursor_state = {};
}

Wiimote::Wiimote(const unsigned int index) : m_index(index), m_bt_device_index(index)
{
  using Translatability = ControllerEmu::Translatability;

  m_speaker_logic.SetWiimoteIndex(m_index);

  m_buttons = new ControllerEmu::Buttons(BUTTONS_GROUP);
  groups.emplace_back(m_buttons);
  for (const char* name : {A_BUTTON, B_BUTTON, ONE_BUTTON, TWO_BUTTON, MINUS_BUTTON, PLUS_BUTTON})
    m_buttons->AddInput(Translatability::DoNotTranslate, name);
  m_buttons->AddInput(Translatability::DoNotTranslate, HOME_BUTTON, "HOME");

  m_dpad = new ControllerEmu::Buttons(DPAD_GROUP);
  groups.emplace_back(m_dpad);
  for (const char* direction : named_directions)
    m_dpad->AddInput(Translatability::Translate, direction);

  // i18n: "Point" refers to the action of pointing a Wii Remote.
  m_ir = new ControllerEmu::Cursor(IR_GROUP, _trans("Point"));
  groups.emplace_back(m_ir);
  m_shake = new ControllerEmu::Shake(_trans("Shake"));
  groups.emplace_back(m_shake);
  m_tilt = new ControllerEmu::Tilt(_trans("Tilt"));
  groups.emplace_back(m_tilt);
  m_swing = new ControllerEmu::Force(_trans("Swing"));
  groups.emplace_back(m_swing);

  // Pointing from motion sensors, with an adjustable camera field of view (in degrees).
  m_imu_ir = new ControllerEmu::IMUCursor("IMUIR", _trans("Point"));
  groups.emplace_back(m_imu_ir);
  const Common::DVec2 default_fov =
      Common::DVec2(CameraLogic::CAMERA_FOV_X, CameraLogic::CAMERA_FOV_Y) / MathUtil::TAU * 360;
  m_imu_ir->AddSetting(&m_fov_x_setting,
                       // i18n: FOV stands for "Field of view".
                       {_trans("Horizontal FOV"),
                        // i18n: The symbol/abbreviation for degrees (unit of angular measure).
                        _trans("°"),
                        // i18n: Refers to emulated wii remote camera properties.
                        _trans("Camera field of view (affects sensitivity of pointing).")},
                       default_fov.x, 0.01, 180);
  m_imu_ir->AddSetting(&m_fov_y_setting,
                       // i18n: FOV stands for "Field of view".
                       {_trans("Vertical FOV"),
                        // i18n: The symbol/abbreviation for degrees (unit of angular measure).
                        _trans("°"),
                        // i18n: Refers to emulated wii remote camera properties.
                        _trans("Camera field of view (affects sensitivity of pointing).")},
                       default_fov.y, 0.01, 180);

  m_ir_passthrough =
      new ControllerEmu::IRPassthrough(IR_PASSTHROUGH_GROUP, _trans("Point (Passthrough)"));
  groups.emplace_back(m_ir_passthrough);
  m_imu_accelerometer =
      new ControllerEmu::IMUAccelerometer(ACCELEROMETER_GROUP, _trans("Accelerometer"));
  groups.emplace_back(m_imu_accelerometer);
  m_imu_gyroscope = new ControllerEmu::IMUGyroscope(GYROSCOPE_GROUP, _trans("Gyroscope"));
  groups.emplace_back(m_imu_gyroscope);

  // Hotkeys that flip the sideways/upright options: two toggles, then two hold-to-flip keys.
  m_hotkeys = new ControllerEmu::ModifySettingsButton(_trans("Hotkeys"));
  groups.emplace_back(m_hotkeys);
  m_hotkeys->AddInput(_trans("Sideways Toggle"), true);
  m_hotkeys->AddInput(_trans("Upright Toggle"), true);
  m_hotkeys->AddInput(_trans("Sideways Hold"), false);
  m_hotkeys->AddInput(_trans("Upright Hold"), false);

  // The attachment list must follow ExtensionNumber.
  m_attachments = new ControllerEmu::Attachments(_trans("Extension"));
  groups.emplace_back(m_attachments);
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::None>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::Nunchuk>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::Classic>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::Guitar>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::Drums>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::Turntable>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::UDrawTablet>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::DrawsomeTablet>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::TaTaCon>());
  m_attachments->AddAttachment(std::make_unique<WiimoteEmu::Shinkansen>());
  m_attachments->AddSetting(&m_motion_plus_setting, {_trans("Attach MotionPlus")}, true);

  m_rumble = new ControllerEmu::ControlGroup(_trans("Rumble"));
  groups.emplace_back(m_rumble);
  m_rumble->AddOutput(Translatability::Translate, _trans("Motor"));

  m_options = new ControllerEmu::ControlGroup(_trans("Options"));
  groups.emplace_back(m_options);
  m_options->AddSetting(&m_speaker_logic.m_speaker_pan_setting,
                        {_trans("Speaker Pan"),
                         // i18n: The percent symbol.
                         _trans("%")},
                        0, -100, 100);
  m_options->AddSetting(&m_battery_setting,
                        {_trans("Battery"),
                         // i18n: The percent symbol.
                         _trans("%")},
                        95, 0, 100);
  // Both may be on at once: the remote then points left with its buttons facing the player.
  m_options->AddSetting(&m_upright_setting,
                        {UPRIGHT_OPTION, nullptr, nullptr, _trans("Upright Wii Remote")}, false);
  m_options->AddSetting(&m_sideways_setting,
                        {SIDEWAYS_OPTION, nullptr, nullptr, _trans("Sideways Wii Remote")}, false);

  Reset();

  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();
}

Wiimote::~Wiimote()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
}

std::string Wiimote::GetName() const
{
  if (m_index == WIIMOTE_BALANCE_BOARD)
    return "BalanceBoard";
  return fmt::format("Wiimote{}", m_index + 1);
}

InputConfig* Wiimote::GetConfig() const
{
  return ::Wiimote::GetConfig();
}

ControllerEmu::ControlGroup* Wiimote::GetWiimoteGroup(WiimoteGroup group) const
{
  switch (group)
  {
  case WiimoteGroup::Buttons:
    return m_buttons;
  case WiimoteGroup::DPad:
    return m_dpad;
  case WiimoteGroup::Shake:
    return m_shake;
  case WiimoteGroup::Point:
    return m_ir;
  case WiimoteGroup::Tilt:
    return m_tilt;
  case WiimoteGroup::Swing:
    return m_swing;
  case WiimoteGroup::Rumble:
    return m_rumble;
  case WiimoteGroup::Attachments:
    return m_attachments;
  case WiimoteGroup::Options:
    return m_options;
  case WiimoteGroup::Hotkeys:
    return m_hotkeys;
  case WiimoteGroup::IMUAccelerometer:
    return m_imu_accelerometer;
  case WiimoteGroup::IMUGyroscope:
    return m_imu_gyroscope;
  case WiimoteGroup::IMUPoint:
    return m_imu_ir;
  case WiimoteGroup::IRPassthrough:
    return m_ir_passthrough;
  }
  ASSERT(false);
  return nullptr;
}

namespace
{
template <typename ExtensionType>
ExtensionType* AttachmentAs(const ControllerEmu::Attachments* attachments, ExtensionNumber number)
{
  return static_cast<ExtensionType*>(attachments->GetAttachmentList()[number].get());
}
}  // namespace

ControllerEmu::ControlGroup* Wiimote::GetNunchukGroup(NunchukGroup group) const
{
  return AttachmentAs<Nunchuk>(m_attachments, ExtensionNumber::NUNCHUK)->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetClassicGroup(ClassicGroup group) const
{
  return AttachmentAs<Classic>(m_attachments, ExtensionNumber::CLASSIC)->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetGuitarGroup(GuitarGroup group) const
{
  return AttachmentAs<Guitar>(m_attachments, ExtensionNumber::GUITAR)->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetDrumsGroup(DrumsGroup group) const
{
  return AttachmentAs<Drums>(m_attachments, ExtensionNumber::DRUMS)->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetTurntableGroup(TurntableGroup group) const
{
  return AttachmentAs<Turntable>(m_attachments, ExtensionNumber::TURNTABLE)->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetUDrawTabletGroup(UDrawTabletGroup group) const
{
  return AttachmentAs<UDrawTablet>(m_attachments, ExtensionNumber::UDRAW_TABLET)->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetDrawsomeTabletGroup(DrawsomeTabletGroup group) const
{
  return AttachmentAs<DrawsomeTablet>(m_attachments, ExtensionNumber::DRAWSOME_TABLET)
      ->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetTaTaConGroup(TaTaConGroup group) const
{
  return AttachmentAs<TaTaCon>(m_attachments, ExtensionNumber::TATACON)->GetGroup(group);
}

ControllerEmu::ControlGroup* Wiimote::GetShinkansenGroup(ShinkansenGroup group) const
{
  return AttachmentAs<Shinkansen>(m_attachments, ExtensionNumber::SHINKANSEN)->GetGroup(group);
}

bool Wiimote::ProcessExtensionPortEvent()
{
  if (m_extension_port.IsDeviceConnected() == bool(m_status.extension))
    return false;

  // A plug or unplug stops data reporting until the game picks a mode again (WiiBrew). This
  // also applies during a read request, which carries on after the status report.
  m_reporting_mode = InputReportID::ReportDisabled;

  DEBUG_LOG_FMT(WIIMOTE, "Extension plugged or unplugged: sending a status report");
  HandleRequestStatus(OutputReportRequestStatus{});

  return true;
}

void Wiimote::UpdateButtonsStatus(const DesiredWiimoteState& target_state)
{
  m_status.buttons.hex = target_state.buttons.hex & ButtonData::BUTTON_MASK;
}

void Wiimote::BuildDesiredWiimoteState(DesiredWiimoteState* target_state,
                                       SensorBarState sensor_bar_state)
{
  // The orientation hotkeys feed IsSideways() and IsUpright(), so they come first.
  m_hotkeys->UpdateState();

  StepDynamics();

  ButtonData& buttons = target_state->buttons;
  buttons.hex = 0;
  m_buttons->GetState(&buttons.hex, CORE_BUTTON_BITS, m_input_override_function);
  m_dpad->GetState(&buttons.hex, IsSideways() ? DPAD_SIDEWAYS_BITS : DPAD_BITS,
                   m_input_override_function);

  // The 8 bit calibration values scaled to the 10 bit data.
  target_state->acceleration =
      ConvertAccelData(GetTotalAcceleration(), ACCEL_ZERO_G << 2, ACCEL_ONE_G << 2);

  if (m_ir_passthrough->enabled.GetValue() && m_ir_passthrough->AreInputsBound())
  {
    target_state->camera_points = PassthroughCameraPoints(m_ir_passthrough);
  }
  else if (sensor_bar_state == SensorBarState::Enabled)
  {
    const Common::Vec2 fov_degrees(float(m_fov_x_setting.GetValue()),
                                   float(m_fov_y_setting.GetValue()));
    target_state->camera_points = CameraLogic::GetCameraPoints(
        GetTotalTransformation(), fov_degrees / 360 * float(MathUtil::TAU));
  }
  else
  {
    // A dark sensor bar: nothing to see.
    target_state->camera_points = DesiredWiimoteState::DEFAULT_CAMERA;
  }

  if (m_motion_plus_setting.GetValue())
    target_state->motion_plus = MotionPlus::GetGyroscopeData(GetTotalAngularVelocity());
  else
    target_state->motion_plus = std::nullopt;

  // The selected extension reads its input (and steps its own simulation, e.g. the Nunchuk's).
  AttachmentAs<Extension>(m_attachments,
                          static_cast<ExtensionNumber>(m_attachments->GetSelectedAttachment()))
      ->BuildDesiredExtensionState(&target_state->extension);
}

u8 Wiimote::GetWiimoteDeviceIndex() const
{
  return m_bt_device_index;
}

void Wiimote::SetWiimoteDeviceIndex(u8 index)
{
  m_bt_device_index = index;
}

// Runs at ::Wiimote::UPDATE_FREQ (200 Hz).
void Wiimote::PrepareInput(WiimoteEmu::DesiredWiimoteState* target_state,
                           SensorBarState sensor_bar_state)
{
  const auto lock = GetStateLock();
  BuildDesiredWiimoteState(target_state, sensor_bar_state);
}

void Wiimote::Update(const WiimoteEmu::DesiredWiimoteState& target_state)
{
  // Nearly every input report carries the buttons.
  UpdateButtonsStatus(target_state);

  // Extension changes made in the GUI take effect here.
  HandleExtensionSwap(static_cast<ExtensionNumber>(target_state.extension.data.index()),
                      target_state.motion_plus.has_value());

  GetActiveExtension()->Update(target_state.extension);

  if (m_is_motion_plus_attached)
    m_motion_plus.Update(target_state.extension);

  // A status report (after a plug event) or a read reply replaces this update's data report.
  if (ProcessExtensionPortEvent() || ProcessReadDataRequest())
    return;

  SendDataReport(target_state);
}

void Wiimote::SendDataReport(const DesiredWiimoteState& target_state)
{
  // After a plug event nothing is reported, not even button changes.
  if (m_reporting_mode == InputReportID::ReportDisabled)
    return;

  // TODO: Non-continuous reporting should only send when the data changed; reports are
  // currently sent every update in every mode.

  DataReportBuilder report(m_reporting_mode);

  if (report.HasCore())
    report.SetCoreData(m_status.buttons);

  if (report.HasAccel())
    report.SetAccelData(target_state.acceleration);

  if (report.HasIR())
  {
    // The camera holds no state of its own between updates, so it can be refreshed here.
    m_camera_logic.Update(target_state.camera_points);

    // The remote fetches the camera data over i2c; the format decides where in it to start.
    const u8 offset = CameraLogic::REPORT_DATA_OFFSET + report.GetIRDataFormatOffset();
    u8* const ir_out = report.GetIRDataPtr();
    const u8 ir_size = report.GetIRDataSize();

    // A disabled camera does not answer (common while games change the sensitivity).
    if (m_i2c_bus.BusRead(CameraLogic::I2C_ADDR, offset, ir_size, ir_out) != ir_size)
      std::fill_n(ir_out, ir_size, u8(0xff));
  }

  if (report.HasExt())
  {
    // The extension's own data was prepared in Update(); the MotionPlus may read it now.
    // TODO: Prepare the MotionPlus data when the bus read happens instead.
    if (m_is_motion_plus_attached)
    {
      m_motion_plus.PrepareInput(target_state.motion_plus.has_value() ?
                                     *target_state.motion_plus :
                                     MotionPlus::GetDefaultGyroscopeData());
    }

    u8* const ext_out = report.GetExtDataPtr();
    const u8 ext_size = report.GetExtDataSize();

    // An unanswered read fills the field with 0xff, as on hardware.
    if (m_i2c_bus.BusRead(ExtensionPort::REPORT_I2C_SLAVE, ExtensionPort::REPORT_I2C_ADDR,
                          ext_size, ext_out) != ext_size)
    {
      std::fill_n(ext_out, ext_size, u8(0xff));
    }
  }

  InterruptDataInputCallback(report.GetDataPtr(), report.GetDataSize());

  // The two interleaved modes alternate.
  if (m_reporting_mode == InputReportID::ReportInterleave1)
    m_reporting_mode = InputReportID::ReportInterleave2;
  else if (m_reporting_mode == InputReportID::ReportInterleave2)
    m_reporting_mode = InputReportID::ReportInterleave1;
}

ButtonData Wiimote::GetCurrentlyPressedButtons()
{
  const auto lock = GetStateLock();

  ButtonData pressed{};
  m_buttons->GetState(&pressed.hex, CORE_BUTTON_BITS, m_input_override_function);
  m_dpad->GetState(&pressed.hex, IsSideways() ? DPAD_SIDEWAYS_BITS : DPAD_BITS,
                   m_input_override_function);
  return pressed;
}

void Wiimote::LoadDefaults(const ControllerInterface& ciface)
{
  EmulatedController::LoadDefaults(ciface);

  const auto set_all = [](ControllerEmu::ControlGroup* group,
                          std::initializer_list<const char*> expressions) {
    int i = 0;
    for (const char* expression : expressions)
      group->SetControlExpression(i++, expression);
  };

#ifdef ANDROID
  m_rumble->SetControlExpression(0, "`Android/0/Device Sensors:Motor 0`");

  set_all(m_imu_accelerometer, {"`Android/0/Device Sensors:Accel Up`",
                                "`Android/0/Device Sensors:Accel Down`",
                                "`Android/0/Device Sensors:Accel Left`",
                                "`Android/0/Device Sensors:Accel Right`",
                                "`Android/0/Device Sensors:Accel Forward`",
                                "`Android/0/Device Sensors:Accel Backward`"});
  set_all(m_imu_gyroscope, {"`Android/0/Device Sensors:Gyro Pitch Up`",
                            "`Android/0/Device Sensors:Gyro Pitch Down`",
                            "`Android/0/Device Sensors:Gyro Roll Left`",
                            "`Android/0/Device Sensors:Gyro Roll Right`",
                            "`Android/0/Device Sensors:Gyro Yaw Left`",
                            "`Android/0/Device Sensors:Gyro Yaw Right`"});
#else
  // A and B on mouse buttons, then 1, 2, -, + and Home on the keyboard.
#if defined HAVE_X11 && HAVE_X11
  const char* const a_button = "`Click 1`";
  const char* const b_button = "`Click 3`";
#elif defined(__APPLE__)
  const char* const a_button = "`Left Click`";
  const char* const b_button = "`Right Click`";
#else
  const char* const a_button = "`Click 0`";
  const char* const b_button = "`Click 1`";
#endif
#ifdef _WIN32
  const char* const home_key = "RETURN";
#else
  const char* const home_key = "Return";
#endif
  set_all(m_buttons, {a_button, b_button, "`1`", "`2`", "Q", "E", home_key});

#ifdef __APPLE__
  const char* const shake_button = "`Middle Click`";
#else
  const char* const shake_button = "`Click 2`";
#endif
  set_all(m_shake, {shake_button, shake_button, shake_button});

  set_all(m_ir, {"`Cursor Y-`", "`Cursor Y+`", "`Cursor X-`", "`Cursor X+`"});

#ifdef _WIN32
  set_all(m_dpad, {"UP", "DOWN", "LEFT", "RIGHT"});
#elif __APPLE__
  set_all(m_dpad, {"`Up Arrow`", "`Down Arrow`", "`Left Arrow`", "`Right Arrow`"});
#else
  set_all(m_dpad, {"Up", "Down", "Left", "Right"});
#endif

  set_all(m_imu_accelerometer, {"`Accel Up`", "`Accel Down`", "`Accel Left`", "`Accel Right`",
                                "`Accel Forward`", "`Accel Backward`"});
  set_all(m_imu_gyroscope, {"`Gyro Pitch Up`", "`Gyro Pitch Down`", "`Gyro Roll Left`",
                            "`Gyro Roll Right`", "`Gyro Yaw Left`", "`Gyro Yaw Right`"});

  // Three inputs (X, Y, size) per IR object.
  for (int object = 1; object <= 4; ++object)
  {
    const int first = (object - 1) * 3;
    m_ir_passthrough->SetControlExpression(first, fmt::format("`IR Object {} X`", object));
    m_ir_passthrough->SetControlExpression(first + 1, fmt::format("`IR Object {} Y`", object));
    m_ir_passthrough->SetControlExpression(first + 2,
                                           fmt::format("`IR Object {} Size`", object));
  }
#endif

  // A Nunchuk is plugged in by default.
  m_attachments->SetSelectedAttachment(ExtensionNumber::NUNCHUK);
  m_attachments->GetAttachmentList()[ExtensionNumber::NUNCHUK]->LoadDefaults();
}

Extension* Wiimote::GetNoneExtension() const
{
  return AttachmentAs<Extension>(m_attachments, ExtensionNumber::NONE);
}

Extension* Wiimote::GetActiveExtension() const
{
  return AttachmentAs<Extension>(m_attachments, m_active_extension);
}

// The option is flipped by the toggle hotkey and by the hold hotkey.
bool Wiimote::IsSideways() const
{
  const auto& modifiers = m_hotkeys->GetSettingsModifier();
  return m_sideways_setting.GetValue() ^ bool(modifiers[0]) ^ bool(modifiers[2]);
}

bool Wiimote::IsUpright() const
{
  const auto& modifiers = m_hotkeys->GetSettingsModifier();
  return m_upright_setting.GetValue() ^ bool(modifiers[1]) ^ bool(modifiers[3]);
}

void Wiimote::SetRumble(bool on)
{
  const auto lock = GetStateLock();
  m_rumble->controls.front()->control_ref->State(on);
}

void Wiimote::RefreshConfig()
{
  m_speaker_logic.SetSpeakerEnabled(Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER));
}

void Wiimote::StepDynamics()
{
  EmulateSwing(&m_swing_state, m_swing, UPDATE_PERIOD);
  EmulateTilt(&m_tilt_state, m_tilt, UPDATE_PERIOD);
  EmulatePoint(&m_point_state, m_ir, m_input_override_function, UPDATE_PERIOD);
  EmulateShake(&m_shake_state, m_shake, UPDATE_PERIOD);
  EmulateIMUCursor(&m_imu_cursor_state, m_imu_ir, m_imu_accelerometer, m_imu_gyroscope,
                   UPDATE_PERIOD);
}

Common::Vec3 Wiimote::GetAcceleration(Common::Vec3 extra_acceleration) const
{
  // Swing acceleration and gravity are rotated with the remote; shaking is not (and never has
  // been).
  const Common::Vec3 moved =
      GetTransformation().Transform(m_swing_state.acceleration + extra_acceleration, 0);
  Common::Vec3 total = GetOrientation() * moved;
  total += m_shake_state.acceleration;
  return total;
}

Common::Vec3 Wiimote::GetAngularVelocity(Common::Vec3 extra_angular_velocity) const
{
  const Common::Vec3 sum = m_tilt_state.angular_velocity + m_swing_state.angular_velocity +
                           m_point_state.angular_velocity + extra_angular_velocity;
  return GetOrientation() * sum;
}

Common::Matrix44 Wiimote::GetTransformation(const Common::Matrix33& extra_rotation) const
{
  // Shake moves the world, then the rotations of tilt, point and swing apply, then the
  // positions of swing and point.
  // TODO: Revisit the order (and make the Nunchuk use the same one).
  const Common::Matrix33 rotation = extra_rotation * GetRotationalMatrix(-m_tilt_state.angle) *
                                    GetRotationalMatrix(-m_point_state.angle) *
                                    GetRotationalMatrix(-m_swing_state.angle);
  const Common::Matrix44 shake = Common::Matrix44::Translate(-m_shake_state.position);
  const Common::Matrix44 offset =
      Common::Matrix44::Translate(-m_swing_state.position - m_point_state.position);
  return shake * Common::Matrix44::FromMatrix33(rotation) * offset;
}

Common::Quaternion Wiimote::GetOrientation() const
{
  // Sideways turns the remote a quarter turn about Z; upright a quarter turn about X.
  const auto quarter_turns = [](double quarter, bool on) { return float(quarter * on); };
  const Common::Quaternion sideways =
      Common::Quaternion::RotateZ(quarter_turns(MathUtil::TAU / -4, IsSideways()));
  const Common::Quaternion upright =
      Common::Quaternion::RotateX(quarter_turns(MathUtil::TAU / 4, IsUpright()));
  return sideways * upright;
}

namespace
{
// Runs the override function for each axis of control_group; returns whether any applied.
bool ApplyAxisOverrides(const ControllerEmu::ControlGroup* control_group, Common::Vec3* vec,
                        const ControllerEmu::InputOverrideFunction& override_function)
{
  if (!override_function)
    return false;

  constexpr std::string_view AXES[] = {ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE,
                                       ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE,
                                       ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE};
  bool overridden = false;
  for (std::size_t i = 0; i != 3; ++i)
  {
    const std::optional<ControlState> value =
        override_function(control_group->name, AXES[i], vec->data[i]);
    if (value)
    {
      vec->data[i] = float(*value);
      overridden = true;
    }
  }
  return overridden;
}
}  // namespace

std::optional<Common::Vec3> Wiimote::OverrideVec3(const ControllerEmu::ControlGroup* control_group,
                                                  std::optional<Common::Vec3> optional_vec) const
{
  Common::Vec3 vec = optional_vec.value_or(Common::Vec3{});
  const bool overridden = ApplyAxisOverrides(control_group, &vec, m_input_override_function);
  if (overridden || optional_vec.has_value())
    return vec;
  return std::nullopt;
}

Common::Vec3 Wiimote::OverrideVec3(const ControllerEmu::ControlGroup* control_group,
                                   Common::Vec3 vec) const
{
  return OverrideVec3(control_group, vec, m_input_override_function);
}

Common::Vec3
Wiimote::OverrideVec3(const ControllerEmu::ControlGroup* control_group, Common::Vec3 vec,
                      const ControllerEmu::InputOverrideFunction& input_override_function)
{
  ApplyAxisOverrides(control_group, &vec, input_override_function);
  return vec;
}

Common::Vec3 Wiimote::GetTotalAcceleration() const
{
  // Without a motion source, gravity alone.
  const Common::Vec3 measured = m_imu_accelerometer->GetState().value_or(
      Common::Vec3(0, 0, float(GRAVITY_ACCELERATION)));
  return OverrideVec3(m_imu_accelerometer, GetAcceleration(measured));
}

Common::Vec3 Wiimote::GetTotalAngularVelocity() const
{
  const Common::Vec3 measured = m_imu_gyroscope->GetState().value_or(Common::Vec3{});
  return OverrideVec3(m_imu_gyroscope, GetAngularVelocity(measured));
}

Common::Matrix44 Wiimote::GetTotalTransformation() const
{
  const Common::Quaternion imu_rotation =
      m_imu_cursor_state.rotation *
      Common::Quaternion::RotateX(m_imu_cursor_state.recentered_pitch);
  return GetTransformation(Common::Matrix33::FromQuaternion(imu_rotation));
}

}  // namespace WiimoteEmu
