// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/GCPadEmu.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QString>

#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/InputConfig.h"

GCPadEmu::GCPadEmu(MappingWindow* window, SubType sub_type) : MappingWidget(window)
{
  BuildGroups(sub_type);
}

void GCPadEmu::BuildGroups(SubType sub_type)
{
  auto* const grid = new QGridLayout;

  const auto place = [this, grid](const QString& title, PadGroup group, int row, int column,
                                  int row_span) {
    grid->addWidget(CreateGroupBox(title, Pad::GetGroup(GetPort(), group)), row, column, row_span,
                    1);
  };

  // Left column: buttons over the D-Pad (and the Triforce buttons for the baseboard).
  place(tr("Buttons"), PadGroup::Buttons, 0, 0, 1);
  place(tr("D-Pad"), PadGroup::DPad, 1, 0, 1);
  if (sub_type == SubType::AMBaseboard)
    place(tr("Triforce"), PadGroup::Triforce, 2, 0, 1);

  // Both sticks take a full-height column each.
  place(tr("Control Stick"), PadGroup::MainStick, 0, 1, -1);
  place(tr("C Stick"), PadGroup::CStick, 0, 2, -1);

  // Right column (grid column 4; column 3 is left empty): triggers, rumble, options.
  place(tr("Triggers"), PadGroup::Triggers, 0, 4, 1);
  place(tr("Rumble"), PadGroup::Rumble, 1, 4, 1);
  place(tr("Options"), PadGroup::Options, 2, 4, 1);

  setLayout(grid);
}

void GCPadEmu::SaveSettings()
{
  InputConfig* const config = Pad::GetConfig();
  config->SaveConfig();
}

void GCPadEmu::LoadSettings()
{
  Pad::LoadConfig();
}

InputConfig* GCPadEmu::GetConfig()
{
  return Pad::GetConfig();
}
