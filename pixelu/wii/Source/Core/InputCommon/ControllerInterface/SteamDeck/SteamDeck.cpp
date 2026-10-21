// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/SteamDeck/SteamDeck.h"

#include <string>
#include <string_view>

#include <hidapi.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ciface::SteamDeck
{
namespace
{
constexpr std::string_view SOURCE_NAME = "SteamDeck";

// Valve's USB ids for the Deck's controller, and the HID interface carrying the input reports.
constexpr unsigned short VALVE_VENDOR_ID = 0x28de;
constexpr unsigned short DECK_PRODUCT_ID = 0x1205;
constexpr int CONTROLS_INTERFACE = 2;

// The Deck's 64-byte input report (report type 9), as documented by Valve's SDL driver.
struct InputReport
{
  u8 version_major;
  u8 version_minor;
  u8 type;
  u8 length;
  u32 sequence;

  u32 buttons_low;
  u32 buttons_high;

  s16 left_pad_x;
  s16 left_pad_y;
  s16 right_pad_x;
  s16 right_pad_y;

  s16 accel_x;
  s16 accel_y;
  s16 accel_z;
  s16 gyro_pitch;
  s16 gyro_roll;
  s16 gyro_yaw;
  s16 orientation_w;
  s16 orientation_x;
  s16 orientation_y;
  s16 orientation_z;

  u16 left_trigger;
  u16 right_trigger;

  s16 left_stick_x;
  s16 left_stick_y;
  s16 right_stick_x;
  s16 right_stick_y;

  u16 left_pad_pressure;
  u16 right_pad_pressure;
  s16 left_stick_touch;
  s16 right_stick_touch;
};
static_assert(sizeof(InputReport) == 64);

constexpr u8 INPUT_REPORT_TYPE = 0x09;

// Steam turns raw gyro reporting off whenever its overlay closes (unless gyro is mapped in Steam
// Input), so it is switched back on every this many updates.
constexpr int IMU_REFRESH_INTERVAL = 250;

// Feature report: settings write (0x87), 3 bytes, IMU mode (0x30) = raw accel and raw gyro
// (0x0018). The first byte is the report id.
constexpr unsigned char IMU_ENABLE_REPORT[65] = {0x00, 0x87, 0x03, 0x30, 0x18, 0x00};

class BitInput final : public Core::Device::Input
{
public:
  BitInput(const char* name, const u32& bits, u32 mask) : m_name(name), m_bits(bits), m_mask(mask)
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return (m_bits & m_mask) != 0; }

private:
  const char* const m_name;
  const u32& m_bits;
  const u32 m_mask;
};

// A report field divided by `scale`.
template <typename Field, bool Detectable = true>
class FieldInput final : public Core::Device::Input
{
public:
  FieldInput(const char* name, const Field& field, ControlState scale)
      : m_name(name), m_field(field), m_scale(scale)
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return ControlState(m_field) / m_scale; }
  bool IsDetectable() const override { return Detectable; }

private:
  const char* const m_name;
  const Field& m_field;
  const ControlState m_scale;
};

using MotionInput = FieldInput<s16, false>;

class Device final : public Core::Device
{
public:
  explicit Device(hid_device* handle);

  std::string GetName() const override { return "Steam Deck"; }
  std::string GetSource() const override { return std::string(SOURCE_NAME); }
  Core::DeviceRemoval UpdateInput() override;

private:
  void AddButtons();
  void AddAxes();
  void AddMotion();

  hid_device* const m_handle;
  InputReport m_report{};
  int m_updates_since_imu_refresh = 0;
};

Device::Device(hid_device* handle) : m_handle{handle}
{
  AddButtons();
  AddAxes();
  AddMotion();
}

void Device::AddButtons()
{
  struct ButtonBit
  {
    const char* name;
    bool high_word;
    u32 mask;
  };
  static constexpr ButtonBit buttons[] = {
      {"R2 Full Pull", false, 1u << 0},
      {"L2 Full Pull", false, 1u << 1},
      {"R1", false, 1u << 2},
      {"L1", false, 1u << 3},
      {"Y", false, 1u << 4},
      {"B", false, 1u << 5},
      {"X", false, 1u << 6},
      {"A", false, 1u << 7},
      {"D-Pad Up", false, 1u << 8},
      {"D-Pad Right", false, 1u << 9},
      {"D-Pad Left", false, 1u << 10},
      {"D-Pad Down", false, 1u << 11},
      {"View", false, 1u << 12},
      {"Steam", false, 1u << 13},
      {"Menu", false, 1u << 14},
      {"L5", false, 1u << 15},
      {"R5", false, 1u << 16},
      {"Left Trackpad Click", false, 1u << 17},
      {"Right Trackpad Click", false, 1u << 18},
      {"Left Trackpad Touch", false, 1u << 19},
      {"Right Trackpad Touch", false, 1u << 20},
      {"L3", false, 1u << 22},
      {"R3", false, 1u << 26},
      {"L4", true, 1u << 9},
      {"R4", true, 1u << 10},
      {"Left Stick Touch", true, 1u << 14},
      {"Right Stick Touch", true, 1u << 15},
      {"Quick Access", true, 1u << 18},
  };

  for (const ButtonBit& button : buttons)
  {
    const u32& word = button.high_word ? m_report.buttons_high : m_report.buttons_low;
    AddInput(new BitInput(button.name, word, button.mask));
  }
}

void Device::AddAxes()
{
  // Full range of the 16-bit fields.
  constexpr ControlState RANGE = 32767;

  AddInput(new FieldInput<u16>("L2", m_report.left_trigger, RANGE));
  AddInput(new FieldInput<u16>("R2", m_report.right_trigger, RANGE));

  // Signed axes, one input per direction.
  struct SignedAxis
  {
    const char* negative_name;
    const char* positive_name;
    s16 InputReport::*field;
  };
  static constexpr SignedAxis axes[] = {
      {"Left Stick X-", "Left Stick X+", &InputReport::left_stick_x},
      {"Left Stick Y-", "Left Stick Y+", &InputReport::left_stick_y},
      {"Right Stick X-", "Right Stick X+", &InputReport::right_stick_x},
      {"Right Stick Y-", "Right Stick Y+", &InputReport::right_stick_y},
      {"Left Trackpad X-", "Left Trackpad X+", &InputReport::left_pad_x},
      {"Left Trackpad Y-", "Left Trackpad Y+", &InputReport::left_pad_y},
      {"Right Trackpad X-", "Right Trackpad X+", &InputReport::right_pad_x},
      {"Right Trackpad Y-", "Right Trackpad Y+", &InputReport::right_pad_y},
  };
  for (const SignedAxis& axis : axes)
  {
    const s16& field = m_report.*axis.field;
    AddInput(new FieldInput<s16>(axis.negative_name, field, -RANGE));
    AddInput(new FieldInput<s16>(axis.positive_name, field, RANGE));
  }

  AddInput(new FieldInput<u16>("Left Trackpad Pressure", m_report.left_pad_pressure, RANGE));
  AddInput(new FieldInput<u16>("Right Trackpad Pressure", m_report.right_pad_pressure, RANGE));
  AddInput(new FieldInput<s16>("Left Stick Capacitance", m_report.left_stick_touch, RANGE));
  AddInput(new FieldInput<s16>("Right Stick Capacitance", m_report.right_stick_touch, RANGE));
}

void Device::AddMotion()
{
  // 16384 counts per g; inputs are in m/s².
  constexpr ControlState accel_scale = 16384.0 / MathUtil::GRAVITY_ACCELERATION;
  AddInput(new MotionInput("Accel Up", m_report.accel_z, -accel_scale));
  AddInput(new MotionInput("Accel Down", m_report.accel_z, accel_scale));
  AddInput(new MotionInput("Accel Left", m_report.accel_x, accel_scale));
  AddInput(new MotionInput("Accel Right", m_report.accel_x, -accel_scale));
  AddInput(new MotionInput("Accel Forward", m_report.accel_y, -accel_scale));
  AddInput(new MotionInput("Accel Backward", m_report.accel_y, accel_scale));

  // About 16.384 counts per degree per second; inputs are in radians per second.
  constexpr ControlState gyro_scale = 16.384 * 360.0 / MathUtil::TAU;
  AddInput(new MotionInput("Gyro Pitch Up", m_report.gyro_pitch, gyro_scale));
  AddInput(new MotionInput("Gyro Pitch Down", m_report.gyro_pitch, -gyro_scale));
  AddInput(new MotionInput("Gyro Roll Left", m_report.gyro_roll, -gyro_scale));
  AddInput(new MotionInput("Gyro Roll Right", m_report.gyro_roll, gyro_scale));
  AddInput(new MotionInput("Gyro Yaw Left", m_report.gyro_yaw, gyro_scale));
  AddInput(new MotionInput("Gyro Yaw Right", m_report.gyro_yaw, -gyro_scale));
}

Core::DeviceRemoval Device::UpdateInput()
{
  if (++m_updates_since_imu_refresh == IMU_REFRESH_INTERVAL)
  {
    m_updates_since_imu_refresh = 0;
    hid_send_feature_report(m_handle, IMU_ENABLE_REPORT, sizeof(IMU_ENABLE_REPORT));
  }

  // Only the newest of the queued reports matters.
  InputReport incoming{};
  bool received = false;
  while (hid_read(m_handle, reinterpret_cast<unsigned char*>(&incoming), sizeof(incoming)) > 0)
    received = true;

  if (!received)
    return Core::DeviceRemoval::Keep;

  const bool well_formed = incoming.version_major == 0x01 && incoming.version_minor == 0x00 &&
                           incoming.type == INPUT_REPORT_TYPE &&
                           incoming.length == sizeof(InputReport);
  if (!well_formed)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Steam Deck bad report");
    return Core::DeviceRemoval::Keep;
  }

  m_report = incoming;
  return Core::DeviceRemoval::Keep;
}

class InputBackend final : public ciface::InputBackend
{
public:
  using ciface::InputBackend::InputBackend;
  void PopulateDevices() override;
};

void InputBackend::PopulateDevices()
{
  INFO_LOG_FMT(CONTROLLERINTERFACE, "SteamDeck PopulateDevices");

  // Find the HID interface with the controls (the device has several).
  hid_device_info* const found = hid_enumerate(VALVE_VENDOR_ID, DECK_PRODUCT_ID);
  std::string controls_path;
  bool have_controls = false;
  for (const hid_device_info* entry = found; entry != nullptr; entry = entry->next)
  {
    INFO_LOG_FMT(CONTROLLERINTERFACE, "Found {} (interface {})", entry->path,
                 entry->interface_number);
    if (entry->interface_number == CONTROLS_INTERFACE)
    {
      controls_path = entry->path;
      have_controls = true;
      break;
    }
  }
  hid_free_enumeration(found);

  if (!have_controls)
  {
    INFO_LOG_FMT(CONTROLLERINTERFACE, "No Steam Deck interface found");
    return;
  }
  INFO_LOG_FMT(CONTROLLERINTERFACE, "Found Steam Deck controls at {}", controls_path);

  hid_device* const handle = hid_open_path(controls_path.c_str());
  if (handle == nullptr)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Steam Deck controls could not be opened");
    return;
  }
  if (hid_set_nonblocking(handle, 1) != 0)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Steam Deck controls could not be set nonblocking");
    return;
  }

  GetControllerInterface().RemoveDevice(
      [](const Core::Device* device) { return device->GetSource() == SOURCE_NAME; });
  GetControllerInterface().AddDevice(std::make_shared<Device>(handle));
}
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}

}  // namespace ciface::SteamDeck
