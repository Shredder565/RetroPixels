// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GBAPadEmu.h"

#include <array>

#include <fmt/format.h>

#include "Core/HW/GBAPad.h"

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/GCPadStatus.h"

namespace
{
// Status bits for the controls, in the order they are added. Select is
// reported as Z.
constexpr std::array<u16, 6> BUTTON_BITS = {
    PAD_BUTTON_B, PAD_BUTTON_A, PAD_TRIGGER_L, PAD_TRIGGER_R, PAD_TRIGGER_Z, PAD_BUTTON_START,
};
constexpr std::array<u16, 4> DPAD_BITS = {
    PAD_BUTTON_UP,
    PAD_BUTTON_DOWN,
    PAD_BUTTON_LEFT,
    PAD_BUTTON_RIGHT,
};

// Default keys (Windows names), by control index.
constexpr std::array<const char*, 6> BUTTON_DEFAULTS = {"`Z`", "`X`", "`Q`",
                                                        "`W`", "`BACK`", "`RETURN`"};
constexpr std::array<const char*, 4> DPAD_DEFAULTS = {"`T`", "`G`", "`F`", "`H`"};
}  // namespace

GBAPad::GBAPad(const unsigned int index) : m_reset_pending(false), m_index(index)
{
  using ControllerEmu::Translatability;

  m_buttons = new ControllerEmu::Buttons(BUTTONS_GROUP);
  groups.emplace_back(m_buttons);
  for (const char* name : {B_BUTTON, A_BUTTON, L_BUTTON, R_BUTTON})
    m_buttons->AddInput(Translatability::DoNotTranslate, name);
  m_buttons->AddInput(Translatability::Translate, SELECT_BUTTON);
  m_buttons->AddInput(Translatability::Translate, START_BUTTON);

  m_dpad = new ControllerEmu::Buttons(DPAD_GROUP);
  groups.emplace_back(m_dpad);
  for (const char* name : named_directions)
    m_dpad->AddInput(Translatability::Translate, name);
}

std::string GBAPad::GetName() const
{
  return fmt::format("GBA{}", m_index + 1);
}

InputConfig* GBAPad::GetConfig() const
{
  return Pad::GetGBAConfig();
}

ControllerEmu::ControlGroup* GBAPad::GetGroup(GBAPadGroup group) const
{
  if (group == GBAPadGroup::Buttons)
    return m_buttons;
  if (group == GBAPadGroup::DPad)
    return m_dpad;
  return nullptr;
}

GCPadStatus GBAPad::GetInput()
{
  const auto state_lock = GetStateLock();

  GCPadStatus status = {};
  m_buttons->GetState(&status.button, BUTTON_BITS.data(), m_input_override_function);
  m_dpad->GetState(&status.button, DPAD_BITS.data(), m_input_override_function);

  // A requested reset is reported once, as X.
  if (m_reset_pending)
    status.button |= PAD_BUTTON_X;
  m_reset_pending = false;

  return status;
}

void GBAPad::SetReset(bool reset)
{
  const auto state_lock = GetStateLock();
  m_reset_pending = reset;
}

void GBAPad::LoadDefaults(const ControllerInterface& ciface)
{
  EmulatedController::LoadDefaults(ciface);

  for (std::size_t i = 0; i < BUTTON_DEFAULTS.size(); ++i)
    m_buttons->SetControlExpression(static_cast<int>(i), BUTTON_DEFAULTS[i]);
  for (std::size_t i = 0; i < DPAD_DEFAULTS.size(); ++i)
    m_dpad->SetControlExpression(static_cast<int>(i), DPAD_DEFAULTS[i]);
}
