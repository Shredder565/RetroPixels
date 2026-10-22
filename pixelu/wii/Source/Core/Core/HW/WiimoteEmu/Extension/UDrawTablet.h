// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class Buttons;
class AnalogStick;
class Triggers;
class ControlGroup;
}  // namespace ControllerEmu

// THQ's uDraw GameTablet: a pressure sensitive stylus on a drawing surface plus a two-way
// rocker. A third-party scheme extension.

namespace WiimoteEmu
{
enum class UDrawTabletGroup
{
  Buttons,
  Stylus,
  Touch,
};

class UDrawTablet : public Extension3rdParty
{
public:
  UDrawTablet();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(UDrawTabletGroup group);

  static constexpr u8 BUTTON_ROCKER_UP = 0x1;
  static constexpr u8 BUTTON_ROCKER_DOWN = 0x2;

  // The six byte report.
  struct DataFormat
  {
    // 12 bit coordinates (x grows to the right, y grows upwards); all ones while the stylus is
    // lifted.
    u8 stylus_x1;
    u8 stylus_y1;
    u8 stylus_x2 : 4;
    u8 stylus_y2 : 4;

    // Reported whether or not the stylus touches the surface.
    u8 pressure;

    // Reads 0xff.
    u8 unk;

    // Active low; bit 2 is always clear, so the idle value is 0xfb.
    u8 buttons;
  };
  static_assert(6 == sizeof(DataFormat), "Wrong size.");

  using DesiredState = DataFormat;

private:
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::AnalogStick* m_stylus;
  ControllerEmu::Triggers* m_touch;
};
}  // namespace WiimoteEmu
