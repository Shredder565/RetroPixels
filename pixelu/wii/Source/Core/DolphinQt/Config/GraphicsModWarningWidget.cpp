// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/GraphicsModWarningWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>

#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/Settings.h"

GraphicsModWarningWidget::GraphicsModWarningWidget(QWidget* parent) : QWidget(parent)
{
  m_message = new QLabel();
  m_settings_button = new QPushButton(tr("Configure Dolphin"));
  m_settings_button->setHidden(true);

  auto* const row = new QHBoxLayout{this};
  row->addWidget(QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxWarning, m_message));
  row->addWidget(m_settings_button);
  row->setContentsMargins(0, 0, 0, 0);

  connect(m_settings_button, &QPushButton::clicked, this,
          &GraphicsModWarningWidget::GraphicsModEnableSettings);
  connect(&Settings::Instance(), &Settings::EnableGfxModsChanged, this,
          &GraphicsModWarningWidget::Refresh);

  Refresh();
}

void GraphicsModWarningWidget::Refresh()
{
  const bool mods_off = !Settings::Instance().GetGraphicModsEnabled();
  if (mods_off)
    m_message->setText(tr("Graphics mods are currently disabled."));
  setHidden(!mods_off);
  m_settings_button->setHidden(!mods_off);
}
