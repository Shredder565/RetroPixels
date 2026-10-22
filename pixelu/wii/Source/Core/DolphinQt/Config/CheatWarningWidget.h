// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

#include <string>

class QLabel;
class QPushButton;

// The strip above a cheat list that says why the codes will not apply right
// now (cheats are off, or the running game needs a restart).
class CheatWarningWidget : public QWidget
{
  Q_OBJECT
public:
  explicit CheatWarningWidget(std::string game_id, bool restart_required, QWidget* parent);

signals:
  void OpenCheatEnableSettings();

private:
  void Refresh(bool running);

  QLabel* m_message;
  QPushButton* m_settings_button;
  const std::string m_game_id;
  const bool m_needs_restart;
};
