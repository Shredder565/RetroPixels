// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"

#include <array>
#include <variant>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> TATACON_ID = {0x00, 0x00, 0xa4, 0x20, 0x01, 0x11};

// Left then right, for both groups.
constexpr std::array<u8, 2> CENTER_BITS = {TaTaCon::CENTER_LEFT, TaTaCon::CENTER_RIGHT};
constexpr std::array<u8, 2> RIM_BITS = {TaTaCon::RIM_LEFT, TaTaCon::RIM_RIGHT};

void AddLeftRight(ControllerEmu::Buttons* group)
{
  using Translatability = ControllerEmu::Translatability;
  group->AddInput(Translatability::Translate, _trans("Left"));
  group->AddInput(Translatability::Translate, _trans("Right"));
}
}  // namespace

// i18n: The drum controller used in "Taiko no Tatsujin" games. Also known as a "TaTaCon".
TaTaCon::TaTaCon() : Extension3rdParty("TaTaCon", _trans("Taiko Drum"))
{
  // i18n: Refers to the "center" of a TaTaCon drum.
  m_center = new ControllerEmu::Buttons(_trans("Center"));
  groups.emplace_back(m_center);
  AddLeftRight(m_center);

  // i18n: Refers to the "rim" of a TaTaCon drum.
  m_rim = new ControllerEmu::Buttons(_trans("Rim"));
  groups.emplace_back(m_rim);
  AddLeftRight(m_rim);
}

void TaTaCon::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  DesiredState desired = {};
  m_center->GetState(&desired.state, CENTER_BITS.data(), m_input_override_function);
  m_rim->GetState(&desired.state, RIM_BITS.data(), m_input_override_function);
  target_state->data = desired;
}

void TaTaCon::Update(const DesiredExtensionState& target_state)
{
  u8 hits = 0;
  if (const auto* const desired = std::get_if<DesiredState>(&target_state.data))
    hits = desired->state;

  // Only the last report byte is written; it is active low.
  m_reg.controller_data[offsetof(DataFormat, state)] = static_cast<u8>(~hits);
}

void TaTaCon::Reset()
{
  EncryptedExtension::Reset();

  m_reg = {};
  m_reg.identifier = TATACON_ID;

  // Presumed all 0xff, as on the other third-party extensions.
  m_reg.calibration.fill(0xff);
}

ControllerEmu::ControlGroup* TaTaCon::GetGroup(TaTaConGroup group)
{
  return GroupByEnum(group, m_center, m_rim);
}
}  // namespace WiimoteEmu
