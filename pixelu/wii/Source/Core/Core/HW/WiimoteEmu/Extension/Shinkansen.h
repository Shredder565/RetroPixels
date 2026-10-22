// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class Buttons;
class ControlGroup;
class Triggers;
}  // namespace ControllerEmu

// The controller of "Densha de GO! Shinkansen": brake and power levers, ten buttons and a
// "doors locked" light. A third-party scheme extension.

namespace WiimoteEmu
{
enum class ShinkansenGroup
{
  Buttons,
  Levers,
  Light,
};

class Shinkansen : public Extension3rdParty
{
public:
  struct DesiredState
  {
    u8 brake;
    u8 power;
    u16 buttons;
  };

  Shinkansen();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(ShinkansenGroup group);

private:
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::Triggers* m_levers;
  ControllerEmu::ControlGroup* m_led;

  // The report as written to the register file (eight bytes; the remote reads six).
  struct DataFormat
  {
    u8 unk0;
    u8 unk1;
    u8 brake;
    u8 power;
    u8 unk4;
    u8 unk5;
    // Active low, stored little endian.
    u16 buttons;
  };
};
}  // namespace WiimoteEmu
