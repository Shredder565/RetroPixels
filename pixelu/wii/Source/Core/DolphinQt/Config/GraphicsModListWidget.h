// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include <QWidget>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"

class GraphicsModWarningWidget;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QModelIndex;
class QPushButton;
class QVBoxLayout;

namespace Core
{
enum class State;
}

namespace UICommon
{
class GameFile;
}

// The Graphics Mods tab: the game's graphics mods in priority order, with
// enable boxes and details of the selected mod. Changes reach a running game
// at once and are written to disk when saved or on destruction.
class GraphicsModListWidget : public QWidget
{
  Q_OBJECT
public:
  explicit GraphicsModListWidget(const UICommon::GameFile& game);
  ~GraphicsModListWidget() override;

  void SaveToDisk();

  const GraphicsModGroupConfig& GetGraphicsModConfig() const;

signals:
  void OpenGraphicsSettings();

private:
  void Reload();
  void OnSelectionChanged();
  void OnCheckToggled(QListWidgetItem* item);
  void OnRowsMoved();
  void ShowDetails(const std::optional<std::string>& mod_path);
  void PushToRunningGame();
  void OpenModFolder();
  void TrackEmulationState(Core::State state);

  bool m_game_running = false;
  bool m_unsaved = false;

  QListWidget* m_list;

  QLabel* m_title;
  QVBoxLayout* m_details;

  QPushButton* m_open_folder;
  QPushButton* m_reload;
  GraphicsModWarningWidget* m_warning;

  std::string m_game_id;
  GraphicsModGroupConfig m_mod_group;
};
