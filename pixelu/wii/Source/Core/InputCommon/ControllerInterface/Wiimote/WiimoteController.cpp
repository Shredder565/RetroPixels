// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/Wiimote/WiimoteController.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "Common/BitUtils.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ciface::WiimoteController
{
using namespace WiimoteCommon;

namespace
{
constexpr char SOURCE_NAME[] = "Bluetooth";

// The pool's remotes need an index; it only shows up in their log messages.
constexpr int POOL_LOG_INDEX = 55;

// Replies not seen within this time are given up on.
constexpr auto REPLY_TIMEOUT = std::chrono::seconds{5};
// How long the battery level from a status report is trusted.
constexpr auto STATUS_LIFETIME = std::chrono::seconds(10);
// How long the MotionPlus is left alone after a mode change (it resets).
constexpr auto MOTION_PLUS_SETTLE_TIME = std::chrono::seconds{2};
// The motor is switched on for part of each period to give weaker rumble.
constexpr auto RUMBLE_PERIOD = std::chrono::milliseconds(100);

// The data reporting mode used: the only one with core buttons, accelerometer, IR and
// extension data all together.
constexpr InputReportID FULL_REPORTING_MODE = InputReportID::ReportCoreAccelIR10Ext6;

// Memory locations.
constexpr u16 EEPROM_ACCEL_CALIBRATION = 0x16;
constexpr u16 EXT_ENCRYPTION = 0xfb;
constexpr u16 EXT_INIT = 0xf0;
constexpr u8 EXT_INIT_VALUE = 0x55;
constexpr u16 EXT_IDENTIFIER = 0xfa;
constexpr u16 EXT_IDENTIFIER_SIZE = 6;
// The MotionPlus copies the pass-through extension's ID here (bytes 0xf6, 0xf8, 0xf9 matter).
constexpr u16 MPLUS_PASSTHROUGH_IDENTIFIER = 0xf6;
constexpr u16 EXT_CALIBRATION = 0x20;
// The MotionPlus copies the pass-through extension's calibration here.
constexpr u16 MPLUS_PASSTHROUGH_CALIBRATION = 0x40;
constexpr u16 EXT_CALIBRATION_SIZE = 0x10;
constexpr u16 CAMERA_SENSITIVITY_1 = 0x00;
constexpr u16 CAMERA_SENSITIVITY_2 = 0x1a;
constexpr u16 CAMERA_MODE = 0x33;
// Writing 0x08 here starts object tracking.
constexpr u16 CAMERA_CONTROL = 0x30;
constexpr u8 CAMERA_TRACKING_ON = 0x08;

// The five sensor bar sensitivity levels of the Wii settings, as written to the camera.
struct CameraSensitivity
{
  std::array<u8, 9> first;
  std::array<u8, 2> second;
};
constexpr std::array<CameraSensitivity, 5> CAMERA_SENSITIVITIES{{
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xfe}, {0xfd, 0x05}},
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xb4}, {0xb3, 0x04}},
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64}, {0x63, 0x03}},
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xc8, 0x00, 0x36}, {0x35, 0x03}},
    {{0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20}, {0x1f, 0x03}},
}};

// The level from the Wii settings (stored there as 1-5), or the middle one if out of range.
u32 WantedCameraSensitivity()
{
  const u32 level = Config::Get(Config::SYSCONF_SENSOR_BAR_SENSITIVITY) - 1;
  return level < CAMERA_SENSITIVITIES.size() ? level : 2;
}

// An input reading `*source / divisor`.
template <typename T>
class ValueInput final : public Core::Device::Input
{
public:
  ValueInput(const T* source, std::string name, ControlState divisor, bool detectable)
      : m_source(source), m_name(std::move(name)), m_divisor(divisor), m_detectable(detectable)
  {
  }

  std::string GetName() const override { return m_name; }
  bool IsDetectable() const override { return m_detectable; }
  ControlState GetState() const override { return ControlState(*m_source) / m_divisor; }

private:
  const T* const m_source;
  const std::string m_name;
  const ControlState m_divisor;
  const bool m_detectable;
};

// A bit of a button bitfield.
template <typename T>
class BitInput final : public Core::Device::Input
{
public:
  BitInput(const T* bits, T mask, std::string name)
      : m_bits(bits), m_mask(mask), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return (*m_bits & m_mask) != 0; }

private:
  const T* const m_bits;
  const T m_mask;
  const std::string m_name;
};

class RumbleOutput final : public Core::Device::Output
{
public:
  explicit RumbleOutput(std::atomic<ControlState>* strength) : m_strength(strength) {}

  std::string GetName() const override { return "Motor"; }
  void SetState(ControlState state) override { *m_strength = state; }

private:
  std::atomic<ControlState>* const m_strength;
};

// Builds a matcher for replies of type R: `on_reply` gets the reply and returns whether it was
// the awaited one.
template <typename R, typename F>
std::function<bool(const WiimoteReal::Report&)> ReplyOf(F on_reply)
{
  return [handler = std::move(on_reply)](const WiimoteReal::Report& report) mutable {
    constexpr std::size_t body_offset = WiimoteReal::REPORT_HID_HEADER_SIZE + 1;
    if (report[WiimoteReal::REPORT_HID_HEADER_SIZE] != u8(R::REPORT_ID))
      return false;

    R reply{};
    if (report.size() < body_offset + sizeof(R))
    {
      // Some third-party remotes send short read replies; the missing bytes are taken as zero.
      WARN_LOG_FMT(WIIMOTE, "WiiRemote: Bad report size ({}) for report {:#x}. Zero-filling.",
                   report.size(), int(R::REPORT_ID));
      std::memcpy(&reply, report.data() + body_offset, report.size() - body_offset);
    }
    else
    {
      std::memcpy(&reply, report.data() + body_offset, sizeof(R));
    }
    return handler(std::as_const(reply));
  };
}

// Matches the acknowledgement of output report `id`.
std::function<bool(const WiimoteReal::Report&)> AckOf(OutputReportID id,
                                                      std::function<void(ErrorCode)> on_ack)
{
  return ReplyOf<InputReportAck>([id, notify = std::move(on_ack)](const InputReportAck& ack) {
    if (ack.rpt_id != id)
      return false;
    notify(ack.error_code);
    return true;
  });
}

template <typename Report>
void SetAddress(Report* report, AddressSpace space, u8 slave, u16 address)
{
  report->space = u8(space);
  report->slave_address = slave;
  report->address[0] = u8(address >> 8);
  report->address[1] = u8(address);
}

}  // namespace

void AddDevice(std::unique_ptr<WiimoteReal::Wiimote> wiimote)
{
  if (!wiimote->Connect(POOL_LOG_INDEX))
  {
    WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to connect.");
    return;
  }

  wiimote->Prepare();
  wiimote->EventLinked();
  g_controller_interface.AddDevice(std::make_shared<Device>(std::move(wiimote)));
}

void ReleaseDevices(std::optional<u32> count)
{
  // Removal is forced: the device must be gone now (not whenever its last reference is
  // dropped) so its remote can be handed out again at once.
  u32 released = 0;
  const auto should_release = [&](const Core::Device* device) {
    if (device->GetSource() != SOURCE_NAME)
      return false;
    if (count == released)
      return false;
    ++released;
    return true;
  };
  g_controller_interface.RemoveDevice(should_release, true);
}

Device::Device(std::unique_ptr<WiimoteReal::Wiimote> wiimote) : m_wiimote(std::move(wiimote))
{
  using Remote = WiimoteEmu::Wiimote;
  using WiimoteEmu::Classic;
  using WiimoteEmu::Nunchuk;

  const auto add_bit = [this]<typename T>(const T* bits, std::type_identity_t<T> mask,
                                          std::string name) {
    AddInput(new BitInput<T>(bits, mask, std::move(name)));
  };
  const auto add_value = [this]<typename T>(const T* source, std::string name, ControlState divisor,
                                            bool detectable) {
    AddInput(new ValueInput<T>(source, std::move(name), divisor, detectable));
  };
  // The negative and the positive half of an axis, named "<name>-" and "<name>+".
  const auto add_axis = [&](const float* source, const std::string& name, bool detectable) {
    add_value(source, name + '-', -1.0, detectable);
    add_value(source, name + '+', 1.0, detectable);
  };
  // An axis whose halves have names of their own, the first one for positive values.
  const auto add_named_axis = [&](const float* source, const std::string& positive,
                                  const std::string& negative) {
    add_value(source, positive, 1.0, false);
    add_value(source, negative, -1.0, false);
  };

  static constexpr std::array<std::pair<const char*, const char*>, 3> accel_axes{{
      {"Accel Left", "Accel Right"},
      {"Accel Backward", "Accel Forward"},
      {"Accel Up", "Accel Down"},
  }};
  static constexpr std::array<std::pair<const char*, const char*>, 3> gyro_axes{{
      {"Gyro Pitch Down", "Gyro Pitch Up"},
      {"Gyro Roll Left", "Gyro Roll Right"},
      {"Gyro Yaw Left", "Gyro Yaw Right"},
  }};

  // The remote itself.
  const u16* const core_bits = &m_buttons.hex;
  add_bit(core_bits, Remote::BUTTON_A, "A");
  add_bit(core_bits, Remote::BUTTON_B, "B");
  add_bit(core_bits, Remote::BUTTON_ONE, "1");
  add_bit(core_bits, Remote::BUTTON_TWO, "2");
  add_bit(core_bits, Remote::BUTTON_MINUS, "-");
  add_bit(core_bits, Remote::BUTTON_PLUS, "+");
  add_bit(core_bits, Remote::BUTTON_HOME, "HOME");

  add_axis(&m_rotation.x, "Pitch", false);
  add_axis(&m_rotation.y, "Roll", false);
  add_axis(&m_rotation.z, "Yaw", false);

  const std::array<u16, 4> remote_dpad{Remote::PAD_UP, Remote::PAD_DOWN, Remote::PAD_LEFT,
                                       Remote::PAD_RIGHT};
  for (std::size_t i = 0; i != remote_dpad.size(); ++i)
    add_bit(core_bits, remote_dpad[i], named_directions[i]);

  for (std::size_t axis = 0; axis != accel_axes.size(); ++axis)
    add_named_axis(&m_accel.data[axis], accel_axes[axis].first, accel_axes[axis].second);

  add_axis(&m_camera.center.x, "IR Center X", false);
  add_axis(&m_camera.center.y, "IR Center Y", false);
  add_value(&m_camera.hidden, "IR Hidden", 1.0, false);
  add_value(&m_camera.distance, "IR Distance", 1.0, false);
  for (std::size_t i = 0; i != m_camera.object_position.size(); ++i)
  {
    const std::size_t number = i + 1;
    add_value(&m_camera.object_position[i].x, fmt::format("IR Object {} X", number), 1.0, false);
    add_value(&m_camera.object_position[i].y, fmt::format("IR Object {} Y", number), 1.0, false);
    add_value(&m_camera.object_size[i], fmt::format("IR Object {} Size", number), 1.0, false);
  }

  for (std::size_t axis = 0; axis != gyro_axes.size(); ++axis)
  {
    add_named_axis(&m_motion_plus.angular_velocity.data[axis], gyro_axes[axis].first,
                   gyro_axes[axis].second);
  }

  // Nunchuk.
  const std::string nunchuk = "Nunchuk ";
  add_bit(&m_nunchuk.buttons, Nunchuk::BUTTON_C, nunchuk + "C");
  add_bit(&m_nunchuk.buttons, Nunchuk::BUTTON_Z, nunchuk + "Z");
  add_axis(&m_nunchuk.stick.x, nunchuk + "X", true);
  add_axis(&m_nunchuk.stick.y, nunchuk + "Y", true);
  for (std::size_t axis = 0; axis != accel_axes.size(); ++axis)
  {
    add_named_axis(&m_nunchuk.accel.data[axis], nunchuk + accel_axes[axis].first,
                   nunchuk + accel_axes[axis].second);
  }

  // Classic Controller.
  const std::string classic = "Classic ";
  const std::array<u16, 4> classic_dpad{Classic::PAD_UP, Classic::PAD_DOWN, Classic::PAD_LEFT,
                                        Classic::PAD_RIGHT};
  for (std::size_t i = 0; i != classic_dpad.size(); ++i)
    add_bit(&m_classic.buttons, classic_dpad[i], classic + named_directions[i]);

  static constexpr std::array<std::pair<u16, const char*>, 11> classic_buttons{{
      {Classic::BUTTON_A, "A"},
      {Classic::BUTTON_B, "B"},
      {Classic::BUTTON_X, "X"},
      {Classic::BUTTON_Y, "Y"},
      {Classic::TRIGGER_L, "L"},
      {Classic::TRIGGER_R, "R"},
      {Classic::BUTTON_ZL, "ZL"},
      {Classic::BUTTON_ZR, "ZR"},
      {Classic::BUTTON_MINUS, "-"},
      {Classic::BUTTON_PLUS, "+"},
      {Classic::BUTTON_HOME, "HOME"},
  }};
  for (const auto& [mask, name] : classic_buttons)
    add_bit(&m_classic.buttons, mask, classic + name);

  add_axis(&m_classic.sticks[0].x, classic + "Left X", true);
  add_axis(&m_classic.sticks[0].y, classic + "Left Y", true);
  add_axis(&m_classic.sticks[1].x, classic + "Right X", true);
  add_axis(&m_classic.sticks[1].y, classic + "Right Y", true);
  add_value(&m_classic.triggers[0], classic + "L-Analog", 1.0, true);
  add_value(&m_classic.triggers[1], classic + "R-Analog", 1.0, true);

  // Status.
  add_value(&m_battery, "Battery", 1.0, false);
  add_value(&m_attached_extension, "Attached Extension", 1.0, false);
  add_value(&m_attached_motion_plus, "Attached MotionPlus", 1.0, false);

  AddOutput(new RumbleOutput(&m_rumble_strength));
}

Device::~Device()
{
  if (!m_wiimote->IsConnected())
    return;

  m_wiimote->EmuStop();
  INFO_LOG_FMT(WIIMOTE, "WiiRemote: Returning remote to pool.");
  WiimoteReal::AddWiimoteToPool(std::move(m_wiimote));
}

std::string Device::GetName() const
{
  return "Wii Remote";
}

std::string Device::GetSource() const
{
  return SOURCE_NAME;
}

int Device::GetSortPriority() const
{
  // Listed last: these come and go.
  return -4;
}

Core::DeviceRemoval Device::UpdateInput()
{
  if (!m_wiimote->IsConnected())
    return Core::DeviceRemoval::Remove;

  UpdateRumble();
  RunTasks();

  WiimoteReal::Report report;
  while (m_wiimote->GetNextReport(&report))
  {
    HandleReport(report);
    RunTasks();
  }

  return Core::DeviceRemoval::Keep;
}

// ----- Report traffic -----

template <typename R>
void Device::Send(R report, AckCallback on_ack)
{
  report.rumble = m_rumble;
  m_wiimote->QueueReport(R::REPORT_ID, &report, sizeof(report));

  if (on_ack)
    Await({AckOf(R::REPORT_ID, std::move(on_ack))});
}

void Device::Await(std::vector<ReplyMatcher> matchers)
{
  m_pending_replies.push_back({Clock::now() + REPLY_TIMEOUT, std::move(matchers)});
}

void Device::ReadMemory(AddressSpace space, u8 slave, u16 address, u16 size, ReadCallback on_read)
{
  OutputReportReadData request{};
  SetAddress(&request, space, slave, address);
  request.size[0] = u8(size >> 8);
  request.size[1] = u8(size);
  Send(request);

  AwaitReadReply(address, size, {}, std::move(on_read));
}

void Device::AwaitReadReply(u16 address, u16 size, std::vector<u8> collected, ReadCallback on_read)
{
  // A busy remote acknowledges the read request with an error instead of replying.
  auto refused = AckOf(OutputReportID::ReadData, [on_read](ErrorCode error) {
    DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Read ack error: {}.", int(error));
    on_read(ReadResult{});
  });

  auto replied = ReplyOf<InputReportReadDataReply>(
      [this, address, size, bytes = std::move(collected),
       done = std::move(on_read)](const InputReportReadDataReply& reply) mutable {
        if (Common::swap16(reply.address) != address)
          return false;

        if (reply.error != u8(ErrorCode::Success))
        {
          DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Read reply error: {}.", int(reply.error));
          done(ReadResult{});
          return true;
        }

        // Replies carry up to 16 bytes; longer reads come in several.
        const int count = reply.size_minus_one + 1;
        bytes.insert(bytes.end(), reply.data, reply.data + count);
        if (count < size)
        {
          AwaitReadReply(static_cast<u16>(address + count), static_cast<u16>(size - count),
                         std::move(bytes), std::move(done));
        }
        else
        {
          done(std::move(bytes));
        }
        return true;
      });

  Await({std::move(replied), std::move(refused)});
}

void Device::WriteMemory(AddressSpace space, u8 slave, u16 address, std::span<const u8> bytes,
                         AckCallback on_done)
{
  OutputReportWriteData request{};
  SetAddress(&request, space, slave, address);

  // One report holds up to 16 bytes; the rest is written once that part is acknowledged.
  constexpr std::size_t chunk = std::size(request.data);
  const std::size_t now = std::min(bytes.size(), chunk);
  request.size = u8(now);
  std::copy_n(bytes.begin(), now, request.data);

  if (bytes.size() <= chunk)
  {
    Send(request, std::move(on_done));
    return;
  }

  Send(request,
       [this, space, slave, address,
        rest = std::vector<u8>(bytes.begin() + static_cast<std::ptrdiff_t>(chunk), bytes.end()),
        then = std::move(on_done)](ErrorCode result) mutable {
         if (result == ErrorCode::Success)
           WriteMemory(space, slave, static_cast<u16>(address + chunk), rest, std::move(then));
         else
           then(result);
       });
}

void Device::WriteByte(AddressSpace space, u8 slave, u16 address, u8 value, AckCallback on_done)
{
  WriteMemory(space, slave, address, std::span<const u8>(&value, 1), std::move(on_done));
}

// ----- Set-up -----

void Device::RunTasks()
{
  // One thing at a time: nothing new starts while a reply is outstanding.
  if (!m_pending_replies.empty())
    return;

  using Step = bool (Device::*)();
  static constexpr Step remote_steps[] = {
      &Device::RequestStatusIfStale,         &Device::SetPlayerLeds, &Device::SetReportingMode,
      &Device::ReadAccelerometerCalibration, &Device::SetUpCamera,   &Device::SilenceSpeaker,
  };
  // These involve the extension port, so they wait while the MotionPlus may be resetting.
  static constexpr Step port_steps[] = {
      &Device::IdentifyActiveExtension,    &Device::ResetUnsupportedExtension,
      &Device::PublishAttachments,         &Device::WakeInactiveMotionPlus,
      &Device::SwitchActiveMotionPlusMode, &Device::IdentifyPassthroughExtension,
      &Device::SettleMotionPlusMode,       &Device::ReadMotionPlusCalibration,
      &Device::ReadExtensionCalibration,
  };

  for (const Step step : remote_steps)
  {
    if ((this->*step)())
      return;
  }
  if (WaitingForMotionPlus())
    return;
  for (const Step step : port_steps)
  {
    if ((this->*step)())
      return;
  }
}

bool Device::RequestStatusIfStale()
{
  if (Clock::now() < m_status_due)
    return false;

  Send(OutputReportRequestStatus{});
  Await({ReplyOf<InputReportStatus>([this](const InputReportStatus& status) {
    DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Received requested status.");
    OnStatus(status);
    return true;
  })});
  return true;
}

bool Device::SetPlayerLeds()
{
  const u8 wanted = PlayerLeds();
  if (m_leds == wanted)
    return false;

  OutputReportLeds request{};
  request.ack = 1;
  request.leds = wanted;
  Send(request, [this, wanted](ErrorCode result) {
    if (result != ErrorCode::Success)
    {
      WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to set LEDs.");
      return;
    }
    DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Set LEDs.");
    m_leds = wanted;
  });
  return true;
}

// Remotes 1-4 light their own LED. Higher numbers are shown as LEDs whose numbers add up to the
// remote's number (5-7: LED 4 and one of 1-3; 8-9: LEDs 4 and 3 and one of 1-2); from 10 on all
// four are lit.
u8 Device::PlayerLeds() const
{
  const int id = GetId();
  if (id < 4)
    return static_cast<u8>(1 << id);
  if (id < 7)
    return static_cast<u8>(0x8 | (1 << (id - 4)));
  if (id < 9)
    return static_cast<u8>(0xc | (1 << (id - 7)));
  return 0xf;
}

bool Device::SetReportingMode()
{
  if (m_reporting_mode == FULL_REPORTING_MODE)
    return false;

  OutputReportMode request{};
  request.ack = 1;
  request.mode = FULL_REPORTING_MODE;
  Send(request, [this](ErrorCode result) {
    if (result != ErrorCode::Success)
    {
      WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to set reporting mode.");
      return;
    }
    m_reporting_mode = FULL_REPORTING_MODE;
    DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Set reporting mode.");
  });
  return true;
}

bool Device::ReadAccelerometerCalibration()
{
  if (m_accel_calibration.has_value())
    return false;

  ReadMemory(AddressSpace::EEPROM, 0, EEPROM_ACCEL_CALIBRATION, sizeof(AccelCalibrationData),
             [this](ReadResult result) {
               if (!result)
               {
                 WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to read accelerometer calibration.");
                 return;
               }
               DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Read accelerometer calibration.");

               std::vector<u8>& bytes = *result;
               const AccelCalibrationData block =
                   Common::BitCastPtr<AccelCalibrationData>(bytes.data());
               m_accel_calibration = block.GetCalibration();

               // A second copy at 0x26 could be tried when the checksum is wrong.
               WiimoteEmu::UpdateCalibrationDataChecksum(bytes, 1);
               if (block.checksum != bytes.back())
                 WARN_LOG_FMT(WIIMOTE, "WiiRemote: Bad accelerometer calibration checksum.");
             });
  return true;
}

bool Device::SetUpCamera()
{
  const u32 wanted_sensitivity = WantedCameraSensitivity();
  if (m_camera.enabled && m_camera.mode_set && m_camera.sensitivity == wanted_sensitivity)
    return false;

  if (!m_camera.enabled)
  {
    // The camera has two enable lines, both switched on in turn.
    OutputReportIRLogicEnable2 enable2{};
    enable2.ack = 1;
    enable2.enable = 1;
    Send(enable2, [this](ErrorCode result) {
      if (result != ErrorCode::Success)
      {
        WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to enable IR.");
        return;
      }

      OutputReportIRLogicEnable enable{};
      enable.ack = 1;
      enable.enable = 1;
      Send(enable, [this](ErrorCode second_result) {
        if (second_result != ErrorCode::Success)
        {
          WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to enable IR.");
          return;
        }
        DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: IR enabled.");
        m_camera.enabled = true;
      });
    });
  }
  else if (m_camera.sensitivity != wanted_sensitivity)
  {
    WriteCameraSensitivity(wanted_sensitivity);
  }
  else
  {
    // Basic mode, the only one whose data fits the reporting mode used.
    WriteByte(AddressSpace::I2CBus, WiimoteEmu::CameraLogic::I2C_ADDR, CAMERA_MODE,
              WiimoteEmu::CameraLogic::IR_MODE_BASIC, [this](ErrorCode result) {
                if (result != ErrorCode::Success)
                {
                  WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to set IR mode.");
                  return;
                }

                WriteByte(AddressSpace::I2CBus, WiimoteEmu::CameraLogic::I2C_ADDR, CAMERA_CONTROL,
                          CAMERA_TRACKING_ON, [this](ErrorCode control_result) {
                            if (control_result != ErrorCode::Success)
                            {
                              WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to enable object tracking.");
                              return;
                            }
                            DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: IR mode set.");
                            m_camera.mode_set = true;
                          });
              });
  }
  return true;
}

void Device::WriteCameraSensitivity(u32 level)
{
  DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Setting IR sensitivity: {}.", level + 1);

  const CameraSensitivity& setting = CAMERA_SENSITIVITIES[level];
  WriteMemory(AddressSpace::I2CBus, WiimoteEmu::CameraLogic::I2C_ADDR, CAMERA_SENSITIVITY_1,
              setting.first, [this, &setting, level](ErrorCode result) {
                if (result != ErrorCode::Success)
                {
                  WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to write IR block 1.");
                  return;
                }

                WriteMemory(AddressSpace::I2CBus, WiimoteEmu::CameraLogic::I2C_ADDR,
                            CAMERA_SENSITIVITY_2, setting.second,
                            [this, level](ErrorCode second_result) {
                              if (second_result != ErrorCode::Success)
                              {
                                WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to write IR block 2.");
                                return;
                              }
                              DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: IR sensitivity set.");
                              m_camera.sensitivity = level;
                            });
              });
}

bool Device::SilenceSpeaker()
{
  if (m_speaker_silenced)
    return false;

  OutputReportSpeakerMute mute{};
  mute.enable = 1;
  mute.ack = 1;
  Send(mute, [this](ErrorCode result) {
    if (result != ErrorCode::Success)
    {
      WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to mute speaker.");
      return;
    }

    OutputReportSpeakerEnable disable{};
    disable.enable = 0;
    disable.ack = 1;
    Send(disable, [this](ErrorCode second_result) {
      if (second_result != ErrorCode::Success)
      {
        WARN_LOG_FMT(WIIMOTE, "WiiRemote: Failed to disable speaker.");
        return;
      }
      DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Speaker muted and disabled.");
      m_speaker_silenced = true;
    });
  });
  return true;
}

bool Device::WaitingForMotionPlus() const
{
  return Clock::now() < m_motion_plus_hold_until;
}

void Device::HoldOffMotionPlus()
{
  DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Wait for M+.");
  m_motion_plus_hold_until = Clock::now() + MOTION_PLUS_SETTLE_TIME;
}

// The ID at 0xfa is that of the active device on the port: an active MotionPlus (which also
// tells its mode) or else the extension. It must be tried before initialising an unknown
// extension, as that initialisation also switches a MotionPlus off.
bool Device::IdentifyActiveExtension()
{
  if (m_extension_port != true)
    return false;
  if (MotionPlusKnown() && (MotionPlusActive() || m_extension.has_value()))
    return false;

  // Turn off encryption first (an active MotionPlus ignores this).
  WriteByte(AddressSpace::I2CBus, WiimoteEmu::ExtensionPort::REPORT_I2C_SLAVE, EXT_ENCRYPTION, 0,
            [this](ErrorCode result) {
              if (result != ErrorCode::Success)
                return;

              ReadMemory(AddressSpace::I2CBus, WiimoteEmu::ExtensionPort::REPORT_I2C_SLAVE,
                         EXT_IDENTIFIER, EXT_IDENTIFIER_SIZE, [this](ReadResult id) {
                           if (!id)
                             return;
                           const std::vector<u8>& bytes = *id;
                           if (bytes[5] == 0x05)
                           {
                             // A MotionPlus; byte 4 is its mode.
                             const auto mode = PassthroughMode(bytes[4]);
                             m_motion_plus.mode = mode;
                             INFO_LOG_FMT(WIIMOTE, "WiiRemote: M+ is active in mode: {}.",
                                          int(mode));
                           }
                           else
                           {
                             m_motion_plus.mode = PassthroughMode{};
                             OnExtensionIdentified(bytes[0], bytes[4], bytes[5]);
                           }
                         });
            });
  return true;
}

// An extension that did not identify itself is initialised (this switches an active MotionPlus
// off), then identified again. This comes before MotionPlus set-up, which needs to know the
// extension to pick the pass-through mode.
bool Device::ResetUnsupportedExtension()
{
  if (m_extension != Extension::Unsupported)
    return false;

  WriteByte(AddressSpace::I2CBus, WiimoteEmu::ExtensionPort::REPORT_I2C_SLAVE, EXT_INIT,
            EXT_INIT_VALUE, [this](ErrorCode result) {
              DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Initialized extension: {}.", int(result));
              m_extension = std::nullopt;
            });
  return true;
}

bool Device::PublishAttachments()
{
  // The rest needs the MotionPlus state.
  if (!MotionPlusKnown())
    return true;

  // Updated only here; following the state more closely makes the input flicker on mode
  // changes.
  m_attached_motion_plus = MotionPlusActive();
  // A removal is known here; an attachment only once the extension is identified.
  if (m_extension_port != true)
    m_attached_extension = WiimoteEmu::ExtensionNumber::NONE;
  return false;
}

// An inactive MotionPlus (at its own address) is polled now and then; when it answers it is
// initialised and switched to the wanted mode.
bool Device::WakeInactiveMotionPlus()
{
  if (MotionPlusActive() || !m_motion_plus_wanted.has_value() ||
      m_motion_plus.mode == m_motion_plus_wanted)
  {
    return false;
  }

  ReadMemory(AddressSpace::I2CBus, WiimoteEmu::MotionPlus::INACTIVE_DEVICE_ADDR,
             WiimoteEmu::MotionPlus::PASSTHROUGH_MODE_OFFSET, 1, [this](ReadResult poll) {
               if (!poll)
               {
                 DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: M+ poll failed.");
                 OnMotionPlusSilent();
                 return;
               }

               WriteByte(AddressSpace::I2CBus, WiimoteEmu::MotionPlus::INACTIVE_DEVICE_ADDR,
                         EXT_INIT, EXT_INIT_VALUE, [this](ErrorCode result) {
                           DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: M+ initialization: {}.", int(result));
                           if (result == ErrorCode::Success)
                             ChangeMotionPlusMode();
                           else
                             OnMotionPlusSilent();
                         });
             });
  return true;
}

void Device::OnMotionPlusSilent()
{
  // With an extension attached there can be no MotionPlus behind it: stop looking for one.
  if (m_extension_port == true)
    m_motion_plus_wanted = PassthroughMode{};
  else
    HoldOffMotionPlus();
}

void Device::ChangeMotionPlusMode()
{
  if (!m_motion_plus_wanted.has_value())
    return;

  const u8 mode = u8(*m_motion_plus_wanted);
  const u8 slave = MotionPlusActive() ? WiimoteEmu::MotionPlus::ACTIVE_DEVICE_ADDR :
                                        WiimoteEmu::MotionPlus::INACTIVE_DEVICE_ADDR;
  WriteByte(AddressSpace::I2CBus, slave, WiimoteEmu::MotionPlus::PASSTHROUGH_MODE_OFFSET, mode,
            [this](ErrorCode result) {
              DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: M+ activation: {}.", int(result));
              HoldOffMotionPlus();
              // The MotionPlus normally resets now (extension port events), but not always:
              // its mode is read back later either way.
              m_motion_plus.mode = std::nullopt;
            });
}

bool Device::SwitchActiveMotionPlusMode()
{
  if (!MotionPlusActive() || !m_motion_plus_wanted.has_value() ||
      m_motion_plus.mode == m_motion_plus_wanted)
  {
    return false;
  }

  ChangeMotionPlusMode();
  return true;
}

// The ID of the extension behind an active MotionPlus, which decides the wanted mode.
bool Device::IdentifyPassthroughExtension()
{
  if (!MotionPlusActive() || m_motion_plus.passthrough_connected != true || m_extension.has_value())
  {
    return false;
  }

  ReadMemory(AddressSpace::I2CBus, WiimoteEmu::MotionPlus::ACTIVE_DEVICE_ADDR,
             MPLUS_PASSTHROUGH_IDENTIFIER, 4, [this](ReadResult id) {
               // The extension may have gone while the read was under way.
               if (!id || m_motion_plus.passthrough_connected != true)
                 return;
               const std::vector<u8>& bytes = *id;
               OnExtensionIdentified(bytes[2], bytes[0], bytes[3]);
             });
  return true;
}

bool Device::SettleMotionPlusMode()
{
  // The rest needs the MotionPlus in its wanted mode.
  if (!MotionPlusInDesiredMode())
    return true;

  // As with the MotionPlus input, published only once things have settled.
  switch (m_extension.value_or(Extension::Unsupported))
  {
  case Extension::Nunchuk:
    m_attached_extension = WiimoteEmu::ExtensionNumber::NUNCHUK;
    break;
  case Extension::Classic:
    m_attached_extension = WiimoteEmu::ExtensionNumber::CLASSIC;
    break;
  case Extension::Unsupported:
    m_attached_extension = WiimoteEmu::ExtensionNumber::NONE;
    break;
  }
  return false;
}

bool Device::ReadMotionPlusCalibration()
{
  if (!MotionPlusActive() || m_motion_plus.calibration.has_value())
    return false;

  using MotionPlusCalibration = WiimoteEmu::MotionPlus::CalibrationData;
  ReadMemory(AddressSpace::I2CBus, WiimoteEmu::MotionPlus::ACTIVE_DEVICE_ADDR, EXT_CALIBRATION,
             sizeof(MotionPlusCalibration), [this](ReadResult result) {
               if (!result)
                 return;
               DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Read M+ calibration.");

               MotionPlusCalibration block =
                   Common::BitCastPtr<MotionPlusCalibration>(result->data());
               const auto stored_lsb = block.crc32_lsb;
               const auto stored_msb = block.crc32_msb;
               block.UpdateChecksum();

               DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Set M+ calibration.");
               WiimoteEmu::MotionPlus::CalibrationBlocks& blocks =
                   m_motion_plus.calibration.emplace();
               blocks.fast = block.fast;
               blocks.slow = block.slow;

               // Another read could be tried, or the MotionPlus given up on.
               if (stored_lsb != block.crc32_lsb || stored_msb != block.crc32_msb)
                 WARN_LOG_FMT(WIIMOTE, "WiiRemote: Bad M+ calibration checksum.");
             });
  return true;
}

void Device::ApplyNunchukCalibration(const WiimoteEmu::Nunchuk::CalibrationData& data, bool trusted)
{
  DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Set Nunchuk calibration.");

  // Defaults: centred, full range, and about 1 g for the accelerometer per WiiBrew.
  NunchukInfo::Calibration& calibration = m_nunchuk.calibration.emplace();
  auto& stick = calibration.stick;
  auto& accel = calibration.accel;
  stick.zero.data.fill(1 << (stick.BITS_OF_PRECISION - 1));
  stick.max.data.fill((1 << stick.BITS_OF_PRECISION) - 1);
  accel.zero.data.fill(1 << (accel.BITS_OF_PRECISION - 1));
  accel.max.data.fill(740);
  if (!trusted)
    return;

  // Some genuine Nunchuks report a zero stick range; the defaults stay then.
  if (const auto stored = data.GetStick(); stored.IsSane())
    stick = stored;
  else
    WARN_LOG_FMT(WIIMOTE,
                 "WiiRemote: Nunchuk stick calibration is not sane. Using fallback values.");

  if (const auto stored = data.GetAccel(); stored.IsSane())
    accel = stored;
  else
    WARN_LOG_FMT(WIIMOTE,
                 "WiiRemote: Nunchuk accel calibration is not sane. Using fallback values.");
}

void Device::ApplyClassicCalibration(const WiimoteEmu::Classic::CalibrationData& data, bool trusted)
{
  DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Set Classic Controller calibration.");

  // Defaults: centred sticks over the full range, triggers from 0 to full.
  ClassicInfo::Calibration& calibration = m_classic.calibration.emplace();
  for (auto* stick : {&calibration.left_stick, &calibration.right_stick})
  {
    stick->zero.data.fill(1 << (stick->BITS_OF_PRECISION - 1));
    stick->max.data.fill((1 << stick->BITS_OF_PRECISION) - 1);
  }
  for (auto* trigger : {&calibration.left_trigger, &calibration.right_trigger})
    trigger->max = (1 << trigger->BITS_OF_PRECISION) - 1;
  if (!trusted)
    return;

  if (const auto stored = data.GetLeftStick(); stored.IsSane())
    calibration.left_stick = stored;
  else
    WARN_LOG_FMT(WIIMOTE,
                 "WiiRemote: CC left stick calibration is not sane. Using fallback values.");

  if (const auto stored = data.GetRightStick(); stored.IsSane())
    calibration.right_stick = stored;
  else
    WARN_LOG_FMT(WIIMOTE,
                 "WiiRemote: CC right stick calibration is not sane. Using fallback values.");

  calibration.left_trigger = data.GetLeftTrigger();
  calibration.right_trigger = data.GetRightTrigger();
}

bool Device::ReadExtensionCalibration()
{
  const bool wanted = (m_extension == Extension::Nunchuk && !m_nunchuk.calibration) ||
                      (m_extension == Extension::Classic && !m_classic.calibration);
  if (!wanted)
    return false;

  const u16 address = MotionPlusActive() ? MPLUS_PASSTHROUGH_CALIBRATION : EXT_CALIBRATION;
  ReadMemory(
      AddressSpace::I2CBus, WiimoteEmu::ExtensionPort::REPORT_I2C_SLAVE, address,
      EXT_CALIBRATION_SIZE, [this](ReadResult result) {
        if (!result)
          return;
        DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Read extension calibration.");

        // The block ends with a two byte checksum. With a bad one the defaults are used
        // (another copy of the block could be tried instead).
        std::vector<u8>& bytes = *result;
        const std::array<u8, 2> stored{bytes[EXT_CALIBRATION_SIZE - 2],
                                       bytes[EXT_CALIBRATION_SIZE - 1]};
        WiimoteEmu::UpdateCalibrationDataChecksum(bytes, 2);
        const bool trusted = stored[0] == bytes[EXT_CALIBRATION_SIZE - 2] &&
                             stored[1] == bytes[EXT_CALIBRATION_SIZE - 1];
        if (!trusted)
          WARN_LOG_FMT(WIIMOTE, "WiiRemote: Bad extension calibration checksum.");

        if (m_extension == Extension::Nunchuk)
        {
          ApplyNunchukCalibration(
              Common::BitCastPtr<WiimoteEmu::Nunchuk::CalibrationData>(bytes.data()), trusted);
        }
        else if (m_extension == Extension::Classic)
        {
          ApplyClassicCalibration(
              Common::BitCastPtr<WiimoteEmu::Classic::CalibrationData>(bytes.data()), trusted);
        }
      });
  return true;
}

void Device::OnExtensionIdentified(u8 id_0, u8 id_4, u8 id_5)
{
  if (id_4 == 0x00 && id_5 == 0x00)
  {
    INFO_LOG_FMT(WIIMOTE, "WiiRemote: Nunchuk is attached.");
    m_extension = Extension::Nunchuk;
    m_motion_plus_wanted = PassthroughMode::Nunchuk;
  }
  else if (id_4 == 0x01 && id_5 == 0x01)
  {
    INFO_LOG_FMT(WIIMOTE, "WiiRemote: Classic Controller is attached.");
    m_extension = Extension::Classic;
    m_motion_plus_wanted = PassthroughMode::Classic;
  }
  else
  {
    // Expected for an extension that is not initialised yet.
    DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Unknown extension: {} {} {}.", id_0, id_4, id_5);
    m_extension = Extension::Unsupported;
  }
}

void Device::OnExtensionPortChange(bool connected)
{
  m_nunchuk = {};
  m_classic = {};
  m_extension = std::nullopt;

  // The wanted MotionPlus mode depends on the new extension, not known yet.
  if (connected)
    m_motion_plus_wanted = std::nullopt;
}

void Device::OnStatus(const InputReportStatus& status)
{
  m_status_due = Clock::now() + STATUS_LIFETIME;
  m_battery = static_cast<float>(status.GetEstimatedCharge() * BATTERY_INPUT_MAX_VALUE);
  m_leds = status.leds;

  if (!status.ir)
    m_camera = {};

  const bool connected = status.extension;
  if (m_extension_port == connected)
    return;

  DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Extension port event: {}.", connected);
  m_extension_port = connected;
  // The remote stops data reporting on a port event.
  m_reporting_mode = InputReportID::ReportDisabled;
  OnExtensionPortChange(connected);
  // Whatever the MotionPlus was doing is unknown now.
  m_motion_plus = {};

  if (connected)
  {
    // An attachment means any MotionPlus reset is over.
    m_motion_plus_hold_until = Clock::now();
  }
  else
  {
    // Nothing on the port: a MotionPlus is off or resetting. Nunchuk mode is wanted, the most
    // common one, which also works with nothing behind the MotionPlus.
    m_motion_plus_wanted = PassthroughMode::Nunchuk;
    m_motion_plus.mode = PassthroughMode{};
  }
}

bool Device::MotionPlusKnown() const
{
  return m_motion_plus.mode.has_value();
}

// Note that an unknown state counts as active.
bool Device::MotionPlusActive() const
{
  return m_motion_plus.mode != PassthroughMode{};
}

bool Device::MotionPlusInDesiredMode() const
{
  return m_motion_plus.mode.has_value() && m_motion_plus.mode == m_motion_plus_wanted;
}

// ----- Incoming data -----

void Device::HandleReport(WiimoteReal::Report& report)
{
  constexpr std::size_t id_offset = WiimoteReal::REPORT_HID_HEADER_SIZE;
  if (report.size() <= id_offset)
  {
    WARN_LOG_FMT(WIIMOTE, "WiiRemote: Bad report size.");
    return;
  }

  auto id = InputReportID(report[id_offset]);

  // Offer the report to the awaited replies in order, dropping those that timed out on the way.
  bool consumed = false;
  for (auto it = m_pending_replies.begin(); it != m_pending_replies.end();)
  {
    if (Clock::now() >= it->give_up_at)
    {
      WARN_LOG_FMT(WIIMOTE, "WiiRemote: Removing expired handler.");
      it = m_pending_replies.erase(it);
      continue;
    }

    for (ReplyMatcher& matcher : it->matchers)
    {
      consumed = matcher(report);
      if (consumed)
        break;
    }
    if (consumed)
    {
      m_pending_replies.erase(it);
      break;
    }
    ++it;
  }

  if (!consumed)
  {
    if (id == InputReportID::Status)
    {
      // Unrequested status reports come on extension port events.
      if (report.size() - id_offset < sizeof(TypedInputData<InputReportStatus>))
        WARN_LOG_FMT(WIIMOTE, "WiiRemote: Bad report size.");
      else
        OnStatus(Common::BitCastPtr<InputReportStatus>(report.data() + id_offset + 1));
    }
    else if (id < InputReportID::ReportCore)
    {
      WARN_LOG_FMT(WIIMOTE, "WiiRemote: Unhandled input report: {}.",
                   ArrayToString(report.data(), u32(report.size())));
    }
  }

  // Non-data reports start with the core buttons too. A data report shows the reporting mode
  // in effect (interleaved modes are not handled properly).
  if (id < InputReportID::ReportCore)
    id = InputReportID::ReportCore;
  else
    m_reporting_mode = id;

  const auto layout = MakeDataReportManipulator(id, report.data() + id_offset + 1);
  if (layout->GetDataSize() + id_offset > report.size())
  {
    WARN_LOG_FMT(WIIMOTE, "WiiRemote: Bad report size.");
    return;
  }

  layout->GetCoreData(&m_buttons);

  if (layout->HasAccel() && m_accel_calibration.has_value())
  {
    AccelData raw{};
    layout->GetAccelData(&raw);
    m_accel = raw.GetNormalizedValue(*m_accel_calibration) * float(MathUtil::GRAVITY_ACCELERATION);
  }

  if (layout->HasIR() && m_camera.enabled && m_camera.mode_set &&
      m_camera.sensitivity == WantedCameraSensitivity())
  {
    ReadCamera(*layout);
  }

  if (MotionPlusKnown())
  {
    if (MotionPlusActive())
      ReadMotionPlusData(layout->GetExtDataPtr(), layout->GetExtDataSize());
    else
      ReadExtensionData(layout->GetExtDataPtr(), layout->GetExtDataSize());
  }

  UpdateOrientation();
}

void Device::ReadCamera(const DataReportManipulator& layout)
{
  using WiimoteEmu::CameraLogic;
  using WiimoteEmu::IRObject;

  // Visible objects are simply averaged, which works well enough (objects leaving the view
  // could be extrapolated instead).
  MathUtil::RunningVariance<Common::Vec2> visible;
  const IRObject far_corner(CameraLogic::CAMERA_RES_X - 1, CameraLogic::CAMERA_RES_Y - 1);

  const auto take = [&](std::size_t slot, IRObject position, u8 size) {
    // Unused slots are all ones, which puts them below the picture.
    if (position.y > far_corner.y)
    {
      m_camera.object_position[slot] = {0.0f, 0.0f};
      m_camera.object_size[slot] = 0.0f;
      return;
    }
    m_camera.object_position[slot].x = static_cast<float>(position.x) / far_corner.x;
    m_camera.object_position[slot].y = static_cast<float>(position.y) / far_corner.y;
    m_camera.object_size[slot] = static_cast<float>(size) / 15.0f;
    visible.Push(Common::Vec2(position));
  };

  const IRReportFormat format = layout.GetIRReportFormat();
  if (format == IRReportFormat::Basic)
  {
    // Basic data has no sizes; a typical one is assumed.
    constexpr u8 assumed_size = 2;
    const std::array<WiimoteEmu::IRBasic, 2> pairs =
        Common::BitCastPtr<std::array<WiimoteEmu::IRBasic, 2>>(layout.GetIRDataPtr());
    for (std::size_t pair = 0; pair != pairs.size(); ++pair)
    {
      take(pair * 2, pairs[pair].GetObject1(), assumed_size);
      take(pair * 2 + 1, pairs[pair].GetObject2(), assumed_size);
    }
  }
  else if (format == IRReportFormat::Extended)
  {
    const std::array<WiimoteEmu::IRExtended, 4> objects =
        Common::BitCastPtr<std::array<WiimoteEmu::IRExtended, 4>>(layout.GetIRDataPtr());
    for (std::size_t slot = 0; slot != objects.size(); ++slot)
      take(slot, objects[slot].GetPosition(), objects[slot].size);
  }
  else
  {
    return;
  }

  const std::size_t count = visible.Count();
  m_camera.hidden = count == 0;

  if (count >= 2)
  {
    // Spread of the objects as a fraction of the view, the vertical one scaled to the
    // horizontal field of view; the distance follows from the sensor bar's LED spacing.
    const Common::Vec2 variance = visible.PopulationVariance();
    const Common::Vec2 view(CameraLogic::CAMERA_RES_X,
                            CameraLogic::CAMERA_RES_Y * CameraLogic::CAMERA_AR);
    const Common::Vec2 spread =
        Common::Vec2(std::sqrt(variance.x), std::sqrt(variance.y)) / view * 2;
    m_camera.distance = CameraLogic::SENSOR_BAR_LED_SEPARATION / spread.Length() / 2 /
                        std::tan(CameraLogic::CAMERA_FOV_X / 2);
  }

  if (count == 0)
    m_camera.center = {};
  else
    m_camera.center = visible.Mean() / Common::Vec2(far_corner) * 2.f - Common::Vec2(1, 1);
}

void Device::ReadMotionPlusData(const u8* bytes, u32 size)
{
  using Format = WiimoteEmu::MotionPlus::DataFormat;
  if (size < sizeof(Format))
    return;

  const Format data = Common::BitCastPtr<Format>(bytes);

  const bool connected = data.extension_connected;
  if (m_motion_plus.passthrough_connected != connected)
  {
    m_motion_plus.passthrough_connected = connected;
    DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: M+ passthrough port event: {}.", connected);
    // Nothing behind the MotionPlus: whatever mode it has will do.
    if (!connected)
      m_motion_plus_wanted = m_motion_plus.mode;
    OnExtensionPortChange(connected);
  }

  if (data.is_mp_data)
  {
    // The calibration tells the sensor orientations; nothing can be done without it.
    if (m_motion_plus.calibration.has_value())
    {
      m_motion_plus.angular_velocity =
          data.GetData().GetAngularVelocity(*m_motion_plus.calibration);
    }
    return;
  }

  if (!MotionPlusInDesiredMode())
  {
    DEBUG_LOG_FMT(WIIMOTE, "WiiRemote: Ignoring unwanted passthrough data.");
    return;
  }

  // Pass-through data has some bits moved around; put them back.
  std::array<u8, sizeof(WiimoteEmu::Nunchuk::DataFormat)> extension{};
  std::copy_n(bytes, std::min<std::size_t>(size, extension.size()), extension.begin());
  WiimoteEmu::MotionPlus::ReversePassthroughModifications(*m_motion_plus.mode, extension.data());
  ReadExtensionData(extension.data(), u32(extension.size()));
}

void Device::ReadExtensionData(const u8* bytes, u32 size)
{
  using WiimoteEmu::Classic;
  using WiimoteEmu::Nunchuk;

  if (m_extension == Extension::Nunchuk)
  {
    if (size < sizeof(WiimoteEmu::MotionPlus::DataFormat))
      return;
    const Nunchuk::DataFormat data = Common::BitCastPtr<Nunchuk::DataFormat>(bytes);
    m_nunchuk.buttons = data.GetButtons();
    if (m_nunchuk.calibration.has_value())
    {
      const NunchukInfo::Calibration& calibration = *m_nunchuk.calibration;
      m_nunchuk.stick = data.GetStick().GetNormalizedValue(calibration.stick);
      m_nunchuk.accel = data.GetAccel().GetNormalizedValue(calibration.accel) *
                        float(MathUtil::GRAVITY_ACCELERATION);
    }
  }
  else if (m_extension == Extension::Classic)
  {
    if (size < sizeof(Classic::DataFormat))
      return;
    const Classic::DataFormat data = Common::BitCastPtr<Classic::DataFormat>(bytes);
    m_classic.buttons = data.GetButtons();
    if (m_classic.calibration.has_value())
    {
      const ClassicInfo::Calibration& calibration = *m_classic.calibration;
      m_classic.sticks[0] = data.GetLeftStick().GetNormalizedValue(calibration.left_stick);
      m_classic.sticks[1] = data.GetRightStick().GetNormalizedValue(calibration.right_stick);
      m_classic.triggers[0] = data.GetLeftTrigger().GetNormalizedValue(calibration.left_trigger);
      m_classic.triggers[1] = data.GetRightTrigger().GetNormalizedValue(calibration.right_trigger);
    }
  }
}

void Device::UpdateOrientation()
{
  const Clock::time_point now = Clock::now();
  const float seconds =
      std::chrono::duration_cast<std::chrono::duration<float>>(now - m_last_report_at).count();
  m_last_report_at = now;

  // Turn by the gyroscope's rotation since the last report.
  const Common::Vec3 turn = m_motion_plus.angular_velocity * -1 * seconds;
  m_orientation = WiimoteEmu::GetRotationFromGyroscope(turn) * m_orientation;

  // Accelerometer and IR pull the result towards what they see, much more strongly when
  // there is no gyroscope to rely on.
  const bool have_gyro = MotionPlusKnown() && MotionPlusActive();

  // Gravity corrects pitch and roll.
  if (m_accel.LengthSquared() != 0)
  {
    const auto weight = static_cast<float>(have_gyro ? 0.04 : 0.5);
    m_orientation = WiimoteEmu::ComplementaryFilter(m_orientation, m_accel, weight);
  }

  // The sensor bar corrects yaw and pitch. Pitch is a little off (the bar is not at eye level),
  // so this weighs less than the accelerometer; it only acts near level, with the bar in view.
  // (Several objects could correct roll too.)
  if (!m_camera.hidden)
  {
    using WiimoteEmu::CameraLogic;
    const Common::Vec3 forward(0, 1, 0);
    const Common::Vec3 bar_angle = Common::Vec3(m_camera.center.y * CameraLogic::CAMERA_FOV_Y, 0,
                                                m_camera.center.x * CameraLogic::CAMERA_FOV_X) /
                                   2;
    const Common::Vec3 bar_direction = WiimoteEmu::GetRotationFromGyroscope(-bar_angle) * forward;
    const auto weight = static_cast<float>(have_gyro ? 0.035 : 0.45f);
    m_orientation = WiimoteEmu::ComplementaryFilter(m_orientation, bar_direction, weight, forward);
  }

  m_orientation = m_orientation.Normalized();

  const Common::Vec3 angles{WiimoteEmu::GetPitch(m_orientation), WiimoteEmu::GetRoll(m_orientation),
                            WiimoteEmu::GetYaw(m_orientation)};
  m_rotation = angles / float(MathUtil::PI);
}

void Device::UpdateRumble()
{
  // Strength s keeps the motor on for s of each period.
  const auto on_for =
      std::chrono::duration_cast<Clock::duration>(RUMBLE_PERIOD * m_rumble_strength.load());
  const auto off_for = RUMBLE_PERIOD - on_for;
  const Clock::time_point now = Clock::now();

  // The motor stays as it is for its current state's share of the period, and for good when
  // the other state's share is zero.
  if (m_rumble)
  {
    if (off_for.count() == 0 || now < m_rumble_toggled_at + on_for)
      return;
  }
  else if (on_for.count() == 0 || now < m_rumble_toggled_at + off_for)
  {
    return;
  }

  m_rumble_toggled_at = now;
  m_rumble = !m_rumble;
  // Send() puts the new motor state in the report.
  Send(OutputReportRumble{});
}

}  // namespace ciface::WiimoteController
