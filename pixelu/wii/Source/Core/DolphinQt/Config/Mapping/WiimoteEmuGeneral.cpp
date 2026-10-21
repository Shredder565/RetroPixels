// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/WiimoteEmuGeneral.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Config/Mapping/WiimoteEmuExtension.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/InputConfig.h"

WiimoteEmuGeneral::WiimoteEmuGeneral(MappingWindow* window, WiimoteEmuExtension* extension)
    : MappingWidget(window), m_extension_page(extension)
{
  BuildGroups();

  connect(m_extension_combo, &QComboBox::currentIndexChanged, this,
          &WiimoteEmuGeneral::OnAttachmentChanged);
  connect(m_extension_combo, &QComboBox::activated, this, &WiimoteEmuGeneral::OnAttachmentSelected);
  connect(this, &MappingWidget::ConfigChanged, this, &WiimoteEmuGeneral::SyncWithConfig);
  connect(this, &MappingWidget::Update, this, &WiimoteEmuGeneral::SyncWithState);
  connect(m_configure_extension, &QPushButton::clicked, GetParent(),
          &MappingWindow::ActivateExtensionTab);
}

ControllerEmu::Attachments* WiimoteEmuGeneral::GetAttachments() const
{
  auto* const group = Wiimote::GetWiimoteGroup(GetPort(), WiimoteEmu::WiimoteGroup::Attachments);
  return static_cast<ControllerEmu::Attachments*>(group);
}

void WiimoteEmuGeneral::BuildGroups()
{
  using WiimoteEmu::WiimoteGroup;
  const auto group = [this](WiimoteGroup which) {
    return Wiimote::GetWiimoteGroup(GetPort(), which);
  };

  auto* const grid = new QGridLayout;

  // Three full-height columns.
  grid->addWidget(CreateGroupBox(tr("Buttons"), group(WiimoteGroup::Buttons)), 0, 0, -1, 1);
  grid->addWidget(CreateGroupBox(tr("D-Pad"), group(WiimoteGroup::DPad)), 0, 1, -1, 1);
  grid->addWidget(CreateGroupBox(tr("Hotkeys"), group(WiimoteGroup::Hotkeys)), 0, 2, -1, 1);

  // Fourth column, top: the extension box. Its form gets a selector row and a
  // "Configure Extension" row in front of the attachment's own settings.
  auto* const attachments_group = group(WiimoteGroup::Attachments);
  QGroupBox* const extension_box = CreateGroupBox(tr("Extension"), attachments_group);
  auto* const attachments = static_cast<ControllerEmu::Attachments*>(attachments_group);

  m_extension_combo = new QComboBox();
  // Shown while the selection comes from an input expression rather than a fixed value.
  m_extension_dynamic_marker = new QLabel(QString::fromUtf8("🎮"));

  auto* const selector_row = new QHBoxLayout;
  selector_row->addWidget(m_extension_combo);
  selector_row->addWidget(m_extension_dynamic_marker);
  selector_row->addWidget(CreateSettingAdvancedMappingButton(attachments->GetSelectionSetting()));
  m_extension_dynamic_marker->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Ignored);

  for (const auto& attachment : attachments->GetAttachmentList())
    m_extension_combo->addItem(tr(attachment->GetDisplayName().c_str()));

  extension_box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* const extension_form = static_cast<QFormLayout*>(extension_box->layout());
  extension_form->insertRow(0, selector_row);

  m_configure_extension = new QPushButton(tr("Configure Extension"));
  m_configure_extension->setDisabled(true);
  m_configure_extension->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  extension_form->insertRow(1, m_configure_extension);

  grid->addWidget(extension_box, 0, 3);

  // Fourth column, below: rumble and options.
  grid->addWidget(CreateGroupBox(tr("Rumble"), group(WiimoteGroup::Rumble)), 1, 3);
  grid->addWidget(CreateGroupBox(tr("Options"), group(WiimoteGroup::Options)), 2, 3);

  setLayout(grid);
}

void WiimoteEmuGeneral::OnAttachmentChanged(int index)
{
  // The Nunchuk is the only extension with its own motion tabs.
  GetParent()->ShowExtensionMotionTabs(index == WiimoteEmu::ExtensionNumber::NUNCHUK);
  m_extension_page->ChangeExtensionType(static_cast<u32>(index));
  m_configure_extension->setEnabled(index != WiimoteEmu::ExtensionNumber::NONE);
}

void WiimoteEmuGeneral::OnAttachmentSelected(int index)
{
  GetAttachments()->SetSelectedAttachment(static_cast<u32>(index));
  SyncWithConfig();
  SaveSettings();
}

void WiimoteEmuGeneral::SyncWithConfig()
{
  ControllerEmu::Attachments* const attachments = GetAttachments();
  m_extension_combo->setCurrentIndex(static_cast<int>(attachments->GetSelectedAttachment()));
  m_extension_dynamic_marker->setVisible(!attachments->GetSelectionSetting().IsSimpleValue());
}

void WiimoteEmuGeneral::SyncWithState()
{
  m_extension_combo->setCurrentIndex(static_cast<int>(GetAttachments()->GetSelectedAttachment()));
}

InputConfig* WiimoteEmuGeneral::GetConfig()
{
  return Wiimote::GetConfig();
}

void WiimoteEmuGeneral::LoadSettings()
{
  Wiimote::LoadConfig();
}

void WiimoteEmuGeneral::SaveSettings()
{
  InputConfig* const config = Wiimote::GetConfig();
  config->SaveConfig();
}
