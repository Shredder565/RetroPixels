// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef _WIN32
// The desktop shortcut code needs the shell headers. They #define names (DeleteFile, interface)
// that clash with identifiers further down, so they come first and those macros are dropped.
#include <QCoreApplication>
#include <shlobj.h>
#include <wil/com.h>
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef interface
#undef interface
#endif
#endif

#include "DolphinQt/GameList/GameList.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QDesktopServices>
#include <QDir>
#include <QErrorMessage>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMap>
#include <QMenu>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QUrl>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/WiiSave.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "DiscIO/Enums.h"

#include "DolphinQt/Config/PropertiesDialog.h"
#include "DolphinQt/ConvertDialog.h"
#include "DolphinQt/GameList/GridProxyModel.h"
#include "DolphinQt/GameList/ListProxyModel.h"
#include "DolphinQt/MenuBar.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/DoubleClickEventFilter.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonAutodismissibleMenu.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/WiiUpdate.h"

#include "UICommon/GameFile.h"

namespace
{
using Column = GameListModel::Column;

constexpr int ColumnIndex(Column column)
{
  return static_cast<int>(column);
}

constexpr int COLUMN_COUNT = ColumnIndex(Column::Count);

// The setting that shows or hides each table column, in column order.
const Config::Info<bool>& ColumnSetting(Column column)
{
  switch (column)
  {
  case Column::Platform:
    return Config::MAIN_GAMELIST_COLUMN_PLATFORM;
  case Column::Banner:
    return Config::MAIN_GAMELIST_COLUMN_BANNER;
  case Column::Title:
    return Config::MAIN_GAMELIST_COLUMN_TITLE;
  case Column::Description:
    return Config::MAIN_GAMELIST_COLUMN_DESCRIPTION;
  case Column::Maker:
    return Config::MAIN_GAMELIST_COLUMN_MAKER;
  case Column::ID:
    return Config::MAIN_GAMELIST_COLUMN_GAME_ID;
  case Column::Country:
    return Config::MAIN_GAMELIST_COLUMN_REGION;
  case Column::Size:
    return Config::MAIN_GAMELIST_COLUMN_FILE_SIZE;
  case Column::FileName:
    return Config::MAIN_GAMELIST_COLUMN_FILE_NAME;
  case Column::FilePath:
    return Config::MAIN_GAMELIST_COLUMN_FILE_PATH;
  case Column::FileFormat:
    return Config::MAIN_GAMELIST_COLUMN_FILE_FORMAT;
  case Column::BlockSize:
    return Config::MAIN_GAMELIST_COLUMN_BLOCK_SIZE;
  case Column::Compression:
    return Config::MAIN_GAMELIST_COLUMN_COMPRESSION;
  case Column::TimePlayed:
    return Config::MAIN_GAMELIST_COLUMN_TIME_PLAYED;
  case Column::Tags:
  default:
    return Config::MAIN_GAMELIST_COLUMN_TAGS;
  }
}

// Icon and fixed-format columns keep their width; the text columns can be resized and share
// whatever room is left (see GameList::FitColumnsToWidth).
QHeaderView::ResizeMode ColumnResizeMode(Column column)
{
  switch (column)
  {
  case Column::Title:
  case Column::Description:
  case Column::Maker:
  case Column::FileName:
  case Column::FilePath:
  case Column::TimePlayed:
  case Column::Tags:
    return QHeaderView::Interactive;
  default:
    return QHeaderView::Fixed;
  }
}

const QString HEADER_STATE_KEY = QStringLiteral("tableheader/state");
const QString GRID_SCALE_KEY = QStringLiteral("gridview/scale");

// Table view tweaks: Home and End jump to the first and last game (QTableView would move to the
// first or last column unless Ctrl is held), and only the left button double-click starts a game.
class GameTableView final : public QTableView
{
public:
  using QTableView::QTableView;

protected:
  QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override
  {
    const bool jump = action == MoveHome || action == MoveEnd;
    return QTableView::moveCursor(action, jump ? (modifiers | Qt::ControlModifier) : modifiers);
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton)
      QTableView::mouseDoubleClickEvent(event);
  }
};

class GameGridView final : public QListView
{
public:
  using QListView::QListView;

protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton)
      QListView::mouseDoubleClickEvent(event);
  }
};

QSortFilterProxyModel* SetUpProxy(QSortFilterProxyModel* proxy, GameListModel* source)
{
  proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
  proxy->setSortRole(GameListModel::SORT_ROLE);
  proxy->setSourceModel(source);
  return proxy;
}

void ShowResultBox(QWidget* parent, bool success, const QString& success_text,
                   const QString& failure_text)
{
  ModalMessageBox box(parent);
  box.setIcon(success ? QMessageBox::Information : QMessageBox::Critical);
  box.setWindowTitle(success ? GameList::tr("Success") : GameList::tr("Failure"));
  box.setText(success ? success_text : failure_text);
  box.exec();
}
}  // namespace

GameList::GameList(QWidget* parent) : QStackedWidget(parent), m_model(this)
{
  m_list_proxy = SetUpProxy(new ListProxyModel(this), &m_model);
  m_grid_proxy = SetUpProxy(new GridProxyModel(this), &m_model);

  BuildTable();
  BuildGrid();
  BuildPlaceholder();

  // The grid has no header of its own; it follows the table's sort column and order.
  m_grid_proxy->sort(m_list_proxy->sortColumn(), m_list_proxy->sortOrder());
  connect(m_list->horizontalHeader(), &QHeaderView::sortIndicatorChanged, m_grid_proxy,
          &GridProxyModel::sort);

  const QSettings& stored = Settings::GetQSettings();
  if (stored.contains(GRID_SCALE_KEY))
    m_model.SetScale(stored.value(GRID_SCALE_KEY).toFloat());

  connect(m_list, &QTableView::doubleClicked, this, &GameList::GameSelected);
  connect(m_grid, &QListView::doubleClicked, this, &GameList::GameSelected);
  // Rows coming and going may switch between the placeholder and a view, and change the count.
  const auto row_signals = {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved};
  for (const auto row_signal : row_signals)
    connect(&m_model, row_signal, this, &GameList::ShowFittingPage);
  for (const auto row_signal : row_signals)
    connect(&m_model, row_signal, this, &GameList::UpdateGameCount);

  addWidget(m_list);
  addWidget(m_grid);
  addWidget(m_empty);
  m_prefer_list = Settings::Instance().GetPreferredView();
  ShowFittingPage();

  for (const bool in : {true, false})
  {
    auto* const shortcut = new QShortcut(in ? QKeySequence::ZoomIn : QKeySequence::ZoomOut, this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, [this, in] { Zoom(in); });
  }

  // Ctrl and the "=/+" key means "Ctrl +" to most people, but Qt only treats Ctrl+Shift+= that
  // way. Accept Ctrl+= for zooming in too, and Ctrl+Shift+- (Ctrl+_) for zooming out, so both
  // directions work with or without Shift.
  const auto* const zoom_in_plain = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), this);
  connect(zoom_in_plain, &QShortcut::activated, this, [this] { Zoom(true); });
  const auto* const zoom_out_shift =
      new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Underscore), this);
  connect(zoom_out_shift, &QShortcut::activated, this, [this] { Zoom(false); });

  connect(&Settings::Instance(), &Settings::MetadataRefreshCompleted, this,
          [this] { m_grid_proxy->invalidate(); });
}

GameList::~GameList()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(HEADER_STATE_KEY, m_list->horizontalHeader()->saveState());
  stored.setValue(GRID_SCALE_KEY, m_model.GetScale());
}

void GameList::PurgeCache()
{
  m_model.PurgeCache();
}

void GameList::BuildTable()
{
  m_list = new GameTableView(this);
  m_list->setModel(m_list_proxy);
  // Whole rows are selected, several at a time; the context menu is our own.
  m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  // Plain striped rows on one line each, without grid lines.
  m_list->setWordWrap(false);
  m_list->setShowGrid(false);
  m_list->setAlternatingRowColors(true);
  // Tab moves focus out of the table instead of between cells.
  m_list->setTabKeyNavigation(false);
  m_list->setSortingEnabled(true);
  m_list->setCurrentIndex(QModelIndex());
  // 32 pixel banners plus a pixel of room above and below.
  m_list->verticalHeader()->setDefaultSectionSize(34);

  QHeaderView* const header = m_list->horizontalHeader();
  const QSettings& stored = Settings::GetQSettings();
  header->restoreState(stored.value(HEADER_STATE_KEY).toByteArray());

  header->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(header, &QWidget::customContextMenuRequested, this, &GameList::ShowColumnsMenu);
  connect(header, &QHeaderView::sortIndicatorChanged, this, &GameList::FitColumnsToWidth);
  connect(header, &QHeaderView::sectionCountChanged, this, &GameList::FitColumnsToWidth);
  connect(header, &QHeaderView::sectionMoved, this, &GameList::FitColumnsToWidth);
  connect(header, &QHeaderView::sectionResized, this,
          [this](int logical_index, int, int) { OnHeaderSectionResized(logical_index); });

  // Without a saved header the list starts sorted by title.
  if (!stored.contains(HEADER_STATE_KEY))
    m_list->sortByColumn(ColumnIndex(Column::Title), Qt::AscendingOrder);

  for (int column = 0; column < COLUMN_COUNT; ++column)
    header->setSectionResizeMode(column, ColumnResizeMode(static_cast<Column>(column)));

  // Cells are padded by 3 pixels on each side: banners are 96 pixels wide, the platform and
  // region icons 32.
  const std::array<std::pair<Column, int>, 5> fixed_widths{{
      {Column::Banner, 102},
      {Column::Platform, 38},
      {Column::Country, 38},
      {Column::Size, 85},
      {Column::ID, 70},
  }};
  for (const auto& [column, width] : fixed_widths)
    m_list->setColumnWidth(ColumnIndex(column), width);

  // The default minimum section size differs between platforms.
  header->setMinimumSectionSize(38);

  ApplyColumnVisibility();

  m_list->verticalHeader()->hide();
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list->setFrameStyle(QFrame::NoFrame);

  header->setSectionsMovable(true);
  header->setHighlightSections(false);

  // Qt quirk: if the first thing the user does is click the empty area below the games and then
  // press a navigation key, Qt selects the first game instead of navigating. Selecting and
  // deselecting a row once avoids that (as long as there were games when the view was built).
  m_list->selectRow(0);
  m_list->clearSelection();

  connect(m_list, &QTableView::customContextMenuRequested, this, &GameList::ShowGameMenu);
  connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged,
          [this](const QItemSelection&, const QItemSelection&) {
            emit SelectionChanged(GetSelectedGame());
          });
}

void GameList::ApplyColumnVisibility()
{
  for (int column = 0; column < COLUMN_COUNT; ++column)
  {
    const bool shown = Config::Get(ColumnSetting(static_cast<Column>(column)));
    m_list->setColumnHidden(column, !shown);
  }
}

void GameList::BuildPlaceholder()
{
  const QString refreshing_text = tr("Refreshing...");
  const QString no_games_text = tr("Dolphin could not find any GameCube/Wii ISOs or WADs.\n"
                                   "Double-click here to set a games directory...");

  m_empty = new QLabel(this);
  m_empty->setText(refreshing_text);
  m_empty->setEnabled(false);
  m_empty->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

  auto* const double_click = new DoubleClickEventFilter{m_empty};
  m_empty->installEventFilter(double_click);
  connect(double_click, &DoubleClickEventFilter::doubleClicked, [this] {
    const QString dir =
        DolphinFileDialog::getExistingDirectory(this, tr("Select a Directory"), QDir::currentPath());
    if (!dir.isEmpty())
      Settings::Instance().AddPath(dir);
  });

  QSizePolicy policy = m_empty->sizePolicy();
  policy.setRetainSizeWhenHidden(true);
  m_empty->setSizePolicy(policy);

  connect(&Settings::Instance(), &Settings::GameListRefreshRequested, this,
          [this, refreshing_text] {
            m_empty->setText(refreshing_text);
            m_empty->setEnabled(false);
          });
  connect(&Settings::Instance(), &Settings::GameListRefreshCompleted, this,
          [this, no_games_text] {
            m_empty->setText(no_games_text);
            m_empty->setEnabled(true);
          });
}

void GameList::resizeEvent(QResizeEvent*)
{
  FitColumnsToWidth();
}

void GameList::BuildGrid()
{
  m_grid = new GameGridView(this);
  m_grid->setModel(m_grid_proxy);
  m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
  // Selecting whole rows keeps selectedRows() working the same way as in the table, and stops
  // Ctrl+A from selecting every column of a game separately.
  m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_grid->setViewMode(QListView::IconMode);
  m_grid->setResizeMode(QListView::Adjust);
  m_grid->setWordWrap(true);
  m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
  m_grid->setFrameStyle(QFrame::NoFrame);

  // The same navigation quirk as in BuildTable().
  m_grid->setCurrentIndex(m_grid->indexAt(QPoint(0, 0)));
  m_grid->clearSelection();

  connect(m_grid, &QTableView::customContextMenuRequested, this, &GameList::ShowGameMenu);
  connect(m_grid->selectionModel(), &QItemSelectionModel::selectionChanged,
          [this](const QItemSelection&, const QItemSelection&) {
            emit SelectionChanged(GetSelectedGame());
          });
}

void GameList::ShowColumnsMenu(const QPoint& pos)
{
  // The header offers the same column toggles as View > List Columns.
  const MenuBar* const menu_bar = MenuBar::GetMenuBar();
  QMenu* const columns = menu_bar ? menu_bar->GetListColumnsMenu() : nullptr;
  if (!columns)
    return;

  const auto* const origin = qobject_cast<QWidget*>(sender());
  columns->exec(origin ? origin->mapToGlobal(pos) : pos);
}

void GameList::ShowGameMenu(const QPoint&)
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  auto* const menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  if (HasMultipleSelected())
    AddMultiSelectionEntries(menu);
  else
    AddSingleGameEntries(menu, GetSelectedGame());

  menu->exec(QCursor::pos());
}

void GameList::AddMultiSelectionEntries(QMenu* menu)
{
  const auto games = GetSelectedGames();

  const bool all_convertible =
      std::ranges::all_of(games, [](const auto& game) { return game->ShouldAllowConversion(); });
  if (all_convertible)
  {
    menu->addAction(tr("Convert Selected Files..."), this, &GameList::ConvertSelection);
    menu->addSeparator();
  }

  const bool all_wii =
      std::ranges::all_of(games, [](const auto& game) { return DiscIO::IsWii(game->GetPlatform()); });
  if (all_wii)
  {
    menu->addAction(tr("Export Wii Saves"), this, &GameList::ExportWiiSaves);
    menu->addSeparator();
  }

  menu->addAction(tr("Delete Selected Files..."), this, &GameList::DeleteSelection);
}

void GameList::AddSingleGameEntries(QMenu* menu,
                                    const std::shared_ptr<const UICommon::GameFile>& game)
{
  auto& system = Core::System::GetInstance();
  auto& settings = Settings::Instance();

  // Mod descriptors (.json) only get the generic entries.
  const bool real_file = !game->IsModDescriptor();
  const DiscIO::Platform platform = game->GetPlatform();

  menu->addAction(tr("&Properties"), this, &GameList::OpenProperties);
  if (real_file && platform != DiscIO::Platform::ELFOrDOL)
    menu->addAction(tr("&Wiki"), this, &GameList::OpenWiki);
  menu->addSeparator();

  if (real_file && DiscIO::IsDisc(platform))
  {
    menu->addAction(tr("Start with Riivolution Patches..."), this,
                    &GameList::StartWithRiivolution);
    menu->addSeparator();
    menu->addAction(tr("Set as &Default ISO"), this, &GameList::SetDefaultISO);
    if (game->ShouldAllowConversion())
      menu->addAction(tr("Convert File..."), this, &GameList::ConvertSelection);

    QAction* const change_disc = menu->addAction(tr("Change &Disc"), this, &GameList::ChangeDisc);
    connect(&settings, &Settings::EmulationStateChanged, change_disc,
            [&system, change_disc] { change_disc->setEnabled(Core::IsRunning(system)); });
    change_disc->setEnabled(Core::IsRunning(system));

    menu->addSeparator();
  }

  if (real_file && platform == DiscIO::Platform::WiiDisc)
  {
    QAction* const disc_update =
        menu->addAction(tr("Perform System Update"), this, [this, path = game->GetFilePath()] {
          WiiUpdate::PerformDiscUpdate(path, this);
          // The update may have installed a newer Wii Menu.
          Settings::Instance().NANDRefresh();
        });
    disc_update->setEnabled(Core::IsUninitialized(system) || !system.IsWii());
  }

  if (real_file && platform == DiscIO::Platform::WiiWAD)
  {
    auto* const install = new QAction(tr("Install to the NAND"), menu);
    auto* const uninstall = new QAction(tr("Uninstall from the NAND"), menu);
    connect(install, &QAction::triggered, this, &GameList::InstallWAD);
    connect(uninstall, &QAction::triggered, this, &GameList::UninstallWAD);

    for (QAction* const action : {install, uninstall})
    {
      action->setEnabled(Core::IsUninitialized(system));
      menu->addAction(action);
    }
    // Uninstalling only makes sense for a title that is on the NAND.
    if (Core::IsUninitialized(system))
      uninstall->setEnabled(WiiUtils::IsTitleInstalled(game->GetTitleID()));

    connect(&settings, &Settings::EmulationStateChanged, menu,
            [install, uninstall, game](Core::State state) {
              const bool stopped = state == Core::State::Uninitialized;
              install->setEnabled(stopped);
              uninstall->setEnabled(stopped && WiiUtils::IsTitleInstalled(game->GetTitleID()));
            });

    menu->addSeparator();
  }

  if (real_file &&
      (platform == DiscIO::Platform::WiiWAD || platform == DiscIO::Platform::WiiDisc))
  {
    QAction* const open_save =
        menu->addAction(tr("Open Wii &Save Folder"), this, &GameList::OpenWiiSaveFolder);
    QAction* const export_save =
        menu->addAction(tr("Export Wii Save"), this, &GameList::ExportWiiSaves);
    open_save->setEnabled(Core::IsUninitialized(system));
    export_save->setEnabled(Core::IsUninitialized(system));
    menu->addSeparator();
  }

  if (real_file && platform == DiscIO::Platform::GameCubeDisc)
  {
    menu->addAction(tr("Open GameCube &Save Folder"), this, &GameList::OpenGCSaveFolder);
    menu->addSeparator();
  }

  menu->addAction(tr("Open &Containing Folder"), this, &GameList::OpenContainingFolder);
  menu->addAction(tr("Delete File..."), this, &GameList::DeleteSelection);
#ifdef _WIN32
  menu->addAction(tr("Add Shortcut to Desktop"), this, [this] {
    if (CreateDesktopShortcut())
      return;
    ModalMessageBox::critical(this, tr("Add Shortcut to Desktop"),
                              tr("There was an issue adding a shortcut to the desktop"));
  });
#endif

  menu->addSeparator();

  AddTagEntries(menu, game->GetFilePath());

  menu->addSeparator();

  auto* const host = new QAction(tr("Host with NetPlay"), menu);
  connect(host, &QAction::triggered, [this, game] { emit NetPlayHost(*game); });
  connect(&settings, &Settings::EmulationStateChanged, menu,
          [host](Core::State state) { host->setEnabled(state == Core::State::Uninitialized); });
  host->setEnabled(Core::IsUninitialized(system));
  menu->addAction(host);
}

void GameList::AddTagEntries(QMenu* menu, const std::string& path)
{
  // The tag submenu stays open while tags are ticked, so several can be changed at once.
  auto* const tags_menu = new QtUtils::NonAutodismissibleMenu(tr("Tags"), menu);
  menu->addMenu(tags_menu);

  const QStringList current = m_model.GetGameTags(path);
  for (const QString& tag : m_model.GetAllTags())
  {
    QAction* const toggle = tags_menu->addAction(tag);
    toggle->setCheckable(true);
    toggle->setChecked(current.contains(tag));
    connect(toggle, &QAction::toggled, [model = &m_model, path, tag](bool checked) {
      if (checked)
        model->AddGameTag(path, tag);
      else
        model->RemoveGameTag(path, tag);
    });
  }

  menu->addAction(tr("New Tag..."), this, &GameList::PromptNewTag);
  menu->addAction(tr("Remove Tag..."), this, &GameList::PromptRemoveTag);
}

void GameList::OpenProperties()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  // One properties window per game: bring an open one to the front instead.
  const auto open_windows = findChildren<PropertiesDialog*>();
  for (PropertiesDialog* const window : open_windows)
  {
    if (window->GetFilePath() == game->GetFilePath())
    {
      window->raise();
      return;
    }
  }

  auto* const properties = new PropertiesDialog(this, *game);
  connect(properties, &PropertiesDialog::OpenGeneralSettings, this, &GameList::OpenGeneralSettings);
  connect(properties, &PropertiesDialog::OpenGraphicsSettings, this,
          &GameList::OpenGraphicsSettings);
  connect(properties, &PropertiesDialog::finished, this,
          [properties] { properties->deleteLater(); });
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(properties, &PropertiesDialog::OpenAchievementSettings, this,
          &GameList::OpenAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  properties->show();
}

void GameList::ExportWiiSaves()
{
  const QString target = DolphinFileDialog::getExistingDirectory(
      this, tr("Select Export Directory"), QString::fromStdString(File::GetUserPath(D_USER_IDX)),
      QFileDialog::ShowDirsOnly);
  if (target.isEmpty())
    return;

  QString failures;
  for (const auto& game : GetSelectedGames())
  {
    const auto result = WiiSave::Export(game->GetTitleID(), target.toStdString());
    if (result == WiiSave::CopyResult::Success)
      continue;
    const std::string name = game->GetName(UICommon::GameFile::Variant::LongAndPossiblyCustom);
    failures += QLatin1Char('\n');
    failures += QString::fromStdString(name);
  }

  if (failures.isEmpty())
  {
    ModalMessageBox::information(this, tr("Save Export"), tr("Successfully exported save files"));
    return;
  }
  ModalMessageBox::critical(this, tr("Save Export"),
                            tr("Failed to export the following save files:") + failures);
}

void GameList::OpenWiki()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  const QString url = QStringLiteral("https://wiki.dolphin-emu.org/dolphin-redirect.php?gameid=") +
                      QString::fromStdString(game->GetGameID());
  QDesktopServices::openUrl(QUrl(url));
}

void GameList::ConvertSelection()
{
  auto games = GetSelectedGames();
  if (games.empty())
    return;

  ConvertDialog dialog{std::move(games), this};
  dialog.exec();
}

void GameList::InstallWAD()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  const bool installed = WiiUtils::InstallWAD(game->GetFilePath());
  ShowResultBox(this, installed, tr("Successfully installed this title to the NAND."),
                tr("Failed to install this title to the NAND."));
}

void GameList::UninstallWAD()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  {
    ModalMessageBox confirm(this);
    confirm.setIcon(QMessageBox::Information);
    confirm.setWindowTitle(tr("Confirm"));
    confirm.setText(tr("Uninstalling the WAD will remove the currently installed version of "
                       "this title from the NAND without deleting its save data. Continue?"));
    confirm.setStandardButtons(QMessageBox::No | QMessageBox::Yes);
    if (confirm.exec() == QMessageBox::No)
      return;
  }

  const bool removed = WiiUtils::UninstallTitle(game->GetTitleID());
  ShowResultBox(this, removed, tr("Successfully removed this title from the NAND."),
                tr("Failed to remove this title from the NAND."));
}

void GameList::StartWithRiivolution()
{
  if (const auto game = GetSelectedGame())
    emit OnStartWithRiivolution(*game);
}

void GameList::SetDefaultISO()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  const QString path = QString::fromStdString(game->GetFilePath());
  Settings::Instance().SetDefaultGame(QDir::toNativeSeparators(path));
}

void GameList::OpenContainingFolder()
{
  if (const auto game = GetSelectedGame())
    QtUtils::ShowFileInFolder(game->GetFilePath());
}

void GameList::OpenWiiSaveFolder()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  const std::string save_folder = game->GetWiiFSPath();
  if (!File::Exists(save_folder))
  {
    ModalMessageBox::information(this, tr("Information"), tr("No save data found."));
    return;
  }

  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(save_folder)));
}

void GameList::OpenGCSaveFolder()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  namespace fs = std::filesystem;
  using ExpansionInterface::EXIDeviceType;

  bool shown = false;
  for (const ExpansionInterface::Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    const EXIDeviceType device = Config::Get(Config::GetInfoForEXIDevice(slot));

    if (device == EXIDeviceType::MemoryCard)
    {
      QtUtils::ShowFileInFolder(Config::GetMemcardPath(slot, game->GetRegion()));
      shown = true;
      continue;
    }
    if (device != EXIDeviceType::MemoryCardFolder)
      continue;

    // A GCI folder: show the first save whose name starts with "<maker>-<4 character game ID>-".
    const std::string folder_override = Config::Get(Config::GetInfoForGCIPathOverride(slot));
    const std::string folder = folder_override.empty() ?
                                   Config::GetGCIFolderPath(slot, game->GetRegion()) :
                                   folder_override;
    const fs::path::string_type prefix =
        fs::path{fmt::format("{}-{}-", game->GetMakerID(), game->GetGameID().substr(0, 4))}
            .native();
    const fs::path gci_extension{".gci"};

    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
    {
      const fs::path& file = it->path();
      if (file.filename().native().starts_with(prefix) &&
          file.extension() == gci_extension.native())
      {
        QtUtils::ShowFileInFolder(file.generic_string());
        shown = true;
        break;
      }
    }
  }

  if (!shown)
    ModalMessageBox::information(this, tr("Information"), tr("No save data found."));
}

#ifdef _WIN32
bool GameList::CreateDesktopShortcut()
{
  const auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED);
  const auto link = wil::CoCreateInstanceNoThrow<ShellLink, IShellLink>();
  if (!link)
    return false;

  // The shortcut starts this executable with "-e <game>".
  const std::wstring executable = QCoreApplication::applicationFilePath().toStdWString();
  if (FAILED(link->SetPath(executable.c_str())))
    return false;

  const auto game = GetSelectedGame();
  const std::wstring arguments = UTF8ToTStr("-e \"" + game->GetFilePath() + "\"");
  if (FAILED(link->SetArguments(arguments.c_str())))
    return false;

  wil::unique_cotaskmem_string desktop;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_NO_ALIAS, nullptr, &desktop)))
    return false;

  // Drop the characters NTFS does not allow in file names.
  std::string name = game->GetName(Core::TitleDatabase());
  constexpr std::string_view forbidden = "<>:\"/\\|?*";
  std::erase_if(name, [forbidden](char c) { return forbidden.find(c) != std::string_view::npos; });

  const std::wstring shortcut_path = std::wstring(desktop.get()) + UTF8ToTStr("\\" + name + ".lnk");
  const auto file = link.try_query<IPersistFile>();
  if (!file)
    return false;

  return SUCCEEDED(file->Save(shortcut_path.c_str(), TRUE));
}
#endif

void GameList::DeleteSelection()
{
  ModalMessageBox confirm(this);
  confirm.setIcon(QMessageBox::Warning);
  confirm.setWindowTitle(tr("Confirm"));
  confirm.setText(tr("Are you sure you want to delete this file?"));
  confirm.setInformativeText(tr("This cannot be undone!"));
  confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
  if (confirm.exec() != QMessageBox::Yes)
    return;

  for (const auto& game : GetSelectedGames())
  {
    // Retry until the file is gone or the user gives up; giving up stops the whole deletion.
    for (;;)
    {
      if (File::Delete(game->GetFilePath()))
      {
        m_model.RemoveGame(game->GetFilePath());
        break;
      }

      ModalMessageBox error(this);
      error.setIcon(QMessageBox::Critical);
      error.setWindowTitle(tr("Failure"));
      error.setText(tr("Failed to delete the selected file."));
      error.setInformativeText(tr("Check whether you have the permissions required to "
                                  "delete the file or whether it's still in use."));
      error.setStandardButtons(QMessageBox::Retry | QMessageBox::Abort);
      if (error.exec() == QMessageBox::Abort)
        return;
    }
  }
}

void GameList::ChangeDisc()
{
  const auto game = GetSelectedGame();
  if (!game)
    return;

  auto& system = Core::System::GetInstance();
  system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{system}, game->GetFilePath());
}

QAbstractItemView* GameList::CurrentView() const
{
  if (currentWidget() == m_list)
    return m_list;
  return m_grid;
}

QSortFilterProxyModel* GameList::CurrentProxy() const
{
  if (currentWidget() == m_list)
    return m_list_proxy;
  return m_grid_proxy;
}

std::shared_ptr<const UICommon::GameFile> GameList::GetSelectedGame() const
{
  const QItemSelectionModel* const selection = CurrentView()->selectionModel();
  if (!selection->hasSelection())
    return {};

  const QModelIndex first = CurrentProxy()->mapToSource(selection->selectedIndexes()[0]);
  return m_model.GetGameFile(first.row());
}

QList<std::shared_ptr<const UICommon::GameFile>> GameList::GetSelectedGames() const
{
  QList<std::shared_ptr<const UICommon::GameFile>> games;
  const QItemSelectionModel* const selection = CurrentView()->selectionModel();
  if (!selection->hasSelection())
    return games;

  for (const QModelIndex& row : selection->selectedRows())
    games.push_back(m_model.GetGameFile(CurrentProxy()->mapToSource(row).row()));
  return games;
}

bool GameList::HasMultipleSelected() const
{
  return CurrentView()->selectionModel()->selectedRows().size() > 1;
}

std::shared_ptr<const UICommon::GameFile> GameList::FindGame(const std::string& file_path) const
{
  return m_model.FindGame(file_path);
}

std::shared_ptr<const UICommon::GameFile>
GameList::FindSecondDisc(const UICommon::GameFile& first_disc) const
{
  return m_model.FindSecondDisc(first_disc);
}

std::string GameList::GetNetPlayName(const UICommon::GameFile& entry) const
{
  return m_model.GetNetPlayName(entry);
}

void GameList::SetViewColumn(int column, bool visible)
{
  m_list->setColumnHidden(column, !visible);
}

void GameList::SetPreferredView(bool prefer_list)
{
  m_prefer_list = prefer_list;
  Settings::Instance().SetPreferredView(prefer_list);
  ShowFittingPage();
}

void GameList::ShowFittingPage()
{
  if (m_model.rowCount(QModelIndex()) == 0)
  {
    setCurrentWidget(m_empty);
    return;
  }
  setCurrentWidget(m_prefer_list ? static_cast<QWidget*>(m_list) : m_grid);
}

void GameList::keyPressEvent(QKeyEvent* event)
{
  const bool start_key = event->key() == Qt::Key_Return && GetSelectedGame() != nullptr;
  if (!start_key)
  {
    QStackedWidget::keyPressEvent(event);
    return;
  }

  // Alt+Enter opens the game's properties, Enter alone starts it.
  if (event->modifiers() == Qt::AltModifier)
    OpenProperties();
  else
    emit GameSelected();
}

void GameList::OnColumnVisibilityToggled(const QString& row, bool visible)
{
  // The View > List Columns entries pass their (translated) labels.
  // Built once, on first use, in column order.
  static const QMap<QString, Column> columns_by_label = [] {
    QMap<QString, Column> labels;
    const auto add = [&labels](const QString& label, Column column) {
      labels.insert(label, column);
    };
    add(tr("Platform"), Column::Platform);
    add(tr("Banner"), Column::Banner);
    add(tr("Title"), Column::Title);
    add(tr("Description"), Column::Description);
    add(tr("Maker"), Column::Maker);
    add(tr("File Name"), Column::FileName);
    add(tr("File Path"), Column::FilePath);
    add(tr("Game ID"), Column::ID);
    add(tr("Region"), Column::Country);
    add(tr("File Size"), Column::Size);
    add(tr("File Format"), Column::FileFormat);
    add(tr("Block Size"), Column::BlockSize);
    add(tr("Compression"), Column::Compression);
    add(tr("Time Played"), Column::TimePlayed);
    add(tr("Tags"), Column::Tags);
    return labels;
  }();

  const Column column = columns_by_label.value(row, Column::Platform);
  m_list->setColumnHidden(ColumnIndex(column), !visible);
}

void GameList::InvalidateProxies()
{
  m_list_proxy->invalidate();
  m_grid_proxy->invalidate();
}

void GameList::OnGameListVisibilityChanged()
{
  InvalidateProxies();
  UpdateGameCount();
}

void GameList::OnHeaderSectionResized(int logical_index)
{
  QHeaderView* const header = m_list->horizontalHeader();

  const int position = header->visualIndex(logical_index);
  const int shown_count = header->count() - header->hiddenSectionCount();
  const auto is_interactive = [header](int visual) {
    return header->sectionResizeMode(header->logicalIndex(visual)) == QHeaderView::Interactive;
  };

  bool resizable_after = false;
  for (int visual = position + 1; visual < shown_count && !resizable_after; ++visual)
    resizable_after = is_interactive(visual);

  if (!resizable_after)
  {
    FitColumnsToWidth();
    return;
  }

  // Dragging a column edge should give or take room from the columns to its right only, so the
  // resizable columns to its left are pinned while the widths are redistributed.
  std::vector<int> pinned;
  for (int visual = 0; visual < position; ++visual)
  {
    if (!is_interactive(visual))
      continue;
    header->setSectionResizeMode(header->logicalIndex(visual), QHeaderView::Fixed);
    pinned.push_back(visual);
  }

  FitColumnsToWidth();

  for (const int visual : pinned)
    header->setSectionResizeMode(header->logicalIndex(visual), QHeaderView::Interactive);
}

void GameList::FitColumnsToWidth()
{
  // Changing widths below fires the header signals that lead back here.
  if (m_fitting_columns)
    return;
  m_fitting_columns = true;

  ApplyColumnVisibility();

  // Instead of a stretching column (which Qt handles badly next to resizable ones), share the
  // width left by the fixed columns among the resizable ones, in proportion to their widths.
  QHeaderView* const header = m_list->horizontalHeader();
  int room = width() - style()->pixelMetric(QStyle::PM_ScrollBarExtent);
  int flexible_total = 0;
  std::vector<int> flexible;
  for (int column = 0; column < COLUMN_COUNT; ++column)
  {
    if (m_list->isColumnHidden(column))
      continue;

    if (header->sectionResizeMode(column) == QHeaderView::Fixed)
    {
      room -= m_list->columnWidth(column);
      continue;
    }
    flexible.push_back(column);
    flexible_total += m_list->columnWidth(column);
  }

  for (const int column : flexible)
  {
    const float share = static_cast<float>(m_list->columnWidth(column)) / flexible_total;
    const float new_width = std::max(5.f, std::ceil(room * share));
    m_list->setColumnWidth(column, static_cast<int>(new_width));
  }

  m_fitting_columns = false;
}

void GameList::PromptNewTag()
{
  const QString tag =
      QInputDialog::getText(this, tr("New tag"), tr("Name for a new tag:"), QLineEdit::Normal,
                            QString{}, nullptr, Qt::WindowCloseButtonHint);
  if (!tag.isEmpty())
    m_model.NewTag(tag);
}

void GameList::PromptRemoveTag()
{
  const QString tag =
      QInputDialog::getText(this, tr("Remove tag"), tr("Name of the tag to remove:"),
                            QLineEdit::Normal, QString{}, nullptr, Qt::WindowCloseButtonHint);
  if (!tag.isEmpty())
    m_model.DeleteTag(tag);
}

void GameList::SetSearchTerm(const QString& term)
{
  m_model.SetSearchTerm(term);
  InvalidateProxies();
  ApplyColumnVisibility();
  UpdateGameCount();
}

void GameList::UpdateGameCount() const
{
  emit GameCountUpdated(m_model.rowCount(QModelIndex{}), m_list_proxy->rowCount());
}

void GameList::Zoom(bool in)
{
  // Steps of a tenth, computed in double precision; the scale never goes below a tenth.
  const double scale = m_model.GetScale();
  if (!in && scale <= 0.1)
    return;
  m_model.SetScale(static_cast<float>(in ? scale + 0.1 : scale - 0.1));

  InvalidateProxies();

  // The grid's captions grow and shrink with the pictures.
  QFont font;
  font.setPointSizeF(m_model.GetScale() * font.pointSize());
  m_grid->setFont(font);
}
