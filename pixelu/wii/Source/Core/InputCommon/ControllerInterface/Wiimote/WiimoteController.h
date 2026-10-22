// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

// Real Wii Remotes used directly as input devices (source "Bluetooth"), outside of the emulated
// Wii Remote slots. The device drives the remote itself: it sets up reporting, the IR camera,
// the MotionPlus and extensions, and turns the reports into inputs.
namespace ciface::WiimoteController
{
// Connects a remote taken from the real Wii Remote pool and adds it as a device.
void AddDevice(std::unique_ptr<WiimoteReal::Wiimote>);
// Removes up to `count` of these devices (all with nullopt) right away; their remotes go back
// to the pool.
void ReleaseDevices(std::optional<u32> count = std::nullopt);

class Device final : public Core::Device
{
public:
  explicit Device(std::unique_ptr<WiimoteReal::Wiimote> wiimote);
  ~Device() override;

  std::string GetName() const override;
  std::string GetSource() const override;
  int GetSortPriority() const override;

  Core::DeviceRemoval UpdateInput() override;

private:
  using Clock = std::chrono::steady_clock;
  using PassthroughMode = WiimoteEmu::MotionPlus::PassthroughMode;

  // Memory read result: the bytes, or nullopt when the remote refused.
  using ReadResult = std::optional<std::vector<u8>>;
  using ReadCallback = std::function<void(ReadResult)>;
  using AckCallback = std::function<void(WiimoteCommon::ErrorCode)>;
  // Looks at an incoming report; returns true if it was the awaited reply and is now used up.
  using ReplyMatcher = std::function<bool(const WiimoteReal::Report&)>;

  // What sits on the extension port (or on the MotionPlus pass-through port).
  enum class Extension
  {
    Nunchuk,
    Classic,
    Unsupported,
  };

  // One reply the remote owes us. The first matcher that accepts a report consumes the entry.
  struct PendingReply
  {
    Clock::time_point give_up_at;
    std::vector<ReplyMatcher> matchers;
  };

  struct MotionPlusInfo
  {
    // Unknown until read back; PassthroughMode{} means the MotionPlus is not active.
    std::optional<PassthroughMode> mode;
    // Last seen "extension connected" flag of MotionPlus data, for detecting changes.
    std::optional<bool> passthrough_connected;
    std::optional<WiimoteEmu::MotionPlus::CalibrationBlocks> calibration;
    Common::Vec3 angular_velocity{};
  };

  struct NunchukInfo
  {
    struct Calibration
    {
      WiimoteEmu::Nunchuk::CalibrationData::AccelCalibration accel;
      WiimoteEmu::Nunchuk::CalibrationData::StickCalibration stick;
    };

    std::optional<Calibration> calibration;
    Common::Vec2 stick{};
    Common::Vec3 accel{};
    u8 buttons = 0;
  };

  struct ClassicInfo
  {
    struct Calibration
    {
      WiimoteEmu::Classic::CalibrationData::StickCalibration left_stick;
      WiimoteEmu::Classic::CalibrationData::StickCalibration right_stick;
      WiimoteEmu::Classic::CalibrationData::TriggerCalibration left_trigger;
      WiimoteEmu::Classic::CalibrationData::TriggerCalibration right_trigger;
    };

    std::optional<Calibration> calibration;
    std::array<Common::Vec2, 2> sticks{};
    std::array<float, 2> triggers{};
    u16 buttons = 0;
  };

  struct CameraInfo
  {
    // Applied sensitivity level (0-4); u32(-1) until one is written.
    u32 sensitivity = u32(-1);
    bool enabled = false;
    bool mode_set = false;

    // Mean of the visible objects, -1 to 1 on each axis.
    Common::Vec2 center{};
    float distance = 0;
    bool hidden = true;

    std::array<Common::Vec2, 4> object_position{};
    std::array<float, 4> object_size{};
  };

  // Report traffic.
  template <typename R>
  void Send(R report, AckCallback on_ack = {});
  void Await(std::vector<ReplyMatcher> matchers);
  void ReadMemory(WiimoteCommon::AddressSpace, u8 slave, u16 address, u16 size, ReadCallback);
  void AwaitReadReply(u16 address, u16 size, std::vector<u8> collected, ReadCallback);
  void WriteMemory(WiimoteCommon::AddressSpace, u8 slave, u16 address, std::span<const u8> bytes,
                   AckCallback on_done);
  void WriteByte(WiimoteCommon::AddressSpace, u8 slave, u16 address, u8 value, AckCallback on_done);

  // The set-up steps RunTasks() goes through; each returns true when RunTasks must stop there.
  void RunTasks();
  bool RequestStatusIfStale();
  bool SetPlayerLeds();
  bool SetReportingMode();
  bool ReadAccelerometerCalibration();
  bool SetUpCamera();
  bool SilenceSpeaker();
  bool WaitingForMotionPlus() const;
  bool IdentifyActiveExtension();
  bool ResetUnsupportedExtension();
  bool PublishAttachments();
  bool WakeInactiveMotionPlus();
  bool SwitchActiveMotionPlusMode();
  bool IdentifyPassthroughExtension();
  bool SettleMotionPlusMode();
  bool ReadMotionPlusCalibration();
  bool ReadExtensionCalibration();
  void ApplyNunchukCalibration(const WiimoteEmu::Nunchuk::CalibrationData&, bool trusted);
  void ApplyClassicCalibration(const WiimoteEmu::Classic::CalibrationData&, bool trusted);

  void WriteCameraSensitivity(u32 level);
  void ChangeMotionPlusMode();
  void OnMotionPlusSilent();
  void HoldOffMotionPlus();
  void OnExtensionIdentified(u8 id_0, u8 id_4, u8 id_5);
  void OnExtensionPortChange(bool connected);
  void OnStatus(const WiimoteCommon::InputReportStatus&);
  u8 PlayerLeds() const;

  bool MotionPlusKnown() const;
  bool MotionPlusActive() const;
  bool MotionPlusInDesiredMode() const;

  // Incoming data.
  void HandleReport(WiimoteReal::Report& report);
  void ReadCamera(const WiimoteCommon::DataReportManipulator&);
  void ReadMotionPlusData(const u8* bytes, u32 size);
  void ReadExtensionData(const u8* bytes, u32 size);
  void UpdateOrientation();
  void UpdateRumble();

  std::unique_ptr<WiimoteReal::Wiimote> m_wiimote;

  WiimoteCommon::DataReportManipulator::CoreData m_buttons{};
  Common::Vec3 m_accel{};
  std::optional<WiimoteCommon::AccelCalibrationData::Calibration> m_accel_calibration;
  // Pitch, roll and yaw, as fractions of half a turn.
  Common::Vec3 m_rotation{};

  MotionPlusInfo m_motion_plus{};
  NunchukInfo m_nunchuk{};
  ClassicInfo m_classic{};
  CameraInfo m_camera{};

  // MotionPlus work waits until this time (the MotionPlus resets after a mode change).
  Clock::time_point m_motion_plus_hold_until = Clock::now();
  // Mode wanted for the MotionPlus, from the extension behind it.
  std::optional<PassthroughMode> m_motion_plus_wanted;

  // A status report is asked for when this time passes (keeps the battery level fresh).
  Clock::time_point m_status_due = Clock::now();
  float m_battery = 0;
  u8 m_leds = 0;
  bool m_speaker_silenced = false;

  // Last seen extension port flag.
  std::optional<bool> m_extension_port;
  // The extension in use (the pass-through one while the MotionPlus is active).
  std::optional<Extension> m_extension;

  // Motor state; every output report carries it.
  bool m_rumble = false;
  // Requested strength, done by switching the motor on and off.
  std::atomic<ControlState> m_rumble_strength{};
  Clock::time_point m_rumble_toggled_at = Clock::now();

  // Starts disabled so that a mode is always set.
  WiimoteCommon::InputReportID m_reporting_mode = WiimoteCommon::InputReportID::ReportDisabled;

  // Values for the "Attached Extension" and "Attached MotionPlus" inputs.
  WiimoteEmu::ExtensionNumber m_attached_extension = WiimoteEmu::ExtensionNumber::NONE;
  bool m_attached_motion_plus = false;

  std::list<PendingReply> m_pending_replies;

  // Orientation of the world relative to the remote.
  Common::Quaternion m_orientation = Common::Quaternion::Identity();
  Clock::time_point m_last_report_at = Clock::now();
};

}  // namespace ciface::WiimoteController
