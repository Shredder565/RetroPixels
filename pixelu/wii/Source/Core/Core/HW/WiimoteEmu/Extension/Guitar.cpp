// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/Guitar.h"

#include <algorithm>
#include <array>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Slider.h"
#include "InputCommon/ControllerEmu/ControlGroup/Triggers.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> GUITAR_ID = {0x00, 0x00, 0xa4, 0x20, 0x01, 0x03};

// Touch slider codes, by slider position. Positions were measured with a PS3 Guitar Hero 5
// guitar (its touch bar shows up as an axis on a PC). A position maps to the first entry at or
// beyond it.
struct SliderCode
{
  ControlState position;
  u8 code;
};
constexpr std::array<SliderCode, 6> SLIDER_CODES = {{
    {-0.4375, 0x04},    // top fret
    {-0.097656, 0x0A},  // second fret
    {0.0, 0x0F},        // not touched
    {0.203125, 0x12},   // third fret
    {0.578125, 0x17},   // fourth fret
    {1.0, 0x1F},        // bottom fret
}};
constexpr u8 SLIDER_UNTOUCHED = 0x0F;

u8 SliderCodeAt(ControlState position)
{
  const auto it = std::find_if(SLIDER_CODES.begin(), SLIDER_CODES.end(),
                               [position](const SliderCode& c) { return !(c.position < position); });
  // Positions beyond the last entry cannot occur (the slider is clamped to -1..1); treat them as
  // the bottom fret.
  return it != SLIDER_CODES.end() ? it->code : SLIDER_CODES.back().code;
}

// Frets in group order, with their report bits.
constexpr std::array<const char*, 5> FRET_NAMES = {_trans("Green"), _trans("Red"),
                                                   _trans("Yellow"), _trans("Blue"),
                                                   _trans("Orange")};
constexpr std::array<u16, 5> FRET_BITS = {Guitar::FRET_GREEN, Guitar::FRET_RED,
                                          Guitar::FRET_YELLOW, Guitar::FRET_BLUE,
                                          Guitar::FRET_ORANGE};

// "-" then "+".
constexpr std::array<u16, 2> BUTTON_BITS = {Guitar::BUTTON_MINUS, Guitar::BUTTON_PLUS};

// Up then down.
constexpr std::array<u16, 2> STRUM_BITS = {Guitar::BAR_UP, Guitar::BAR_DOWN};
}  // namespace

Guitar::Guitar() : Extension1stParty(_trans("Guitar"))
{
  using Translatability = ControllerEmu::Translatability;

  m_frets = new ControllerEmu::Buttons(_trans("Frets"));
  groups.emplace_back(m_frets);
  for (const char* name : FRET_NAMES)
    m_frets->AddInput(Translatability::Translate, name);

  m_strum = new ControllerEmu::Buttons(_trans("Strum"));
  groups.emplace_back(m_strum);
  m_strum->AddInput(Translatability::Translate, _trans("Up"));
  m_strum->AddInput(Translatability::Translate, _trans("Down"));

  m_buttons = new ControllerEmu::Buttons(_trans("Buttons"));
  groups.emplace_back(m_buttons);
  m_buttons->AddInput(Translatability::DoNotTranslate, "-");
  m_buttons->AddInput(Translatability::DoNotTranslate, "+");

  m_stick = new ControllerEmu::OctagonAnalogStick(
      _trans("Stick"), ControlState(STICK_GATE_RADIUS) / STICK_RADIUS);
  groups.emplace_back(m_stick);

  m_whammy = new ControllerEmu::Triggers(_trans("Whammy"));
  groups.emplace_back(m_whammy);
  m_whammy->AddInput(Translatability::Translate, _trans("Bar"));

  m_slider_bar = new ControllerEmu::Slider(_trans("Slider Bar"));
  groups.emplace_back(m_slider_bar);
}

void Guitar::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  using ControllerEmu::MapFloat;

  DataFormat report = {};

  const auto stick = m_stick->GetState(m_input_override_function);
  report.sx = MapFloat<u8>(stick.x, STICK_CENTER, 0, STICK_RANGE) & 0x3f;
  report.sy = MapFloat<u8>(stick.y, STICK_CENTER, 0, STICK_RANGE) & 0x3f;

  // An unmapped slider reads as untouched.
  const bool slider_mapped = m_slider_bar->controls[0]->control_ref->BoundCount() &&
                             m_slider_bar->controls[1]->control_ref->BoundCount();
  if (slider_mapped)
    report.sb = SliderCodeAt(m_slider_bar->GetState(m_input_override_function).value) & 0x1f;
  else
    report.sb = SLIDER_UNTOUCHED;

  const auto whammy = m_whammy->GetState(m_input_override_function);
  report.whammy = MapFloat<u8>(whammy.data[0], 0, 0, 0x1F) & 0x1f;

  m_buttons->GetState(&report.bt, BUTTON_BITS.data(), m_input_override_function);
  m_frets->GetState(&report.bt, FRET_BITS.data(), m_input_override_function);
  m_strum->GetState(&report.bt, STRUM_BITS.data(), m_input_override_function);

  // The report wants released buttons as 1.
  report.bt = static_cast<u16>(~report.bt);

  target_state->data = report;
}

void Guitar::Update(const DesiredExtensionState& target_state)
{
  DefaultExtensionUpdate<DataFormat>(&m_reg, target_state);
}

void Guitar::Reset()
{
  EncryptedExtension::Reset();

  // No calibration data is known for the guitar; the block stays zero.
  m_reg.identifier = GUITAR_ID;
}

ControllerEmu::ControlGroup* Guitar::GetGroup(GuitarGroup group)
{
  switch (group)
  {
  case GuitarGroup::Buttons:
    return m_buttons;
  case GuitarGroup::Frets:
    return m_frets;
  case GuitarGroup::Strum:
    return m_strum;
  case GuitarGroup::Whammy:
    return m_whammy;
  case GuitarGroup::Stick:
    return m_stick;
  case GuitarGroup::SliderBar:
    return m_slider_bar;
  }
  ASSERT(false);
  return nullptr;
}
}  // namespace WiimoteEmu
