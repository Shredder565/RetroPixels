// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include <QWidget>

#include "Common/CommonTypes.h"

namespace ActionReplay
{
struct ARCode;
}

class CheatCodeEditor;
class CheatWarningWidget;
#ifdef USE_RETRO_ACHIEVEMENTS
class HardcoreWarningWidget;
#endif  // USE_RETRO_ACHIEVEMENTS
class QListWidget;
class QListWidgetItem;
class QPushButton;

// The Action Replay tab: lists a game's AR codes, lets them be switched on
// and off, edited and reordered, and keeps the user's game INI up to date.
class ARCodeWidget : public QWidget
{
  Q_OBJECT
public:
  explicit ARCodeWidget(std::string game_id, u16 game_revision, bool restart_required = true);
  ~ARCodeWidget() override;

  void ChangeGame(std::string game_id, u16 game_revision);
  void AddCode(ActionReplay::ARCode code);

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
  void ApplyIfRunning();
  bool AllEnabled() const;
  void RefreshToggleAllButton();

  void OnCheckToggled(QListWidgetItem* item);
  void OnSelectionChanged();
  void OnRowsMoved();
  void ShowSortMenu();
  void SortByName();
  void PutEnabledFirst();
  void PutDisabledFirst();

  void NewCode();
  void EditSelected();
  void RemoveSelected();
  void ToggleAll();

  std::string m_game_id;
  u16 m_game_revision;

  CheatWarningWidget* m_cheat_warning;
#ifdef USE_RETRO_ACHIEVEMENTS
  HardcoreWarningWidget* m_hardcore_warning;
#endif  // USE_RETRO_ACHIEVEMENTS
  QListWidget* m_list;
  QPushButton* m_add_button;
  QPushButton* m_edit_button;
  QPushButton* m_remove_button;
  QPushButton* m_toggle_all_button;

  CheatCodeEditor* m_editor;

  std::vector<ActionReplay::ARCode> m_codes;
  bool m_restart_required;
};
