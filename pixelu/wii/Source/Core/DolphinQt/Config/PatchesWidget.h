// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include <QWidget>

#include "Common/CommonTypes.h"
#include "Core/PatchEngine.h"

#ifdef USE_RETRO_ACHIEVEMENTS
class HardcoreWarningWidget;
#endif  // USE_RETRO_ACHIEVEMENTS
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace UICommon
{
class GameFile;
}

// The Patches tab of the game properties: the game's OnFrame patches, which
// can be switched on and off, added, cloned, edited and removed.
class PatchesWidget : public QWidget
{
  Q_OBJECT
public:
  explicit PatchesWidget(const UICommon::GameFile& game);

#ifdef USE_RETRO_ACHIEVEMENTS
signals:
  void OpenAchievementSettings();
#endif  // USE_RETRO_ACHIEVEMENTS

private:
  void RebuildList();
  void RefreshButtons();
  void WritePatches();
  bool RunEditor(PatchEngine::Patch& patch);

  void OnCheckToggled(QListWidgetItem* item);
  void AddPatch();
  void EditSelected();
  void RemoveSelected();

#ifdef USE_RETRO_ACHIEVEMENTS
  HardcoreWarningWidget* m_hardcore_warning;
#endif  // USE_RETRO_ACHIEVEMENTS
  QListWidget* m_list;
  QPushButton* m_add_button;
  QPushButton* m_edit_button;
  QPushButton* m_remove_button;

  std::vector<PatchEngine::Patch> m_patches;
  std::string m_game_id;
  u16 m_game_revision;
};
