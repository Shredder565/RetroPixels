// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/GCMicrophone.h"

#include <QGroupBox>
#include <QHBoxLayout>

#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "InputCommon/InputConfig.h"

GCMicrophone::GCMicrophone(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void GCMicrophone::BuildGroups()
{
  // The microphone button lives in the GameCube pad configuration of the same port.
  auto* const mic_group = Pad::GetGroup(GetPort(), PadGroup::Mic);

  auto* const row = new QHBoxLayout();
  row->addWidget(CreateGroupBox(tr("Microphone"), mic_group));
  setLayout(row);
}

void GCMicrophone::SaveSettings()
{
  InputConfig* const config = Pad::GetConfig();
  config->SaveConfig();
}

InputConfig* GCMicrophone::GetConfig()
{
  return Pad::GetConfig();
}

void GCMicrophone::LoadSettings()
{
  Pad::LoadConfig();
}
