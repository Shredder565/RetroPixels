// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDockWidget>

#include "Common/CommonTypes.h"

class BreakpointWidget;
class ClickableStatusBar;
class CodeWidget;
namespace Core
{
enum class State;
class System;
}  // namespace Core
struct JitBlock;
class JitBlockProxyModel;
class JitBlockTableModel;
namespace JitBlockTableModelColumn
{
enum EnumType : int;
}
namespace JitBlockTableModelUserRole
{
enum EnumType : int;
}
class MemoryWidget;
class QCloseEvent;
class QFont;
class QHideEvent;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QSplitter;
class QTableView;

// Dock listing the JIT's compiled blocks (filterable and sortable) and showing, for the current
// block, the guest code it covers next to the host code generated for it.
class JITWidget final : public QDockWidget
{
  Q_OBJECT

  using Column = JitBlockTableModelColumn::EnumType;
  using UserRole = JitBlockTableModelUserRole::EnumType;

signals:
  void HideSignal();
  void ShowSignal();
  void SetCodeAddress(u32 address);

public:
  explicit JITWidget(Core::System& system, QWidget* parent = nullptr);
  ~JITWidget() override;

  JITWidget(const JITWidget&) = delete;
  JITWidget(JITWidget&&) = delete;
  JITWidget& operator=(const JITWidget&) = delete;
  JITWidget& operator=(JITWidget&&) = delete;

  // Shows the blocks that cover a guest address (translated from an effective one if asked).
  void OnRequestPPCComparison(u32 address, bool translate_address);

private:
  void closeEvent(QCloseEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

  void BuildPanes();
  void BuildMenus();
  void RestoreLayout();
  void StoreLayout() const;
  void Listen(bool on);

  void RefreshProfilingButton();
  void RefreshButtons();
  void ApplyFont(const QFont& font);
  void ClearPanes();
  void ShowFreeSpace();
  void StateChanged(Core::State state);
  void ShowBlock(const JitBlock& block);
  void ShowRow(const QModelIndex& index);
  void ShowCurrentRowIfPaused();
  void EraseSelectedBlocks();
  void ShowTableMenu(const QPoint& pos);

  Core::System& m_system;

  QLineEdit* m_covers_filter = nullptr;
  QPushButton* m_clear_button = nullptr;
  QPushButton* m_profiling_button = nullptr;
  QPushButton* m_wipe_button = nullptr;
  QTableView* m_blocks_view = nullptr;
  JitBlockProxyModel* m_filter_model = nullptr;
  JitBlockTableModel* m_blocks_model = nullptr;
  QPlainTextEdit* m_guest_pane = nullptr;
  QPlainTextEdit* m_near_pane = nullptr;
  QPlainTextEdit* m_far_pane = nullptr;
  QSplitter* m_vertical_split = nullptr;
  QSplitter* m_horizontal_split = nullptr;
  ClickableStatusBar* m_status = nullptr;

  QMenu* m_row_menu = nullptr;
  QMenu* m_columns_menu = nullptr;

  QList<QMetaObject::Connection> m_links;
};
