// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "DolphinQt/GameList/GameListModel.h"

class QVBoxLayout;
class QListWidget;
class QDialogButtonBox;

namespace UICommon
{
class GameFile;
}

// Lets the NetPlay host pick one game from the main game list, shown by its NetPlay name.
class GameListDialog : public QDialog
{
  Q_OBJECT
public:
  explicit GameListDialog(const GameListModel& game_list_model, QWidget* parent);

  int exec() override;
  const UICommon::GameFile& GetSelectedGame() const;

private:
  void CreateWidgets();
  void ConnectWidgets();
  void PopulateGameList();

  const GameListModel& m_game_list_model;
  QVBoxLayout* m_main_layout = nullptr;
  QListWidget* m_game_list = nullptr;
  QDialogButtonBox* m_button_box = nullptr;
};
