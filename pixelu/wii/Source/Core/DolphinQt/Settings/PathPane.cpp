// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/PathPane.h"

#include <QCheckBox>
#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigText.h"
#include "DolphinQt/Config/ConfigControls/ConfigUserPath.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"

PathPane::PathPane(QWidget* parent) : QWidget(parent)
{
  setWindowTitle(tr("Paths"));

  auto* const page = new QVBoxLayout;
  page->addWidget(BuildGameFolders());
  page->addLayout(BuildPathGrid());
  setLayout(page);
}

QGroupBox* PathPane::BuildGameFolders()
{
  Settings& settings = Settings::Instance();
  auto* const box = new QGroupBox(tr("Game Folders"));
  auto* const column = new QVBoxLayout;

  // The list mirrors Settings' folder list, which is the one that gets edited.
  m_folder_list = new QListWidget;
  m_folder_list->insertItems(0, settings.GetPaths());
  m_folder_list->setSpacing(1);
  connect(&settings, &Settings::PathAdded, this,
          [this](const QString& folder) { m_folder_list->addItem(folder); });
  connect(&settings, &Settings::PathRemoved, this, [this](const QString& folder) {
    for (QListWidgetItem* const entry : m_folder_list->findItems(folder, Qt::MatchExactly))
      delete entry;
  });
  connect(m_folder_list, &QListWidget::itemSelectionChanged, this,
          [this] { m_remove_button->setEnabled(!m_folder_list->selectedItems().isEmpty()); });
  column->addWidget(m_folder_list);

  auto* const buttons = new QHBoxLayout;
  buttons->addStretch();
  QPushButton* const add_button = new NonDefaultQPushButton(tr("Add..."));
  m_remove_button = new NonDefaultQPushButton(tr("Remove"));
  m_remove_button->setEnabled(false);

  auto* const recurse = new ConfigBool(tr("Search Subfolders"), Config::MAIN_RECURSIVE_ISO_PATHS);
  auto* const watch = new QCheckBox(tr("Check for Game List Changes in the Background"));
  watch->setChecked(settings.IsAutoRefreshEnabled());

  buttons->addWidget(add_button);
  buttons->addWidget(m_remove_button);
  column->addLayout(buttons);
  column->addWidget(recurse);
  column->addWidget(watch);

  // A different recursion setting means a different set of games.
  connect(recurse, &QCheckBox::toggled, this, [] { Settings::Instance().RefreshGameList(); });
  connect(watch, &QCheckBox::toggled, &settings, &Settings::SetAutoRefreshEnabled);
  connect(add_button, &QPushButton::clicked, this, &PathPane::PickGameFolder);
  connect(m_remove_button, &QPushButton::clicked, this, &PathPane::RemoveSelectedFolder);

  box->setLayout(column);
  return box;
}

void PathPane::AddPathRow(QGridLayout* grid, int row, const QString& label, ConfigText* edit,
                          void (PathPane::*browse)())
{
  QPushButton* const browse_button = new NonDefaultQPushButton(QStringLiteral("..."));
  connect(browse_button, &QPushButton::clicked, this, browse);
  grid->addWidget(new QLabel(label), row, 0);
  grid->addWidget(edit, row, 1);
  grid->addWidget(browse_button, row, 2);
}

QGridLayout* PathPane::BuildPathGrid()
{
  auto* const grid = new QGridLayout;
  grid->setColumnStretch(1, 1);

  m_default_game = new ConfigText(Config::MAIN_DEFAULT_ISO);
  connect(&Settings::Instance(), &Settings::DefaultGameChanged, this,
          [this](const QString& game) { m_default_game->setText(game); });
  AddPathRow(grid, 0, tr("Default ISO:"), m_default_game, &PathPane::PickDefaultGame);

  m_nand_root = new ConfigUserPath(D_WIIROOT_IDX, Config::MAIN_FS_PATH);
  AddPathRow(grid, 1, tr("Wii NAND Root:"), m_nand_root, &PathPane::PickNandRoot);

  m_dump_folder = new ConfigUserPath(D_DUMP_IDX, Config::MAIN_DUMP_PATH);
  AddPathRow(grid, 2, tr("Dump Path:"), m_dump_folder, &PathPane::PickDumpFolder);

  m_load_folder = new ConfigUserPath(D_LOAD_IDX, Config::MAIN_LOAD_PATH);
  AddPathRow(grid, 3, tr("Load Path:"), m_load_folder, &PathPane::PickLoadFolder);

  m_resource_pack_folder = new ConfigUserPath(D_RESOURCEPACK_IDX, Config::MAIN_RESOURCEPACK_PATH);
  AddPathRow(grid, 4, tr("Resource Pack Path:"), m_resource_pack_folder,
             &PathPane::PickResourcePackFolder);

  m_wfs_folder = new ConfigUserPath(D_WFSROOT_IDX, Config::MAIN_WFS_PATH);
  AddPathRow(grid, 5, tr("WFS Path:"), m_wfs_folder, &PathPane::PickWfsFolder);

  return grid;
}

QString PathPane::AskForFolder(const QString& caption, unsigned int user_path_index)
{
  const QString start = QString::fromStdString(File::GetUserPath(user_path_index));
  return QDir::toNativeSeparators(DolphinFileDialog::getExistingDirectory(this, caption, start));
}

void PathPane::SetFolder(ConfigText* edit, const Config::Info<std::string>& setting,
                         const QString& folder)
{
  if (folder.isEmpty())
    return;
  edit->setText(folder);
  Config::SetBase(setting, folder.toStdString());
}

void PathPane::PickGameFolder()
{
  const QString folder = QDir::toNativeSeparators(
      DolphinFileDialog::getExistingDirectory(this, tr("Select a Directory"), QDir::currentPath()));
  if (!folder.isEmpty())
    Settings::Instance().AddPath(folder);
}

void PathPane::RemoveSelectedFolder()
{
  const QListWidgetItem* const selected = m_folder_list->currentItem();
  if (selected)
    Settings::Instance().RemovePath(selected->text());
}

void PathPane::PickDefaultGame()
{
  const QString filter =
      QStringLiteral("%1 (*.elf *.dol *.gcm *.bin *.iso *.tgc *.wbfs *.ciso *.gcz *.wia *.rvz "
                     "hif_000000.nfs *.wad *.m3u *.json);;%2 (*)")
          .arg(tr("All GC/Wii files"))
          .arg(tr("All Files"));
  const QString game = QDir::toNativeSeparators(DolphinFileDialog::getOpenFileName(
      this, tr("Select a Game"), Settings::Instance().GetDefaultGame(), filter));
  if (!game.isEmpty())
    Settings::Instance().SetDefaultGame(game);
}

void PathPane::PickNandRoot()
{
  const QString folder = AskForFolder(tr("Select Wii NAND Root"), D_WIIROOT_IDX);
  if (!folder.isEmpty())
    m_nand_root->SetTextAndUpdate(folder);
}

void PathPane::PickDumpFolder()
{
  const QString folder = AskForFolder(tr("Select Dump Path"), D_DUMP_IDX);
  if (!folder.isEmpty())
    m_dump_folder->SetTextAndUpdate(folder);
}

void PathPane::PickLoadFolder()
{
  SetFolder(m_load_folder, Config::MAIN_LOAD_PATH,
            AskForFolder(tr("Select Load Path"), D_LOAD_IDX));
}

void PathPane::PickResourcePackFolder()
{
  SetFolder(m_resource_pack_folder, Config::MAIN_RESOURCEPACK_PATH,
            AskForFolder(tr("Select Resource Pack Path"), D_RESOURCEPACK_IDX));
}

void PathPane::PickWfsFolder()
{
  SetFolder(m_wfs_folder, Config::MAIN_WFS_PATH,
            AskForFolder(tr("Select WFS Path"), D_WFSROOT_IDX));
}
