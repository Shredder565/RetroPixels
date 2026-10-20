// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>

#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

namespace WiimoteEmu
{
// Everything one update of an emulated remote reports, gathered from the user's input before
// it is turned into HID reports. This is what movies record and netplay exchanges.
struct DesiredWiimoteState
{
  // A remote lying still, face up: 1 g along +Z.
  static constexpr WiimoteCommon::AccelData DEFAULT_ACCELERATION = WiimoteCommon::AccelData(
      {Wiimote::ACCEL_ZERO_G << 2, Wiimote::ACCEL_ZERO_G << 2, Wiimote::ACCEL_ONE_G << 2});

  // The camera sees nothing.
  static constexpr std::array<CameraPoint, 4> DEFAULT_CAMERA = {CameraPoint(), CameraPoint(),
                                                                CameraPoint(), CameraPoint()};

  // Only the button bits are used.
  WiimoteCommon::ButtonData buttons{};
  WiimoteCommon::AccelData acceleration = DEFAULT_ACCELERATION;
  std::array<CameraPoint, 4> camera_points = DEFAULT_CAMERA;
  std::optional<MotionPlus::DataFormat::Data> motion_plus = std::nullopt;
  DesiredExtensionState extension;
};

// Compact encoding of a DesiredWiimoteState for netplay and movies: at most 1 header byte,
// 5 bytes of buttons and acceleration, 12 of camera, 6 of MotionPlus and 6 of extension data.
// Only the first `length` bytes of `data` are meaningful.
struct SerializedWiimoteState
{
  u8 length;
  std::array<u8, 30> data;
};

SerializedWiimoteState SerializeDesiredState(const DesiredWiimoteState& state);
bool DeserializeDesiredState(DesiredWiimoteState* state, const SerializedWiimoteState& serialized);
}  // namespace WiimoteEmu
