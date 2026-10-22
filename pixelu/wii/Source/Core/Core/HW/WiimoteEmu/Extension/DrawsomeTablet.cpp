// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/DrawsomeTablet.h"

#include <array>
#include <cmath>
#include <memory>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Triggers.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> DRAWSOME_ID = {0xff, 0x00, 0xa4, 0x20, 0x00, 0x13};

// Pen coordinate range. Y runs downwards; its bottom edge lies 0x100 beyond the calibrated
// 0x15ff because the game's menus need the pen to go a little off the drawing area.
constexpr u16 PEN_LEFT = 0x0000;
constexpr u16 PEN_RIGHT = 0x27ff;
constexpr u16 PEN_TOP = 0x0000;
constexpr u16 PEN_BOTTOM = 0x15ff + 0x100;

constexpr u16 PEN_CENTER_X = (PEN_RIGHT + PEN_LEFT + 1) / 2;
constexpr u16 PEN_CENTER_Y = (PEN_TOP + PEN_BOTTOM + 1) / 2;

constexpr u16 PRESSURE_MAX = 0x7ff;

// Values of the 5 bit status field: 0x08 is always set, 0x10 flags a lifted pen.
constexpr u8 STATUS_IDLE = 0x08;
constexpr u8 STATUS_LIFTED = 0x10;
}  // namespace

// i18n: The "Drawsome" (combination of "Draw" and "Awesome") tablet wiimote extension by Ubisoft.
DrawsomeTablet::DrawsomeTablet() : Extension3rdParty("Drawsome", _trans("Drawsome Tablet"))
{
  m_stylus = new ControllerEmu::AnalogStick(_trans("Stylus"),
                                            std::make_unique<ControllerEmu::SquareStickGate>(1.0));
  groups.emplace_back(m_stylus);

  using Translatability = ControllerEmu::Translatability;
  m_touch = new ControllerEmu::Triggers(_trans("Touch"));
  groups.emplace_back(m_touch);
  m_touch->AddInput(Translatability::Translate, _trans("Pressure"));
  m_touch->AddInput(Translatability::Translate, _trans("Lift"));
}

void DrawsomeTablet::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  using ControllerEmu::MapFloat;

  DataFormat& report = target_state->data.emplace<DataFormat>();

  // The pen keeps reporting its last position while lifted, so the position is always sent.
  const auto stylus = m_stylus->GetState(m_input_override_function);
  const u16 x = MapFloat<u16>(stylus.x, PEN_CENTER_X, PEN_LEFT, PEN_RIGHT);
  const u16 y = MapFloat<u16>(-stylus.y, PEN_CENTER_Y, PEN_TOP, PEN_BOTTOM);

  report.stylus_x1 = u8(x);
  report.stylus_x2 = u8(x >> 8);
  report.stylus_y1 = u8(y);
  report.stylus_y2 = u8(y >> 8);

  const auto touch = m_touch->GetState(m_input_override_function);

  const bool lifted = std::lround(touch.data[1]) != 0;
  report.status = static_cast<u8>(STATUS_IDLE | (lifted ? STATUS_LIFTED : 0));

  const u16 pressure = MapFloat<u16>(touch.data[0], 0, 0, PRESSURE_MAX);
  report.pressure1 = u8(pressure);
  report.pressure2 = u8(pressure >> 8);
}

void DrawsomeTablet::Update(const DesiredExtensionState& target_state)
{
  DefaultExtensionUpdate<DataFormat>(&m_reg, target_state);
}

void DrawsomeTablet::Reset()
{
  EncryptedExtension::Reset();

  m_reg.identifier = DRAWSOME_ID;

  // Presumed all 0xff, as on the other third-party extensions.
  m_reg.calibration.fill(0xff);
}

ControllerEmu::ControlGroup* DrawsomeTablet::GetGroup(DrawsomeTabletGroup group)
{
  return GroupByEnum(group, m_stylus, m_touch);
}
}  // namespace WiimoteEmu
