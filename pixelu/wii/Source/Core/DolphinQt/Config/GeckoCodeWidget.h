// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include <QWidget>

#include "Common/CommonTypes.h"

class CheatCodeEditor;
class CheatWarningWidget;
#ifdef USE_RETRO_ACHIEVEMENTS
class HardcoreWarningWidget;
#endif  // USE_RETRO_ACHIEVEMENTS
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextEdit;

namespace Gecko
{
class GeckoCode;
}

// The Gecko tab: a game's Gecko codes with their details, editing, sorting,
// downloading from the code server and saving to the user's game INI.
class GeckoCodeWidget : public QWidget
{
  Q_OBJECT
public:
  explicit GeckoCodeWidget(std::string game_id, std::string gametdb_id, u16 game_revision,
                           bool restart_required = true);
  ~GeckoCodeWidget() override;

  void ChangeGame(std::string game_id, std::string gametdb_id, u16 game_revision);

signals:
  void OpenGeneralSettings();
#ifdef USE_RETRO_ACHIEVEMENTS
  void OpenAchievementSettings();
#endif  // USE_RETRO_ACHIEVEMENTS

private:
  void BuildLayout();
  void WireUp();

  void ReadCodes();
  void WriteCodes();
  void RebuildList();
  void ActivateIfRunning();
  bool AllEnabled() const;
  void RefreshToggleAllButton();

  void ShowSelectedDetails();
  void OnCheckToggled(QListWidgetItem* item);
  void OnRowsMoved();
  void ShowSortMenu();
  void SortByName();
  void PutEnabledFirst();
  void PutDisabledFirst();

  void NewCode();
  void EditCurrent();
  void RemoveCurrent();
  void FetchFromServer();
  void ToggleAll();

  std::string m_game_id;
  std::string m_gametdb_id;
  u16 m_game_revision;

  CheatWarningWidget* m_cheat_warning;
#ifdef USE_RETRO_ACHIEVEMENTS
  HardcoreWarningWidget* m_hardcore_warning;
#endif  // USE_RETRO_ACHIEVEMENTS
  QListWidget* m_list;
  QLabel* m_name;
  QLabel* m_creator;
  QTextEdit* m_notes;
  QTextEdit* m_lines;
  QPushButton* m_add_button;
  QPushButton* m_edit_button;
  QPushButton* m_remove_button;
  QPushButton* m_download_button;
  QPushButton* m_toggle_all_button;
  CheatCodeEditor* m_editor;
  std::vector<Gecko::GeckoCode> m_codes;
  bool m_restart_required;
};
