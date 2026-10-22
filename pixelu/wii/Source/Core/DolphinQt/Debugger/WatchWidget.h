// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDockWidget>

#include <vector>

#include "Common/CommonTypes.h"

class QAction;
class QCloseEvent;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QToolBar;

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

// Dock listing watched memory words (label, address, value as hex/decimal/string/float) with
// optional locking of the value every frame.
class WatchWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit WatchWidget(QWidget* parent = nullptr);
  ~WatchWidget() override;

  void AddWatch(const QString& name, u32 addr);
signals:
  void RequestMemoryBreakpoint(u32 addr);
  void ShowMemory(u32 addr);

protected:
  void closeEvent(QCloseEvent*) override;
  void showEvent(QShowEvent* event) override;

private:
  void BuildLayout();
  void RefreshIcons();
  void RefreshButtons();
  void Refill();
  void AppendBlankRow(int row);

  void AskForNewWatch();
  void RemoveSelected();
  void RemoveAll();
  void LoadFromIni();
  void SaveToIni();

  void ShowRowMenu();
  void CellEdited(QTableWidgetItem* item);

  std::vector<int> SelectedWatchIndexes() const;
  void FreezeValue(const Core::CPUThreadGuard& guard, u32 address);
  void RemoveWatch(const Core::CPUThreadGuard& guard, int index);
  void RemoveWatchAndRefill(int index);
  void SetSelectedLocked(bool locked);

  Core::System& m_system;

  QAction* m_new_action = nullptr;
  QAction* m_delete_action = nullptr;
  QAction* m_clear_action = nullptr;
  QAction* m_load_action = nullptr;
  QAction* m_save_action = nullptr;
  QToolBar* m_bar = nullptr;
  QTableWidget* m_list = nullptr;

  bool m_refilling = false;
};
