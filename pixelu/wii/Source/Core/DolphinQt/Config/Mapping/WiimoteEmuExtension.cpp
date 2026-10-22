// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/WiimoteEmuExtension.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/DrawsomeTablet.h"
#include "Core/HW/WiimoteEmu/Extension/Drums.h"
#include "Core/HW/WiimoteEmu/Extension/Guitar.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/Extension/Shinkansen.h"
#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"
#include "Core/HW/WiimoteEmu/Extension/Turntable.h"
#include "Core/HW/WiimoteEmu/Extension/UDrawTablet.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/InputConfig.h"

using WiimoteEmu::ExtensionNumber;

WiimoteEmuExtension::WiimoteEmuExtension(MappingWindow* window) : MappingWidget(window)
{
  // The frames are created (and laid out left to right) in this order.
  m_frames[ExtensionNumber::CLASSIC] = BuildClassicFrame();
  m_frames[ExtensionNumber::DRUMS] = BuildDrumsFrame();
  m_frames[ExtensionNumber::GUITAR] = BuildGuitarFrame();
  m_frames[ExtensionNumber::NONE] = BuildEmptyFrame();
  m_frames[ExtensionNumber::NUNCHUK] = BuildNunchukFrame();
  m_frames[ExtensionNumber::TURNTABLE] = BuildTurntableFrame();
  m_frames[ExtensionNumber::UDRAW_TABLET] = BuildUDrawFrame();
  m_frames[ExtensionNumber::DRAWSOME_TABLET] = BuildDrawsomeFrame();
  m_frames[ExtensionNumber::TATACON] = BuildTaTaConFrame();
  m_frames[ExtensionNumber::SHINKANSEN] = BuildShinkansenFrame();

  auto* const row = new QHBoxLayout();
  for (const ExtensionNumber shown :
       {ExtensionNumber::CLASSIC, ExtensionNumber::DRUMS, ExtensionNumber::GUITAR,
        ExtensionNumber::NONE, ExtensionNumber::NUNCHUK, ExtensionNumber::TURNTABLE,
        ExtensionNumber::UDRAW_TABLET, ExtensionNumber::DRAWSOME_TABLET, ExtensionNumber::TATACON,
        ExtensionNumber::SHINKANSEN})
  {
    row->addWidget(m_frames[shown]);
  }
  setLayout(row);

  ChangeExtensionType(ExtensionNumber::NONE);
}

QGroupBox* WiimoteEmuExtension::BuildClassicFrame()
{
  using WiimoteEmu::ClassicGroup;
  auto* const frame = new QGroupBox(tr("Classic Controller"), this);
  const auto group = [this](ClassicGroup which) {
    return Wiimote::GetClassicGroup(GetPort(), which);
  };

  auto* const grid = new QGridLayout();
  grid->addWidget(CreateGroupBox(tr("Buttons"), group(ClassicGroup::Buttons)), 0, 0);
  grid->addWidget(CreateGroupBox(tr("D-Pad"), group(ClassicGroup::DPad)), 1, 0);
  grid->addWidget(CreateGroupBox(tr("Left Stick"), group(ClassicGroup::LeftStick)), 0, 1, -1, 1);
  grid->addWidget(CreateGroupBox(tr("Right Stick"), group(ClassicGroup::RightStick)), 0, 2, -1, 1);
  grid->addWidget(CreateGroupBox(tr("Triggers"), group(ClassicGroup::Triggers)), 0, 3, -1, 1);
  frame->setLayout(grid);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildDrumsFrame()
{
  using WiimoteEmu::DrumsGroup;
  auto* const frame = new QGroupBox(tr("Drum Kit"), this);
  const auto group = [this](DrumsGroup which) { return Wiimote::GetDrumsGroup(GetPort(), which); };

  auto* const grid = new QGridLayout();
  grid->addWidget(CreateGroupBox(tr("Stick"), group(DrumsGroup::Stick)), 0, 0, -1, 1);
  grid->addWidget(CreateGroupBox(tr("Pads"), group(DrumsGroup::Pads)), 0, 1);
  grid->addWidget(CreateGroupBox(tr("Buttons"), group(DrumsGroup::Buttons)), 1, 1);
  frame->setLayout(grid);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildGuitarFrame()
{
  using WiimoteEmu::GuitarGroup;
  auto* const frame = new QGroupBox(tr("Guitar"), this);
  const auto group = [this](GuitarGroup which) {
    return Wiimote::GetGuitarGroup(GetPort(), which);
  };

  // Three columns: the stick; strum and frets; buttons, whammy and slider bar.
  auto* const left = new QVBoxLayout();
  left->addWidget(CreateGroupBox(tr("Stick"), group(GuitarGroup::Stick)));

  auto* const middle = new QVBoxLayout();
  middle->addWidget(CreateGroupBox(tr("Strum"), group(GuitarGroup::Strum)));
  middle->addWidget(CreateGroupBox(tr("Frets"), group(GuitarGroup::Frets)));

  auto* const right = new QVBoxLayout();
  right->addWidget(CreateGroupBox(tr("Buttons"), group(GuitarGroup::Buttons)));
  right->addWidget(CreateGroupBox(tr("Whammy"), group(GuitarGroup::Whammy)));
  right->addWidget(CreateGroupBox(tr("Slider Bar"), group(GuitarGroup::SliderBar)));

  auto* const columns = new QHBoxLayout();
  columns->addLayout(left);
  columns->addLayout(middle);
  columns->addLayout(right);
  frame->setLayout(columns);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildEmptyFrame()
{
  // Untitled frame with a centred notice, shown while nothing is attached.
  auto* const frame = new QGroupBox(this);
  auto* const notice = new QLabel(tr("No extension selected."));
  notice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  notice->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

  auto* const row = new QHBoxLayout();
  row->addWidget(notice);
  frame->setLayout(row);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildNunchukFrame()
{
  using WiimoteEmu::NunchukGroup;
  auto* const frame = new QGroupBox(tr("Nunchuk"), this);
  const auto group = [this](NunchukGroup which) {
    return Wiimote::GetNunchukGroup(GetPort(), which);
  };

  auto* const grid = new QGridLayout();
  grid->addWidget(CreateGroupBox(tr("Stick"), group(NunchukGroup::Stick)), 0, 0);
  grid->addWidget(CreateGroupBox(tr("Buttons"), group(NunchukGroup::Buttons)), 0, 1);
  frame->setLayout(grid);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildTurntableFrame()
{
  using WiimoteEmu::TurntableGroup;
  auto* const frame = new QGroupBox(tr("DJ Turntable"), this);
  const auto group = [this](TurntableGroup which) {
    return Wiimote::GetTurntableGroup(GetPort(), which);
  };

  auto* const grid = new QGridLayout();
  grid->addWidget(CreateGroupBox(tr("Stick"), group(TurntableGroup::Stick)), 0, 0, -1, 1);
  grid->addWidget(CreateGroupBox(tr("Buttons"), group(TurntableGroup::Buttons)), 0, 1);
  grid->addWidget(CreateGroupBox(tr("Effect"), group(TurntableGroup::EffectDial)), 1, 1, -1, 1);
  // i18n: "Table" refers to a turntable
  grid->addWidget(CreateGroupBox(tr("Left Table"), group(TurntableGroup::LeftTable)), 0, 2);
  // i18n: "Table" refers to a turntable
  grid->addWidget(CreateGroupBox(tr("Right Table"), group(TurntableGroup::RightTable)), 1, 2);
  grid->addWidget(CreateGroupBox(tr("Crossfade"), group(TurntableGroup::Crossfade)), 2, 2);
  frame->setLayout(grid);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildUDrawFrame()
{
  using WiimoteEmu::UDrawTabletGroup;
  auto* const frame = new QGroupBox(tr("uDraw GameTablet"), this);
  const auto group = [this](UDrawTabletGroup which) {
    return Wiimote::GetUDrawTabletGroup(GetPort(), which);
  };

  auto* const row = new QHBoxLayout();
  row->addWidget(CreateGroupBox(tr("Buttons"), group(UDrawTabletGroup::Buttons)));
  row->addWidget(CreateGroupBox(tr("Stylus"), group(UDrawTabletGroup::Stylus)));
  row->addWidget(CreateGroupBox(tr("Touch"), group(UDrawTabletGroup::Touch)));
  frame->setLayout(row);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildDrawsomeFrame()
{
  using WiimoteEmu::DrawsomeTabletGroup;
  auto* const frame = new QGroupBox(tr("Drawsome Tablet"), this);
  const auto group = [this](DrawsomeTabletGroup which) {
    return Wiimote::GetDrawsomeTabletGroup(GetPort(), which);
  };

  auto* const row = new QHBoxLayout();
  row->addWidget(CreateGroupBox(tr("Stylus"), group(DrawsomeTabletGroup::Stylus)));
  row->addWidget(CreateGroupBox(tr("Touch"), group(DrawsomeTabletGroup::Touch)));
  frame->setLayout(row);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildTaTaConFrame()
{
  using WiimoteEmu::TaTaConGroup;
  auto* const frame = new QGroupBox(tr("Taiko Drum"), this);
  const auto group = [this](TaTaConGroup which) {
    return Wiimote::GetTaTaConGroup(GetPort(), which);
  };

  auto* const row = new QHBoxLayout();
  row->addWidget(CreateGroupBox(tr("Center"), group(TaTaConGroup::Center)));
  row->addWidget(CreateGroupBox(tr("Rim"), group(TaTaConGroup::Rim)));
  frame->setLayout(row);
  return frame;
}

QGroupBox* WiimoteEmuExtension::BuildShinkansenFrame()
{
  using WiimoteEmu::ShinkansenGroup;
  auto* const frame = new QGroupBox(tr("Shinkansen"), this);
  const auto group = [this](ShinkansenGroup which) {
    return Wiimote::GetShinkansenGroup(GetPort(), which);
  };

  auto* const row = new QHBoxLayout();
  row->addWidget(CreateGroupBox(tr("Levers"), group(ShinkansenGroup::Levers)));
  row->addWidget(CreateGroupBox(tr("Buttons"), group(ShinkansenGroup::Buttons)));
  row->addWidget(CreateGroupBox(tr("Light"), group(ShinkansenGroup::Light)));
  frame->setLayout(row);
  return frame;
}

void WiimoteEmuExtension::ChangeExtensionType(u32 type)
{
  for (u32 number = 0; number < m_frames.size(); ++number)
    m_frames[number]->setHidden(number != type);
}

InputConfig* WiimoteEmuExtension::GetConfig()
{
  return Wiimote::GetConfig();
}

void WiimoteEmuExtension::SaveSettings()
{
  InputConfig* const config = Wiimote::GetConfig();
  config->SaveConfig();
}

void WiimoteEmuExtension::LoadSettings()
{
  Wiimote::LoadConfig();
}
