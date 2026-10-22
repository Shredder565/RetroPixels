// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/ChunkFile.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "Core/HW/WiimoteEmu/I2CBus.h"

namespace WiimoteEmu
{
// Index of each attachment in the remote's "Extension" list. The order is stored in movies and
// netplay data, so it must not change.
enum ExtensionNumber : u8
{
  NONE,
  // clang-format off
  NUNCHUK, CLASSIC, GUITAR, DRUMS, TURNTABLE, UDRAW_TABLET, DRAWSOME_TABLET, TATACON, SHINKANSEN,
  // clang-format on
  MAX
};

// The connector at the bottom of the remote (and at the end of a MotionPlus). Something is
// always attached: the "None" extension stands for an empty port.
class ExtensionPort
{
public:
  // Extension input is read from i2c slave 0x52 starting at register 0x00.
  static constexpr u8 REPORT_I2C_SLAVE = 0x52;
  static constexpr u8 REPORT_I2C_ADDR = 0x00;

  explicit ExtensionPort(I2CBus* i2c_bus);

  // State of the port's device-detect line.
  bool IsDeviceConnected() const;

  // Unplugs the current device from the bus and plugs in the given one.
  void AttachExtension(Extension* dev);

private:
  Extension* m_extension = nullptr;
  I2CBus& m_i2c_bus;
};

}  // namespace WiimoteEmu
