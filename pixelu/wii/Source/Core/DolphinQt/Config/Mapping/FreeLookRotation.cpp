// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/FreeLookRotation.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "Core/FreeLookManager.h"
#include "DolphinQt/Config/ControllerInterface/ControllerInterfaceWindow.h"
#include "InputCommon/InputConfig.h"

FreeLookRotation::FreeLookRotation(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void FreeLookRotation::OpenAlternateInputSources()
{
  // Window-modal and deleted on close, so the tab stays usable afterwards.
  auto* const sources = new ControllerInterfaceWindow(this);
  sources->setAttribute(Qt::WA_DeleteOnClose, true);
  sources->setWindowModality(Qt::WindowModality::WindowModal);
  sources->show();
}

void FreeLookRotation::BuildGroups()
{
  auto* const grid = new QGridLayout;

  // Top row: a note about motion input with a shortcut to the input source settings.
  auto* const note = new QLabel(
      tr("Note: motion input may require configuring alternate input sources before use."));
  note->setWordWrap(true);
  auto* const sources_button = new QPushButton(tr("Alternate Input Sources"));
  connect(sources_button, &QPushButton::clicked, this, [this] { OpenAlternateInputSources(); });

  auto* const note_row = new QHBoxLayout();
  note_row->addWidget(note, 1);
  note_row->addWidget(sources_button, 0, Qt::AlignRight);
  grid->addLayout(note_row, 0, 0, 1, -1);

  // Below it, the rotation group itself.
  auto* const rotation = FreeLook::GetInputGroup(GetPort(), FreeLookGroup::Rotation);
  grid->addWidget(CreateGroupBox(tr("Incremental Rotation (rad/sec)"), rotation), 1, 0);

  setLayout(grid);
}

void FreeLookRotation::LoadSettings()
{
  FreeLook::LoadInputConfig();
}

InputConfig* FreeLookRotation::GetConfig()
{
  return FreeLook::GetInputConfig();
}

void FreeLookRotation::SaveSettings()
{
  InputConfig* const config = FreeLook::GetInputConfig();
  config->SaveConfig();
}
