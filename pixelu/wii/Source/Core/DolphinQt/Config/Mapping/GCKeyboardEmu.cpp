// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/GCKeyboardEmu.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QStyle>
#include <QVBoxLayout>

#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCKeyboardEmu.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "InputCommon/InputConfig.h"

GCKeyboardEmu::GCKeyboardEmu(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void GCKeyboardEmu::BuildGroups()
{
  auto* const page = new QVBoxLayout{this};

  // Most people opening this page wanted the standard controller, so warn first.
  auto* const warning =
      new QLabel(tr("You are configuring a \"Keyboard Controller\". This device is exclusively for "
                    "\"Phantasy Star Online Episode I & II\". If you are unsure, turn back now and "
                    "configure a \"Standard Controller\"."));
  warning->setWordWrap(true);
  page->addWidget(QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxWarning, warning));

  // The keys are split into six untitled groups, shown side by side.
  static constexpr KeyboardGroup key_rows[] = {KeyboardGroup::Kb0x, KeyboardGroup::Kb1x,
                                               KeyboardGroup::Kb2x, KeyboardGroup::Kb3x,
                                               KeyboardGroup::Kb4x, KeyboardGroup::Kb5x};
  auto* const keys = new QHBoxLayout;
  for (const KeyboardGroup key_row : key_rows)
    keys->addWidget(CreateGroupBox(QString{}, Keyboard::GetGroup(GetPort(), key_row)));
  page->addLayout(keys);
}

InputConfig* GCKeyboardEmu::GetConfig()
{
  return Keyboard::GetConfig();
}

void GCKeyboardEmu::SaveSettings()
{
  InputConfig* const config = Keyboard::GetConfig();
  config->SaveConfig();
}

void GCKeyboardEmu::LoadSettings()
{
  Keyboard::LoadConfig();
}
