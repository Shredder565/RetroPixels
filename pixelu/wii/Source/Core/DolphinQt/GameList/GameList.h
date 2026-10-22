// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include <QStackedWidget>

#include "DolphinQt/GameList/GameListModel.h"

class QAbstractItemView;
class QLabel;
class QListView;
class QMenu;
class QSortFilterProxyModel;
class QTableView;

namespace UICommon
{
class GameFile;
}

// The main window's game browser. It shows the games either as a table (the "list") or as a
// grid of covers/banners, and a hint label when there are no games at all.
class GameList final : public QStackedWidget
{
  Q_OBJECT

public:
  explicit GameList(QWidget* parent = nullptr);
  ~GameList() override;

  std::shared_ptr<const UICommon::GameFile> GetSelectedGame() const;
  QList<std::shared_ptr<const UICommon::GameFile>> GetSelectedGames() const;
  bool HasMultipleSelected() const;
  std::shared_ptr<const UICommon::GameFile> FindGame(const std::string& path) const;
  std::shared_ptr<const UICommon::GameFile> FindSecondDisc(const UICommon::GameFile& game) const;
  std::string GetNetPlayName(const UICommon::GameFile& game) const;

  void SetListView() { SetPreferredView(true); }
  void SetGridView() { SetPreferredView(false); }
  void SetViewColumn(int col, bool view);
  void SetSearchTerm(const QString& term);

  void OnColumnVisibilityToggled(const QString& row, bool visible);
  void OnGameListVisibilityChanged();

  void UpdateGameCount() const;

  void resizeEvent(QResizeEvent* event) override;

  void PurgeCache();

  const GameListModel& GetGameListModel() const { return m_model; }

signals:
  void GameSelected();
  void GameCountUpdated(int total_games, int visible_games) const;
  void OnStartWithRiivolution(const UICommon::GameFile& game);
  void NetPlayHost(const UICommon::GameFile& game);
  void SelectionChanged(const std::shared_ptr<const UICommon::GameFile>& game_file);
  void OpenGeneralSettings();
  void OpenGraphicsSettings();
#ifdef USE_RETRO_ACHIEVEMENTS
  void OpenAchievementSettings();
#endif  // USE_RETRO_ACHIEVEMENTS

protected:
  void keyPressEvent(QKeyEvent* event) override;

private:
  // Building the three pages.
  void BuildTable();
  void BuildGrid();
  void BuildPlaceholder();

  // Context menus and what their entries do.
  void ShowColumnsMenu(const QPoint& pos);
  void ShowGameMenu(const QPoint& pos);
  void AddMultiSelectionEntries(QMenu* menu);
  void AddSingleGameEntries(QMenu* menu, const std::shared_ptr<const UICommon::GameFile>& game);
  void AddTagEntries(QMenu* menu, const std::string& path);
  void OpenProperties();
  void OpenWiki();
  void StartWithRiivolution();
  void SetDefaultISO();
  void ConvertSelection();
  void ChangeDisc();
  void InstallWAD();
  void UninstallWAD();
  void OpenWiiSaveFolder();
  void ExportWiiSaves();
  void OpenGCSaveFolder();
  void OpenContainingFolder();
  void DeleteSelection();
#ifdef _WIN32
  bool CreateDesktopShortcut();
#endif
  void PromptNewTag();
  void PromptRemoveTag();

  // Table layout.
  void ApplyColumnVisibility();
  void OnHeaderSectionResized(int logical_index);
  void FitColumnsToWidth();

  // Views and zoom.
  void SetPreferredView(bool list);
  void ShowFittingPage();
  QAbstractItemView* CurrentView() const;
  QSortFilterProxyModel* CurrentProxy() const;
  void InvalidateProxies();
  void Zoom(bool in);

  GameListModel m_model;
  QSortFilterProxyModel* m_list_proxy = nullptr;
  QSortFilterProxyModel* m_grid_proxy = nullptr;

  QTableView* m_list = nullptr;
  QListView* m_grid = nullptr;
  QLabel* m_empty = nullptr;
  bool m_prefer_list = true;
  bool m_fitting_columns = false;
};
