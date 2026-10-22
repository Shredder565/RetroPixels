// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class QPushButton;
class QTableWidget;
class QTableWidgetItem;

namespace ResourcePack
{
class ResourcePack;
}

// Lists the resource packs found in the user's ResourcePacks folder (the last pack of the
// manager's list in the first row) and lets the user install, uninstall, delete and reorder them.
// Note: deliberately no Q_OBJECT, so tr() translates in the QDialog context.
class ResourcePackManager : public QDialog
{
public:
  explicit ResourcePackManager(QWidget* parent = nullptr);

private:
  void BuildLayout();
  void WireUp();

  void FillTable();
  void UpdateButtons();

  // The pack behind the first selected cell, or null when nothing is selected.
  ResourcePack::ResourcePack* SelectedPack() const;
  // Table rows and pack indices run in opposite directions.
  int PackIndexOfRow(int row) const;

  void ToggleInstalled();
  void InstallSelected();
  void UninstallSelected();
  void DeleteSelected();
  void MoveSelectedUp();
  void MoveSelectedDown();
  void Rescan();
  void ShowPackFolder();
  void FollowLink(QTableWidgetItem* item);

  QTableWidget* m_packs = nullptr;
  QPushButton* m_folder_button = nullptr;
  QPushButton* m_toggle_button = nullptr;
  QPushButton* m_delete_button = nullptr;
  QPushButton* m_rescan_button = nullptr;
  QPushButton* m_up_button = nullptr;
  QPushButton* m_down_button = nullptr;
};
