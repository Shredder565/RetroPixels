// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>

#include <QDockWidget>
#include <QString>

#include "Common/CommonTypes.h"

class QAction;
class QCloseEvent;
class QPoint;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QToolBar;
class QWidget;

namespace Core
{
class System;
}

// Dock listing the instruction breakpoints and memory checks, with toggles per column and
// load/save to the game's INI.
class BreakpointWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit BreakpointWidget(QWidget* parent = nullptr);
  ~BreakpointWidget() override;

  void AddBP(u32 addr);
  void AddBP(u32 addr, bool break_on_hit, bool log_on_hit, const QString& condition);
  void AddAddressMBP(u32 addr, bool on_read = true, bool on_write = true, bool do_log = true,
                     bool do_break = true, const QString& condition = {});
  void AddRangedMBP(u32 from, u32 to, bool do_read = true, bool do_write = true, bool do_log = true,
                    bool do_break = true, const QString& condition = {});
  void UpdateButtonsEnabled();
  void Update();

signals:
  void ShowCode(u32 address);
  void ShowMemory(u32 address);

protected:
  void closeEvent(QCloseEvent*) override;
  void showEvent(QShowEvent* event) override;

private:
  void BuildLayout();
  void RefreshIcons();
  void ApplyFont(const QFont& font);

  void CellClicked(QTableWidgetItem* item);
  void CellEdited(QTableWidgetItem* item);
  void ShowRowMenu(const QPoint& pos);

  void ToggleBreaking();
  void ClearAll();
  void CreateNew();
  void OpenEditor(u32 address, bool instruction);
  void LoadFromIni();
  void SaveToIni();

  void ReplaceBreakpoint(u32 address, int column, std::optional<QString> text = std::nullopt);
  void ReplaceMemCheck(u32 address, int column, std::optional<QString> text = std::nullopt);
  void StoreMemCheck(u32 from, u32 to, bool ranged, bool on_read, bool on_write, bool do_log,
                     bool do_break, const QString& condition);

  Core::System& m_system;

  QToolBar* m_bar = nullptr;
  QTableWidget* m_list = nullptr;
  QAction* m_toggle_action = nullptr;
  QAction* m_new_action = nullptr;
  QAction* m_clear_action = nullptr;
  QAction* m_load_action = nullptr;
  QAction* m_save_action = nullptr;
};
