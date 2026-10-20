// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/Drums.h"

#include <cmath>
#include <cstddef>
#include <variant>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> DRUMS_ID = {0x01, 0x00, 0xa4, 0x20, 0x01, 0x03};

// The pads in group order: name, report bit and velocity id. The order also decides which of
// several simultaneous new hits is reported first.
struct PadInfo
{
  const char* name;
  u8 bit;
  Drums::VelocityID velocity_id;
};
constexpr std::array<PadInfo, 6> PADS = {{
    {_trans("Red"), Drums::PAD_RED, Drums::VelocityID::Red},
    {_trans("Yellow"), Drums::PAD_YELLOW, Drums::VelocityID::Yellow},
    {_trans("Blue"), Drums::PAD_BLUE, Drums::VelocityID::Blue},
    {_trans("Orange"), Drums::PAD_ORANGE, Drums::VelocityID::Orange},
    {_trans("Green"), Drums::PAD_GREEN, Drums::VelocityID::Green},
    {_trans("Bass"), Drums::PAD_BASS, Drums::VelocityID::Bass},
}};

constexpr std::array<u8, PADS.size()> PAD_BITS = [] {
  std::array<u8, PADS.size()> bits{};
  for (std::size_t i = 0; i != PADS.size(); ++i)
    bits[i] = PADS[i].bit;
  return bits;
}();

// "-" then "+".
constexpr std::array<u8, 2> BUTTON_BITS = {Drums::BUTTON_MINUS, Drums::BUTTON_PLUS};

// Reports for which a hit pad's bit stays set.
constexpr u8 HIT_REPORTS = 10;

// Softest possible hit; also the velocity field when no velocity is sent.
constexpr u8 SOFTEST = 0x7f;
}  // namespace

Drums::Drums() : Extension1stParty("Drums", _trans("Drum Kit"))
{
  using Translatability = ControllerEmu::Translatability;

  m_pads = new ControllerEmu::Buttons(_trans("Pads"));
  groups.emplace_back(m_pads);
  for (const PadInfo& pad : PADS)
    m_pads->AddInput(Translatability::Translate, pad.name);

  m_pads->AddSetting(&m_hit_strength_setting,
                     // i18n: Refers to how hard emulated drum pads are struck.
                     {_trans("Hit Strength"),
                      // i18n: The symbol for percent.
                      _trans("%")},
                     50);

  m_buttons = new ControllerEmu::Buttons(_trans("Buttons"));
  groups.emplace_back(m_buttons);
  m_buttons->AddInput(Translatability::DoNotTranslate, "-");
  m_buttons->AddInput(Translatability::DoNotTranslate, "+");

  m_stick = new ControllerEmu::OctagonAnalogStick(_trans("Stick"), GATE_RADIUS);
  groups.emplace_back(m_stick);
}

void Drums::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  DesiredState& desired = target_state->data.emplace<DesiredState>();

  const auto stick = m_stick->GetState(m_input_override_function);
  desired.stick_x = ControllerEmu::MapFloat<u8>(stick.x, STICK_CENTER, STICK_MIN, STICK_MAX);
  desired.stick_y = ControllerEmu::MapFloat<u8>(stick.y, STICK_CENTER, STICK_MIN, STICK_MAX);

  desired.buttons = 0;
  m_buttons->GetState(&desired.buttons, BUTTON_BITS.data(), m_input_override_function);

  desired.drum_pads = 0;
  m_pads->GetState(&desired.drum_pads, PAD_BITS.data(), m_input_override_function);

  // Hit strength 0-100% maps to softness 127-0.
  const long hardness = std::lround(m_hit_strength_setting.GetValue() * 0x7F / 100);
  desired.softness = static_cast<u8>(SOFTEST - hardness);
}

void Drums::Update(const DesiredExtensionState& target_state)
{
  // Without drum input, report an idle kit.
  DesiredState desired{STICK_CENTER, STICK_CENTER, 0, 0, SOFTEST};
  if (const auto* const state = std::get_if<DesiredState>(&target_state.data))
    desired = *state;

  // Track new hits: pads down now that were up in the previous update.
  m_new_pad_hits = static_cast<u8>(m_new_pad_hits | (desired.drum_pads & ~m_prev_pad_input));
  m_prev_pad_input = desired.drum_pads;

  // At most one hit's velocity per report. Its pad bit only shows from this report on, so a
  // real kit never reports two simultaneous hits either.
  u8 velocity = SOFTEST;
  const PadInfo* reported_hit = nullptr;
  for (std::size_t i = 0; i != PADS.size(); ++i)
  {
    if ((m_new_pad_hits & PADS[i].bit) == 0)
      continue;

    m_new_pad_hits = static_cast<u8>(m_new_pad_hits & ~PADS[i].bit);
    m_pad_remaining_frames[i] = HIT_REPORTS;
    reported_hit = &PADS[i];
    velocity = desired.softness;
    break;
  }

  u8 pads_shown = 0;
  for (std::size_t i = 0; i != PADS.size(); ++i)
  {
    if (m_pad_remaining_frames[i] == 0)
      continue;
    pads_shown = static_cast<u8>(pads_shown | PADS[i].bit);
    --m_pad_remaining_frames[i];
  }

  DataFormat report = {};
  report.stick_x = desired.stick_x & 0x3f;
  report.stick_y = desired.stick_y & 0x3f;
  report.unk1 = 0b11;
  report.unk2 = 0b11;
  report.unk5 = 0b11;

  const bool has_velocity = reported_hit != nullptr;
  report.velocity_id = static_cast<u8>(has_velocity ? reported_hit->velocity_id : VelocityID::None) & 0x7f;
  report.no_velocity_data_1 = has_velocity ? 0 : 1;
  report.no_velocity_data_2 = has_velocity ? 0 : 1;

  // The seven velocity bits are scattered over bytes 2, 3 and 4.
  report.buttons = desired.buttons;
  report.velocity0 = velocity & 1;
  report.velocity1 = (velocity >> 1) & 1;
  report.velocity2 = (velocity >> 2) & 1;
  report.velocity3 = (velocity >> 3) & 1;
  report.velocity4 = (velocity >> 4) & 0b111;

  // Buttons and pads are active low (the button byte's velocity bits are not inverted).
  report.buttons = static_cast<u8>(report.buttons ^ 0x7e);
  report.drum_pads = static_cast<u8>(~pads_shown);

  Common::BitCastPtr<DataFormat>(m_reg.controller_data) = report;
}

void Drums::Reset()
{
  EncryptedExtension::Reset();

  m_reg.identifier = DRUMS_ID;

  // A real kit reports both calibration blocks as all 0xff.
  m_reg.calibration.fill(0xff);

  m_prev_pad_input = 0;
  m_new_pad_hits = 0;
  m_pad_remaining_frames = {};
}

void Drums::DoState(PointerWrap& p)
{
  EncryptedExtension::DoState(p);

  p.Do(m_prev_pad_input);
  p.Do(m_new_pad_hits);
  p.Do(m_pad_remaining_frames);
}

ControllerEmu::ControlGroup* Drums::GetGroup(DrumsGroup group)
{
  switch (group)
  {
  case DrumsGroup::Buttons:
    return m_buttons;
  case DrumsGroup::Pads:
    return m_pads;
  case DrumsGroup::Stick:
    return m_stick;
  }
  ASSERT(false);
  return nullptr;
}
}  // namespace WiimoteEmu
