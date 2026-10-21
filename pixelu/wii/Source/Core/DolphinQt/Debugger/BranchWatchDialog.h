// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include <QDialog>
#include <QIcon>
#include <QList>
#include <QModelIndexList>

#include "Common/CommonTypes.h"

namespace Core
{
class BranchWatch;
class CPUThreadGuard;
enum class State;
class System;
}  // namespace Core
class PPCSymbolDB;

class BranchWatchProxyModel;
class BranchWatchTableModel;
class CodeWidget;
class QAction;
class QMenu;
class QPoint;
class QPushButton;
class QStatusBar;
class QTableView;
class QTimer;
class QToolBar;
class QWidget;

namespace BranchWatchTableModelColumn
{
enum EnumType : int;
}
namespace BranchWatchTableModelUserRole
{
enum EnumType : int;
}

// The Branch Watch tool: records branches the emulated CPU takes, lets the user narrow them down
// step by step, and offers patches, breakpoints and navigation for the survivors.
class BranchWatchDialog : public QDialog
{
  Q_OBJECT

  using Column = BranchWatchTableModelColumn::EnumType;
  using UserRole = BranchWatchTableModelUserRole::EnumType;

public:
  explicit BranchWatchDialog(Core::System& system, Core::BranchWatch& branch_watch,
                             PPCSymbolDB& ppc_symbol_db, CodeWidget* code_widget,
                             QWidget* parent = nullptr);
  ~BranchWatchDialog() override;

  BranchWatchDialog(const BranchWatchDialog&) = delete;
  BranchWatchDialog(BranchWatchDialog&&) = delete;
  BranchWatchDialog& operator=(const BranchWatchDialog&) = delete;
  BranchWatchDialog& operator=(BranchWatchDialog&&) = delete;

  // Refreshes the hit counts (and the status line while blacklisting). Stepping the CPU does not
  // report a state change, so the code widget calls this itself after a step.
  void Update() const;

protected:
  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  using ModelStep = void (BranchWatchTableModel::*)(const Core::CPUThreadGuard&);

  // Construction pieces, called in this order.
  void BuildTable(PPCSymbolDB& ppc_symbol_db);
  void BuildToolControls();
  void BuildBranchTypeFilters();
  void BuildAddressFilters();
  void BuildConditionFilters();
  void BuildMiscControls();
  void BuildRowMenus();
  void BuildColumnMenu();
  QMenu* BuildToolbarMenu();
  QWidget* BuildMenuBar(QMenu* toolbar_menu);

  // Tool controls and menu bar.
  void ToggleRecording(bool on) const;
  void RunStep(ModelStep step);
  void ClearWatch();
  void PathWasTaken();
  void PathNotTaken();
  void WasOverwritten();
  void NotOverwritten();
  void SaveToDefault();
  void SaveToChosen();
  void LoadFromDefault();
  void LoadFromChosen();
  void ChooseAutoSaveTarget(bool on);
  void ToggleControls(bool hidden) const;
  void ShowHelp();

  // Table interaction. The rows a context-menu action works on are kept in m_menu_rows.
  void ClickCell(const QModelIndex& index) const;
  void OpenRowMenu(const QPoint& pos) const;
  void OpenColumnMenu(const QPoint& pos) const;
  void DeleteMenuRows() const;
  void DeleteSelectedRows() const;
  void CopyAddresses() const;
  void PatchTargets(u32 instruction) const;
  void PatchOrigins(u32 (*edit)(u32)) const;
  void AddBreakpoints(bool break_on_hit, bool log_on_hit) const;

  QMenu* PrepareRowMenu(int column) const;
  void RefreshOpenRowMenu(bool running) const;
  void PrepareInstructionMenu(bool running) const;
  void PrepareConditionMenu(bool running) const;
  void PrepareOriginMenu(bool running) const;
  void PrepareDestinationMenu(bool running) const;
  void PrepareSymbolMenu(bool running) const;
  void ShowBreakpointCoverage() const;
  [[nodiscard]] bool AllMenuRowsConditional() const;

  // Life cycle.
  void Attach();
  void Detach();
  void StateChanged(Core::State state) const;
  void ReloadIcons();
  void RestoreSettings();
  void StoreSettings() const;
  void RefreshStatus() const;

  // Snapshot files.
  void WriteSnapshot(const Core::CPUThreadGuard& guard, const std::string& path);
  void ReadSnapshot(const Core::CPUThreadGuard& guard, const std::string& path);
  void AutoSave(const Core::CPUThreadGuard& guard);

  Core::System& m_system;
  Core::BranchWatch& m_watch;
  CodeWidget* m_code_widget;

  QTableView* m_table = nullptr;
  BranchWatchProxyModel* m_filter = nullptr;
  BranchWatchTableModel* m_model = nullptr;
  QStatusBar* m_status = nullptr;
  QToolBar* m_controls = nullptr;
  QTimer* m_poll = nullptr;

  QPushButton* m_record_button = nullptr;
  QPushButton* m_clear_button = nullptr;
  QPushButton* m_taken_button = nullptr;
  QPushButton* m_not_taken_button = nullptr;
  QPushButton* m_overwritten_button = nullptr;
  QPushButton* m_not_overwritten_button = nullptr;
  QPushButton* m_wipe_hits_button = nullptr;

  // Toolbar sections, as the actions QToolBar returns for them (used to show or hide each).
  QAction* m_branch_type_section = nullptr;
  QAction* m_address_section = nullptr;
  QAction* m_condition_section = nullptr;
  QAction* m_misc_section = nullptr;

  QAction* m_autosave_action = nullptr;
  QAction* m_invert_condition_action = nullptr;
  QAction* m_invert_decrement_action = nullptr;
  QAction* m_unconditional_action = nullptr;
  QAction* m_nop_action = nullptr;
  QAction* m_blr_action = nullptr;
  QAction* m_copy_action = nullptr;
  QMenu* m_breakpoint_menu = nullptr;
  QAction* m_break_action = nullptr;
  QAction* m_log_action = nullptr;
  QAction* m_break_log_action = nullptr;

  QMenu* m_instruction_menu = nullptr;
  QMenu* m_condition_menu = nullptr;
  QMenu* m_origin_menu = nullptr;
  QMenu* m_destination_menu = nullptr;
  QMenu* m_symbol_menu = nullptr;
  QMenu* m_hits_menu = nullptr;
  QMenu* m_column_menu = nullptr;

  QIcon m_all_icon;
  QIcon m_some_icon;

  QList<QMetaObject::Connection> m_links;
  mutable QModelIndexList m_menu_rows;
  std::optional<std::string> m_autosave_path;
};
