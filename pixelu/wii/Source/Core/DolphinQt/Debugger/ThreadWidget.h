// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDockWidget>

#include "Common/CommonTypes.h"
#include "Common/Debug/Threads.h"

class QCloseEvent;
class QGroupBox;
class QLineEdit;
class QShowEvent;
class QTableWidget;

// Dock showing the game's OS thread list (from the OS globals in low memory), the saved context
// of the selected thread and a stack walk from its r1.
class ThreadWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit ThreadWidget(QWidget* parent = nullptr);
  ~ThreadWidget() override;

signals:
  void RequestBreakpoint(u32 addr);
  void RequestMemoryBreakpoint(u32 addr);
  void RequestWatch(QString name, u32 addr);
  void RequestViewInCode(u32 addr);
  void RequestViewInMemory(u32 addr);

protected:
  void closeEvent(QCloseEvent*) override;
  void showEvent(QShowEvent* event) override;

private:
  void BuildLayout();
  QGroupBox* BuildGlobalsBox();
  QGroupBox* BuildQueueBox();
  QGroupBox* BuildThreadListBox();
  QGroupBox* BuildContextBox();
  QGroupBox* BuildStackBox();

  void ShowAddressMenu(QTableWidget* table);

  void Refresh();
  void ShowContext(const Common::Debug::PartialContext& context);
  void ShowStack(const Core::CPUThreadGuard& guard, const Common::Debug::PartialContext& context);
  void ThreadPicked(int row);

  QGroupBox* m_globals_box = nullptr;
  QLineEdit* m_context_field = nullptr;
  QLineEdit* m_thread_field = nullptr;
  QLineEdit* m_default_field = nullptr;
  QLineEdit* m_head_field = nullptr;
  QLineEdit* m_tail_field = nullptr;
  QTableWidget* m_thread_list = nullptr;
  QTableWidget* m_context_grid = nullptr;
  QTableWidget* m_stack_list = nullptr;
  Common::Debug::Threads m_snapshot;
};
