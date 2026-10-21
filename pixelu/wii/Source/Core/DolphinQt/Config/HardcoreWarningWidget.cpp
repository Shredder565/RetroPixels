// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Config/HardcoreWarningWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

#include "Core/AchievementManager.h"

#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/Settings.h"

HardcoreWarningWidget::HardcoreWarningWidget(QWidget* parent) : QWidget(parent)
{
  auto* const message = new QLabel(tr("Only approved codes will be applied in hardcore mode."));
  QWidget* const warning = QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxWarning, message);
  m_settings_button = new QPushButton(tr("Achievement Settings"));

  auto* const row = new QHBoxLayout{this};
  row->addWidget(warning, 1);
  row->addWidget(m_settings_button);
  row->setContentsMargins(0, 0, 0, 0);

  connect(m_settings_button, &QPushButton::clicked, this,
          &HardcoreWarningWidget::OpenAchievementSettings);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this] { Refresh(); });

  Refresh();
}

void HardcoreWarningWidget::Refresh()
{
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  setHidden(!hardcore);
}
#endif  // USE_RETRO_ACHIEVEMENTS
