// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include <QWidget>

#include "Common/Config/Config.h"

class ConfigText;
class QGridLayout;
class QGroupBox;
class QListWidget;
class QPushButton;

// "Paths" settings page: the game folder list plus the default ISO and the user folders.
class PathPane final : public QWidget
{
  Q_OBJECT
public:
  explicit PathPane(QWidget* parent = nullptr);

private:
  QGroupBox* BuildGameFolders();
  QGridLayout* BuildPathGrid();
  void AddPathRow(QGridLayout* grid, int row, const QString& label, ConfigText* edit,
                  void (PathPane::*browse)());

  QString AskForFolder(const QString& caption, unsigned int user_path_index);
  void SetFolder(ConfigText* edit, const Config::Info<std::string>& setting, const QString& folder);

  void PickGameFolder();
  void RemoveSelectedFolder();
  void PickDefaultGame();
  void PickNandRoot();
  void PickDumpFolder();
  void PickLoadFolder();
  void PickResourcePackFolder();
  void PickWfsFolder();

  QListWidget* m_folder_list;
  QPushButton* m_remove_button;

  ConfigText* m_default_game;
  ConfigText* m_nand_root;
  ConfigText* m_dump_folder;
  ConfigText* m_load_folder;
  ConfigText* m_resource_pack_folder;
  ConfigText* m_wfs_folder;
};
