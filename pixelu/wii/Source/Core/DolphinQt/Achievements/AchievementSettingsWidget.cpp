// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Achievements/AchievementSettingsWidget.h"

#include <initializer_list>
#include <string>

#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Core.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "UICommon/DiscordPresence.h"

#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"

namespace
{
ToolTipCheckBox* Option(const QString& label, const QString& description)
{
  auto* box = new ToolTipCheckBox(label);
  box->SetDescription(description);
  return box;
}

// Yes/No question; true for Yes.
bool Confirm(QWidget* parent, const QString& title, const QString& text)
{
  return ModalMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No,
                                   QMessageBox::NoButton, Qt::ApplicationModal) == QMessageBox::Yes;
}
}  // namespace

AchievementSettingsWidget::AchievementSettingsWidget(QWidget* parent) : QWidget(parent)
{
  BuildLayout();
  LoadSettings();
  ConnectSignals();

  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &AchievementSettingsWidget::LoadSettings);
}

void AchievementSettingsWidget::UpdateData(int login_failed_code)
{
  if (login_failed_code != RC_OK)
  {
    QString reason;
    switch (login_failed_code)
    {
    case RC_LOGIN_REQUIRED:
    case RC_EXPIRED_TOKEN:
      reason = tr("Logged Out - Credentials Expired");
      break;
    case RC_INVALID_CREDENTIALS:
      reason = tr("Login Failed - Invalid Username/Password");
      break;
    case RC_NO_RESPONSE:
      reason = tr("Login Failed - No Internet Connection");
      break;
    default:
      reason = tr("Login Failed - Server Error");
      break;
    }
    m_common_login_failed->setText(reason);
    m_common_login_failed->setVisible(true);
  }
  LoadSettings();
}

void AchievementSettingsWidget::BuildLayout()
{
  m_common_layout = new QVBoxLayout();

  // Account.
  m_common_integration_enabled_input = Option(
      tr("Enable RetroAchievements.org Integration"),
      tr("Enable integration with RetroAchievements for earning achievements and competing in "
         "leaderboards.<br><br>"
         "Must log in with a RetroAchievements account to use. "
         "Dolphin does not save your password locally and uses an API token to maintain login."));
  m_common_username_label = new QLabel(tr("Username"));
  m_common_username_input = new QLineEdit(QStringLiteral(""));
  // The password is only ever typed here; it is sent once to log in and never stored.
  m_common_password_label = new QLabel(tr("Password"));
  m_common_password_input = new QLineEdit(QStringLiteral(""));
  m_common_password_input->setEchoMode(QLineEdit::Password);
  // Only one of the two buttons is visible at a time, depending on the login state.
  m_common_login_button = new QPushButton(tr("Log In"));
  m_common_logout_button = new QPushButton(tr("Log Out"));
  // Red error line, shown after a failed attempt.
  m_common_login_failed = new QLabel(tr("Login Failed"));
  m_common_login_failed->setStyleSheet(QStringLiteral("QLabel { color : red; }"));
  m_common_login_failed->setVisible(false);

  // How achievements are unlocked.
  m_common_hardcore_enabled_input = Option(
      tr("Enable Hardcore Mode"),
      tr("Enable Hardcore Mode on RetroAchievements.<br><br>"
         "Hardcore Mode is intended to provide an experience as close to gaming on the original "
         "hardware as possible. RetroAchievements rankings are primarily oriented towards "
         "Hardcore points (Softcore points are tracked but not as heavily emphasized) and "
         "leaderboards require Hardcore Mode to be on.<br><br>"
         "To ensure this experience, the following features will be disabled, as they give "
         "emulator players an advantage over console players:"
         "<br>- Loading states"
         "<br>-- Saving states is allowed"
         "<br>- Emulator speeds below 100%"
         "<br>-- Frame advance is disabled"
         "<br>-- Turbo is allowed"
         "<br>- Cheats"
         "<br>- Memory patches"
         "<br>-- File patches are allowed"
         "<br>- Debug UI"
         "<br>- Freelook<br><br>"
         "<dolphin_emphasis>This cannot be turned on while a game is playing.</dolphin_emphasis>"
         "<br>Close your current game before enabling."
         "<br>Be aware that turning Hardcore Mode off while a game is running requires the game "
         "to be closed before re-enabling."));
  m_common_unofficial_enabled_input = Option(
      tr("Enable Unofficial Achievements"),
      tr("Enable unlocking unofficial achievements as well as official achievements.<br><br>"
         "Unofficial achievements may be optional or unfinished achievements that have not been "
         "deemed official by RetroAchievements and may be useful for testing or simply for "
         "fun.<br><br>"
         "Setting takes effect on next game load."));
  m_common_encore_enabled_input = Option(
      tr("Enable Encore Achievements"),
      tr("Enable unlocking achievements in Encore Mode.<br><br>"
         "Encore Mode re-enables achievements the player has already unlocked on the site so "
         "that the player will be notified if they meet the unlock conditions again, useful for "
         "custom speedrun criteria or simply for fun.<br><br>"
         "Setting takes effect on next game load."));
  m_common_spectator_enabled_input = Option(
      tr("Enable Spectator Mode"),
      tr("Enable unlocking achievements in Spectator Mode.<br><br>"
         "While in Spectator Mode, achievements and leaderboards will be processed and displayed "
         "on screen, but will not be submitted to the server.<br><br>"
         "If this is on at game launch, it will not be turned off until game close, because a "
         "RetroAchievements session will not be created.<br><br>"
         "If this is off at game launch, it can be toggled freely while the game is running."));

  // What is shown while playing.
  m_common_leaderboard_tracker_enabled_input =
      Option(tr("Show Leaderboard Tracker"),
             tr("Show the on-screen RetroAchievements leaderboard tracker.<br><br>"
                "This appears in the bottom-right corner while competing in a leaderboard."));
  m_common_challenge_indicators_enabled_input = Option(
      tr("Show Challenge Indicators"),
      tr("Show the on-screen RetroAchievements challenge indicators.<br><br>"
         "These appear as achievement badges in the bottom-right corner while tracking progress "
         "within a challenge."));
  m_common_discord_presence_enabled_input =
      Option(tr("Enable Discord Presence"),
             tr("Use RetroAchievements rich presence in your Discord status.<br><br>"
                "Show Current Game on Discord must be enabled."));
  m_common_progress_enabled_input = Option(
      tr("Enable Progress Notifications"),
      tr("Enable progress notifications on achievements.<br><br>"
         "Displays a brief popup message whenever the player makes progress on an achievement "
         "that tracks an accumulated value, such as 60 out of 120 stars."));

  // Account block, top to bottom.
  for (QWidget* account_part : std::initializer_list<QWidget*>{
           m_common_integration_enabled_input, m_common_username_label, m_common_username_input,
           m_common_password_label, m_common_password_input, m_common_login_button,
           m_common_logout_button, m_common_login_failed})
  {
    m_common_layout->addWidget(account_part);
  }

  // i18n: Settings that affect the functionality of unlocking achievements.
  m_common_layout->addWidget(new QLabel(tr("Function Settings")));
  for (ToolTipCheckBox* option :
       {m_common_hardcore_enabled_input, m_common_unofficial_enabled_input,
        m_common_encore_enabled_input, m_common_spectator_enabled_input})
  {
    m_common_layout->addWidget(option);
  }

  // i18n: Settings that affect how achievements are displayed while playing.
  m_common_layout->addWidget(new QLabel(tr("Display Settings")));
#ifdef USE_DISCORD_PRESENCE
  m_common_layout->addWidget(m_common_discord_presence_enabled_input);
#endif  // USE_DISCORD_PRESENCE
  for (ToolTipCheckBox* option :
       {m_common_leaderboard_tracker_enabled_input, m_common_challenge_indicators_enabled_input,
        m_common_progress_enabled_input})
  {
    m_common_layout->addWidget(option);
  }

  m_common_layout->setAlignment(Qt::AlignTop);
  setLayout(m_common_layout);
}

void AchievementSettingsWidget::ConnectSignals()
{
  connect(m_common_integration_enabled_input, &QCheckBox::toggled, this,
          &AchievementSettingsWidget::ToggleRAIntegration);
  connect(m_common_login_button, &QPushButton::clicked, this, &AchievementSettingsWidget::Login);
  connect(m_common_logout_button, &QPushButton::clicked, this, &AchievementSettingsWidget::Logout);
  connect(m_common_hardcore_enabled_input, &QCheckBox::toggled, this,
          &AchievementSettingsWidget::ToggleHardcore);
  connect(m_common_spectator_enabled_input, &QCheckBox::toggled, this,
          &AchievementSettingsWidget::ToggleSpectator);
  connect(m_common_discord_presence_enabled_input, &QCheckBox::toggled, this,
          &AchievementSettingsWidget::ToggleDiscordPresence);

  // These only need saving.
  for (ToolTipCheckBox* option :
       {m_common_unofficial_enabled_input, m_common_encore_enabled_input,
        m_common_leaderboard_tracker_enabled_input, m_common_challenge_indicators_enabled_input,
        m_common_progress_enabled_input})
  {
    connect(option, &QCheckBox::toggled, this, &AchievementSettingsWidget::SaveSettings);
  }
}

void AchievementSettingsWidget::LoadSettings()
{
  Core::System& system = Core::System::GetInstance();

  const bool enabled = Config::Get(Config::RA_ENABLED);
  const bool hardcore_enabled = Config::Get(Config::RA_HARDCORE_ENABLED);
  const bool logged_out = Config::Get(Config::RA_API_TOKEN).empty();
  const std::string username = Config::Get(Config::RA_USERNAME);

  SignalBlocking(m_common_integration_enabled_input)->setChecked(enabled);

  // Account fields: the name stays visible, the password and Log In only while logged out.
  // Logging in needs emulation to be stopped.
  SignalBlocking(m_common_username_label)->setEnabled(enabled);
  if (!username.empty())
    SignalBlocking(m_common_username_input)->setText(QString::fromStdString(username));
  const bool name_editable = enabled && logged_out;
  SignalBlocking(m_common_username_input)->setEnabled(name_editable);

  for (QWidget* const password_part :
       std::initializer_list<QWidget*>{m_common_password_label, m_common_password_input})
  {
    SignalBlocking(password_part)->setVisible(logged_out);
    SignalBlocking(password_part)->setEnabled(enabled);
  }

  SignalBlocking(m_common_login_button)->setVisible(logged_out);
  SignalBlocking(m_common_login_button)->setEnabled(enabled && Core::IsUninitialized(system));
  SignalBlocking(m_common_login_button)
      ->setText(!enabled || Core::IsUninitialized(system) ?
                    tr("Log In") :
                    tr("To log in, stop the current emulation."));
  SignalBlocking(m_common_logout_button)->setVisible(!logged_out);
  SignalBlocking(m_common_logout_button)->setEnabled(enabled);

  const auto show_option = [](ToolTipCheckBox* option, const Config::Info<bool>& setting,
                              bool usable) {
    SignalBlocking(option)->setChecked(Config::Get(setting));
    SignalBlocking(option)->setEnabled(usable);
  };

  // Hardcore can always be turned off, but only turned on with nothing running or replaying.
  show_option(m_common_hardcore_enabled_input, Config::RA_HARDCORE_ENABLED,
              enabled && (hardcore_enabled ||
                          (Core::IsUninitialized(system) && !system.GetMovie().IsPlayingInput())));
  show_option(m_common_unofficial_enabled_input, Config::RA_UNOFFICIAL_ENABLED, enabled);
  show_option(m_common_encore_enabled_input, Config::RA_ENCORE_ENABLED, enabled);
  show_option(m_common_spectator_enabled_input, Config::RA_SPECTATOR_ENABLED, enabled);
  show_option(m_common_leaderboard_tracker_enabled_input, Config::RA_LEADERBOARD_TRACKER_ENABLED,
              enabled);
  show_option(m_common_challenge_indicators_enabled_input, Config::RA_CHALLENGE_INDICATORS_ENABLED,
              enabled);
  show_option(m_common_discord_presence_enabled_input, Config::RA_DISCORD_PRESENCE_ENABLED,
              enabled && Config::Get(Config::MAIN_USE_DISCORD_PRESENCE));
  show_option(m_common_progress_enabled_input, Config::RA_PROGRESS_ENABLED, enabled);
}

void AchievementSettingsWidget::SaveSettings()
{
  // One change notification for the lot.
  Config::ConfigChangeCallbackGuard batch;

  const std::pair<const Config::Info<bool>&, ToolTipCheckBox*> options[] = {
      {Config::RA_ENABLED, m_common_integration_enabled_input},
      {Config::RA_HARDCORE_ENABLED, m_common_hardcore_enabled_input},
      {Config::RA_UNOFFICIAL_ENABLED, m_common_unofficial_enabled_input},
      {Config::RA_ENCORE_ENABLED, m_common_encore_enabled_input},
      {Config::RA_SPECTATOR_ENABLED, m_common_spectator_enabled_input},
      {Config::RA_LEADERBOARD_TRACKER_ENABLED, m_common_leaderboard_tracker_enabled_input},
      {Config::RA_CHALLENGE_INDICATORS_ENABLED, m_common_challenge_indicators_enabled_input},
      {Config::RA_DISCORD_PRESENCE_ENABLED, m_common_discord_presence_enabled_input},
      {Config::RA_PROGRESS_ENABLED, m_common_progress_enabled_input},
  };
  for (const auto& [setting, option] : options)
    Config::SetBaseOrCurrent(setting, option->isChecked());
  Config::Save();
}

void AchievementSettingsWidget::ToggleRAIntegration()
{
  SaveSettings();

  auto& manager = AchievementManager::GetInstance();
  if (Config::Get(Config::RA_ENABLED))
    manager.Init(reinterpret_cast<void*>(winId()));
  else
    manager.Shutdown();
}

void AchievementSettingsWidget::Login()
{
  m_common_login_failed->setVisible(false);
  Config::SetBaseOrCurrent(Config::RA_USERNAME, m_common_username_input->text().toStdString());
  AchievementManager::GetInstance().Login(m_common_password_input->text().toStdString());
  // The password is never kept.
  m_common_password_input->setText(QString());
  SaveSettings();
}

void AchievementSettingsWidget::Logout()
{
  if (!Confirm(this, tr("Confirm Logout"),
               tr("Are you sure you want to log out of RetroAchievements?")))
  {
    return;
  }
  AchievementManager::GetInstance().Logout();
  SaveSettings();
}

void AchievementSettingsWidget::ToggleHardcore()
{
  // Turning hardcore off (the setting is still on at this point) has to be confirmed.
  if (Config::Get(Config::RA_HARDCORE_ENABLED) &&
      !Confirm(this, tr("Confirm Hardcore Off"),
               tr("Are you sure you want to turn hardcore mode off?")))
  {
    SignalBlocking(m_common_hardcore_enabled_input)->setChecked(true);
    return;
  }
  SaveSettings();
}

void AchievementSettingsWidget::ToggleSpectator()
{
  SaveSettings();
  AchievementManager::GetInstance().SetSpectatorMode();
}

void AchievementSettingsWidget::ToggleDiscordPresence()
{
  SaveSettings();
  Discord::UpdateDiscordPresence();
}

#endif  // USE_RETRO_ACHIEVEMENTS
