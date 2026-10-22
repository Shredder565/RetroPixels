// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/BitField.h"
#include "Common/Swap.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class AnalogStick;
class Triggers;
class ControlGroup;
}  // namespace ControllerEmu

// Ubisoft's "Drawsome" tablet: a pressure sensitive pen. A third-party scheme extension.

namespace WiimoteEmu
{
enum class DrawsomeTabletGroup
{
  Stylus,
  Touch,
};

class DrawsomeTablet : public Extension3rdParty
{
public:
  DrawsomeTablet();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(DrawsomeTabletGroup group);

  // The six byte report.
  struct DataFormat
  {
    // 16 bit little endian pen coordinates.
    u8 stylus_x1;
    u8 stylus_x2;
    u8 stylus_y1;
    u8 stylus_y2;

    // 11 bit pressure: low eight bits here, top three in the next byte.
    u8 pressure1;
    union
    {
      BitField<0, 3, u8> pressure2;
      BitField<3, 5, u8> status;
    };
  };
  static_assert(6 == sizeof(DataFormat), "Wrong size.");

  using DesiredState = DataFormat;

private:
  ControllerEmu::AnalogStick* m_stylus;
  ControllerEmu::Triggers* m_touch;
};
}  // namespace WiimoteEmu
