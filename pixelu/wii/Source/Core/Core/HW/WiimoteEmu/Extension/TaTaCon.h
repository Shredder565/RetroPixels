// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class Buttons;
}  // namespace ControllerEmu

// The "Taiko no Tatsujin" drum (TaTaCon): left and right halves of the drum face and rim.
// A third-party scheme extension.

namespace WiimoteEmu
{
enum class TaTaConGroup
{
  Center,
  Rim,
};

class TaTaCon : public Extension3rdParty
{
public:
  // The six byte report; only the last byte carries data (active low).
  struct DataFormat
  {
    std::array<u8, 5> nothing;
    u8 state;
  };
  static_assert(sizeof(DataFormat) == 6, "Wrong size");

  struct DesiredState
  {
    u8 state;
  };

  TaTaCon();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(TaTaConGroup group);

  static constexpr u8 CENTER_LEFT = 0x40;
  static constexpr u8 CENTER_RIGHT = 0x10;
  static constexpr u8 RIM_LEFT = 0x20;
  static constexpr u8 RIM_RIGHT = 0x08;

private:
  ControllerEmu::Buttons* m_center;
  ControllerEmu::Buttons* m_rim;
};
}  // namespace WiimoteEmu
