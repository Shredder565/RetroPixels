// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QWidget>

class QGroupBox;
class QVBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class ToolTipCheckBox;

// The Settings tab: turning RetroAchievements on, logging in and out, and the gameplay and
// display options.
class AchievementSettingsWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit AchievementSettingsWidget(QWidget* parent);
  // Shows why a login failed (unless the code is RC_OK) and reloads the settings.
  void UpdateData(int login_failed_code);

private:
  void BuildLayout();
  void ConnectSignals();

  void LoadSettings();
  void SaveSettings();

  void ToggleRAIntegration();
  void Login();
  void Logout();
  void ToggleHardcore();
  void ToggleSpectator();
  void ToggleDiscordPresence();

  QGroupBox* m_common_box;
  QVBoxLayout* m_common_layout;
  ToolTipCheckBox* m_common_integration_enabled_input;
  QLabel* m_common_login_failed;
  QLabel* m_common_username_label;
  QLineEdit* m_common_username_input;
  QLabel* m_common_password_label;
  QLineEdit* m_common_password_input;
  QPushButton* m_common_login_button;
  QPushButton* m_common_logout_button;
  ToolTipCheckBox* m_common_hardcore_enabled_input;
  ToolTipCheckBox* m_common_unofficial_enabled_input;
  ToolTipCheckBox* m_common_encore_enabled_input;
  ToolTipCheckBox* m_common_spectator_enabled_input;
  ToolTipCheckBox* m_common_leaderboard_tracker_enabled_input;
  ToolTipCheckBox* m_common_challenge_indicators_enabled_input;
  ToolTipCheckBox* m_common_discord_presence_enabled_input;
  ToolTipCheckBox* m_common_progress_enabled_input;
};

#endif  // USE_RETRO_ACHIEVEMENTS
