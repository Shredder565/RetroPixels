// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/UDrawTablet.h"

#include <array>
#include <cmath>
#include <memory>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/Triggers.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> UDRAW_ID = {0xff, 0x00, 0xa4, 0x20, 0x01, 0x12};

constexpr std::array<u8, 2> ROCKER_BITS = {UDrawTablet::BUTTON_ROCKER_UP,
                                           UDrawTablet::BUTTON_ROCKER_DOWN};

// The drawing area in tablet coordinates, tuned for "uDraw Studio: Instant Artist". (A real
// tablet reports roughly 0x4f-0x7b3 by 0x53-0x5b4 when touched.)
constexpr u16 AREA_LEFT = 0x56;
constexpr u16 AREA_RIGHT = 0x780;
constexpr u16 AREA_BOTTOM = 0x65;
constexpr u16 AREA_TOP = 0x5a5;

// The coordinate value reported while the stylus is lifted.
constexpr u16 LIFTED = 0xfff;

// Maps -1..1 onto [low, high].
u16 AreaCoordinate(ControlState value, u16 low, u16 high)
{
  const double middle = (high + low) / 2.0;
  return u16(middle + value * (high - middle));
}
}  // namespace

UDrawTablet::UDrawTablet() : Extension3rdParty("uDraw", _trans("uDraw GameTablet"))
{
  using Translatability = ControllerEmu::Translatability;

  m_buttons = new ControllerEmu::Buttons(_trans("Buttons"));
  groups.emplace_back(m_buttons);
  m_buttons->AddInput(Translatability::Translate, _trans("Rocker Up"));
  m_buttons->AddInput(Translatability::Translate, _trans("Rocker Down"));

  m_stylus = new ControllerEmu::AnalogStick(_trans("Stylus"),
                                            std::make_unique<ControllerEmu::SquareStickGate>(1.0));
  groups.emplace_back(m_stylus);

  m_touch = new ControllerEmu::Triggers(_trans("Touch"));
  groups.emplace_back(m_touch);
  m_touch->AddInput(Translatability::Translate, _trans("Pressure"));
  m_touch->AddInput(Translatability::Translate, _trans("Lift"));
}

// Note: unlike the other extensions this does not apply the input override function, which
// has always been the case for the tablet.
void UDrawTablet::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  DataFormat report = {};

  const auto touch = m_touch->GetState();

  // Real sensors give about 0x08-0xf2; the full byte range is used here.
  report.pressure = static_cast<u8>(touch.data[0] * 0xff);

  const bool lifted = std::lround(touch.data[1]) != 0;

  // Read even while lifted so input is sampled the same way every update.
  const auto stylus = m_stylus->GetState();

  u16 x = LIFTED;
  u16 y = LIFTED;
  if (!lifted)
  {
    x = AreaCoordinate(stylus.x, AREA_LEFT, AREA_RIGHT);
    y = AreaCoordinate(stylus.y, AREA_BOTTOM, AREA_TOP);
  }

  report.stylus_x1 = static_cast<u8>(x);
  report.stylus_x2 = static_cast<u8>(x >> 8) & 0xf;
  report.stylus_y1 = static_cast<u8>(y);
  report.stylus_y2 = static_cast<u8>(y >> 8) & 0xf;

  m_buttons->GetState(&report.buttons, ROCKER_BITS.data());
  report.buttons = static_cast<u8>(report.buttons ^ 0xfb);

  report.unk = 0xff;

  target_state->data = report;
}

void UDrawTablet::Update(const DesiredExtensionState& target_state)
{
  DefaultExtensionUpdate<DataFormat>(&m_reg, target_state);
}

void UDrawTablet::Reset()
{
  EncryptedExtension::Reset();

  m_reg.identifier = UDRAW_ID;

  // A real tablet's calibration area reads as all 0xff.
  m_reg.calibration.fill(0xff);
}

ControllerEmu::ControlGroup* UDrawTablet::GetGroup(UDrawTabletGroup group)
{
  switch (group)
  {
  case UDrawTabletGroup::Buttons:
    return m_buttons;
  case UDrawTabletGroup::Stylus:
    return m_stylus;
  case UDrawTabletGroup::Touch:
    return m_touch;
  }
  ASSERT(false);
  return nullptr;
}

}  // namespace WiimoteEmu
