// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/CheatWarningWidget.h"

#include <utility>

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/Settings.h"

CheatWarningWidget::CheatWarningWidget(std::string game_id, bool restart_required, QWidget* parent)
    : QWidget(parent), m_game_id(std::move(game_id)), m_needs_restart(restart_required)
{
  m_message = new QLabel();
  m_settings_button = new QPushButton(tr("Configure Dolphin"));
  m_settings_button->setHidden(true);

  auto* const row = new QHBoxLayout{this};
  row->addWidget(QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxWarning, m_message));
  row->addWidget(m_settings_button);
  row->setContentsMargins(0, 0, 0, 0);

  connect(m_settings_button, &QPushButton::clicked, this,
          &CheatWarningWidget::OpenCheatEnableSettings);

  Settings& settings = Settings::Instance();
  connect(&settings, &Settings::EnableCheatsChanged, this,
          [this] { Refresh(Core::IsRunning(Core::System::GetInstance())); });
  connect(&settings, &Settings::EmulationStateChanged, this, [this](Core::State state) {
    Refresh(state == Core::State::Running || state == Core::State::Paused);
  });

  Refresh(Core::IsRunning(Core::System::GetInstance()));
}

void CheatWarningWidget::Refresh(bool running)
{
  // "Cheats are off" wins over the restart note, and is the only case that
  // offers the settings button.
  const bool this_game_running =
      running && SConfig::GetInstance().GetGameID() == m_game_id && m_needs_restart;
  if (this_game_running)
    m_message->setText(tr("Changing cheats will only take effect when the game is restarted."));

  const bool cheats_off = !Settings::Instance().GetCheatsEnabled();
  if (cheats_off)
    m_message->setText(tr("Dolphin's cheat system is currently disabled."));

  setHidden(!this_game_running && !cheats_off);
  m_settings_button->setHidden(!cheats_off);
}
