// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include <QDockWidget>
#include <QString>

#include "Common/CommonTypes.h"
#include "DolphinQt/Debugger/CodeViewWidget.h"

class BranchWatchDialog;
class QCloseEvent;
class QLineEdit;
class QShowEvent;
class QSplitter;
class QListWidget;
class QPushButton;
class QTableWidget;
class QToolButton;

namespace Common
{
struct Symbol;
}
namespace Core
{
class System;
}
class PPCSymbolDB;

// The "Code" dock: the disassembly view plus searchable lists of the call stack, symbols, notes,
// and the calls into and out of the function being viewed. Also hosts the stepping commands.
class CodeWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit CodeWidget(QWidget* parent = nullptr);
  ~CodeWidget() override;

  void Step();
  void StepOver();
  void StepOut();
  void Skip();
  void ShowPC();
  void SetPC();

  void OnBranchWatchDialog();
  void OnSetCodeAddress(u32 address);
  void ToggleBreakpoint();
  void AddBreakpoint();
  void SetAddress(u32 address, CodeViewWidget::SetAddressUpdate update);

  void Update();
  void UpdateSymbols();
  void ActivateSearchAddress();
signals:
  void RequestPPCComparison(u32 address, bool translate_address);
  void ShowMemory(u32 address);

private:
  void closeEvent(QCloseEvent*) override;
  void showEvent(QShowEvent* event) override;

  QWidget* BuildCodePane();
  void BuildSidePanes();
  QLineEdit* AddSidePane(const QString& title, QWidget* content);
  void Wire();

  void FollowPCUnlessLocked();
  void DemangleSettingChanged();
  void SymbolsChanged();
  void JumpToTypedAddress();
  void SymbolFilterChanged();
  void JumpToSymbol();
  void JumpToNote();
  void JumpToListedAddress(const QListWidget* list);

  [[nodiscard]] const Common::Symbol* SymbolAtView() const;
  void RefreshCallstack();
  void RefreshNotes();
  void RefreshCalls(const Common::Symbol* symbol);
  void RefreshCallers(const Common::Symbol* symbol);
  [[nodiscard]] QString CallEntry(const Common::Symbol& target, u32 address) const;
  [[nodiscard]] const std::string& DisplayName(const Common::Symbol& symbol) const;

  Core::System& m_system;
  PPCSymbolDB& m_symbol_db;
  bool m_demangle = false;
  QString m_symbol_filter;

  BranchWatchDialog* m_branch_watch_dialog = nullptr;

  QLineEdit* m_address_box = nullptr;
  QToolButton* m_lock_button = nullptr;
  QPushButton* m_branch_watch_button = nullptr;
  CodeViewWidget* m_code_view = nullptr;

  QListWidget* m_callstack = nullptr;
  QLineEdit* m_callstack_filter = nullptr;
  QListWidget* m_symbols = nullptr;
  QListWidget* m_notes = nullptr;
  QLineEdit* m_symbols_filter = nullptr;
  QListWidget* m_calls = nullptr;
  QLineEdit* m_calls_filter = nullptr;
  QListWidget* m_callers = nullptr;
  QLineEdit* m_callers_filter = nullptr;

  QSplitter* m_side_split = nullptr;
  QSplitter* m_main_split = nullptr;
};
