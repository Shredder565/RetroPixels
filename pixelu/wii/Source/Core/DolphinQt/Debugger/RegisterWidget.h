// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <string>

#include <QDockWidget>

#include "Common/CommonTypes.h"
#include "DolphinQt/Debugger/RegisterColumn.h"

class QTableWidget;
class QCloseEvent;
class QShowEvent;
namespace Core
{
class System;
}

// Dock showing the CPU registers in a grid of label/value cells; values are editable where the
// register can be written.
class RegisterWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit RegisterWidget(QWidget* parent = nullptr);
  ~RegisterWidget() override;

signals:
  void RequestTableUpdate();
  void RequestViewInCode(u32 addr);
  void RequestViewInMemory(u32 addr);
  void RequestMemoryBreakpoint(u32 addr);
  void RequestWatch(QString name, u32 addr);
  void UpdateTable();
  void UpdateValue(QTableWidgetItem* item);
  void UpdateValueType(QTableWidgetItem* item);

protected:
  void closeEvent(QCloseEvent*) override;
  void showEvent(QShowEvent* event) override;

private:
  void BuildLayout();
  void FillGrid();
  void ApplyFont(const QFont& font);
  void CellEdited(QTableWidgetItem* item);
  void ShowCellMenu();
  void RefreshIfPaused();
  void RunUntilRegisterChanges(const std::string& reg) const;

  void Place(int row, int column, RegisterType type, const std::string& name,
             std::function<u64()> read, std::function<void(u64)> write);
  void SetFormatQuietly(RegisterColumn* cell, RegisterDisplay format);

  Core::System& m_system;

  QTableWidget* m_grid = nullptr;
  bool m_refreshing = false;
};
