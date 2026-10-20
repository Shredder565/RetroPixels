// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class AnalogStick;
class Buttons;
class ControlGroup;
class Triggers;
class Slider;
}  // namespace ControllerEmu

// The Guitar Hero guitar: five frets, a strum bar, a whammy bar, a stick and (on later
// models) a touch slider. A first-party scheme extension.

namespace WiimoteEmu
{
enum class GuitarGroup
{
  Buttons,
  Frets,
  Strum,
  Whammy,
  Stick,
  SliderBar
};

class Guitar : public Extension1stParty
{
public:
  // The six byte report.
  struct DataFormat
  {
    // Stick, 0-63 per axis. The top bits read 1 on Guitar Hero 3 guitars, 0 on World Tour.
    u8 sx : 6;
    u8 pad1 : 2;

    u8 sy : 6;
    u8 pad2 : 2;

    // Touch slider (World Tour and later).
    u8 sb : 5;
    u8 pad3 : 3;

    u8 whammy : 5;
    u8 pad4 : 3;

    // Active-low buttons.
    u16 bt;
  };
  static_assert(sizeof(DataFormat) == 6, "Wrong size");

  using DesiredState = DataFormat;

  Guitar();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(GuitarGroup group);

  static constexpr u16 BUTTON_PLUS = 0x04;
  static constexpr u16 BUTTON_MINUS = 0x10;
  static constexpr u16 BAR_DOWN = 0x40;

  static constexpr u16 BAR_UP = 0x0100;
  static constexpr u16 FRET_YELLOW = 0x0800;
  static constexpr u16 FRET_GREEN = 0x1000;
  static constexpr u16 FRET_BLUE = 0x2000;
  static constexpr u16 FRET_RED = 0x4000;
  static constexpr u16 FRET_ORANGE = 0x8000;

  static const u8 STICK_CENTER = 0x20;
  static const u8 STICK_RADIUS = 0x1f;
  static const u8 STICK_RANGE = 0x3f;

  // An estimate; not measured on hardware.
  static const u8 STICK_GATE_RADIUS = 0x16;

private:
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::Buttons* m_frets;
  ControllerEmu::Buttons* m_strum;
  ControllerEmu::Triggers* m_whammy;
  ControllerEmu::AnalogStick* m_stick;
  ControllerEmu::Slider* m_slider_bar;
};
}  // namespace WiimoteEmu
